#include "Track.h"

Track::Track() {
    id = generateId();
    project = std::make_unique<Project>();
}

Track::~Track() = default;

bool Track::isMuted() const {
    return mute;
}

bool Track::isAudible(const std::vector<Track*>& allTracks) const {
    bool anySolo = false;
    for (const auto& t : allTracks) {
        if (t && t->solo) {
            anySolo = true;
            break;
        }
    }
    if (anySolo)
        return solo && !mute;
    return !mute;
}

juce::String Track::trackTypeToString(TrackType t) {
    switch (t) {
        case TrackType::Accompaniment: return "accompaniment";
        case TrackType::Vocal: return "vocal";
    }
    return "accompaniment";
}

TrackType Track::stringToTrackType(const juce::String& s) {
    if (s.equalsIgnoreCase("vocal"))
        return TrackType::Vocal;
    return TrackType::Accompaniment;
}

juce::String Track::generateId() {
    static std::atomic<int> counter{0};
    auto now = juce::Time::getMillisecondCounterHiRes();
    return "track_" + juce::String(static_cast<int64_t>(now)) + "_" + juce::String(counter++);
}
