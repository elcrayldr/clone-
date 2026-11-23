#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <cstdint>

namespace
{
constexpr int stateMagic = 0x434C4431;
constexpr int stateVersion = 1;
constexpr size_t maxStateBytes = 192u * 1024u * 1024u;
constexpr int maxSamplesPerPad = 576000;
}

DrumLabProcessor::DrumLabProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& sample : padSamples) sample.store(nullptr, std::memory_order_relaxed);
    for (auto& velocity : pendingVelocity) velocity.store(0.0f, std::memory_order_relaxed);
}

void DrumLabProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = juce::jlimit(8000.0, 384000.0, sampleRate);
    for (auto& voice : voices) voice = {};
}

void DrumLabProcessor::releaseResources()
{
    for (auto& voice : voices) voice = {};
}

bool DrumLabProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void DrumLabProcessor::triggerPad(int pad, float velocity) noexcept
{
    if (pad >= 0 && pad < padCount)
        pendingVelocity[static_cast<size_t>(pad)].store(juce::jlimit(0.0f, 1.0f, velocity),
                                                       std::memory_order_release);
}

bool DrumLabProcessor::isPadLoaded(int pad) const noexcept
{
    return pad >= 0 && pad < padCount
        && padSamples[static_cast<size_t>(pad)].load(std::memory_order_acquire) != nullptr;
}

void DrumLabProcessor::startVoice(int midiNote, float velocity) noexcept
{
    if (midiNote < firstMidiNote || midiNote >= firstMidiNote + padCount || velocity <= 0.0f)
        return;

    Voice* target = nullptr;
    for (auto& voice : voices)
        if (!voice.active) { target = &voice; break; }
    if (target == nullptr) target = &voices.front();

    *target = {};
    target->active = true;
    target->velocity = juce::jlimit(0.0f, 1.0f, velocity);
    target->sample = padSamples[static_cast<size_t>(midiNote - firstMidiNote)]
                         .load(std::memory_order_acquire);
    if (target->sample == nullptr) target->active = false;
}

float DrumLabProcessor::renderVoice(Voice& voice) noexcept
{
    if (!voice.active || voice.sample == nullptr) return 0.0f;

    const auto& audio = voice.sample->audio;
    const double position = static_cast<double>(voice.age) * voice.sample->sampleRate / currentSampleRate;
    const int index0 = static_cast<int>(position);
    if (index0 >= audio.getNumSamples())
    {
        voice.active = false;
        voice.sample = nullptr;
        return 0.0f;
    }

    const int index1 = juce::jmin(index0 + 1, audio.getNumSamples() - 1);
    const float fraction = static_cast<float>(position - index0);
    const float a = audio.getSample(0, index0);
    const float b = audio.getSample(0, index1);
    ++voice.age;
    return (a + (b - a) * fraction) * voice.velocity;
}

void DrumLabProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    for (int pad = 0; pad < padCount; ++pad)
    {
        const float velocity = pendingVelocity[static_cast<size_t>(pad)].exchange(
            0.0f, std::memory_order_acq_rel);
        if (velocity > 0.0f) startVoice(firstMidiNote + pad, velocity);
    }

    auto event = midi.begin();
    const auto eventEnd = midi.end();
    juce::MidiMessageMetadata metadata;
    bool hasEvent = event != eventEnd;
    if (hasEvent) metadata = *event;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        while (hasEvent && metadata.samplePosition <= i)
        {
            const auto message = metadata.getMessage();
            if (message.isNoteOn())
                startVoice(message.getNoteNumber(), message.getFloatVelocity());
            ++event;
            hasEvent = event != eventEnd;
            if (hasEvent) metadata = *event;
        }

        float mixed = 0.0f;
        for (auto& voice : voices) mixed += renderVoice(voice);
        mixed = std::tanh(mixed);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.setSample(channel, i, mixed);
    }
}

int DrumLabProcessor::importAndSlice(const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) { error = "Unsupported or unreadable audio file."; return 0; }

    if (reader->lengthInSamples <= 0
        || reader->lengthInSamples > static_cast<juce::int64>(reader->sampleRate * 600.0))
    {
        error = "Audio duration must be 10 minutes or less.";
        return 0;
    }

    juce::AudioBuffer<float> decoded(static_cast<int>(reader->numChannels),
                                     static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&decoded, 0, decoded.getNumSamples(), 0, true, true))
    {
        error = "Could not decode the selected audio file.";
        return 0;
    }

    juce::AudioBuffer<float> mono(1, decoded.getNumSamples());
    for (int i = 0; i < decoded.getNumSamples(); ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < decoded.getNumChannels(); ++ch) sum += decoded.getSample(ch, i);
        mono.setSample(0, i, sum / static_cast<float>(decoded.getNumChannels()));
    }

    const int window = juce::jmax(64, static_cast<int>(reader->sampleRate * 0.005));
    const int refractory = static_cast<int>(reader->sampleRate * 0.060);
    std::vector<float> energy;
    energy.reserve(static_cast<size_t>((mono.getNumSamples() + window - 1) / window));
    float maxEnergy = 0.0f;

    for (int start = 0; start < mono.getNumSamples(); start += window)
    {
        const int count = juce::jmin(window, mono.getNumSamples() - start);
        float sum = 0.0f;
        for (int n = 0; n < count; ++n)
        {
            const float x = mono.getSample(0, start + n);
            sum += x * x;
        }
        const float rms = std::sqrt(sum / static_cast<float>(count));
        energy.push_back(rms);
        maxEnergy = juce::jmax(maxEnergy, rms);
    }

    const float threshold = juce::jmax(0.0025f, maxEnergy * 0.12f);
    std::vector<int> onsets;
    onsets.reserve(padCount);
    int lastOnset = -refractory;

    for (size_t i = 1; i < energy.size() && onsets.size() < padCount; ++i)
    {
        if (energy[i] >= threshold && energy[i] > energy[i - 1] * 1.35f)
        {
            const int position = juce::jmax(0, static_cast<int>(i) * window - window / 2);
            if (position - lastOnset >= refractory)
            {
                onsets.push_back(position);
                lastOnset = position;
            }
        }
    }

    if (onsets.empty())
    {
        error = "No clear transients detected. Try audio with stronger drum hits.";
        return 0;
    }

    const int maxLength = static_cast<int>(reader->sampleRate * 1.25);
    std::vector<std::unique_ptr<Sample>> newSamples;
    newSamples.reserve(onsets.size());

    for (size_t i = 0; i < onsets.size(); ++i)
    {
        const int start = onsets[i];
        const int next = (i + 1 < onsets.size()) ? onsets[i + 1] : mono.getNumSamples();
        const int length = juce::jlimit(64, maxLength, next - start);
        auto sample = std::make_unique<Sample>();
        sample->sampleRate = reader->sampleRate;
        sample->audio.setSize(1, length);
        sample->audio.copyFrom(0, 0, mono, 0, start, length);
        newSamples.push_back(std::move(sample));
    }

    std::lock_guard<std::mutex> lock(sampleMutex);
    if (retainedSamples.size() + newSamples.size() > 512)
    {
        error = "Sample retention limit reached. Restart the plugin before importing again.";
        return 0;
    }

    for (auto& pointer : padSamples) pointer.store(nullptr, std::memory_order_release);
    const int count = static_cast<int>(newSamples.size());
    for (int i = 0; i < count; ++i)
    {
        const auto* pointer = newSamples[static_cast<size_t>(i)].get();
        retainedSamples.push_back(std::move(newSamples[static_cast<size_t>(i)]));
        padSamples[static_cast<size_t>(i)].store(pointer, std::memory_order_release);
    }
    return count;
}

bool DrumLabProcessor::exportKit(const juce::File& folder, juce::String& error)
{
    if (!folder.createDirectory() && !folder.isDirectory())
    {
        error = "Could not create destination folder.";
        return false;
    }

    juce::WavAudioFormat wav;
    int exported = 0;
    for (int pad = 0; pad < padCount; ++pad)
    {
        const auto* sample = padSamples[static_cast<size_t>(pad)].load(std::memory_order_acquire);
        if (sample == nullptr) continue;

        const auto file = folder.getChildFile("DRUMLAB_PAD_"
            + juce::String(pad + 1).paddedLeft('0', 2) + ".wav");
        std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
        if (stream == nullptr) { error = "Could not create " + file.getFileName(); return false; }

        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(stream.get(), sample->sampleRate, 1, 24, {}, 0));
        if (writer == nullptr) { error = "Could not write " + file.getFileName(); return false; }
        stream.release();
        if (!writer->writeFromAudioSampleBuffer(sample->audio, 0, sample->audio.getNumSamples()))
        {
            error = "Write failed for " + file.getFileName();
            return false;
        }
        ++exported;
    }

    if (exported == 0) { error = "There are no loaded pads to export."; return false; }
    return true;
}

void DrumLabProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    std::lock_guard<std::mutex> lock(sampleMutex);
    juce::MemoryOutputStream stream(destData, false);
    stream.writeInt(stateMagic);
    stream.writeInt(stateVersion);
    stream.writeInt(padCount);

    for (int pad = 0; pad < padCount; ++pad)
    {
        const auto* sample = padSamples[static_cast<size_t>(pad)].load(std::memory_order_acquire);
        const int length = sample != nullptr ? sample->audio.getNumSamples() : 0;
        stream.writeInt(length);
        if (length > 0)
        {
            stream.writeDouble(sample->sampleRate);
            stream.write(sample->audio.getReadPointer(0), static_cast<size_t>(length) * sizeof(float));
        }
    }
}

void DrumLabProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 12 + padCount * 4
        || static_cast<size_t>(sizeInBytes) > maxStateBytes) return;

    juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);
    if (stream.readInt() != stateMagic || stream.readInt() != stateVersion
        || stream.readInt() != padCount) return;

    std::array<std::unique_ptr<Sample>, padCount> restored {};
    int restoredCount = 0;
    size_t totalBytes = 0;

    for (int pad = 0; pad < padCount; ++pad)
    {
        const int length = stream.readInt();
        if (length < 0 || length > maxSamplesPerPad) return;
        if (length == 0) continue;

        const double rate = stream.readDouble();
        if (!std::isfinite(rate) || rate < 8000.0 || rate > 384000.0) return;
        const size_t bytes = static_cast<size_t>(length) * sizeof(float);
        totalBytes += bytes;
        if (totalBytes > maxStateBytes
            || stream.getNumBytesRemaining() < static_cast<juce::int64>(bytes)) return;

        auto sample = std::make_unique<Sample>();
        sample->sampleRate = rate;
        sample->audio.setSize(1, length);
        if (stream.read(sample->audio.getWritePointer(0), static_cast<int>(bytes))
            != static_cast<int>(bytes)) return;

        for (int i = 0; i < length; ++i)
            if (!std::isfinite(sample->audio.getSample(0, i))) return;

        restored[static_cast<size_t>(pad)] = std::move(sample);
        ++restoredCount;
    }

    if (stream.getNumBytesRemaining() != 0) return;

    std::lock_guard<std::mutex> lock(sampleMutex);
    if (retainedSamples.size() + static_cast<size_t>(restoredCount) > 512) return;

    for (int pad = 0; pad < padCount; ++pad)
    {
        auto& sample = restored[static_cast<size_t>(pad)];
        const Sample* pointer = sample.get();
        if (sample != nullptr) retainedSamples.push_back(std::move(sample));
        padSamples[static_cast<size_t>(pad)].store(pointer, std::memory_order_release);
    }
}

juce::AudioProcessorEditor* DrumLabProcessor::createEditor()
{
    return new DrumLabEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumLabProcessor();
}
