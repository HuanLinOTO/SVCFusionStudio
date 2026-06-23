#pragma once

#include "../JuceHeader.h"
#include "Project.h"
#include <memory>

enum class TrackType {
    Accompaniment,
    Vocal
};

struct Track {
    juce::String id;
    juce::String name;
    TrackType type = TrackType::Accompaniment;
    bool mute = false;
    bool solo = false;
    std::unique_ptr<Project> project;

    Track();
    ~Track();

    Track(Track&&) = default;
    Track& operator=(Track&&) = default;

    Track(const Track&) = delete;
    Track& operator=(const Track&) = delete;

    Project* getProject() const { return project.get(); }

    bool isVocal() const { return type == TrackType::Vocal; }
    bool isAccompaniment() const { return type == TrackType::Accompaniment; }

    float getVolume() const { return project ? project->getVolume() : 0.0f; }
    void setVolume(float vol) { if (project) project->setVolume(vol); }

    bool isMuted() const;
    bool isAudible(const std::vector<Track*>& allTracks) const;

    static juce::String trackTypeToString(TrackType t);
    static TrackType stringToTrackType(const juce::String& s);
    static juce::String generateId();
};
