#pragma once

#include "../JuceHeader.h"
#include "Project.h"

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
    static constexpr int FORMAT_VERSION = 1;

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

private:
    // Note serialization
    static juce::var noteToJson(const Note& note);
    static bool noteFromJson(Note& note, const juce::var& json);

    // Pitch data serialization
    static juce::var pitchDataToJson(const AudioData& audioData);
    static bool pitchDataFromJson(AudioData& audioData, const juce::var& json);

    // Array helpers (compact string format)
    static juce::String floatArrayToString(const std::vector<float>& arr, int precision = 4);
    static std::vector<float> stringToFloatArray(const juce::String& str);
    static juce::String boolArrayToString(const std::vector<bool>& arr);
    static std::vector<bool> stringToBoolArray(const juce::String& str);

    ProjectSerializer() = delete;
};
