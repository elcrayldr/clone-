#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>
#include <memory>

class CrayLdrDrumkitCloneAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit CrayLdrDrumkitCloneAudioProcessorEditor(CrayLdrDrumkitCloneAudioProcessor&);
    ~CrayLdrDrumkitCloneAudioProcessorEditor() override = default;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    CrayLdrDrumkitCloneAudioProcessor& audioProcessor;
    juce::Label title;
    juce::Label subtitle;
    juce::Label selection;
    juce::Label status;
    juce::TextButton importButton { "IMPORT AUDIO" };
    juce::TextButton exportButton { "EXPORT KIT" };
    juce::TextButton exportMidiButton { "EXPORT MIDI" };
    juce::Label patternLabel;
    juce::Label tempoLabel;
    juce::Slider tempoSlider;
    std::array<std::unique_ptr<juce::ToggleButton>, 16> stepButtons;
    std::unique_ptr<juce::FileChooser> midiChooser;
    std::unique_ptr<juce::FileChooser> exportChooser;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::array<std::unique_ptr<juce::TextButton>, 64> padButtons;
    int selectedPad = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CrayLdrDrumkitCloneAudioProcessorEditor)
};
