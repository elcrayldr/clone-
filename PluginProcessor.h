#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class DrumLabProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int padCount = 64;
    static constexpr int firstMidiNote = 36;
    DrumLabProcessor();
    ~DrumLabProcessor() override = default;

    void prepareToPlay(double, int) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "CRAY LDR DRUM LAB V1"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    void triggerPad(int pad, float velocity = 0.9f) noexcept;
    bool isPadLoaded(int pad) const noexcept;
    int importAndSlice(const juce::File&, juce::String& error);
    bool exportKit(const juce::File&, juce::String& error);

private:
    struct Sample { juce::AudioBuffer<float> audio; double sampleRate = 44100.0; };
    struct Voice { bool active = false; int age = 0; float velocity = 0.0f; const Sample* sample = nullptr; };
    static constexpr int maxVoices = 32;
    std::array<Voice, maxVoices> voices {};
    std::array<std::atomic<const Sample*>, padCount> padSamples {};
    std::array<std::atomic<float>, padCount> pendingVelocity {};
    std::vector<std::unique_ptr<Sample>> retainedSamples;
    mutable std::mutex sampleMutex;
    double currentSampleRate = 44100.0;
    void startVoice(int midiNote, float velocity) noexcept;
    float renderVoice(Voice&) noexcept;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumLabProcessor)
};
