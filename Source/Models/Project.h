#pragma once

#include "../JuceHeader.h"
#include "Note.h"
#include <vector>
#include <memory>
#include <utility>

/**
 * Container for audio data and extracted features.
 */
struct AudioData
{
    struct SomeDebugEvent {
        int startFrame = 0;
        int endFrame = 0;
        int attachedStartFrame = 0;
        float midiNote = 0.0f;
        bool isRest = false;
        float durationSeconds = 0.0f;
        int durationFrames = 0;
    };

    struct SomeDebugChunk {
        int chunkIndex = 0;
        int startFrame = 0;
        int endFrame = 0;
        int shortRestThreshold = 0;
        std::vector<SomeDebugEvent> events;
    };

    juce::AudioBuffer<float> waveform;
    juce::AudioBuffer<float> originalWaveform;  // pristine copy for blend (never modified after analysis)
    juce::AudioBuffer<float> harmonicWaveform;
    juce::AudioBuffer<float> noiseWaveform;
    int sampleRate = 44100;
    
    // Extracted features
    std::vector<std::vector<float>> melSpectrogram;  // [T, NUM_MELS]
    std::vector<float> f0;                            // [T] (composed: base + delta, dense)
    std::vector<float> baseF0;                        // [T] (cached base pitch in Hz)
    std::vector<float> basePitch;                     // [T] base pitch in MIDI (dense)
    std::vector<float> deltaPitch;                    // [T] delta pitch in MIDI (dense)
    std::vector<float> voicingCurve;                  // [T] hnsep harmonic energy in %
    std::vector<float> breathCurve;                   // [T] hnsep noise energy in %
    std::vector<float> tensionCurve;                  // [T] hnsep spectral tilt control
    std::vector<float> shfcCurve;                     // [T] SVC-only pitch offset in semitones
    std::vector<bool> voicedMask;                     // [T] uv mask (true = voiced, F0-based)
    std::vector<bool> vadMask;                        // [T] energy-based VAD (true = has audio energy, captures consonants)
    std::vector<std::pair<int, int>> someChunkRanges; // [N] SOME slicer chunks in frame range [start, end)
    std::vector<SomeDebugChunk> someDebugChunks;      // raw SOME outputs for debug visualization

    /** When true, melSpectrogram contains SVC-generated mel (not analysis mel).
     *  Incremental synthesis should use stored mel through vocoder
     *  rather than re-running SVC inference — this allows stretch/pitch edits
     *  to operate on SVC mel without costly re-inference. */
    bool melFromSVC = false;
    bool waveformFromSVC = false;
    bool hnsepBasesFromSVC = false;
    bool svcEnabled = false;
    bool svcVoicebankWasDirectory = true;
    bool svcRendered = false;
    bool hnsepCurvesTargetSVC = false;
    juce::String svcVoicebankName;
    juce::String svcVoicebankPath;
    
    float getDuration() const
    {
        if (waveform.getNumSamples() == 0) return 0.0f;
        return static_cast<float>(waveform.getNumSamples()) / sampleRate;
    }
    
    int getNumFrames() const
    {
        return static_cast<int>(melSpectrogram.size());
    }
};

/**
 * Loop playback range in seconds.
 */
struct LoopRange
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    bool enabled = false;

    bool isValid() const { return enabled && endSeconds > startSeconds; }
};

/**
 * Project data container.
 */
class Project
{
public:
    Project();
    ~Project() = default;

    /** Deep copy of this project. */
    std::unique_ptr<Project> clone() const;
    
    // File operations
    void setFilePath(const juce::File& file) { filePath = file; }
    juce::File getFilePath() const { return filePath; }
    void setProjectFilePath(const juce::File& file) { projectFilePath = file; }
    juce::File getProjectFilePath() const { return projectFilePath; }
    void setAudioSha256(const juce::String& sha) { audioSha256 = sha; }
    juce::String getAudioSha256() const { return audioSha256; }
    juce::String getName() const { return name; }
    void setName(const juce::String& n) { name = n; }
    
    // Audio data
    AudioData& getAudioData() { return audioData; }
    const AudioData& getAudioData() const { return audioData; }
    
    // Notes
    std::vector<Note>& getNotes() { return notes; }
    const std::vector<Note>& getNotes() const { return notes; }
    void addNote(Note note) { notes.push_back(std::move(note)); }
    void clearNotes() { notes.clear(); }

    Note* getNoteAtFrame(int frame);
    std::vector<Note*> getNotesInRange(int startFrame, int endFrame);
    std::vector<Note*> getSelectedNotes();
    bool removeNoteByStartFrame(int startFrame);
    std::vector<Note*> getDirtyNotes();
    void selectAllNotes(bool includeRests = false);
    void deselectAllNotes();
    void clearAllDirty();
    
    // Global settings
    float getGlobalPitchOffset() const { return globalPitchOffset; }
    void setGlobalPitchOffset(float offset) { globalPitchOffset = offset; }
    
    /** When true, global pitch offset is applied to F0 before SVC (default).
     *  When false, global pitch offset is only applied at vocoder stage (post-SVC). */
    bool isPitchOffsetBeforeSVC() const { return pitchOffsetBeforeSVC; }
    void setPitchOffsetBeforeSVC(bool before) { pitchOffsetBeforeSVC = before; }
    
    float getFormantShift() const { return formantShift; }
    void setFormantShift(float shift) { formantShift = shift; }
    
    float getVolume() const { return volume; }
    void setVolume(float vol) { volume = vol; }
    
    // Get adjusted F0 with all modifications applied (including global pitch offset)
    std::vector<float> getAdjustedF0() const;
    
    // Get adjusted F0 for a specific frame range (including global pitch offset)
    std::vector<float> getAdjustedF0ForRange(int startFrame, int endFrame) const;
    
    // Get adjusted F0 WITHOUT global pitch offset (for post-SVC mode: SVC uses this)
    std::vector<float> getAdjustedF0NoGlobalOffset() const;
    std::vector<float> getAdjustedF0ForRangeNoGlobalOffset(int startFrame, int endFrame) const;
    
    // Get frame range that needs resynthesis (based on dirty notes)
    // Returns {-1, -1} if no dirty notes
    std::pair<int, int> getDirtyFrameRange() const;
    
    // Check if any notes are dirty
    bool hasDirtyNotes() const;
    
    // F0 direct edit dirty tracking (for Draw mode)
    void setF0DirtyRange(int startFrame, int endFrame);
    void clearF0DirtyRange();
    bool hasF0DirtyRange() const;
    std::pair<int, int> getF0DirtyRange() const;

    // SVC conditioning dirty tracking (parameters that require SVC re-inference)
    void setSvcConditioningDirtyRange(int startFrame, int endFrame);
    bool hasSvcConditioningDirtyRange() const;
    std::pair<int, int> getSvcConditioningDirtyRange() const;
    
    // Modified state
    bool isModified() const { return modified; }
    void setModified(bool mod) { modified = mod; }

    // Loop range
    const LoopRange& getLoopRange() const { return loopRange; }
    void setLoopRange(double startSeconds, double endSeconds);
    void setLoopEnabled(bool enabled);
    void clearLoopRange();

private:
    juce::String name = "Untitled";
    juce::File filePath;
    juce::File projectFilePath;
    juce::String audioSha256;
    
    AudioData audioData;
    std::vector<Note> notes;
    
    float globalPitchOffset = 0.0f;
    bool pitchOffsetBeforeSVC = true;  // true=offset→SVC, false=SVC→offset
    float formantShift = 0.0f;
    float volume = 0.0f;  // dB
    
    // F0 direct edit dirty range
    int f0DirtyStart = -1;
    int f0DirtyEnd = -1;
    int svcConditioningDirtyStart = -1;
    int svcConditioningDirtyEnd = -1;
    
    bool modified = false;

    LoopRange loopRange;
};
