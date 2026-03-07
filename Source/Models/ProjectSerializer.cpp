#include "ProjectSerializer.h"
#include "../Utils/PitchCurveProcessor.h"

bool ProjectSerializer::saveToFile(const Project& project, const juce::File& file) {
    auto json = toJson(project);
    auto jsonString = juce::JSON::toString(json, true); // Pretty print

    return file.replaceWithText(jsonString);
}

bool ProjectSerializer::loadFromFile(Project& project, const juce::File& file) {
    auto jsonString = file.loadFileAsString();
    if (jsonString.isEmpty())
        return false;

    auto json = juce::JSON::parse(jsonString);
    if (!json.isObject())
        return false;

    return fromJson(project, json);
}

juce::var ProjectSerializer::toJson(const Project& project) {
    auto* obj = new juce::DynamicObject();

    // Metadata
    obj->setProperty("formatVersion", FORMAT_VERSION);
    obj->setProperty("name", project.getName());
    obj->setProperty("audioPath", project.getFilePath().getFullPathName());
    obj->setProperty("audioSha256", project.getAudioSha256());

    // Audio settings
    obj->setProperty("sampleRate", project.getAudioData().sampleRate);

    // Global parameters
    obj->setProperty("globalPitchOffset", project.getGlobalPitchOffset());
    obj->setProperty("pitchOffsetBeforeSVC", project.isPitchOffsetBeforeSVC());
    obj->setProperty("formantShift", project.getFormantShift());
    obj->setProperty("volume", project.getVolume());

    // Loop range
    const auto& loopRange = project.getLoopRange();
    auto* loopObj = new juce::DynamicObject();
    loopObj->setProperty("enabled", loopRange.enabled);
    loopObj->setProperty("start", loopRange.startSeconds);
    loopObj->setProperty("end", loopRange.endSeconds);
    obj->setProperty("loop", juce::var(loopObj));

    // Notes array
    juce::Array<juce::var> notesArray;
    for (const auto& note : project.getNotes()) {
        notesArray.add(noteToJson(note));
    }
    obj->setProperty("notes", notesArray);

    // Pitch data
    obj->setProperty("pitchData", pitchDataToJson(project.getAudioData()));

    return juce::var(obj);
}

bool ProjectSerializer::fromJson(Project& project, const juce::var& json) {
    if (!json.isObject())
        return false;

    // Metadata
    project.setName(json.getProperty("name", "Untitled").toString());
    project.setFilePath(juce::File(json.getProperty("audioPath", "").toString()));
    project.setAudioSha256(json.getProperty("audioSha256", "").toString());

    // Audio settings
    auto& audioData = project.getAudioData();
    audioData.sampleRate = json.getProperty("sampleRate", 44100);

    // Global parameters
    project.setGlobalPitchOffset(static_cast<float>(json.getProperty("globalPitchOffset", 0.0)));
    project.setPitchOffsetBeforeSVC(static_cast<bool>(json.getProperty("pitchOffsetBeforeSVC", true)));
    project.setFormantShift(static_cast<float>(json.getProperty("formantShift", 0.0)));
    project.setVolume(static_cast<float>(json.getProperty("volume", 0.0)));

    // Loop range
    auto loopVar = json.getProperty("loop", juce::var());
    if (loopVar.isObject()) {
        const double loopStart = loopVar.getProperty("start", 0.0);
        const double loopEnd = loopVar.getProperty("end", 0.0);
        project.setLoopRange(loopStart, loopEnd);
        project.setLoopEnabled(loopVar.getProperty("enabled", false));
    }

    // Notes
    project.clearNotes();
    auto notesVar = json.getProperty("notes", juce::var());
    if (notesVar.isArray()) {
        for (int i = 0; i < notesVar.size(); ++i) {
            Note note;
            if (noteFromJson(note, notesVar[i])) {
                project.addNote(std::move(note));
            }
        }
    }

    // Pitch data
    auto pitchDataVar = json.getProperty("pitchData", juce::var());
    if (pitchDataVar.isObject()) {
        pitchDataFromJson(audioData, pitchDataVar);
    }

    // Rebuild curves if needed
    if (!audioData.f0.empty() && (audioData.basePitch.empty() || audioData.deltaPitch.empty())) {
        PitchCurveProcessor::rebuildCurvesFromSource(project, audioData.f0);
    }

    project.setModified(false);
    return true;
}

juce::var ProjectSerializer::noteToJson(const Note& note) {
    auto* obj = new juce::DynamicObject();

    obj->setProperty("startFrame", note.getStartFrame());
    obj->setProperty("endFrame", note.getEndFrame());
    obj->setProperty("midiNote", note.getMidiNote());
    obj->setProperty("pitchOffset", note.getPitchOffset());
    obj->setProperty("volumeDb", note.getVolumeDb());
    obj->setProperty("rest", note.isRest());

    // Vibrato
    auto* vibrato = new juce::DynamicObject();
    vibrato->setProperty("enabled", note.isVibratoEnabled());
    vibrato->setProperty("rateHz", note.getVibratoRateHz());
    vibrato->setProperty("depthSemitones", note.getVibratoDepthSemitones());
    vibrato->setProperty("phaseRadians", note.getVibratoPhaseRadians());
    obj->setProperty("vibrato", juce::var(vibrato));

    // Lyric/Phoneme
    if (note.hasLyric())
        obj->setProperty("lyric", note.getLyric());
    if (note.hasPhoneme())
        obj->setProperty("phoneme", note.getPhoneme());

    return juce::var(obj);
}

bool ProjectSerializer::noteFromJson(Note& note, const juce::var& json) {
    if (!json.isObject())
        return false;

    note.setStartFrame(json.getProperty("startFrame", 0));
    note.setEndFrame(json.getProperty("endFrame", 0));
    note.setMidiNote(static_cast<float>(json.getProperty("midiNote", 60.0)));
    note.setPitchOffset(static_cast<float>(json.getProperty("pitchOffset", 0.0)));
    note.setVolumeDb(static_cast<float>(json.getProperty("volumeDb", 0.0)));
    note.setRest(json.getProperty("rest", false));

    // Vibrato
    auto vibratoVar = json.getProperty("vibrato", juce::var());
    if (vibratoVar.isObject()) {
        note.setVibratoEnabled(vibratoVar.getProperty("enabled", false));
        note.setVibratoRateHz(static_cast<float>(vibratoVar.getProperty("rateHz", 5.0)));
        note.setVibratoDepthSemitones(static_cast<float>(vibratoVar.getProperty("depthSemitones", 0.0)));
        note.setVibratoPhaseRadians(static_cast<float>(vibratoVar.getProperty("phaseRadians", 0.0)));
    }

    // Lyric/Phoneme
    auto lyric = json.getProperty("lyric", juce::var());
    if (!lyric.isVoid())
        note.setLyric(lyric.toString());

    auto phoneme = json.getProperty("phoneme", juce::var());
    if (!phoneme.isVoid())
        note.setPhoneme(phoneme.toString());

    return true;
}

juce::var ProjectSerializer::pitchDataToJson(const AudioData& audioData) {
    auto* obj = new juce::DynamicObject();

    // Store as compact strings for efficiency
    obj->setProperty("f0", floatArrayToString(audioData.f0, 2));
    obj->setProperty("basePitch", floatArrayToString(audioData.basePitch, 4));
    obj->setProperty("deltaPitch", floatArrayToString(audioData.deltaPitch, 4));
    obj->setProperty("voicedMask", boolArrayToString(audioData.voicedMask));
    obj->setProperty("vadMask", boolArrayToString(audioData.vadMask));

    return juce::var(obj);
}

bool ProjectSerializer::pitchDataFromJson(AudioData& audioData, const juce::var& json) {
    if (!json.isObject())
        return false;

    audioData.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    audioData.baseF0 = audioData.f0; // Initialize baseF0 from loaded f0
    audioData.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    audioData.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    audioData.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    audioData.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());

    return true;
}

juce::String ProjectSerializer::floatArrayToString(const std::vector<float>& arr, int precision) {
    if (arr.empty())
        return {};

    juce::StringArray parts;
    parts.ensureStorageAllocated(static_cast<int>(arr.size()));

    for (float v : arr) {
        parts.add(juce::String(v, precision));
    }

    return parts.joinIntoString(" ");
}

std::vector<float> ProjectSerializer::stringToFloatArray(const juce::String& str) {
    if (str.isEmpty())
        return {};

    juce::StringArray parts;
    parts.addTokens(str, " ", "");

    std::vector<float> result;
    result.reserve(static_cast<size_t>(parts.size()));

    for (const auto& p : parts) {
        if (p.isNotEmpty())
            result.push_back(p.getFloatValue());
    }

    return result;
}

juce::String ProjectSerializer::boolArrayToString(const std::vector<bool>& arr) {
    if (arr.empty())
        return {};

    juce::String result;
    result.preallocateBytes(arr.size());

    for (bool b : arr) {
        result << (b ? '1' : '0');
    }

    return result;
}

std::vector<bool> ProjectSerializer::stringToBoolArray(const juce::String& str) {
    if (str.isEmpty())
        return {};

    std::vector<bool> result;
    result.reserve(static_cast<size_t>(str.length()));

    for (int i = 0; i < str.length(); ++i) {
        result.push_back(str[i] == '1');
    }

    return result;
}
