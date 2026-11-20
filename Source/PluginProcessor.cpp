#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DrumkitOnsetDetector.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
constexpr float twoPi = 6.2831853071795864769f;
constexpr float silenceThreshold = 0.00001f;
constexpr double maxImportSeconds = 600.0;
constexpr double maxSampleSeconds = 1.25;
constexpr size_t maxStateBytes = 256u * 1024u * 1024u;
constexpr size_t maxStateSamples = 32u * 1024u * 1024u;
constexpr juce::int64 maxDecodedAudioSamples = 64ll * 1024ll * 1024ll;
}

CrayLdrDrumkitCloneAudioProcessor::CrayLdrDrumkitCloneAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& pad : patternSteps)
        for (auto& step : pad)
            step.store(0, std::memory_order_relaxed);
}

CrayLdrDrumkitCloneAudioProcessor::~CrayLdrDrumkitCloneAudioProcessor()
{
    importWorkers.removeAllJobs(true, -1);
}

void CrayLdrDrumkitCloneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    std::lock_guard<std::mutex> lock(importMutex);
    juce::MemoryOutputStream stream(destData, false);
    stream.writeInt(0x434C4452); // CLDR
    stream.writeInt(2);          // state format version
    stream.writeInt(padCount);

    for (int pad = 0; pad < padCount; ++pad)
    {
        const auto* sample = padSamplePointers[static_cast<size_t>(pad)].load(std::memory_order_acquire);
        const int length = sample != nullptr ? sample->audio.getNumSamples() : 0;
        stream.writeInt(length);
        if (length > 0)
        {
            stream.writeDouble(sample->sampleRate);
            stream.write(sample->audio.getReadPointer(0), static_cast<size_t>(length) * sizeof(float));
        }
    }

    stream.writeFloat(patternTempo.load(std::memory_order_acquire));
    for (int pad = 0; pad < padCount; ++pad)
        for (int step = 0; step < patternStepCount; ++step)
            stream.writeByte(patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].load(std::memory_order_acquire));
}

void CrayLdrDrumkitCloneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 12 + padCount * static_cast<int>(sizeof(std::int32_t))
        || static_cast<size_t>(sizeInBytes) > maxStateBytes)
        return;

    std::lock_guard<std::mutex> lock(importMutex);
    juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);
    const int magic = stream.readInt();
    const int version = stream.readInt();
    const int storedPadCount = stream.readInt();
    if (magic != 0x434C4452 || (version != 1 && version != 2) || storedPadCount != padCount)
        return;

    std::array<std::unique_ptr<SampleData>, padCount> restored {};
    size_t requiredSlots = 0;
    size_t totalStateSamples = 0;
    for (int pad = 0; pad < padCount; ++pad)
    {
        const int length = stream.readInt();
        if (length < 0 || length > 480000)
            return;
        if (length == 0)
            continue;

        const double rate = stream.readDouble();
        if (!std::isfinite(rate) || rate < 8000.0 || rate > 384000.0
            || static_cast<double>(length) > rate * maxSampleSeconds + 1.0)
            return;
        totalStateSamples += static_cast<size_t>(length);
        if (totalStateSamples > maxStateSamples)
            return;

        auto sample = std::make_unique<SampleData>();
        sample->sampleRate = rate;
        sample->audio.setSize(1, length);
        const auto bytes = static_cast<size_t>(length) * sizeof(float);
        if (stream.getNumBytesRemaining() < static_cast<juce::int64>(bytes)
            || !stream.read(sample->audio.getWritePointer(0), static_cast<int>(bytes)))
            return;

        for (int i = 0; i < length; ++i)
            if (!std::isfinite(sample->audio.getSample(0, i)))
                return;

        restored[static_cast<size_t>(pad)] = std::move(sample);
        ++requiredSlots;
    }

    float restoredTempo = 120.0f;
    std::array<std::array<std::uint8_t, patternStepCount>, padCount> restoredPattern {};
    if (version >= 2)
    {
        constexpr size_t patternStateBytes = sizeof(float) + static_cast<size_t>(padCount * patternStepCount);
        if (stream.getNumBytesRemaining() < static_cast<juce::int64>(patternStateBytes))
            return;
        restoredTempo = stream.readFloat();
        if (!std::isfinite(restoredTempo) || restoredTempo < 40.0f || restoredTempo > 300.0f)
            return;
        for (int pad = 0; pad < padCount; ++pad)
            for (int step = 0; step < patternStepCount; ++step)
            {
                const auto value = static_cast<std::uint8_t>(stream.readByte());
                if (value > 1)
                    return;
                restoredPattern[static_cast<size_t>(pad)][static_cast<size_t>(step)] = value;
            }
    }

    if (stream.getNumBytesRemaining() != 0 || samplePool.size() + requiredSlots > maxRetainedSamples)
        return;

    for (int pad = 0; pad < padCount; ++pad)
    {
        auto& sample = restored[static_cast<size_t>(pad)];
        if (sample == nullptr)
        {
            padSamplePointers[static_cast<size_t>(pad)].store(nullptr, std::memory_order_release);
            continue;
        }

        const auto* pointer = sample.get();
        samplePool.push_back(std::move(sample));
        padSamplePointers[static_cast<size_t>(pad)].store(pointer, std::memory_order_release);
    }

    patternTempo.store(restoredTempo, std::memory_order_release);
    for (int pad = 0; pad < padCount; ++pad)
        for (int step = 0; step < patternStepCount; ++step)
            patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].store(
                restoredPattern[static_cast<size_t>(pad)][static_cast<size_t>(step)], std::memory_order_release);
}

void CrayLdrDrumkitCloneAudioProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = juce::jmax(8000.0, sampleRate);
    for (auto& voice : voices)
        voice = {};
}

void CrayLdrDrumkitCloneAudioProcessor::releaseResources()
{
    for (auto& voice : voices)
        voice = {};
}

bool CrayLdrDrumkitCloneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void CrayLdrDrumkitCloneAudioProcessor::triggerPad(int padIndex, float velocity) noexcept
{
    if (padIndex >= 0 && padIndex < padCount)
        pendingPadVelocities[static_cast<size_t>(padIndex)].store(juce::jlimit(0.0f, 1.0f, velocity), std::memory_order_release);
}

bool CrayLdrDrumkitCloneAudioProcessor::isPadLoaded(int padIndex) const noexcept
{
    return padIndex >= 0 && padIndex < padCount
        && padSamplePointers[static_cast<size_t>(padIndex)].load(std::memory_order_acquire) != nullptr;
}

void CrayLdrDrumkitCloneAudioProcessor::startVoice(int midiNote, float velocity) noexcept
{
    if (midiNote < firstMidiNote || midiNote > lastMidiNote || velocity <= 0.0f)
        return;

    auto* target = &voices.front();
    for (auto& voice : voices)
    {
        if (!voice.active)
        {
            target = &voice;
            break;
        }
        if (voice.age > target->age)
            target = &voice;
    }

    *target = {};
    target->active = true;
    target->note = midiNote;
    target->velocity = juce::jlimit(0.0f, 1.0f, velocity);
    target->noiseState = 0x9e3779b9u ^ static_cast<std::uint32_t>(midiNote * 7919 + midiNote * 17);
    const int pad = midiNote - firstMidiNote;
    target->sample = padSamplePointers[static_cast<size_t>(pad)].load(std::memory_order_acquire);
}

float CrayLdrDrumkitCloneAudioProcessor::renderVoice(Voice& voice) noexcept
{
    if (!voice.active)
        return 0.0f;

    if (voice.sample != nullptr)
    {
        const auto& audio = voice.sample->audio;
        const double sourcePosition = static_cast<double>(voice.age) * voice.sample->sampleRate / currentSampleRate;
        const int index0 = static_cast<int>(sourcePosition);
        if (index0 >= audio.getNumSamples())
        {
            voice.active = false;
            voice.sample = nullptr;
            return 0.0f;
        }

        const int index1 = juce::jmin(index0 + 1, audio.getNumSamples() - 1);
        const float fraction = static_cast<float>(sourcePosition - static_cast<double>(index0));
        const float a = audio.getSample(0, index0);
        const float b = audio.getSample(0, index1);
        const float value = a + (b - a) * fraction;
        ++voice.age;
        return value * voice.velocity;
    }

    const int pad = voice.note - firstMidiNote;
    const int family = pad / 8;
    const float t = static_cast<float>(voice.age / currentSampleRate);
    const float velocity = voice.velocity;
    float sample = 0.0f;

    voice.noiseState ^= voice.noiseState << 13;
    voice.noiseState ^= voice.noiseState >> 17;
    voice.noiseState ^= voice.noiseState << 5;
    const float noise = (static_cast<float>(voice.noiseState & 0xffffu) / 32767.5f) - 1.0f;

    const float decay = std::exp(-t * (family == 4 || family == 7 ? 4.0f : 9.0f));
    const float shortDecay = std::exp(-t * 22.0f);
    const float pitch = 1.0f + static_cast<float>(pad % 8) * 0.045f;

    switch (family)
    {
        case 0: // Kick
        {
            const float f = (48.0f + 95.0f * std::exp(-t * 28.0f)) * pitch;
            voice.phase += twoPi * f / static_cast<float>(currentSampleRate);
            sample = std::sin(static_cast<float>(voice.phase)) * std::exp(-t * 7.0f);
            break;
        }
        case 1: // Snare
            voice.phase += twoPi * (175.0f * pitch) / static_cast<float>(currentSampleRate);
            sample = (0.72f * noise * shortDecay) + (0.28f * std::sin(static_cast<float>(voice.phase)) * decay);
            break;
        case 2: // Clap
        {
            const float pulse = std::exp(-std::fmod(t, 0.035f) * 95.0f);
            sample = noise * pulse * std::exp(-t * 13.0f);
            break;
        }
        case 3: // Closed hat
            sample = noise * std::exp(-t * 65.0f);
            break;
        case 4: // Open hat
            sample = noise * decay * 0.65f;
            break;
        case 5: // Tom
        {
            const float f = (105.0f + 75.0f * std::exp(-t * 12.0f)) * pitch;
            voice.phase += twoPi * f / static_cast<float>(currentSampleRate);
            sample = std::sin(static_cast<float>(voice.phase)) * std::exp(-t * 5.5f);
            break;
        }
        case 6: // Percussion
            voice.phase += twoPi * (280.0f + static_cast<float>(pad % 8) * 65.0f) / static_cast<float>(currentSampleRate);
            sample = (0.55f * std::sin(static_cast<float>(voice.phase)) + 0.45f * noise) * std::exp(-t * 15.0f);
            break;
        default: // Cymbal
            sample = noise * decay * 0.55f;
            voice.phase += twoPi * (3300.0f + static_cast<float>(pad % 8) * 220.0f) / static_cast<float>(currentSampleRate);
            sample += 0.12f * std::sin(static_cast<float>(voice.phase)) * decay;
            break;
    }

    ++voice.age;
    const float maxDuration = family == 3 ? 0.22f : (family == 4 || family == 7 ? 1.5f : 0.9f);
    if (t >= maxDuration || (std::abs(sample) < silenceThreshold && t > 0.08f))
        voice.active = false;

    return sample * velocity * 0.22f;
}

int CrayLdrDrumkitCloneAudioProcessor::importAudioFile(const juce::File& file, juce::String& errorMessage)
{
    std::lock_guard<std::mutex> importLock(importMutex);
    errorMessage.clear();
    if (!file.existsAsFile())
    {
        errorMessage = "The selected audio file does not exist.";
        return 0;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        errorMessage = "Unsupported or unreadable audio format. Use WAV, AIFF, FLAC or Ogg Vorbis.";
        return 0;
    }

    if (reader->sampleRate < 8000.0 || reader->sampleRate > 384000.0 || reader->lengthInSamples <= 0
        || static_cast<double>(reader->lengthInSamples) / reader->sampleRate > maxImportSeconds
        || reader->numChannels < 1 || reader->numChannels > 2
        || reader->lengthInSamples > std::numeric_limits<int>::max()
        || reader->lengthInSamples * static_cast<juce::int64>(reader->numChannels) > maxDecodedAudioSamples)
    {
        errorMessage = "Audio exceeds import limits: 8–384 kHz, mono/stereo, 10 minutes, and 256 MB decoded audio.";
        return 0;
    }

    const int totalSamples = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> source(static_cast<int>(reader->numChannels), totalSamples);
    if (!reader->read(&source, 0, totalSamples, 0, true, true))
    {
        errorMessage = "Could not decode the selected audio file.";
        return 0;
    }

    const int frameSize = juce::jmax(1, static_cast<int>(reader->sampleRate * 0.005));
    const int frameCount = (totalSamples + frameSize - 1) / frameSize;
    std::vector<float> energy(static_cast<size_t>(frameCount), 0.0f);
    std::vector<float> highBandEnergy(static_cast<size_t>(frameCount), 0.0f);

    for (int frame = 0; frame < frameCount; ++frame)
    {
        const int begin = frame * frameSize;
        const int end = juce::jmin(begin + frameSize, totalSamples);
        double sumSquares = 0.0;
        double highSquares = 0.0;
        int count = 0;
        int highCount = 0;

        for (int channel = 0; channel < source.getNumChannels(); ++channel)
        {
            float previous = begin > 0 ? source.getSample(channel, begin - 1) : 0.0f;
            for (int sample = begin; sample < end; ++sample)
            {
                const float value = source.getSample(channel, sample);
                sumSquares += static_cast<double>(value) * value;
                const float difference = value - previous;
                highSquares += static_cast<double>(difference) * difference;
                previous = value;
                ++count;
                ++highCount;
            }
        }

        energy[static_cast<size_t>(frame)] = count > 0
            ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(count))) : 0.0f;
        highBandEnergy[static_cast<size_t>(frame)] = highCount > 0
            ? static_cast<float>(std::sqrt(highSquares / static_cast<double>(highCount))) : 0.0f;
    }

    const float maxEnergy = *std::max_element(energy.begin(), energy.end());
    if (maxEnergy < 0.0001f)
    {
        errorMessage = "The file is silent or its level is too low for transient detection.";
        return 0;
    }

    const auto detected = crayldr::detectOnsets(energy, highBandEnergy, frameSize, reader->sampleRate);
    std::vector<std::pair<int, float>> onsets;
    onsets.reserve(detected.size());
    for (const auto& onset : detected)
        onsets.emplace_back(onset.samplePosition, onset.strength);

    if (onsets.empty())
    {
        errorMessage = "No usable transients detected. Try a louder or less compressed source.";
        return 0;
    }

    if (onsets.size() > static_cast<size_t>(padCount))
    {
        std::partial_sort(onsets.begin(), onsets.begin() + padCount, onsets.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        onsets.resize(static_cast<size_t>(padCount));
    }
    std::sort(onsets.begin(), onsets.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    const size_t retainedCount = samplePool.size();
    if (retainedCount >= maxRetainedSamples)
    {
        errorMessage = "Sample memory safety limit reached. Restart the plugin before importing another kit.";
        return 0;
    }

    const int preRoll = static_cast<int>(reader->sampleRate * 0.010);
    const int maxLength = static_cast<int>(reader->sampleRate * maxSampleSeconds);
    int loaded = 0;
    std::array<std::unique_ptr<SampleData>, padCount> importedSamples {};

    const size_t availableSlots = maxRetainedSamples - retainedCount;
    const size_t maxToLoad = juce::jmin(static_cast<size_t>(padCount), availableSlots);

    for (size_t i = 0; i < onsets.size() && static_cast<size_t>(loaded) < maxToLoad; ++i)
    {
        const int start = juce::jmax(0, onsets[i].first - preRoll);
        int end = juce::jmin(totalSamples, start + maxLength);
        if (i + 1 < onsets.size())
        {
            const int nextStart = juce::jmax(0, onsets[i + 1].first - preRoll);
            if (nextStart - start > static_cast<int>(reader->sampleRate * 0.035))
                end = juce::jmin(end, nextStart);
        }

        const int length = end - start;
        if (length < static_cast<int>(reader->sampleRate * 0.025))
            continue;

        auto sample = std::make_unique<SampleData>();
        sample->audio.setSize(1, length);
        sample->sampleRate = reader->sampleRate;

        for (int position = 0; position < length; ++position)
        {
            float mono = 0.0f;
            for (int channel = 0; channel < source.getNumChannels(); ++channel)
                mono += source.getSample(channel, start + position);
            mono /= static_cast<float>(source.getNumChannels());
            sample->audio.setSample(0, position, mono);
        }

        // Short edge fades prevent clicks without softening the main transient.
        const int fadeInSamples = juce::jmin(length / 2, juce::jmax(1, static_cast<int>(reader->sampleRate * 0.001)));
        const int fadeOutSamples = juce::jmin(length / 2, juce::jmax(1, static_cast<int>(reader->sampleRate * 0.003)));
        auto* sampleData = sample->audio.getWritePointer(0);
        for (int position = 0; position < fadeInSamples; ++position)
        {
            const float gain = fadeInSamples > 1
                ? static_cast<float>(position) / static_cast<float>(fadeInSamples - 1) : 1.0f;
            sampleData[position] *= gain;
        }
        for (int position = 0; position < fadeOutSamples; ++position)
        {
            const float gain = fadeOutSamples > 1
                ? static_cast<float>(fadeOutSamples - 1 - position) / static_cast<float>(fadeOutSamples - 1) : 0.0f;
            sampleData[length - fadeOutSamples + position] *= gain;
        }

        importedSamples[static_cast<size_t>(loaded)] = std::move(sample);
        ++loaded;
    }

    if (loaded == 0)
    {
        errorMessage = "Transients were detected, but no segments were long enough to load.";
        return 0;
    }

    // Publish the replacement kit only after every segment has been prepared successfully.
    for (int pad = 0; pad < padCount; ++pad)
    {
        if (pad < loaded)
        {
            const auto* samplePointer = importedSamples[static_cast<size_t>(pad)].get();
            samplePool.push_back(std::move(importedSamples[static_cast<size_t>(pad)]));
            padSamplePointers[static_cast<size_t>(pad)].store(samplePointer, std::memory_order_release);
        }
        else
        {
            padSamplePointers[static_cast<size_t>(pad)].store(nullptr, std::memory_order_release);
        }
    }
    return loaded;
}


void CrayLdrDrumkitCloneAudioProcessor::importAudioFileAsync(
    juce::File file, std::function<void(int, juce::String)> completion)
{
    importWorkers.addJob([this, file = std::move(file), completion = std::move(completion)]() mutable
    {
        juce::String error;
        const int count = importAudioFile(file, error);
        juce::MessageManager::callAsync(
            [completion = std::move(completion), count, error = std::move(error)]() mutable
            {
                if (completion)
                    completion(count, error);
            });
    });
}

bool CrayLdrDrumkitCloneAudioProcessor::exportKitToDirectory(
    const juce::File& directory, juce::String& errorMessage)
{
    std::lock_guard<std::mutex> lock(importMutex);
    errorMessage.clear();
    if (!directory.createDirectory() && !directory.isDirectory())
    {
        errorMessage = "Could not create the selected export directory.";
        return false;
    }

    juce::WavAudioFormat wav;
    int exported = 0;
    for (int pad = 0; pad < padCount; ++pad)
    {
        const auto* sample = padSamplePointers[static_cast<size_t>(pad)].load(std::memory_order_acquire);
        if (sample == nullptr || sample->audio.getNumSamples() <= 0)
            continue;

        const auto note = firstMidiNote + pad;
        const auto name = "Pad_" + juce::String(pad + 1).paddedLeft('0', 2)
            + "_MIDI_" + juce::String(note) + ".wav";
        const auto outputFile = directory.getChildFile(name);
        auto outputStream = std::unique_ptr<juce::OutputStream>(outputFile.createOutputStream());
        if (outputStream == nullptr)
        {
            errorMessage = "Could not create file: " + outputFile.getFileName();
            return false;
        }

        auto* rawWriter = wav.createWriterFor(outputStream.get(), sample->sampleRate, 1u, 24, {}, 0);
        if (rawWriter == nullptr)
        {
            errorMessage = "Could not initialise WAV writer for: " + outputFile.getFileName();
            return false;
        }
        outputStream.release();
        std::unique_ptr<juce::AudioFormatWriter> writer(rawWriter);

        if (!writer->writeFromAudioSampleBuffer(sample->audio, 0, sample->audio.getNumSamples()))
        {
            errorMessage = "Failed while writing: " + outputFile.getFileName();
            return false;
        }
        ++exported;
    }

    if (exported == 0)
    {
        errorMessage = "There are no imported samples to export.";
        return false;
    }

    juce::String manifest = "CRAY LDR - DRUMKIT CLONE KIT\\nWAV format: PCM 24-bit mono\\n";
    manifest += "Pad\\tMIDI note\\tSample rate\\tFrames\\tFile\\n";
    for (int pad = 0; pad < padCount; ++pad)
    {
        const auto* sample = padSamplePointers[static_cast<size_t>(pad)].load(std::memory_order_acquire);
        if (sample == nullptr)
            continue;
        const auto note = firstMidiNote + pad;
        const auto name = "Pad_" + juce::String(pad + 1).paddedLeft('0', 2)
            + "_MIDI_" + juce::String(note) + ".wav";
        manifest += juce::String(pad + 1) + "\\t" + juce::String(note) + "\\t"
            + juce::String(sample->sampleRate, 0) + "\\t"
            + juce::String(sample->audio.getNumSamples()) + "\\t" + name + "\\n";
    }

    if (!directory.getChildFile("KIT_MANIFEST.txt").replaceWithText(manifest))
    {
        errorMessage = "WAV files were exported, but the kit manifest could not be written.";
        return false;
    }
    return true;
}


void CrayLdrDrumkitCloneAudioProcessor::setPatternStep(int pad, int step, bool enabled) noexcept
{
    if (pad >= 0 && pad < padCount && step >= 0 && step < patternStepCount)
        patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].store(
            enabled ? 1u : 0u, std::memory_order_release);
}

bool CrayLdrDrumkitCloneAudioProcessor::getPatternStep(int pad, int step) const noexcept
{
    if (pad < 0 || pad >= padCount || step < 0 || step >= patternStepCount)
        return false;
    return patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].load(std::memory_order_acquire) != 0;
}

void CrayLdrDrumkitCloneAudioProcessor::setPatternTempo(float bpm) noexcept
{
    if (std::isfinite(bpm))
        patternTempo.store(juce::jlimit(40.0f, 300.0f, bpm), std::memory_order_release);
}

float CrayLdrDrumkitCloneAudioProcessor::getPatternTempo() const noexcept
{
    return patternTempo.load(std::memory_order_acquire);
}

bool CrayLdrDrumkitCloneAudioProcessor::exportMidiPattern(
    const juce::File& file, juce::String& errorMessage) const
{
    errorMessage.clear();
    const float bpm = getPatternTempo();
    juce::MidiMessageSequence sequence;
    sequence.addEvent(juce::MidiMessage::tempoMetaEvent(
        static_cast<int>(std::round(60000000.0 / static_cast<double>(bpm)))), 0.0);
    sequence.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0.0);

    int eventCount = 0;
    constexpr double ticksPerSixteenth = 240.0;
    constexpr double noteLengthTicks = 120.0;
    for (int pad = 0; pad < padCount; ++pad)
    {
        for (int step = 0; step < patternStepCount; ++step)
        {
            if (!getPatternStep(pad, step))
                continue;

            const double tick = static_cast<double>(step) * ticksPerSixteenth;
            const int note = firstMidiNote + pad;
            sequence.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), tick);
            sequence.addEvent(juce::MidiMessage::noteOff(1, note), tick + noteLengthTicks);
            ++eventCount;
        }
    }

    if (eventCount == 0)
    {
        errorMessage = "The 16-step pattern is empty.";
        return false;
    }

    sequence.updateMatchedPairs();
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote(960);
    midi.addTrack(sequence);

    juce::FileOutputStream output(file);
    if (!output.openedOk())
    {
        errorMessage = "Could not create MIDI file: " + file.getFileName();
        return false;
    }

    if (!midi.writeTo(output))
    {
        errorMessage = "Failed while writing the MIDI pattern.";
        return false;
    }
    return true;
}

void CrayLdrDrumkitCloneAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    for (int pad = 0; pad < padCount; ++pad)
    {
        const float velocity = pendingPadVelocities[static_cast<size_t>(pad)].exchange(0.0f, std::memory_order_acq_rel);
        if (velocity > 0.0f)
            startVoice(firstMidiNote + pad, velocity);
    }

    auto midiIt = midiMessages.begin();
    auto midiEnd = midiMessages.end();
    juce::MidiMessageMetadata midiEvent;
    bool hasMidiEvent = midiIt != midiEnd;
    if (hasMidiEvent)
        midiEvent = *midiIt;

    for (int sampleIndex = 0; sampleIndex < buffer.getNumSamples(); ++sampleIndex)
    {
        while (hasMidiEvent && midiEvent.samplePosition <= sampleIndex)
        {
            const auto message = midiEvent.getMessage();
            if (message.isNoteOn())
                startVoice(message.getNoteNumber(), message.getFloatVelocity());
            ++midiIt;
            hasMidiEvent = midiIt != midiEnd;
            if (hasMidiEvent)
                midiEvent = *midiIt;
        }

        float mixed = 0.0f;
        for (auto& voice : voices)
            mixed += renderVoice(voice);

        mixed = std::tanh(mixed);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.setSample(channel, sampleIndex, mixed);
    }
}

juce::AudioProcessorEditor* CrayLdrDrumkitCloneAudioProcessor::createEditor()
{
    return new CrayLdrDrumkitCloneAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CrayLdrDrumkitCloneAudioProcessor();
}
