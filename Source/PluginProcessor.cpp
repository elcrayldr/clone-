#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr float twoPi = 6.2831853071795864769f;
constexpr float silenceThreshold = 0.00001f;
constexpr double maxImportSeconds = 600.0;
constexpr double maxSampleSeconds = 1.25;
}

CrayLdrDrumkitCloneAudioProcessor::CrayLdrDrumkitCloneAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

void CrayLdrDrumkitCloneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    std::lock_guard<std::mutex> lock(importMutex);
    juce::MemoryOutputStream stream(destData, false);
    stream.writeInt(0x434C4452); // CLDR
    stream.writeInt(1);          // state format version
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
}

void CrayLdrDrumkitCloneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 12)
        return;

    std::lock_guard<std::mutex> lock(importMutex);
    juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);
    if (stream.readInt() != 0x434C4452 || stream.readInt() != 1 || stream.readInt() != padCount)
        return;

    std::array<std::unique_ptr<SampleData>, padCount> restored {};
    size_t requiredSlots = 0;
    for (int pad = 0; pad < padCount; ++pad)
    {
        const int length = stream.readInt();
        if (length < 0 || length > 192000)
            return;
        if (length == 0)
            continue;

        const double rate = stream.readDouble();
        if (!std::isfinite(rate) || rate < 8000.0 || rate > 384000.0)
            return;

        auto sample = std::make_unique<SampleData>();
        sample->sampleRate = rate;
        sample->audio.setSize(1, length);
        const auto bytes = static_cast<size_t>(length) * sizeof(float);
        if (stream.getNumBytesRemaining() < static_cast<ssize_t>(bytes)
            || !stream.read(sample->audio.getWritePointer(0), bytes))
            return;

        for (int i = 0; i < length; ++i)
            if (!std::isfinite(sample->audio.getSample(0, i)))
                return;

        restored[static_cast<size_t>(pad)] = std::move(sample);
        ++requiredSlots;
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
        errorMessage = "Unsupported or unreadable audio format. Use WAV or AIFF in this prototype.";
        return 0;
    }

    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0
        || static_cast<double>(reader->lengthInSamples) / reader->sampleRate > maxImportSeconds)
    {
        errorMessage = "Audio duration is invalid or exceeds the 10-minute prototype limit.";
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

    for (int frame = 0; frame < frameCount; ++frame)
    {
        const int begin = frame * frameSize;
        const int end = juce::jmin(begin + frameSize, totalSamples);
        double sumSquares = 0.0;
        int count = 0;
        for (int channel = 0; channel < source.getNumChannels(); ++channel)
        {
            for (int sample = begin; sample < end; ++sample)
            {
                const double value = source.getSample(channel, sample);
                sumSquares += value * value;
                ++count;
            }
        }
        energy[static_cast<size_t>(frame)] = count > 0
            ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(count))) : 0.0f;
    }

    const float maxEnergy = *std::max_element(energy.begin(), energy.end());
    if (maxEnergy < 0.001f)
    {
        errorMessage = "The file is silent or its level is too low for prototype transient detection.";
        return 0;
    }

    const float threshold = juce::jmax(0.002f, maxEnergy * 0.085f);
    const int refractoryFrames = juce::jmax(1, static_cast<int>(0.075 * reader->sampleRate / frameSize));
    std::vector<std::pair<int, float>> onsets;
    int lastOnsetFrame = -refractoryFrames;

    for (int frame = 1; frame + 1 < frameCount; ++frame)
    {
        const float current = energy[static_cast<size_t>(frame)];
        if (current > threshold
            && current >= energy[static_cast<size_t>(frame - 1)]
            && current > energy[static_cast<size_t>(frame + 1)]
            && frame - lastOnsetFrame >= refractoryFrames)
        {
            onsets.emplace_back(frame * frameSize, current);
            lastOnsetFrame = frame;
        }
    }

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

    for (int pad = 0; pad < padCount; ++pad)
        padSamplePointers[static_cast<size_t>(pad)].store(nullptr, std::memory_order_release);

    const int preRoll = static_cast<int>(reader->sampleRate * 0.010);
    const int maxLength = static_cast<int>(reader->sampleRate * maxSampleSeconds);
    int loaded = 0;

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

        const auto* samplePointer = sample.get();
        samplePool.push_back(std::move(sample));
        padSamplePointers[static_cast<size_t>(loaded)].store(samplePointer, std::memory_order_release);
        ++loaded;
    }

    if (loaded == 0)
        errorMessage = "Transients were detected, but no segments were long enough to load.";
    return loaded;
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
