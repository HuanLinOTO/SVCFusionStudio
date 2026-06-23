#pragma once

#include "../JuceHeader.h"
#include "Project.h"
#include "Track.h"
#include <vector>
#include <memory>

/**
 * Handles Project serialization to/from JSON format.
 *
 * Design principles:
 * - Decoupled from Project class (Project doesn't know about serialization details)
 * - Uses JUCE's built-in JSON support (no external dependencies)
 * - Stateless utility class
 */
class ProjectSerializer {
public:
    static constexpr int FORMAT_VERSION = 3;

    /**
     * Save project to JSON file.
     */
    static bool saveToFile(const Project& project, const juce::File& file);

    /**
     * Load project from JSON file.
     */
    static bool loadFromFile(Project& project, const juce::File& file);

    /**
     * Convert project to JSON object.
     */
    static juce::var toJson(const Project& project);

    /**
     * Load project from JSON object.
     */
    static bool fromJson(Project& project, const juce::var& json);

    // ── Multi-track serialization (v3) ──

    /**
     * Save multiple tracks to JSON file.
     */
    static bool saveTracksToFile(const std::vector<std::unique_ptr<Track>>& tracks,
                                 const juce::File& file,
                                 const LoopRange& sessionLoopRange = {});

    /**
     * Load tracks from JSON file.
     * Returns vector of tracks and session loop range.
     */
    static std::pair<std::vector<std::unique_ptr<Track>>, LoopRange>
    loadTracksFromFile(const juce::File& file);

    /**
     * Convert tracks to JSON object.
     */
    static juce::var tracksToJson(const std::vector<std::unique_ptr<Track>>& tracks,
                                  const LoopRange& sessionLoopRange = {});

    /**
     * Load tracks from JSON object.
     */
    static std::pair<std::vector<std::unique_ptr<Track>>, LoopRange>
    tracksFromJson(const juce::var& json);

private:
    // Note serialization
    static juce::var noteToJson(const Note& note);
    static bool noteFromJson(Note& note, const juce::var& json);

    // Pitch data serialization
    static juce::var pitchDataToJson(const AudioData& audioData);
    static bool pitchDataFromJson(AudioData& audioData, const juce::var& json);

    // Single track serialization (used internally by multi-track)
    static juce::var trackToJson(const Track& track);
    static std::unique_ptr<Track> trackFromJson(const juce::var& json);

    // Loop range serialization
    static juce::var loopRangeToJson(const LoopRange& lr);
    static LoopRange loopRangeFromJson(const juce::var& json);

    // Array helpers (compact string format)
    static juce::String floatArrayToString(const std::vector<float>& arr, int precision = 4);
    static std::vector<float> stringToFloatArray(const juce::String& str);
    static juce::String boolArrayToString(const std::vector<bool>& arr);
    static std::vector<bool> stringToBoolArray(const juce::String& str);

    ProjectSerializer() = delete;
};
