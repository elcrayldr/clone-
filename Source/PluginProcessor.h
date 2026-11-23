#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>

class CrayLdrDrumkitCloneAudioProcessor final : public juce::AudioProcessor
{
public:
    CrayLdrDrumkitCloneAudioProcessor();
    ~CrayLdrDrumkitCloneAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.5; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    static constexpr int firstMidiNote = 36;
    static constexpr int padCount = 64;
    static constexpr int lastMidiNote = firstMidiNote + padCount - 1;
    static constexpr int patternStepCount = 16;

    void triggerPad(int padIndex, float velocity = 0.9f) noexcept;
    bool isPadLoaded(int padIndex) const noexcept;
    int importAudioFile(const juce::File& file, juce::String& errorMessage);
    void importAudioFileAsync(juce::File file, std::function<void(int, juce::String)> completion);
    bool exportKitToDirectory(const juce::File& directory, juce::String& errorMessage);
    void setPatternStep(int pad, int step, bool enabled) noexcept;
    bool getPatternStep(int pad, int step) const noexcept;
    void setPatternTempo(float bpm) noexcept;
    float getPatternTempo() const noexcept;
    bool exportMidiPattern(const juce::File& file, juce::String& errorMessage) const;

private:
    struct SampleData
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
    };

    struct Voice
    {
        bool active = false;
        int note = 0;
        double phase = 0.0;
        int age = 0;
        float velocity = 0.0f;
        std::uint32_t noiseState = 0x1234567u;
        const SampleData* sample = nullptr;
    };

    static constexpr int maxVoices = 32;
    std::array<Voice, maxVoices> voices {};
    std::array<std::atomic<const SampleData*>, padCount> padSamplePointers {};
    std::vector<std::unique_ptr<SampleData>> samplePool;
    std::mutex importMutex;
    std::unique_ptr<juce::ThreadPool> importWorkers;
    static constexpr size_t maxRetainedSamples = 256;
    std::array<std::atomic<float>, padCount> pendingPadVelocities {};
    std::array<std::array<std::atomic<std::uint8_t>, patternStepCount>, padCount> patternSteps {};
    std::atomic<float> patternTempo { 120.0f };
    double currentSampleRate = 44100.0;

    void startVoice(int midiNote, float velocity) noexcept;
    float renderVoice(Voice& voice) noexcept;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CrayLdrDrumkitCloneAudioProcessor)
};
