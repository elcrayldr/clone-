#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>

class CrayLdrDrumLabV1AudioProcessor final : public juce::AudioProcessor
{
public:
    CrayLdrDrumLabV1AudioProcessor();
    ~CrayLdrDrumLabV1AudioProcessor() override;

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
        std::uint32_t generation = 0;
    };

    static constexpr int maxVoices = 32;
    std::array<Voice, maxVoices> voices {};
    std::array<std::atomic<const SampleData*>, padCount> padSamplePointers {};
    using Kit = std::array<std::unique_ptr<SampleData>, padCount>;
    struct RetiredSample
    {
        std::unique_ptr<SampleData> data;
        std::uint32_t generation = 0;
    };
    // samplePool owns the samples the pads point at right now. Replaced kits move to
    // retiredSamples and are freed only after the audio thread has provably dropped them.
    std::vector<std::unique_ptr<SampleData>> samplePool;
    std::vector<RetiredSample> retiredSamples;
    std::atomic<std::uint32_t> kitGeneration { 1 };
    std::atomic<std::uint32_t> audioSeenGeneration { 0 };
    std::mutex importMutex;
    std::unique_ptr<juce::ThreadPool> importWorkers;
    static constexpr size_t maxRetainedSamples = 1024; // limit for current + not-yet-freed retired samples
    std::array<std::atomic<float>, padCount> pendingPadVelocities {};
    std::array<std::array<std::atomic<std::uint8_t>, patternStepCount>, padCount> patternSteps {};
    std::atomic<float> patternTempo { 120.0f };
    double currentSampleRate = 44100.0;

    bool restoreStateInternal(const void* data, int sizeInBytes);
    bool publishKitLocked(Kit& kit);
    void sweepRetiredLocked();
    void startVoice(int midiNote, float velocity) noexcept;
    float renderVoice(Voice& voice) noexcept;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CrayLdrDrumLabV1AudioProcessor)
};