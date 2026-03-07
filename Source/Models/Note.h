#pragma once

#include "../JuceHeader.h"
#include <vector>

/**
 * Represents a single note/pitch segment.
 *
 * Time stretching model:
 * - srcStartFrame/srcEndFrame: Position in original waveform (fixed after detection)
 * - startFrame/endFrame: Position in output timeline (can be changed by dragging)
 * - stretchRatio = (endFrame - startFrame) / (srcEndFrame - srcStartFrame)
 *
 * Pitch model:
 * - midiNote: The base pitch of the note (can be changed by dragging)
 * - deltaPitch: Per-frame deviation from base pitch (preserved during drag)
 * - f0Values: Original F0 values from detection (for reference)
 *
 * When dragging a note up/down:
 * - midiNote changes
 * - deltaPitch stays the same
 * - Actual pitch = midiNote + deltaPitch[frame]
 *
 * When stretching a note:
 * - srcStartFrame/srcEndFrame stay the same (original position)
 * - startFrame/endFrame change (output position)
 * - deltaPitch is resampled to match new length
 */
class Note
{
public:
    Note() = default;
    Note(int startFrame, int endFrame, float midiNote);

    // Source frame range (position in original waveform, fixed after detection)
    int getSrcStartFrame() const { return srcStartFrame; }
    int getSrcEndFrame() const { return srcEndFrame; }
    void setSrcStartFrame(int frame) { srcStartFrame = frame; }
    void setSrcEndFrame(int frame) { srcEndFrame = frame; }
    int getSrcDurationFrames() const { return srcEndFrame - srcStartFrame; }

    // Destination frame range (position in output timeline, can be changed)
    int getStartFrame() const { return startFrame; }
    int getEndFrame() const { return endFrame; }
    void setStartFrame(int frame) { startFrame = frame; }
    void setEndFrame(int frame) { endFrame = frame; }
    int getDurationFrames() const { return endFrame - startFrame; }

    // Time stretch ratio (output length / source length)
    float getStretchRatio() const {
        int srcLen = srcEndFrame - srcStartFrame;
        if (srcLen <= 0) return 1.0f;
        return float(endFrame - startFrame) / float(srcLen);
    }

    // Check if note is stretched (ratio != 1.0)
    bool isStretched() const {
        return std::abs(getStretchRatio() - 1.0f) > 0.001f;
    }

    // Pitch
    float getMidiNote() const { return midiNote; }
    void setMidiNote(float note) { midiNote = note; }
    float getPitchOffset() const { return pitchOffset; }
    void setPitchOffset(float offset) { pitchOffset = offset; }
    float getAdjustedMidiNote() const { return midiNote + pitchOffset; }
    float getVolumeDb() const { return volumeDb; }
    void setVolumeDb(float db) { volumeDb = db; }

    // Delta pitch (per-frame deviation from base pitch in semitones)
    const std::vector<float>& getDeltaPitch() const { return deltaPitch; }
    void setDeltaPitch(std::vector<float> delta) { deltaPitch = std::move(delta); }
    bool hasDeltaPitch() const { return !deltaPitch.empty(); }

    // Vibrato
    bool isVibratoEnabled() const { return vibratoEnabled; }
    void setVibratoEnabled(bool enabled) { vibratoEnabled = enabled; }
    float getVibratoRateHz() const { return vibratoRateHz; }
    void setVibratoRateHz(float hz) { vibratoRateHz = hz; }
    float getVibratoDepthSemitones() const { return vibratoDepthSemitones; }
    void setVibratoDepthSemitones(float semitones) { vibratoDepthSemitones = semitones; }
    float getVibratoPhaseRadians() const { return vibratoPhaseRadians; }
    void setVibratoPhaseRadians(float radians) { vibratoPhaseRadians = radians; }

    // F0 values (original detected values)
    const std::vector<float>& getF0Values() const { return f0Values; }
    void setF0Values(std::vector<float> values) { f0Values = std::move(values); }
    std::vector<float> getAdjustedF0() const;

    // Get F0 values based on current midiNote + deltaPitch
    std::vector<float> computeF0FromDelta() const;

    // Waveform clip (original samples for this note)
    const std::vector<float>& getClipWaveform() const { return clipWaveform; }
    void setClipWaveform(std::vector<float> samples) { clipWaveform = std::move(samples); }
    bool hasClipWaveform() const { return !clipWaveform.empty(); }

    // Mel spectrogram clip (original mel frames for this note)
    const std::vector<std::vector<float>>& getClipMel() const { return clipMel; }
    void setClipMel(std::vector<std::vector<float>> mel) { clipMel = std::move(mel); }
    bool hasClipMel() const { return !clipMel.empty(); }

    // Selection
    bool isSelected() const { return selected; }
    void setSelected(bool sel) { selected = sel; }

    // Dirty flag (for incremental synthesis)
    bool isDirty() const { return dirty; }
    void setDirty(bool d) { dirty = d; }
    void markDirty() { dirty = true; }
    void clearDirty() { dirty = false; }

    // Rest note (no pitch, just a placeholder for silence)
    bool isRest() const { return rest; }
    void setRest(bool r) { rest = r; }

    // Lyric (character/syllable for this note)
    juce::String getLyric() const { return lyric; }
    void setLyric(const juce::String& text) { lyric = text; }
    bool hasLyric() const { return lyric.isNotEmpty(); }

    // Phoneme (pronunciation for this note)
    juce::String getPhoneme() const { return phoneme; }
    void setPhoneme(const juce::String& ph) { phoneme = ph; }
    bool hasPhoneme() const { return phoneme.isNotEmpty(); }

    // Check if frame is within note
    bool containsFrame(int frame) const;

private:
    // Source position (in original waveform, fixed after detection)
    int srcStartFrame = 0;
    int srcEndFrame = 0;

    // Destination position (in output timeline, can be changed by stretching)
    int startFrame = 0;
    int endFrame = 0;

    float midiNote = 60.0f;
    float pitchOffset = 0.0f;
    float volumeDb = 0.0f; // Per-note gain in dB (0 = unity)

    std::vector<float> deltaPitch;  // Per-frame deviation from midiNote in semitones

    bool vibratoEnabled = false;
    float vibratoRateHz = 5.0f;
    float vibratoDepthSemitones = 0.0f;
    float vibratoPhaseRadians = 0.0f;

    std::vector<float> f0Values;
    std::vector<float> clipWaveform;
    std::vector<std::vector<float>> clipMel;  // Mel spectrogram clip [T, numMels]
    bool selected = false;
    bool dirty = false;  // For incremental synthesis
    bool rest = false;   // Rest note (silence placeholder)

    juce::String lyric;   // Lyric text (e.g., "a", "SP" for silence)
    juce::String phoneme; // Phoneme (e.g., "a", "sp", for pronunciation)
};
