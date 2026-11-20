#include "PluginEditor.h"

CrayLdrDrumkitCloneAudioProcessorEditor::CrayLdrDrumkitCloneAudioProcessorEditor(CrayLdrDrumkitCloneAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(820, 590);
    title.setText("CRAY LDR  /  DRUMKIT CLONE", juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(25.0f, juce::Font::bold)));
    title.setColour(juce::Label::textColourId, juce::Colour(0xffffc928));
    addAndMakeVisible(title);

    importButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffb88b12));
    importButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    importButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Import a song or instrumental", juce::File{}, "*.wav;*.aif;*.aiff;*.mp3");
        const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        fileChooser->launchAsync(flags, [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File{})
                return;

            status.setText("ANALYSING: " + file.getFileName(), juce::dontSendNotification);
            juce::String error;
            const int count = audioProcessor.importAudioFile(file, error);
            if (count > 0)
                status.setText("IMPORTED " + juce::String(count) + " TRANSIENT SEGMENTS  |  " + file.getFileName(), juce::dontSendNotification);
            else
                status.setText("IMPORT FAILED: " + error, juce::dontSendNotification);
        });
    };
    addAndMakeVisible(importButton);

    subtitle.setText("64-PAD MIDI ENGINE  |  DEVELOPMENT BUILD  |  NOTES 36-99", juce::dontSendNotification);
    subtitle.setFont(juce::Font(juce::FontOptions(12.0f)));
    subtitle.setColour(juce::Label::textColourId, juce::Colour(0xffa7a7a7));
    addAndMakeVisible(subtitle);

    selection.setText("SELECTED: PAD 01  /  MIDI NOTE 36", juce::dontSendNotification);
    selection.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    selection.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(selection);

    status.setText("IMPORT: no audio source loaded", juce::dontSendNotification);
    status.setFont(juce::Font(juce::FontOptions(11.0f)));
    status.setColour(juce::Label::textColourId, juce::Colour(0xffb7b7b7));
    addAndMakeVisible(status);

    for (int i = 0; i < static_cast<int>(padButtons.size()); ++i)
    {
        padButtons[static_cast<size_t>(i)] = std::make_unique<juce::TextButton>(juce::String(i + 1).paddedLeft('0', 2));
        auto& button = *padButtons[static_cast<size_t>(i)];
        button.setClickingTogglesState(false);
        button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff292929));
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb88b12));
        button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        button.onClick = [this, i]
        {
            audioProcessor.triggerPad(i, 0.9f);
            selectedPad = i;
            selection.setText("SELECTED: PAD " + juce::String(i + 1).paddedLeft('0', 2)
                + "  /  MIDI NOTE " + juce::String(CrayLdrDrumkitCloneAudioProcessor::firstMidiNote + i),
                juce::dontSendNotification);
        };
        addAndMakeVisible(button);
    }
}

void CrayLdrDrumkitCloneAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff151515));
    g.setColour(juce::Colour(0xff303030));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(8.0f), 8.0f, 1.0f);
    g.setColour(juce::Colour(0xffffc928));
    g.fillRect(16, 78, getWidth() - 32, 2);
}

void CrayLdrDrumkitCloneAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(20);
    auto header = area.removeFromTop(38);
    title.setBounds(header.removeFromLeft(490));
    importButton.setBounds(header.removeFromRight(150));
    subtitle.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);
    selection.setBounds(area.removeFromTop(24));
    status.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);

    constexpr int columns = 8;
    constexpr int rows = 8;
    const int gap = 7;
    const int cellWidth = (area.getWidth() - gap * (columns - 1)) / columns;
    const int cellHeight = (area.getHeight() - gap * (rows - 1)) / rows;

    for (int i = 0; i < static_cast<int>(padButtons.size()); ++i)
    {
        const int row = i / columns;
        const int column = i % columns;
        padButtons[static_cast<size_t>(i)]->setBounds(
            area.getX() + column * (cellWidth + gap),
            area.getY() + row * (cellHeight + gap),
            cellWidth, cellHeight);
    }
}
