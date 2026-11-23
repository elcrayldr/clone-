#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DrumkitOnsetDetector.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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

CrayLdrDrumLabV1AudioProcessor::CrayLdrDrumLabV1AudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (auto& sample : padSamplePointers)
        sample.store(nullptr, std::memory_order_relaxed);

    for (auto& velocity : pendingPadVelocities)
        velocity.store(0.0f, std::memory_order_relaxed);

    for (auto& pad : patternSteps)
        for (auto& step : pad)
            step.store(0, std::memory_order_relaxed);
}

CrayLdrDrumLabV1AudioProcessor::~CrayLdrDrumLabV1AudioProcessor()
{
    if (importWorkers != nullptr)
        importWorkers->removeAllJobs(true, -1);
}

namespace
{
constexpr int stateMagic = 0x434C4452; // CLDR
constexpr int stateVersion = 3;
constexpr size_t maxEncodedStateBytes = 48u * 1024u * 1024u;
constexpr int maxPadSamples = 480000;
enum PadCodec : std::uint8_t { codecEmpty = 0, codecRawFloat = 1, codecFlac = 2 };

std::uint32_t fnv1a(const void* data, size_t size) noexcept
{
    auto* p = static_cast<const std::uint8_t*>(data);
    std::uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// Diagnostic log: %TEMP%\CRAY_LDR_DRUM_LAB_V1_state.log. Never throws, never blocks audio.
void stateLog(const juce::String& message) noexcept
{
    try
    {
        auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getChildFile("CRAY_LDR_DRUM_LAB_V1_state.log");
        if (file.getSize() > 256 * 1024)
            file.deleteFile();
        file.appendText(juce::Time::getCurrentTime().toString(true, true, true, true) + "  " + message + "\n");
    }
    catch (...) {}
}

// Returns the codec used (codecEmpty on failure). Lossless FLAC (24-bit) keeps the state small;
// raw float is the fallback (e.g. peaks above 0 dBFS, which FLAC would clip).
std::uint8_t encodePad(const juce::AudioBuffer<float>& audio, double rate, juce::MemoryBlock& payload)
{
    const int length = audio.getNumSamples();
    if (length <= 0 || audio.getNumChannels() < 1)
        return codecEmpty;
    payload.reset();

    try
    {
        if (audio.getMagnitude(0, 0, length) <= 1.0f)
        {
            juce::FlacAudioFormat flac;
            // The writer owns the stream; on failure a tiny stream object may leak, which is
            // preferable to a double delete.
            auto* stream = new juce::MemoryOutputStream(payload, false);
            std::unique_ptr<juce::AudioFormatWriter> writer(
                flac.createWriterFor(stream, rate, 1u, 24, {}, 5));
            if (writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, length))
            {
                writer.reset(); // flush into payload
                if (payload.getSize() > 0)
                    return codecFlac;
            }
            writer.reset();
            payload.reset();
        }
    }
    catch (...) { payload.reset(); }

    payload.append(audio.getReadPointer(0), static_cast<size_t>(length) * sizeof(float));
    return codecRawFloat;
}

bool decodePad(std::uint8_t codec, const std::uint8_t* payload, size_t size, int length, juce::AudioBuffer<float>& out)
{
    try
    {
        out.setSize(1, length);
        if (codec == codecRawFloat)
        {
            if (size != static_cast<size_t>(length) * sizeof(float))
                return false;
            std::memcpy(out.getWritePointer(0), payload, size);
        }
        else if (codec == codecFlac)
        {
            juce::FlacAudioFormat flac;
            auto* stream = new juce::MemoryInputStream(payload, size, false);
            std::unique_ptr<juce::AudioFormatReader> reader(flac.createReaderFor(stream, true));
            if (reader == nullptr || reader->numChannels != 1 || reader->lengthInSamples != length)
                return false;
            if (!reader->read(&out, 0, length, 0, true, true))
                return false;
        }
        else
            return false;

        const auto* p = out.getReadPointer(0);
        for (int i = 0; i < length; ++i)
            if (!std::isfinite(p[i]))
                return false;
        return true;
    }
    catch (...) { return false; }
}
} // namespace

void CrayLdrDrumLabV1AudioProcessor::sweepRetiredLocked()
{
    const auto seen = audioSeenGeneration.load(std::memory_order_acquire);
    retiredSamples.erase(std::remove_if(retiredSamples.begin(), retiredSamples.end(),
        [seen](const RetiredSample& r) { return r.generation <= seen; }), retiredSamples.end());
}

// Must be called with importMutex held. Publishes a whole kit atomically from the audio thread's
// point of view: pointers first, then the generation counter; old samples are retired, not freed.
bool CrayLdrDrumLabV1AudioProcessor::publishKitLocked(Kit& kit)
{
    sweepRetiredLocked();
    if (retiredSamples.size() + samplePool.size() > maxRetainedSamples)
        return false;

    std::vector<std::unique_ptr<SampleData>> newPool;
    newPool.reserve(static_cast<size_t>(padCount));
    for (int pad = 0; pad < padCount; ++pad)
    {
        auto& sample = kit[static_cast<size_t>(pad)];
        const SampleData* pointer = sample.get();
        if (sample != nullptr)
            newPool.push_back(std::move(sample));
        padSamplePointers[static_cast<size_t>(pad)].store(pointer, std::memory_order_release);
    }

    const auto newGeneration = kitGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (auto& old : samplePool)
        retiredSamples.push_back({ std::move(old), newGeneration });
    samplePool = std::move(newPool);
    return true;
}

void CrayLdrDrumLabV1AudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    destData.reset();
    const auto startTime = juce::Time::getMillisecondCounter();
    int storedPads = 0;

    try
    {
        juce::MemoryOutputStream body;
        body.writeFloat(patternTempo.load(std::memory_order_acquire));
        for (int pad = 0; pad < padCount; ++pad)
            for (int step = 0; step < patternStepCount; ++step)
                body.writeByte(static_cast<char>(
                    patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].load(std::memory_order_acquire) != 0 ? 1 : 0));

        {
            std::lock_guard<std::mutex> lock(importMutex);
            size_t total = 0;
            for (int pad = 0; pad < padCount; ++pad)
            {
                const auto* sample = padSamplePointers[static_cast<size_t>(pad)].load(std::memory_order_acquire);
                juce::MemoryBlock payload;
                const auto codec = sample != nullptr ? encodePad(sample->audio, sample->sampleRate, payload)
                                                     : static_cast<std::uint8_t>(codecEmpty);
                if (codec == codecEmpty || total + payload.getSize() > maxEncodedStateBytes)
                {
                    if (codec != codecEmpty)
                        stateLog("getState: pad " + juce::String(pad + 1) + " omitted, state size cap reached");
                    body.writeByte(static_cast<char>(codecEmpty));
                    continue;
                }
                body.writeByte(static_cast<char>(codec));
                body.writeInt(sample->audio.getNumSamples());
                body.writeDouble(sample->sampleRate);
                body.writeInt(static_cast<int>(payload.getSize()));
                body.write(payload.getData(), payload.getSize());
                total += payload.getSize();
                ++storedPads;
            }
        }

        juce::MemoryOutputStream out(destData, false);
        out.writeInt(stateMagic);
        out.writeInt(stateVersion);
        out.writeInt(padCount);
        out.writeInt(static_cast<int>(body.getDataSize()));
        out.writeInt(static_cast<int>(fnv1a(body.getData(), body.getDataSize())));
        out.write(body.getData(), body.getDataSize());
    }
    catch (...)
    {
        destData.reset();
        stateLog("getState: exception while building state");
    }

    if (destData.getSize() == 0) // never hand the host an empty state
    {
        juce::MemoryOutputStream out(destData, false);
        out.writeInt(stateMagic);
        out.writeInt(stateVersion);
        out.writeInt(padCount);
        out.writeInt(0);
        out.writeInt(0);
    }

    stateLog("getState: bytes=" + juce::String(static_cast<juce::int64>(destData.getSize()))
             + " pads=" + juce::String(storedPads)
             + " ms=" + juce::String(static_cast<int>(juce::Time::getMillisecondCounter() - startTime)));
}

void CrayLdrDrumLabV1AudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // Hosts may call this while the audio device is active. Copy the host-owned block
    // immediately, then parse/decode FLAC and allocate sample buffers on the worker.
    // The worker is owned by this processor; the destructor waits for queued jobs.
    if (data == nullptr || sizeInBytes < 12 || static_cast<size_t>(sizeInBytes) > maxStateBytes)
    {
        stateLog("setState: rejected invalid input size");        return;
    }

    try
    {
        juce::MemoryBlock stateCopy(data, static_cast<size_t>(sizeInBytes));
        std::lock_guard<std::mutex> lock(importMutex);
        if (importWorkers == nullptr)
            importWorkers = std::make_unique<juce::ThreadPool>(1);

        importWorkers->addJob([this, stateCopy = std::move(stateCopy), sizeInBytes]() mutable
        {
            try
            {
                const bool ok = restoreStateInternal(stateCopy.getData(), sizeInBytes);
                stateLog(juce::String("setState: size=") + juce::String(sizeInBytes)
                         + (ok ? " restored (background)" : " ignored (kept current state)"));
            }
            catch (const std::exception& e)
            {
                stateLog(juce::String("setState: background exception ") + e.what());
            }
            catch (...)
            {
                stateLog("setState: background unknown exception");
            }
        });
    }
    catch (const std::exception& e)
    {
        stateLog(juce::String("setState: could not queue restore: ") + e.what());
    }
    catch (...)
    {
        stateLog("setState: could not queue restore (unknown exception)");
    }
}

bool CrayLdrDrumLabV1AudioProcessor::restoreStateInternal(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 12 || static_cast<size_t>(sizeInBytes) > maxStateBytes)
        return false;

    juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);
    const int magic = stream.readInt();
    const int version = stream.readInt();
    const int storedPadCount = stream.readInt();
    if (magic != stateMagic || version < 1 || version > stateVersion || storedPadCount != padCount)
    {
        stateLog("setState: unknown header magic/version/padCount");
        return false;
    }

    Kit restored {};
    float restoredTempo = 120.0f;
    std::array<std::array<std::uint8_t, patternStepCount>, padCount> restoredPattern {};
    bool havePattern = false;
    int padsOk = 0, padsBad = 0;

    if (version >= 3)
    {
        if (sizeInBytes < 20)
            return false;
        const int bodySize = stream.readInt();
        const auto crc = static_cast<std::uint32_t>(stream.readInt());
        if (bodySize <= 0)
            return true; // valid empty state: keep defaults, do not touch the current kit
        if (static_cast<juce::int64>(bodySize) > stream.getNumBytesRemaining())
        {
            stateLog("setState: truncated body");
            return false;
        }
        const auto* body = static_cast<const std::uint8_t*>(data) + 20;
        if (fnv1a(body, static_cast<size_t>(bodySize)) != crc)
        {
            stateLog("setState: checksum mismatch");
            return false;
        }

        juce::MemoryInputStream b(body, static_cast<size_t>(bodySize), false);
        constexpr size_t patternBytes = static_cast<size_t>(padCount * patternStepCount);
        if (b.getNumBytesRemaining() < static_cast<juce::int64>(sizeof(float) + patternBytes))
            return false;
        restoredTempo = b.readFloat();
        restoredTempo = std::isfinite(restoredTempo) ? juce::jlimit(40.0f, 300.0f, restoredTempo) : 120.0f;
        for (int pad = 0; pad < padCount; ++pad)
            for (int step = 0; step < patternStepCount; ++step)
                restoredPattern[static_cast<size_t>(pad)][static_cast<size_t>(step)] = b.readByte() != 0 ? 1 : 0;
        havePattern = true;

        for (int pad = 0; pad < padCount; ++pad)
        {
            if (b.isExhausted())
                break;
            const auto codec = static_cast<std::uint8_t>(b.readByte());
            if (codec == codecEmpty)
                continue;
            if (b.getNumBytesRemaining() < 16)
                break;
            const int length = b.readInt();
            const double rate = b.readDouble();
            const int payloadBytes = b.readInt();
            if (payloadBytes <= 0 || static_cast<juce::int64>(payloadBytes) > b.getNumBytesRemaining())
                break; // cannot resync after a bad size: stop reading samples, keep what we have
            const auto* payload = body + static_cast<size_t>(b.getPosition());
            b.setPosition(b.getPosition() + payloadBytes);

            auto sample = std::make_unique<SampleData>();
            if (length < 1 || length > maxPadSamples || !std::isfinite(rate) || rate < 8000.0 || rate > 384000.0
                || static_cast<double>(length) > rate * maxSampleSeconds + 1.0
                || !decodePad(codec, payload, static_cast<size_t>(payloadBytes), length, sample->audio))
            {
                ++padsBad;
                stateLog("setState: pad " + juce::String(pad + 1) + " could not be restored, skipped");
                continue;
            }
            sample->sampleRate = rate;
            restored[static_cast<size_t>(pad)] = std::move(sample);
            ++padsOk;
        }
    }
    else // legacy v1/v2: raw float samples, written by earlier builds
    {
        size_t totalStateSamples = 0;
        for (int pad = 0; pad < padCount; ++pad)
        {
            if (stream.getNumBytesRemaining() < 4)
                return false;
            const int length = stream.readInt();
            if (length < 0 || length > maxPadSamples)
                return false;
            if (length == 0)
                continue;
            if (stream.getNumBytesRemaining() < 8)
                return false;
            const double rate = stream.readDouble();
            if (!std::isfinite(rate) || rate < 8000.0 || rate > 384000.0
                || static_cast<double>(length) > rate * maxSampleSeconds + 1.0)
                return false;
            totalStateSamples += static_cast<size_t>(length);
            if (totalStateSamples > maxStateSamples)
                return false;
            const auto bytes = static_cast<size_t>(length) * sizeof(float);
            if (stream.getNumBytesRemaining() < static_cast<juce::int64>(bytes))
                return false;
            auto sample = std::make_unique<SampleData>();
            sample->sampleRate = rate;
            sample->audio.setSize(1, length);
            if (stream.read(sample->audio.getWritePointer(0), static_cast<int>(bytes)) != static_cast<int>(bytes))
                return false;
            bool finite = true;
            for (int i = 0; i < length && finite; ++i)
                finite = std::isfinite(sample->audio.getSample(0, i));
            if (!finite)
            {
                ++padsBad;
                continue;
            }
            restored[static_cast<size_t>(pad)] = std::move(sample);
            ++padsOk;
        }

        if (version >= 2 && stream.getNumBytesRemaining() >= static_cast<juce::int64>(sizeof(float) + padCount * patternStepCount))
        {
            restoredTempo = stream.readFloat();
            restoredTempo = std::isfinite(restoredTempo) ? juce::jlimit(40.0f, 300.0f, restoredTempo) : 120.0f;
            for (int pad = 0; pad < padCount; ++pad)
                for (int step = 0; step < patternStepCount; ++step)
                    restoredPattern[static_cast<size_t>(pad)][static_cast<size_t>(step)] = stream.readByte() != 0 ? 1 : 0;
            havePattern = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(importMutex);
        if (!publishKitLocked(restored))
        {
            stateLog("setState: retired-sample limit reached, kit not applied");
            return false;
        }
    }

    if (havePattern)
    {
        patternTempo.store(restoredTempo, std::memory_order_release);
        for (int pad = 0; pad < padCount; ++pad)
            for (int step = 0; step < patternStepCount; ++step)
                patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].store(
                    restoredPattern[static_cast<size_t>(pad)][static_cast<size_t>(step)], std::memory_order_release);
    }
    stateLog("setState: version=" + juce::String(version) + " padsOk=" + juce::String(padsOk) + " padsBad=" + juce::String(padsBad));
    return true;
}

void CrayLdrDrumLabV1AudioProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = juce::jmax(8000.0, sampleRate);
    for (auto& voice : voices)
        voice = {};
}

void CrayLdrDrumLabV1AudioProcessor::releaseResources()
{
    for (auto& voice : voices)
        voice = {};
}

bool CrayLdrDrumLabV1AudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void CrayLdrDrumLabV1AudioProcessor::triggerPad(int padIndex, float velocity) noexcept
{
    if (padIndex >= 0 && padIndex < padCount)
        pendingPadVelocities[static_cast<size_t>(padIndex)].store(juce::jlimit(0.0f, 1.0f, velocity), std::memory_order_release);
}

bool CrayLdrDrumLabV1AudioProcessor::isPadLoaded(int padIndex) const noexcept
{
    return padIndex >= 0 && padIndex < padCount
        && padSamplePointers[static_cast<size_t>(padIndex)].load(std::memory_order_acquire) != nullptr;
}

void CrayLdrDrumLabV1AudioProcessor::startVoice(int midiNote, float velocity) noexcept
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
    target->generation = kitGeneration.load(std::memory_order_acquire);
    target->sample = padSamplePointers[static_cast<size_t>(pad)].load(std::memory_order_acquire);}

float CrayLdrDrumLabV1AudioProcessor::renderVoice(Voice& voice) noexcept
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

int CrayLdrDrumLabV1AudioProcessor::importAudioFile(const juce::File& file, juce::String& errorMessage)
{
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

    const int preRoll = static_cast<int>(reader->sampleRate * 0.010);
    const int maxLength = static_cast<int>(reader->sampleRate * maxSampleSeconds);
    int loaded = 0;
    std::array<std::unique_ptr<SampleData>, padCount> importedSamples {};

    const size_t maxToLoad = static_cast<size_t>(padCount);

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

    if (loaded == 0)    {
        errorMessage = "Transients were detected, but no segments were long enough to load.";
        return 0;
    }

    // Publish the replacement kit only after every segment has been prepared successfully.
    {
        std::lock_guard<std::mutex> lock(importMutex);
        if (!publishKitLocked(importedSamples))
        {
            errorMessage = "Sample memory safety limit reached. Restart the plugin before importing another kit.";
            return 0;
        }
    }
    return loaded;
}


void CrayLdrDrumLabV1AudioProcessor::importAudioFileAsync(
    juce::File file, std::function<void(int, juce::String)> completion)
{
    // Share the same lazily-created worker with state restoration. Protect the pool
    // pointer because import and host state callbacks can arrive from different threads.
    auto completionPtr = std::make_shared<std::function<void(int, juce::String)>>(std::move(completion));
    try
    {
        std::lock_guard<std::mutex> lock(importMutex);
        if (importWorkers == nullptr)
            importWorkers = std::make_unique<juce::ThreadPool>(1);

        importWorkers->addJob([this, file = std::move(file), completionPtr]() mutable
        {
            juce::String error;
            const int count = importAudioFile(file, error);
            juce::MessageManager::callAsync(
                [completionPtr, count, error = std::move(error)]() mutable
                {
                    if (*completionPtr)
                        (*completionPtr)(count, error);
                });
        });
    }
    catch (const std::exception& e)
    {
        stateLog(juce::String("importAsync: could not queue import: ") + e.what());
        juce::MessageManager::callAsync([completionPtr]()
        {
            if (*completionPtr)
                (*completionPtr)(0, "Could not start background import.");
        });
    }
    catch (...)
    {
        stateLog("importAsync: could not queue import (unknown exception)");
        juce::MessageManager::callAsync([completionPtr]()
        {
            if (*completionPtr)
                (*completionPtr)(0, "Could not start background import.");
        });
    }
}

bool CrayLdrDrumLabV1AudioProcessor::exportKitToDirectory(
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

    juce::String manifest = "CRAY LDR - DRUM LAB V1 KIT\nWAV format: PCM 24-bit mono\n";
    manifest += "Pad\tMIDI note\tSample rate\tFrames\tFile\n";
    for (int pad = 0; pad < padCount; ++pad)
    {
        const auto* sample = padSamplePointers[static_cast<size_t>(pad)].load(std::memory_order_acquire);
        if (sample == nullptr)
            continue;
        const auto note = firstMidiNote + pad;
        const auto name = "Pad_" + juce::String(pad + 1).paddedLeft('0', 2)
            + "_MIDI_" + juce::String(note) + ".wav";
        manifest += juce::String(pad + 1) + "\t" + juce::String(note) + "\t"
            + juce::String(sample->sampleRate, 0) + "\t"
            + juce::String(sample->audio.getNumSamples()) + "\t" + name + "\n";
    }

    if (!directory.getChildFile("KIT_MANIFEST.txt").replaceWithText(manifest))
    {
        errorMessage = "WAV files were exported, but the kit manifest could not be written.";
        return false;
    }
    return true;
}


void CrayLdrDrumLabV1AudioProcessor::setPatternStep(int pad, int step, bool enabled) noexcept
{
    if (pad >= 0 && pad < padCount && step >= 0 && step < patternStepCount)
        patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].store(
            enabled ? 1u : 0u, std::memory_order_release);
}

bool CrayLdrDrumLabV1AudioProcessor::getPatternStep(int pad, int step) const noexcept
{
    if (pad < 0 || pad >= padCount || step < 0 || step >= patternStepCount)
        return false;
    return patternSteps[static_cast<size_t>(pad)][static_cast<size_t>(step)].load(std::memory_order_acquire) != 0;
}

void CrayLdrDrumLabV1AudioProcessor::setPatternTempo(float bpm) noexcept
{
    if (std::isfinite(bpm))
        patternTempo.store(juce::jlimit(40.0f, 300.0f, bpm), std::memory_order_release);
}

float CrayLdrDrumLabV1AudioProcessor::getPatternTempo() const noexcept
{
    return patternTempo.load(std::memory_order_acquire);
}

bool CrayLdrDrumLabV1AudioProcessor::exportMidiPattern(
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

void CrayLdrDrumLabV1AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const auto currentGeneration = kitGeneration.load(std::memory_order_acquire);
    for (auto& voice : voices)
        if (voice.active && voice.generation != currentGeneration)
            voice = {};
    audioSeenGeneration.store(currentGeneration, std::memory_order_release);

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

juce::AudioProcessorEditor* CrayLdrDrumLabV1AudioProcessor::createEditor()
{
    return new CrayLdrDrumLabV1AudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CrayLdrDrumLabV1AudioProcessor();
}