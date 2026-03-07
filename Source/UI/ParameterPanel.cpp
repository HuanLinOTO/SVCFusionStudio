#include "ParameterPanel.h"
#include "StyledComponents.h"
#include "../Utils/Localization.h"
#include <cmath>

ParameterPanel::ParameterPanel()
{
    // Note info
    addAndMakeVisible(noteInfoLabel);
    noteInfoLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    noteInfoLabel.setText(TR("param.no_selection"), juce::dontSendNotification);
    noteInfoLabel.setJustificationType(juce::Justification::centred);

    // Setup sliders
    setupSlider(pitchOffsetSlider, pitchOffsetLabel, TR("param.pitch_offset"), -24.0, 24.0, 0.0);
    setupSlider(noteVolumeSlider, noteVolumeLabel, "Note Volume (dB)", -24.0, 12.0, 0.0);

    // Volume knob setup
    addAndMakeVisible(volumeKnob);
    addAndMakeVisible(volumeValueLabel);
    volumeKnob.setRange(-12.0, 12.0, 0.1);  // Symmetric dB range, 0 in center
    volumeKnob.setValue(0.0);  // 0 dB = unity gain
    volumeKnob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    volumeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeKnob.setDoubleClickReturnValue(true, 0.0);  // Double-click resets to 0 dB
    volumeKnob.addListener(this);
    volumeKnob.setLookAndFeel(&KnobLookAndFeel::getInstance());
    volumeValueLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    volumeValueLabel.setJustificationType(juce::Justification::centred);
    volumeValueLabel.setText("0.0 dB", juce::dontSendNotification);

    setupSlider(formantShiftSlider, formantShiftLabel, TR("param.formant_shift"), -12.0, 12.0, 0.0);
    setupSlider(globalPitchSlider, globalPitchLabel, TR("param.global_pitch"), -24.0, 24.0, 0.0);

    // Section labels
    pitchSectionLabel.setText(TR("param.pitch"), juce::dontSendNotification);
    volumeSectionLabel.setText(TR("param.volume"), juce::dontSendNotification);
    formantSectionLabel.setText(TR("param.formant"), juce::dontSendNotification);
    globalSectionLabel.setText(TR("param.global"), juce::dontSendNotification);

    for (auto* label : { &pitchSectionLabel, &volumeSectionLabel,
                         &formantSectionLabel, &globalSectionLabel })
    {
        addAndMakeVisible(label);
        label->setColour(juce::Label::textColourId, APP_COLOR_PRIMARY);
        label->setFont(
            juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    }

    // Formant slider disabled (not implemented yet)
    formantShiftSlider.setEnabled(false);
    // Global pitch slider is now enabled!
    globalPitchSlider.setEnabled(true);

    // Pitch offset mode toggle (pre/post SVC)
    addAndMakeVisible(pitchOffsetPreSVCToggle);
    pitchOffsetPreSVCToggle.setButtonText(TR("param.pitch_offset_pre_svc"));
    pitchOffsetPreSVCToggle.setTooltip(TR("param.pitch_offset_pre_svc.tooltip"));
    pitchOffsetPreSVCToggle.setToggleState(true, juce::dontSendNotification);
    pitchOffsetPreSVCToggle.setColour(juce::ToggleButton::textColourId, APP_COLOR_TEXT_PRIMARY);
    pitchOffsetPreSVCToggle.setColour(juce::ToggleButton::tickColourId, APP_COLOR_PRIMARY);
    pitchOffsetPreSVCToggle.onClick = [this]() {
        if (project) {
            project->setPitchOffsetBeforeSVC(pitchOffsetPreSVCToggle.getToggleState());
            if (onPitchOffsetModeChanged)
                onPitchOffsetModeChanged(pitchOffsetPreSVCToggle.getToggleState());
        }
    };
}

ParameterPanel::~ParameterPanel()
{
    volumeKnob.setLookAndFeel(nullptr);
}

void ParameterPanel::setupSlider(juce::Slider& slider, juce::Label& label,
                                  const juce::String& name, double min, double max, double def)
{
    addAndMakeVisible(slider);
    addAndMakeVisible(label);

    slider.setRange(min, max, 0.01);
    slider.setValue(def);
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 55, 22);
    slider.addListener(this);

    // Slider track colors - darker background for better contrast
    slider.setColour(juce::Slider::backgroundColourId, APP_COLOR_SURFACE_ALT);
    slider.setColour(juce::Slider::trackColourId, APP_COLOR_PRIMARY.withAlpha(0.75f));
    slider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);

    // Text box colors - match dark theme with subtle border
    slider.setColour(juce::Slider::textBoxTextColourId, APP_COLOR_TEXT_PRIMARY);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, APP_COLOR_SURFACE);
    slider.setColour(juce::Slider::textBoxOutlineColourId, APP_COLOR_BORDER);
    slider.setColour(juce::Slider::textBoxHighlightColourId, APP_COLOR_PRIMARY.withAlpha(0.3f));

    label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
}

void ParameterPanel::paint(juce::Graphics& g)
{
    auto drawCard = [&g](const juce::Rectangle<int>& bounds)
    {
        if (bounds.isEmpty())
            return;
        const float radius = 10.0f;
        g.setColour(APP_COLOR_SURFACE_RAISED);
        g.fillRoundedRectangle(bounds.toFloat(), radius);

        juce::Path borderPath;
        borderPath.addRoundedRectangle(bounds.toFloat().reduced(0.5f), radius);
        juce::ColourGradient borderGradient(
            APP_COLOR_BORDER_HIGHLIGHT, bounds.getX(), bounds.getY(),
            APP_COLOR_BORDER.darker(0.3f), bounds.getRight(), bounds.getBottom(), false);
        g.setGradientFill(borderGradient);
        g.strokePath(borderPath, juce::PathStrokeType(1.1f));

        g.setColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.4f));
        g.drawRoundedRectangle(bounds.toFloat().reduced(1.2f), radius - 1.0f, 0.6f);
    };

    drawCard(noteCardBounds);
    drawCard(pitchCardBounds);
    drawCard(volumeCardBounds);
    drawCard(formantCardBounds);
    drawCard(globalCardBounds);
}

void ParameterPanel::resized()
{
    auto bounds = getLocalBounds().reduced(12);
    const int cardGap = 10;

    // Note info card
    noteCardBounds = bounds.removeFromTop(44);
    auto noteArea = noteCardBounds.reduced(10);
    noteInfoLabel.setBounds(noteArea);
    bounds.removeFromTop(cardGap);

    // Pitch card
    pitchCardBounds = bounds.removeFromTop(136);
    auto pitchArea = pitchCardBounds.reduced(10);
    pitchSectionLabel.setBounds(pitchArea.removeFromTop(18));
    pitchArea.removeFromTop(6);
    pitchOffsetLabel.setBounds(pitchArea.removeFromTop(18));
    pitchOffsetSlider.setBounds(pitchArea.removeFromTop(26));
    pitchArea.removeFromTop(4);
    noteVolumeLabel.setBounds(pitchArea.removeFromTop(18));
    noteVolumeSlider.setBounds(pitchArea.removeFromTop(26));
    bounds.removeFromTop(cardGap);

    // Volume card
    volumeCardBounds = bounds.removeFromTop(124);
    auto volumeArea = volumeCardBounds.reduced(10);
    volumeSectionLabel.setBounds(volumeArea.removeFromTop(18));
    volumeArea.removeFromTop(6);
    const int knobSize = 62;
    auto knobArea = volumeArea.removeFromTop(knobSize + 18);
    volumeKnob.setBounds(knobArea.getX() + (knobArea.getWidth() - knobSize) / 2,
                         knobArea.getY(), knobSize, knobSize);
    volumeValueLabel.setBounds(knobArea.getX(), knobArea.getBottom() - 16,
                               knobArea.getWidth(), 16);
    bounds.removeFromTop(cardGap);

    // Formant card
    formantCardBounds = bounds.removeFromTop(92);
    auto formantArea = formantCardBounds.reduced(10);
    formantSectionLabel.setBounds(formantArea.removeFromTop(18));
    formantArea.removeFromTop(6);
    formantShiftLabel.setBounds(formantArea.removeFromTop(18));
    formantShiftSlider.setBounds(formantArea.removeFromTop(26));
    bounds.removeFromTop(cardGap);

    // Global card
    globalCardBounds = bounds.removeFromTop(118);
    auto globalArea = globalCardBounds.reduced(10);
    globalSectionLabel.setBounds(globalArea.removeFromTop(18));
    globalArea.removeFromTop(6);
    globalPitchLabel.setBounds(globalArea.removeFromTop(18));
    globalPitchSlider.setBounds(globalArea.removeFromTop(26));
    globalArea.removeFromTop(4);
    pitchOffsetPreSVCToggle.setBounds(globalArea.removeFromTop(22));
}

void ParameterPanel::sliderValueChanged(juce::Slider* slider)
{
    if (isUpdating) return;

    if (slider == &pitchOffsetSlider && selectedNote)
    {
        selectedNote->setPitchOffset(static_cast<float>(slider->getValue()));
        selectedNote->markDirty();  // Mark as dirty for incremental synthesis

        if (onParameterChanged)
            onParameterChanged();
    }
    else if (slider == &noteVolumeSlider && selectedNote)
    {
        selectedNote->setVolumeDb(static_cast<float>(slider->getValue()));
        selectedNote->markDirty();

        if (onParameterChanged)
            onParameterChanged();
    }
    else if (slider == &globalPitchSlider && project)
    {
        project->setGlobalPitchOffset(static_cast<float>(slider->getValue()));

        // Mark all notes as dirty for full resynthesis
        for (auto& note : project->getNotes())
            note.markDirty();

        if (onGlobalPitchChanged)
            onGlobalPitchChanged();
    }
    else if (slider == &volumeKnob)
    {
        // Update display
        float dB = static_cast<float>(slider->getValue());
        volumeValueLabel.setText(juce::String(dB, 1) + " dB", juce::dontSendNotification);

        // Notify listener
        if (onVolumeChanged)
            onVolumeChanged(dB);
    }
}

void ParameterPanel::sliderDragStarted(juce::Slider* slider)
{
    if (isUpdating)
        return;

    if (slider == &pitchOffsetSlider && selectedNote)
    {
        pitchOffsetDragging = true;
        dragStartPitchOffset = selectedNote->getPitchOffset();
    }
    else if (slider == &noteVolumeSlider && selectedNote)
    {
        noteVolumeDragging = true;
        dragStartNoteVolumeDb = selectedNote->getVolumeDb();
    }
}

void ParameterPanel::sliderDragEnded(juce::Slider* slider)
{
    if (slider == &pitchOffsetSlider && selectedNote)
    {
        if (pitchOffsetDragging && undoManager)
        {
            const float endPitchOffset = selectedNote->getPitchOffset();
            if (std::abs(endPitchOffset - dragStartPitchOffset) > 1.0e-6f)
            {
                auto action = std::make_unique<PitchOffsetAction>(
                    selectedNote, dragStartPitchOffset, endPitchOffset,
                    [this](Note* note) {
                        if (note)
                            note->markDirty();
                    });
                undoManager->addAction(std::move(action));
            }
        }
        pitchOffsetDragging = false;

        // Trigger incremental synthesis when slider drag ends
        if (onParameterEditFinished)
            onParameterEditFinished();
    }
    else if (slider == &noteVolumeSlider && selectedNote)
    {
        if (noteVolumeDragging && undoManager)
        {
            const float endVolumeDb = selectedNote->getVolumeDb();
            if (std::abs(endVolumeDb - dragStartNoteVolumeDb) > 1.0e-6f)
            {
                auto action = std::make_unique<NoteVolumeAction>(
                    selectedNote, dragStartNoteVolumeDb, endVolumeDb,
                    [this](Note* note) {
                        if (note)
                            note->markDirty();
                    });
                undoManager->addAction(std::move(action));
            }
        }
        noteVolumeDragging = false;

        if (onParameterEditFinished)
            onParameterEditFinished();
    }
    else if (slider == &globalPitchSlider && project)
    {
        // Global pitch changed, need full SVC re-conversion or resynthesis
        if (onGlobalPitchEditFinished)
            onGlobalPitchEditFinished();
        else if (onParameterEditFinished)
            onParameterEditFinished();
    }
}

void ParameterPanel::buttonClicked(juce::Button* button)
{
    juce::ignoreUnused(button);
}

void ParameterPanel::setProject(Project* proj)
{
    project = proj;
    updateGlobalSliders();
}

void ParameterPanel::setSelectedNote(Note* note)
{
    selectedNote = note;
    updateFromNote();
}

void ParameterPanel::updateFromNote()
{
    isUpdating = true;

    if (selectedNote)
    {
        float midi = selectedNote->getAdjustedMidiNote();
        int octave = static_cast<int>(midi / 12) - 1;
        int noteIndex = static_cast<int>(midi) % 12;
        static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F",
                                           "F#", "G", "G#", "A", "A#", "B" };

        juce::String noteInfo = juce::String(noteNames[noteIndex]) +
                                juce::String(octave) +
                                " (" + juce::String(midi, 1) + ")";
        noteInfoLabel.setText(noteInfo, juce::dontSendNotification);

        pitchOffsetSlider.setValue(selectedNote->getPitchOffset());
        pitchOffsetSlider.setEnabled(true);
        noteVolumeSlider.setValue(selectedNote->getVolumeDb());
        noteVolumeSlider.setEnabled(true);
    }
    else
    {
        noteInfoLabel.setText(TR("param.no_selection"), juce::dontSendNotification);
        pitchOffsetSlider.setValue(0.0);
        pitchOffsetSlider.setEnabled(false);
        noteVolumeSlider.setValue(0.0);
        noteVolumeSlider.setEnabled(false);
    }

    isUpdating = false;
}

void ParameterPanel::updateGlobalSliders()
{
    isUpdating = true;

    if (project)
    {
        globalPitchSlider.setValue(project->getGlobalPitchOffset());
        globalPitchSlider.setEnabled(true);
        pitchOffsetPreSVCToggle.setToggleState(project->isPitchOffsetBeforeSVC(),
                                               juce::dontSendNotification);
        pitchOffsetPreSVCToggle.setEnabled(true);
    }
    else
    {
        globalPitchSlider.setValue(0.0);
        globalPitchSlider.setEnabled(false);
        pitchOffsetPreSVCToggle.setToggleState(true, juce::dontSendNotification);
        pitchOffsetPreSVCToggle.setEnabled(false);
    }

    isUpdating = false;
}
