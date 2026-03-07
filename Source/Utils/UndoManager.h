#pragma once

#include "../JuceHeader.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include <vector>
#include <memory>
#include <functional>
#include <limits>

/**
 * Base class for undoable actions.
 */
class UndoableAction
{
public:
    virtual ~UndoableAction() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    virtual juce::String getName() const = 0;
};

/**
 * Action for changing a note's pitch offset.
 */
class PitchOffsetAction : public UndoableAction
{
public:
    PitchOffsetAction(Note* note, float oldOffset, float newOffset,
                      std::function<void(Note*)> onNoteChanged = nullptr)
        : note(note), oldOffset(oldOffset), newOffset(newOffset),
          onNoteChanged(onNoteChanged) {}
    
    void undo() override
    {
        if (!note) return;
        note->setPitchOffset(oldOffset);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }
    void redo() override
    {
        if (!note) return;
        note->setPitchOffset(newOffset);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }
    juce::String getName() const override { return "Change Pitch Offset"; }
    
private:
    Note* note;
    float oldOffset;
    float newOffset;
    std::function<void(Note*)> onNoteChanged;
};

/**
 * Action for changing a note's volume (dB).
 */
class NoteVolumeAction : public UndoableAction
{
public:
    NoteVolumeAction(Note* note, float oldVolumeDb, float newVolumeDb,
                     std::function<void(Note*)> onNoteChanged = nullptr)
        : note(note), oldVolumeDb(oldVolumeDb), newVolumeDb(newVolumeDb),
          onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (!note) return;
        note->setVolumeDb(oldVolumeDb);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }

    void redo() override
    {
        if (!note) return;
        note->setVolumeDb(newVolumeDb);
        note->markDirty();
        if (onNoteChanged)
            onNoteChanged(note);
    }

    juce::String getName() const override { return "Change Note Volume"; }

private:
    Note* note;
    float oldVolumeDb;
    float newVolumeDb;
    std::function<void(Note*)> onNoteChanged;
};

/**
 * Action for changing multiple F0 values (hand-drawing).
 */
struct F0FrameEdit
{
    int idx = -1;
    float oldF0 = 0.0f;
    float newF0 = 0.0f;
    float oldDelta = 0.0f;
    float newDelta = 0.0f;
    bool oldVoiced = false;
    bool newVoiced = false;
};

class F0EditAction : public UndoableAction
{
public:
    F0EditAction(std::vector<float>* f0Array,
                 std::vector<float>* deltaPitchArray,
                 std::vector<bool>* voicedMask,
                 std::vector<F0FrameEdit> edits,
                 std::function<void(int, int)> onF0Changed = nullptr)
        : f0Array(f0Array), deltaPitchArray(deltaPitchArray), voicedMask(voicedMask), edits(std::move(edits)), onF0Changed(onF0Changed) {}

    void undo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.oldF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.oldDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.oldVoiced;
        }
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    void redo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.newF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.newDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.newVoiced;
        }
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    juce::String getName() const override { return "Edit Pitch Curve"; }

private:
    std::vector<float>* f0Array;
    std::vector<float>* deltaPitchArray;
    std::vector<bool>* voicedMask;
    std::vector<F0FrameEdit> edits;
    std::function<void(int, int)> onF0Changed;  // Callback with (minFrame, maxFrame) to trigger resynthesis
};

/**
 * Action for dragging a note to change pitch (MIDI note + F0 values).
 */
class NotePitchDragAction : public UndoableAction
{
public:
    NotePitchDragAction(Note* note, std::vector<float>* f0Array,
                        float oldMidi, float newMidi,
                        std::vector<F0FrameEdit> f0Edits,
                        std::function<void(Note*)> onNoteChanged = nullptr)
        : note(note), f0Array(f0Array), oldMidi(oldMidi), newMidi(newMidi),
          f0Edits(std::move(f0Edits)), onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (note) {
            note->setMidiNote(oldMidi);
            note->markDirty();
        }
        if (f0Array) {
            for (const auto& e : f0Edits) {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.oldF0;
            }
        }
        // Notify that note changed, so base pitch can be recalculated
        if (onNoteChanged && note) {
            onNoteChanged(note);
        }
    }

    void redo() override
    {
        if (note) {
            note->setMidiNote(newMidi);
            note->markDirty();
        }
        if (f0Array) {
            for (const auto& e : f0Edits) {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.newF0;
            }
        }
        // Notify that note changed, so base pitch can be recalculated
        if (onNoteChanged && note) {
            onNoteChanged(note);
        }
    }

    juce::String getName() const override { return "Drag Note Pitch"; }

private:
    Note* note;
    std::vector<float>* f0Array;
    float oldMidi;
    float newMidi;
    std::vector<F0FrameEdit> f0Edits;
    std::function<void(Note*)> onNoteChanged;  // Callback when note MIDI changes
};

/**
 * Action for dragging multiple notes to change pitch.
 */
class MultiNotePitchDragAction : public UndoableAction
{
public:
    MultiNotePitchDragAction(std::vector<Note*> notes, std::vector<float>* f0Array,
                             std::vector<float> oldMidis, float pitchDelta,
                             std::vector<F0FrameEdit> f0Edits,
                             std::function<void(const std::vector<Note*>&)> onNotesChanged = nullptr)
        : notes(std::move(notes)), f0Array(f0Array), oldMidis(std::move(oldMidis)),
          pitchDelta(pitchDelta), f0Edits(std::move(f0Edits)), onNotesChanged(onNotesChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldMidis.size(); ++i) {
            if (notes[i]) {
                notes[i]->setMidiNote(oldMidis[i]);
                notes[i]->markDirty();
            }
        }
        if (f0Array) {
            for (const auto& e : f0Edits) {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.oldF0;
            }
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldMidis.size(); ++i) {
            if (notes[i]) {
                notes[i]->setMidiNote(oldMidis[i] + pitchDelta);
                notes[i]->markDirty();
            }
        }
        if (f0Array) {
            for (const auto& e : f0Edits) {
                if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size()))
                    (*f0Array)[e.idx] = e.newF0;
            }
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Drag Multiple Notes"; }

private:
    std::vector<Note*> notes;
    std::vector<float>* f0Array;
    std::vector<float> oldMidis;
    float pitchDelta;
    std::vector<F0FrameEdit> f0Edits;
    std::function<void(const std::vector<Note*>&)> onNotesChanged;
};

/**
 * Action for snapping a note to the nearest semitone (double-click).
 * Combines midiNote and pitchOffset into a rounded integer MIDI value.
 */
class NoteSnapToSemitoneAction : public UndoableAction
{
public:
    NoteSnapToSemitoneAction(Note* note,
                             float oldMidi, float oldOffset,
                             float newMidi,
                             std::function<void(Note*)> onNoteChanged = nullptr)
        : note(note), oldMidi(oldMidi), oldOffset(oldOffset),
          newMidi(newMidi), onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (note) {
            note->setMidiNote(oldMidi);
            note->setPitchOffset(oldOffset);
            note->markDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    void redo() override
    {
        if (note) {
            note->setMidiNote(newMidi);
            note->setPitchOffset(0.0f);
            note->markDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    juce::String getName() const override { return "Snap to Semitone"; }

private:
    Note* note;
    float oldMidi;
    float oldOffset;
    float newMidi;
    std::function<void(Note*)> onNoteChanged;
};

/**
 * Action for snapping multiple notes to the nearest semitone.
 */
class MultiNoteSnapToSemitoneAction : public UndoableAction
{
public:
    MultiNoteSnapToSemitoneAction(const std::vector<Note*>& notes,
                                  std::vector<float> oldMidis,
                                  std::vector<float> oldOffsets,
                                  std::vector<float> newMidis,
                                  std::function<void(const std::vector<Note*>&)> onNotesChanged = nullptr)
        : notes(notes),
          oldMidis(std::move(oldMidis)),
          oldOffsets(std::move(oldOffsets)),
          newMidis(std::move(newMidis)),
          onNotesChanged(onNotesChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size(); ++i) {
            auto* note = notes[i];
            if (!note)
                continue;
            note->setMidiNote(oldMidis[i]);
            note->setPitchOffset(oldOffsets[i]);
            note->markDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size(); ++i) {
            auto* note = notes[i];
            if (!note)
                continue;
            note->setMidiNote(newMidis[i]);
            note->setPitchOffset(0.0f);
            note->markDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Snap Notes to Semitone"; }

private:
    std::vector<Note*> notes;
    std::vector<float> oldMidis;
    std::vector<float> oldOffsets;
    std::vector<float> newMidis;
    std::function<void(const std::vector<Note*>&)> onNotesChanged;
};

/**
 * Action for splitting a note into two.
 */
class NoteSplitAction : public UndoableAction
{
public:
    NoteSplitAction(Project* proj, const Note& original, const Note& firstPart, const Note& secondPart,
                    std::function<void()> onChanged = nullptr)
        : project(proj), originalNote(original), firstNote(firstPart), secondNote(secondPart),
          onChanged(onChanged) {}

    void undo() override
    {
        if (!project) return;
        // Remove the second note and restore original
        project->removeNoteByStartFrame(secondNote.getStartFrame());
        // Find and restore the first note to original state
        for (auto& note : project->getNotes()) {
            if (note.getStartFrame() == firstNote.getStartFrame()) {
                note = originalNote;
                break;
            }
        }
        if (onChanged) onChanged();
    }

    void redo() override
    {
        if (!project) return;
        // Split again: modify first note and add second
        for (auto& note : project->getNotes()) {
            if (note.getStartFrame() == originalNote.getStartFrame()) {
                note = firstNote;
                break;
            }
        }
        project->addNote(secondNote);
        if (onChanged) onChanged();
    }

    juce::String getName() const override { return "Split Note"; }

private:
    Project* project;
    Note originalNote;
    Note firstNote;
    Note secondNote;
    std::function<void()> onChanged;
};

/**
 * Action for stretching note timing between two adjacent notes.
 */
class NoteTimingStretchAction : public UndoableAction
{
public:
    NoteTimingStretchAction(Note* leftNote,
                            Note* rightNote,
                            std::vector<float>* deltaPitchArray,
                            std::vector<bool>* voicedMaskArray,
                            std::vector<std::vector<float>>* melSpectrogram,
                            int rangeStart,
                            int rangeEnd,
                            int oldLeftStart, int oldLeftEnd,
                            int oldRightStart, int oldRightEnd,
                            int newLeftStart, int newLeftEnd,
                            int newRightStart, int newRightEnd,
                            std::vector<float> oldLeftClip,
                            std::vector<float> newLeftClip,
                            std::vector<float> oldRightClip,
                            std::vector<float> newRightClip,
                            std::vector<float> oldDelta,
                            std::vector<float> newDelta,
                            std::vector<bool> oldVoiced,
                            std::vector<bool> newVoiced,
                            std::vector<std::vector<float>> oldMel,
                            std::vector<std::vector<float>> newMel,
                            std::function<void(int, int)> onRangeChanged = nullptr)
        : left(leftNote), right(rightNote),
          deltaPitchArray(deltaPitchArray), voicedMaskArray(voicedMaskArray),
          melSpectrogram(melSpectrogram),
          rangeStart(rangeStart), rangeEnd(rangeEnd),
          oldLeftStart(oldLeftStart), oldLeftEnd(oldLeftEnd),
          oldRightStart(oldRightStart), oldRightEnd(oldRightEnd),
          newLeftStart(newLeftStart), newLeftEnd(newLeftEnd),
          newRightStart(newRightStart), newRightEnd(newRightEnd),
          oldLeftClip(std::move(oldLeftClip)),
          newLeftClip(std::move(newLeftClip)),
          oldRightClip(std::move(oldRightClip)),
          newRightClip(std::move(newRightClip)),
          oldDelta(std::move(oldDelta)), newDelta(std::move(newDelta)),
          oldVoiced(std::move(oldVoiced)), newVoiced(std::move(newVoiced)),
          oldMel(std::move(oldMel)), newMel(std::move(newMel)),
          onRangeChanged(std::move(onRangeChanged)) {}

    void undo() override
    {
        applyState(oldLeftStart, oldLeftEnd, oldRightStart, oldRightEnd,
                   oldLeftClip, oldRightClip, oldDelta, oldVoiced, oldMel);
    }

    void redo() override
    {
        applyState(newLeftStart, newLeftEnd, newRightStart, newRightEnd,
                   newLeftClip, newRightClip, newDelta, newVoiced, newMel);
    }

    juce::String getName() const override { return "Stretch Note Timing"; }

private:
    void applyState(int leftStart, int leftEnd,
                    int rightStart, int rightEnd,
                    const std::vector<float>& leftClip,
                    const std::vector<float>& rightClip,
                    const std::vector<float>& delta,
                    const std::vector<bool>& voiced,
                    const std::vector<std::vector<float>>& mel)
    {
        if (left) {
            left->setStartFrame(leftStart);
            left->setEndFrame(leftEnd);
            left->markDirty();
            left->setClipWaveform(leftClip);
        }
        if (right) {
            right->setStartFrame(rightStart);
            right->setEndFrame(rightEnd);
            right->markDirty();
            right->setClipWaveform(rightClip);
        }

        if (deltaPitchArray && rangeEnd > rangeStart &&
            delta.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (deltaPitchArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*deltaPitchArray)[static_cast<size_t>(i)] =
                        delta[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (voicedMaskArray && rangeEnd > rangeStart &&
            voiced.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (voicedMaskArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*voicedMaskArray)[static_cast<size_t>(i)] =
                        voiced[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (melSpectrogram && rangeEnd > rangeStart &&
            mel.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (melSpectrogram->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*melSpectrogram)[static_cast<size_t>(i)] =
                        mel[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (onRangeChanged && rangeEnd > rangeStart)
            onRangeChanged(rangeStart, rangeEnd);
    }

    Note* left = nullptr;
    Note* right = nullptr;
    std::vector<float>* deltaPitchArray = nullptr;
    std::vector<bool>* voicedMaskArray = nullptr;
    std::vector<std::vector<float>>* melSpectrogram = nullptr;
    int rangeStart = 0;
    int rangeEnd = 0;
    int oldLeftStart = 0;
    int oldLeftEnd = 0;
    int oldRightStart = 0;
    int oldRightEnd = 0;
    int newLeftStart = 0;
    int newLeftEnd = 0;
    int newRightStart = 0;
    int newRightEnd = 0;
    std::vector<float> oldLeftClip;
    std::vector<float> newLeftClip;
    std::vector<float> oldRightClip;
    std::vector<float> newRightClip;
    std::vector<float> oldDelta;
    std::vector<float> newDelta;
    std::vector<bool> oldVoiced;
    std::vector<bool> newVoiced;
    std::vector<std::vector<float>> oldMel;
    std::vector<std::vector<float>> newMel;
    std::function<void(int, int)> onRangeChanged;
};

/**
 * Action for ripple-stretching note timing (left note resampled, right side shifted).
 */
class NoteTimingRippleAction : public UndoableAction
{
public:
    NoteTimingRippleAction(Note* leftNote,
                           Note* rightNote,
                           std::vector<Note*> rippleNotes,
                           std::vector<float>* deltaPitchArray,
                           std::vector<bool>* voicedMaskArray,
                           std::vector<std::vector<float>>* melSpectrogram,
                           int rangeStart,
                           int rangeEnd,
                           int oldLeftStart, int oldLeftEnd,
                           int newLeftStart, int newLeftEnd,
                           std::vector<int> oldNoteStarts,
                           std::vector<int> oldNoteEnds,
                           std::vector<int> newNoteStarts,
                           std::vector<int> newNoteEnds,
                           std::vector<float> oldLeftClip,
                           std::vector<float> newLeftClip,
                           std::vector<float> oldRightClip,
                           std::vector<float> newRightClip,
                           std::vector<float> oldDelta,
                           std::vector<float> newDelta,
                           std::vector<bool> oldVoiced,
                           std::vector<bool> newVoiced,
                           std::vector<std::vector<float>> oldMel,
                           std::vector<std::vector<float>> newMel,
                           std::function<void(int, int)> onRangeChanged = nullptr)
        : left(leftNote), right(rightNote), rippleNotes(std::move(rippleNotes)),
          deltaPitchArray(deltaPitchArray), voicedMaskArray(voicedMaskArray),
          melSpectrogram(melSpectrogram),
          rangeStart(rangeStart), rangeEnd(rangeEnd),
          oldLeftStart(oldLeftStart), oldLeftEnd(oldLeftEnd),
          newLeftStart(newLeftStart), newLeftEnd(newLeftEnd),
          oldNoteStarts(std::move(oldNoteStarts)),
          oldNoteEnds(std::move(oldNoteEnds)),
          newNoteStarts(std::move(newNoteStarts)),
          newNoteEnds(std::move(newNoteEnds)),
          oldLeftClip(std::move(oldLeftClip)),
          newLeftClip(std::move(newLeftClip)),
          oldRightClip(std::move(oldRightClip)),
          newRightClip(std::move(newRightClip)),
          oldDelta(std::move(oldDelta)), newDelta(std::move(newDelta)),
          oldVoiced(std::move(oldVoiced)), newVoiced(std::move(newVoiced)),
          oldMel(std::move(oldMel)), newMel(std::move(newMel)),
          onRangeChanged(std::move(onRangeChanged)) {}

    void undo() override {
        applyState(oldLeftStart, oldLeftEnd, oldNoteStarts, oldNoteEnds,
                   oldLeftClip, oldRightClip, oldDelta, oldVoiced, oldMel);
    }
    void redo() override {
        applyState(newLeftStart, newLeftEnd, newNoteStarts, newNoteEnds,
                   newLeftClip, newRightClip, newDelta, newVoiced, newMel);
    }

    juce::String getName() const override { return "Ripple Stretch Timing"; }

private:
    void applyState(int leftStart, int leftEnd,
                    const std::vector<int>& noteStarts,
                    const std::vector<int>& noteEnds,
                    const std::vector<float>& leftClip,
                    const std::vector<float>& rightClip,
                    const std::vector<float>& delta,
                    const std::vector<bool>& voiced,
                    const std::vector<std::vector<float>>& mel)
    {
        if (left) {
            left->setStartFrame(leftStart);
            left->setEndFrame(leftEnd);
            left->markDirty();
            left->setClipWaveform(leftClip);
        }
        if (right)
            right->setClipWaveform(rightClip);

        for (size_t i = 0; i < rippleNotes.size() && i < noteStarts.size() && i < noteEnds.size(); ++i) {
            if (rippleNotes[i]) {
                rippleNotes[i]->setStartFrame(noteStarts[i]);
                rippleNotes[i]->setEndFrame(noteEnds[i]);
                rippleNotes[i]->markDirty();
            }
        }

        if (deltaPitchArray && rangeEnd > rangeStart &&
            delta.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (deltaPitchArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*deltaPitchArray)[static_cast<size_t>(i)] =
                        delta[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (voicedMaskArray && rangeEnd > rangeStart &&
            voiced.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (voicedMaskArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*voicedMaskArray)[static_cast<size_t>(i)] =
                        voiced[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (melSpectrogram && rangeEnd > rangeStart &&
            mel.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (melSpectrogram->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*melSpectrogram)[static_cast<size_t>(i)] =
                        mel[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (onRangeChanged && rangeEnd > rangeStart)
            onRangeChanged(rangeStart, rangeEnd);
    }

    Note* left = nullptr;
    Note* right = nullptr;
    std::vector<Note*> rippleNotes;
    std::vector<float>* deltaPitchArray = nullptr;
    std::vector<bool>* voicedMaskArray = nullptr;
    std::vector<std::vector<float>>* melSpectrogram = nullptr;
    int rangeStart = 0;
    int rangeEnd = 0;
    int oldLeftStart = 0;
    int oldLeftEnd = 0;
    int newLeftStart = 0;
    int newLeftEnd = 0;
    std::vector<int> oldNoteStarts;
    std::vector<int> oldNoteEnds;
    std::vector<int> newNoteStarts;
    std::vector<int> newNoteEnds;
    std::vector<float> oldLeftClip;
    std::vector<float> newLeftClip;
    std::vector<float> oldRightClip;
    std::vector<float> newRightClip;
    std::vector<float> oldDelta;
    std::vector<float> newDelta;
    std::vector<bool> oldVoiced;
    std::vector<bool> newVoiced;
    std::vector<std::vector<float>> oldMel;
    std::vector<std::vector<float>> newMel;
    std::function<void(int, int)> onRangeChanged;
};

/**
 * Simple undo manager for the pitch editor.
 */
class PitchUndoManager
{
public:
    PitchUndoManager(size_t maxHistory = 100) : maxHistory(maxHistory) {}
    
    void addAction(std::unique_ptr<UndoableAction> action)
    {
        // Clear redo stack when new action is added
        redoStack.clear();
        redoStack.shrink_to_fit();  // Release memory

        undoStack.push_back(std::move(action));

        // Limit history size
        while (undoStack.size() > maxHistory)
        {
            undoStack.erase(undoStack.begin());
        }

        if (onHistoryChanged)
            onHistoryChanged();
    }
    
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    
    void undo()
    {
        if (undoStack.empty()) return;
        
        auto action = std::move(undoStack.back());
        undoStack.pop_back();
        
        action->undo();
        redoStack.push_back(std::move(action));
        
        if (onHistoryChanged)
            onHistoryChanged();
    }
    
    void redo()
    {
        if (redoStack.empty()) return;
        
        auto action = std::move(redoStack.back());
        redoStack.pop_back();
        
        action->redo();
        undoStack.push_back(std::move(action));
        
        if (onHistoryChanged)
            onHistoryChanged();
    }
    
    void clear()
    {
        undoStack.clear();
        undoStack.shrink_to_fit();  // Release memory
        redoStack.clear();
        redoStack.shrink_to_fit();  // Release memory

        if (onHistoryChanged)
            onHistoryChanged();
    }
    
    juce::String getUndoName() const
    {
        return undoStack.empty() ? "" : undoStack.back()->getName();
    }
    
    juce::String getRedoName() const
    {
        return redoStack.empty() ? "" : redoStack.back()->getName();
    }
    
    std::function<void()> onHistoryChanged;
    
private:
    std::vector<std::unique_ptr<UndoableAction>> undoStack;
    std::vector<std::unique_ptr<UndoableAction>> redoStack;
    size_t maxHistory;
};
