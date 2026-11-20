#include "PluginEditor.h"

CrayLdrDrumkitCloneAudioProcessorEditor::CrayLdrDrumkitCloneAudioProcessorEditor(CrayLdrDrumkitCloneAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(900, 680);

    title.setText("CRAY LDR  /  DRUMKIT CLONE", juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(25.0f, juce::Font::bold)));
    title.setColour(juce::Label::textColourId, juce::Colour(0xffffc928));
    addAndMakeVisible(title);

    importButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffb88b12));
    importButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    importButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Import a song or instrumental", juce::File{}, "*.wav;*.aif;*.aiff;*.flac;*.ogg");
        const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        juce::Component::SafePointer<CrayLdrDrumkitCloneAudioProcessorEditor> safeThis(this);
        fileChooser->launchAsync(flags, [safeThis](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;
            const auto file = chooser.getResult();
            if (file == juce::File{})
                return;

            safeThis->status.setText("ANALYSING IN BACKGROUND: " + file.getFileName(), juce::dontSendNotification);
            safeThis->importButton.setEnabled(false);
            safeThis->audioProcessor.importAudioFileAsync(file,
                [safeThis, fileName = file.getFileName()](int count, juce::String error)
                {
                    if (safeThis == nullptr)
                        return;
                    safeThis->importButton.setEnabled(true);
                    if (count > 0)
                    {
                        safeThis->status.setText("IMPORTED " + juce::String(count) + " SLICES  |  " + fileName, juce::dontSendNotification);
                        for (int pad = 0; pad < static_cast<int>(safeThis->padButtons.size()); ++pad)
                            safeThis->padButtons[static_cast<size_t>(pad)]->setColour(juce::TextButton::buttonColourId,
                                safeThis->audioProcessor.isPadLoaded(pad) ? juce::Colour(0xff72550d) : juce::Colour(0xff292929));
                    }
                    else
                        safeThis->status.setText("IMPORT FAILED: " + error, juce::dontSendNotification);
                });
        });
    };
    addAndMakeVisible(importButton);

    exportButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    exportButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    exportButton.onClick = [this]
    {
        exportChooser = std::make_unique<juce::FileChooser>(
            "Export drum kit", juce::File{}, "");
        const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
        juce::Component::SafePointer<CrayLdrDrumkitCloneAudioProcessorEditor> safeThis(this);
        exportChooser->launchAsync(flags, [safeThis](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;
            const auto folder = chooser.getResult();
            if (folder == juce::File{})
                return;
            juce::String error;
            if (safeThis->audioProcessor.exportKitToDirectory(folder, error))
                safeThis->status.setText("KIT EXPORTED: " + folder.getFullPathName(), juce::dontSendNotification);
            else
                safeThis->status.setText("EXPORT FAILED: " + error, juce::dontSendNotification);
        });
    };
    addAndMakeVisible(exportButton);

    exportMidiButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff454545));
    exportMidiButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    exportMidiButton.onClick = [this]
    {
        midiChooser = std::make_unique<juce::FileChooser>(
            "Export 16-step MIDI pattern",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile("CRAY_LDR_Drumkit_Pattern.mid"), "*.mid");
        const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;
        juce::Component::SafePointer<CrayLdrDrumkitCloneAudioProcessorEditor> safeThis(this);
        midiChooser->launchAsync(flags, [safeThis](const juce::FileChooser& chooser)
        {
            if (safeThis == nullptr)
                return;
            auto file = chooser.getResult();
            if (file == juce::File{})
                return;
            if (!file.hasFileExtension(".mid"))
                file = file.withFileExtension("mid");
            juce::String error;
            if (safeThis->audioProcessor.exportMidiPattern(file, error))
                safeThis->status.setText("MIDI EXPORTED: " + file.getFileName(), juce::dontSendNotification);
            else
                safeThis->status.setText("MIDI EXPORT FAILED: " + error, juce::dontSendNotification);
        });
    };
    addAndMakeVisible(exportMidiButton);

    subtitle.setText("64-PAD MIDI ENGINE  |  NOTES 36-99", juce::dontSendNotification);
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

    patternLabel.setText("16-STEP PATTERN  /  EDIT SELECTED PAD", juce::dontSendNotification);
    patternLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    patternLabel.setColour(juce::Label::textColourId, juce::Colour(0xffbdbdbd));
    addAndMakeVisible(patternLabel);

    tempoLabel.setText("BPM", juce::dontSendNotification);
    tempoLabel.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
    tempoLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(tempoLabel);

    tempoSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tempoSlider.setRange(40.0, 300.0, 1.0);
    tempoSlider.setValue(audioProcessor.getPatternTempo(), juce::dontSendNotification);
    tempoSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    tempoSlider.onValueChange = [this]
    {
        audioProcessor.setPatternTempo(static_cast<float>(tempoSlider.getValue()));
    };
    addAndMakeVisible(tempoSlider);

    for (int step = 0; step < static_cast<int>(stepButtons.size()); ++step)
    {
        stepButtons[static_cast<size_t>(step)] = std::make_unique<juce::ToggleButton>(juce::String(step + 1));
        auto& button = *stepButtons[static_cast<size_t>(step)];
        button.setClickingTogglesState(true);
        button.setColour(juce::ToggleButton::tickColourId, juce::Colour(0xffffc928));
        button.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
        button.setToggleState(audioProcessor.getPatternStep(selectedPad, step), juce::dontSendNotification);
        button.onClick = [this, step]
        {
            audioProcessor.setPatternStep(selectedPad, step,
                stepButtons[static_cast<size_t>(step)]->getToggleState());
        };
        addAndMakeVisible(button);
    }

    for (int i = 0; i < static_cast<int>(padButtons.size()); ++i)
    {
        padButtons[static_cast<size_t>(i)] = std::make_unique<juce::TextButton>(
            juce::String(i + 1).paddedLeft('0', 2));
        auto& button = *padButtons[static_cast<size_t>(i)];
        button.setClickingTogglesState(false);
        button.setColour(juce::TextButton::buttonColourId,
            audioProcessor.isPadLoaded(i) ? juce::Colour(0xff72550d) : juce::Colour(0xff292929));
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb88b12));
        button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        button.onClick = [this, i]
        {
            audioProcessor.triggerPad(i, 0.9f);
            selectedPad = i;
            selection.setText("SELECTED: PAD " + juce::String(i + 1).paddedLeft('0', 2)
                + "  /  MIDI NOTE " + juce::String(CrayLdrDrumkitCloneAudioProcessor::firstMidiNote + i),
                juce::dontSendNotification);
            for (int step = 0; step < static_cast<int>(stepButtons.size()); ++step)
                stepButtons[static_cast<size_t>(step)]->setToggleState(
                    audioProcessor.getPatternStep(selectedPad, step), juce::dontSendNotification);
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
    title.setBounds(header.removeFromLeft(390));
    exportMidiButton.setBounds(header.removeFromRight(112));
    header.removeFromRight(7);
    exportButton.setBounds(header.removeFromRight(112));
    header.removeFromRight(7);
    importButton.setBounds(header.removeFromRight(132));

    subtitle.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);
    selection.setBounds(area.removeFromTop(24));
    status.setBounds(area.removeFromTop(20));
    area.removeFromTop(8);

    auto patternArea = area.removeFromBottom(68);
    area.removeFromBottom(8);
    auto patternHeader = patternArea.removeFromTop(22);
    patternLabel.setBounds(patternHeader.removeFromLeft(300));
    tempoLabel.setBounds(patternHeader.removeFromRight(36));
    tempoSlider.setBounds(patternHeader.removeFromRight(150));

    constexpr int stepGap = 4;
    const int stepWidth = (patternArea.getWidth() - stepGap * 15) / 16;
    for (int step = 0; step < static_cast<int>(stepButtons.size()); ++step)
        stepButtons[static_cast<size_t>(step)]->setBounds(
            patternArea.getX() + step * (stepWidth + stepGap),
            patternArea.getY() + 2, stepWidth, patternArea.getHeight() - 2);

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
