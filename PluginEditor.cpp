#include "PluginEditor.h"

DrumLabEditor::DrumLabEditor(DrumLabProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(900, 680);

    title.setText("CRAY LDR  /  DRUM LAB V1", juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
    title.setColour(juce::Label::textColourId, juce::Colour(0xffffc928));
    addAndMakeVisible(title);

    subtitle.setText("64-PAD SAMPLER  |  MIDI NOTES 36-99  |  TRANSIENT SLICER",
                     juce::dontSendNotification);
    subtitle.setFont(juce::Font(juce::FontOptions(12.0f)));
    subtitle.setColour(juce::Label::textColourId, juce::Colour(0xffb7b7b7));
    addAndMakeVisible(subtitle);

    status.setText("READY  |  Import audio to detect and slice drum transients.",
                   juce::dontSendNotification);
    status.setFont(juce::Font(juce::FontOptions(11.0f)));
    status.setColour(juce::Label::textColourId, juce::Colour(0xffc8c8c8));
    addAndMakeVisible(status);

    importButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffb88b12));
    importButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    importButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Import audio and slice transients", juce::File{},
            "*.wav;*.aif;*.aiff;*.flac;*.ogg");
        juce::Component::SafePointer<DrumLabEditor> safeThis(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
            [safeThis](const juce::FileChooser& fc)
            {
                if (safeThis == nullptr) return;
                const auto file = fc.getResult();
                if (file == juce::File{}) return;

                safeThis->importButton.setEnabled(false);
                safeThis->status.setText("ANALYSING: " + file.getFileName(),
                                         juce::dontSendNotification);
                juce::String error;
                const int count = safeThis->processor.importAndSlice(file, error);
                safeThis->importButton.setEnabled(true);

                if (count > 0)
                {
                    safeThis->status.setText("READY  |  " + juce::String(count)
                        + " slices loaded into pads 01-" + juce::String(count).paddedLeft('0', 2),
                        juce::dontSendNotification);
                    for (int i = 0; i < DrumLabProcessor::padCount; ++i)
                        safeThis->pads[static_cast<size_t>(i)]->setColour(
                            juce::TextButton::buttonColourId,
                            safeThis->processor.isPadLoaded(i) ? juce::Colour(0xff72550d)
                                                               : juce::Colour(0xff292929));
                }
                else
                    safeThis->status.setText("IMPORT FAILED  |  " + error,
                                             juce::dontSendNotification);
            });
    };
    addAndMakeVisible(importButton);

    exportButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff353535));
    exportButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    exportButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser>("Export drum kit", juce::File{});
        juce::Component::SafePointer<DrumLabEditor> safeThis(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories,
            [safeThis](const juce::FileChooser& fc)
            {
                if (safeThis == nullptr) return;
                const auto folder = fc.getResult();
                if (folder == juce::File{}) return;
                juce::String error;
                if (safeThis->processor.exportKit(folder, error))
                    safeThis->status.setText("KIT EXPORTED  |  " + folder.getFullPathName(),
                                             juce::dontSendNotification);
                else
                    safeThis->status.setText("EXPORT FAILED  |  " + error,
                                             juce::dontSendNotification);
            });
    };
    addAndMakeVisible(exportButton);

    for (int i = 0; i < DrumLabProcessor::padCount; ++i)
    {
        pads[static_cast<size_t>(i)] = std::make_unique<juce::TextButton>(
            juce::String(i + 1).paddedLeft('0', 2));
        auto& button = *pads[static_cast<size_t>(i)];
        button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff292929));
        button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb88b12));
        button.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        button.onClick = [this, i] { processor.triggerPad(i, 0.9f); };
        addAndMakeVisible(button);
    }
}

void DrumLabEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff151515));
    g.setColour(juce::Colour(0xff303030));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(8.0f), 8.0f, 1.0f);
    g.setColour(juce::Colour(0xffffc928));
    g.fillRect(20, 76, getWidth() - 40, 2);
}

void DrumLabEditor::resized()
{
    auto area = getLocalBounds().reduced(20);
    auto header = area.removeFromTop(42);
    title.setBounds(header.removeFromLeft(430));
    exportButton.setBounds(header.removeFromRight(120));
    header.removeFromRight(8);
    importButton.setBounds(header.removeFromRight(170));

    subtitle.setBounds(area.removeFromTop(26));
    status.setBounds(area.removeFromTop(28));
    area.removeFromTop(10);

    constexpr int columns = 8;
    constexpr int gap = 7;
    const int cellWidth = (area.getWidth() - gap * (columns - 1)) / columns;
    const int cellHeight = (area.getHeight() - gap * (columns - 1)) / columns;
    for (int i = 0; i < DrumLabProcessor::padCount; ++i)
    {
        const int row = i / columns;
        const int column = i % columns;
        pads[static_cast<size_t>(i)]->setBounds(
            area.getX() + column * (cellWidth + gap),
            area.getY() + row * (cellHeight + gap),
            cellWidth, cellHeight);
    }
}
