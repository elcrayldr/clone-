#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include <array>
#include <memory>

class DrumLabEditor final : public juce::AudioProcessorEditor
{
public:
    explicit DrumLabEditor(DrumLabProcessor&);
    ~DrumLabEditor() override = default;
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    DrumLabProcessor& processor;
    juce::Label title, subtitle, status;
    juce::TextButton importButton { "IMPORT / SLICE AUDIO" };
    juce::TextButton exportButton { "EXPORT KIT" };
    std::array<std::unique_ptr<juce::TextButton>, DrumLabProcessor::padCount> pads;
    std::unique_ptr<juce::FileChooser> chooser;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DrumLabEditor)
};
