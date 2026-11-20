#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace
{
constexpr float twoPi = 6.2831853071795864769f;
constexpr float silenceThreshold = 0.00001f;
}

CrayLdrDrumkitCloneAudioProcessor::CrayLdrDrumkitCloneAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
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
        voice.active = false;
}

bool CrayLdrDrumkitCloneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
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
}

float CrayLdrDrumkitCloneAudioProcessor::renderVoice(Voice& voice) noexcept
{
    if (!voice.active)
        return 0.0f;

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
    if (t >= maxDuration || std::abs(sample) < silenceThreshold && t > 0.08f)
        voice.active = false;

    return sample * velocity * 0.22f;
}

void CrayLdrDrumkitCloneAudioProcessor::triggerPad(int padIndex, float velocity) noexcept
{
    if (padIndex >= 0 && padIndex < padCount)
        pendingPadVelocities[static_cast<size_t>(padIndex)].store(juce::jlimit(0.0f, 1.0f, velocity), std::memory_order_release);
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

    auto midiIt = midiMessages.cbegin();

    for (int sampleIndex = 0; sampleIndex < buffer.getNumSamples(); ++sampleIndex)
    {
        while (midiIt != midiMessages.cend() && midiIt->samplePosition == sampleIndex)
        {
            const auto message = midiIt->getMessage();
            if (message.isNoteOn())
                startVoice(message.getNoteNumber(), message.getFloatVelocity());
            ++midiIt;
        }

        float mixed = 0.0f;
        for (auto& voice : voices)
            mixed += renderVoice(voice);

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
