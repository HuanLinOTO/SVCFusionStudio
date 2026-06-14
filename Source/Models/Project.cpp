#include "Project.h"
#include "../Utils/Constants.h"
#include "../Utils/PitchCurveProcessor.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float twoPi = 6.2831853071795864769f;
}

Project::Project()
{
}

Note* Project::getNoteAtFrame(int frame)
{
    for (auto& note : notes)
    {
        if (note.containsFrame(frame))
            return &note;
    }
    return nullptr;
}

std::vector<Note*> Project::getNotesInRange(int startFrame, int endFrame)
{
    std::vector<Note*> result;
    for (auto& note : notes)
    {
        if (note.getStartFrame() < endFrame && note.getEndFrame() > startFrame)
            result.push_back(&note);
    }
    return result;
}

std::vector<Note*> Project::getSelectedNotes()
{
    std::vector<Note*> result;
    for (auto& note : notes)
    {
        if (note.isSelected())
            result.push_back(&note);
    }
    return result;
}

bool Project::removeNoteByStartFrame(int startFrame)
{
    for (auto it = notes.begin(); it != notes.end(); ++it)
    {
        if (it->getStartFrame() == startFrame)
        {
            notes.erase(it);
            return true;
        }
    }
    return false;
}

void Project::deselectAllNotes()
{
    for (auto& note : notes)
        note.setSelected(false);
}

void Project::selectAllNotes(bool includeRests)
{
    for (auto& note : notes)
    {
        if (!includeRests && note.isRest())
            continue;
        note.setSelected(true);
    }
}

std::vector<Note*> Project::getDirtyNotes()
{
    std::vector<Note*> result;
    for (auto& note : notes)
    {
        if (note.isDirty())
            result.push_back(&note);
    }
    return result;
}

void Project::clearAllDirty()
{
    for (auto& note : notes)
        note.clearDirty();
    // Also clear F0 dirty range
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
    svcConditioningDirtyStart = -1;
    svcConditioningDirtyEnd = -1;
}

bool Project::hasDirtyNotes() const
{
    for (const auto& note : notes)
    {
        if (note.isDirty())
            return true;
    }
    return false;
}

void Project::setF0DirtyRange(int startFrame, int endFrame)
{
    if (f0DirtyStart < 0 || startFrame < f0DirtyStart)
        f0DirtyStart = startFrame;
    if (f0DirtyEnd < 0 || endFrame > f0DirtyEnd)
        f0DirtyEnd = endFrame;
}

void Project::clearF0DirtyRange()
{
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
}

bool Project::hasF0DirtyRange() const
{
    return f0DirtyStart >= 0 && f0DirtyEnd >= 0;
}

std::pair<int, int> Project::getF0DirtyRange() const
{
    return {f0DirtyStart, f0DirtyEnd};
}

void Project::setSvcConditioningDirtyRange(int startFrame, int endFrame)
{
    if (svcConditioningDirtyStart < 0 || startFrame < svcConditioningDirtyStart)
        svcConditioningDirtyStart = startFrame;
    if (svcConditioningDirtyEnd < 0 || endFrame > svcConditioningDirtyEnd)
        svcConditioningDirtyEnd = endFrame;
}

bool Project::hasSvcConditioningDirtyRange() const
{
    return svcConditioningDirtyStart >= 0 && svcConditioningDirtyEnd >= 0;
}

std::pair<int, int> Project::getSvcConditioningDirtyRange() const
{
    return {svcConditioningDirtyStart, svcConditioningDirtyEnd};
}

std::pair<int, int> Project::getDirtyFrameRange() const
{
    int minStart = -1;
    int maxEnd = -1;
    
    // Check dirty notes
    for (const auto& note : notes)
    {
        if (note.isDirty())
        {
            if (minStart < 0 || note.getStartFrame() < minStart)
                minStart = note.getStartFrame();
            if (maxEnd < 0 || note.getEndFrame() > maxEnd)
                maxEnd = note.getEndFrame();
        }
    }
    
    // Also include F0 dirty range from Draw mode edits
    if (f0DirtyStart >= 0)
    {
        if (minStart < 0 || f0DirtyStart < minStart)
            minStart = f0DirtyStart;
    }
    if (f0DirtyEnd >= 0)
    {
        if (maxEnd < 0 || f0DirtyEnd > maxEnd)
            maxEnd = f0DirtyEnd;
    }

    if (svcConditioningDirtyStart >= 0)
    {
        if (minStart < 0 || svcConditioningDirtyStart < minStart)
            minStart = svcConditioningDirtyStart;
    }
    if (svcConditioningDirtyEnd >= 0)
    {
        if (maxEnd < 0 || svcConditioningDirtyEnd > maxEnd)
            maxEnd = svcConditioningDirtyEnd;
    }
    
    return {minStart, maxEnd};
}

std::vector<float> Project::getAdjustedF0() const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Compose base + delta as dense curve; UV blending is handled downstream
    // by synthesis masks, so we do not zero F0 here.
    std::vector<float> adjustedF0 = PitchCurveProcessor::composeF0(*this,
                                                                   /*applyUvMask=*/false,
                                                                   globalPitchOffset);

    // Apply vibrato per note on top of composed curve
    for (const auto& note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int start = std::max(0, note.getStartFrame());
        const int end = std::min(note.getEndFrame(), static_cast<int>(adjustedF0.size()));

        for (int i = start; i < end; ++i)
        {
            if (i < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[i])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(i - start) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(i)] *= std::pow(2.0f, vib / 12.0f);
        }
    }

    return adjustedF0;
}

std::vector<float> Project::getAdjustedF0NoGlobalOffset() const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Same as getAdjustedF0() but with globalPitchOffset = 0
    std::vector<float> adjustedF0 = PitchCurveProcessor::composeF0(*this,
                                                                   /*applyUvMask=*/false,
                                                                   /*globalPitchOffset=*/0.0f);

    // Apply vibrato per note
    for (const auto& note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int start = std::max(0, note.getStartFrame());
        const int end = std::min(note.getEndFrame(), static_cast<int>(adjustedF0.size()));

        for (int i = start; i < end; ++i)
        {
            if (i < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[i])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(i - start) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(i)] *= std::pow(2.0f, vib / 12.0f);
        }
    }

    return adjustedF0;
}

std::vector<float> Project::getAdjustedF0ForRangeNoGlobalOffset(int startFrame, int endFrame) const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    startFrame = std::max(0, startFrame);
    endFrame = std::min(endFrame, static_cast<int>(audioData.basePitch.size()));
    if (startFrame >= endFrame)
        return {};

    const int rangeSize = endFrame - startFrame;
    std::vector<float> adjustedF0(static_cast<size_t>(rangeSize), 0.0f);

    for (int i = 0; i < rangeSize; ++i)
    {
        const int globalIdx = startFrame + i;
        const float base = audioData.basePitch[static_cast<size_t>(globalIdx)];
        const float delta = (globalIdx < static_cast<int>(audioData.deltaPitch.size()))
                                ? audioData.deltaPitch[static_cast<size_t>(globalIdx)]
                                : 0.0f;
        float midi = base + delta;  // no globalPitchOffset
        adjustedF0[static_cast<size_t>(i)] = midiToFreq(midi);
    }

    // Apply vibrato for overlapping notes
    for (const auto& note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int overlapStart = std::max(note.getStartFrame(), startFrame);
        const int overlapEnd = std::min(note.getEndFrame(), endFrame);
        for (int frame = overlapStart; frame < overlapEnd; ++frame)
        {
            const int localIdx = frame - startFrame;
            if (frame < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[frame])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(frame - note.getStartFrame()) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(localIdx)] *= std::pow(2.0f, vib / 12.0f);
        }
    }

    return adjustedF0;
}

std::vector<float> Project::getAdjustedF0ForRange(int startFrame, int endFrame) const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Clamp range
    startFrame = std::max(0, startFrame);
    endFrame = std::min(endFrame, static_cast<int>(audioData.basePitch.size()));

    if (startFrame >= endFrame)
        return {};

    const int rangeSize = endFrame - startFrame;
    std::vector<float> adjustedF0(static_cast<size_t>(rangeSize), 0.0f);

    for (int i = 0; i < rangeSize; ++i)
    {
        const int globalIdx = startFrame + i;
        const float base = audioData.basePitch[static_cast<size_t>(globalIdx)];
        const float delta = (globalIdx < static_cast<int>(audioData.deltaPitch.size()))
                                ? audioData.deltaPitch[static_cast<size_t>(globalIdx)]
                                : 0.0f;
        float midi = base + delta + globalPitchOffset;
        adjustedF0[static_cast<size_t>(i)] = midiToFreq(midi);
    }

    // Apply vibrato for overlapping notes
    for (const auto& note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int overlapStart = std::max(note.getStartFrame(), startFrame);
        const int overlapEnd = std::min(note.getEndFrame(), endFrame);
        for (int frame = overlapStart; frame < overlapEnd; ++frame)
        {
            const int localIdx = frame - startFrame;
            if (frame < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[frame])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(frame - note.getStartFrame()) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(localIdx)] *= std::pow(2.0f, vib / 12.0f);
        }
    }

    return adjustedF0;
}

void Project::setLoopRange(double startSeconds, double endSeconds)
{
    if (startSeconds > endSeconds)
        std::swap(startSeconds, endSeconds);

    const double duration = audioData.getDuration();
    if (duration > 0.0)
    {
        startSeconds = juce::jlimit(0.0, duration, startSeconds);
        endSeconds = juce::jlimit(0.0, duration, endSeconds);
    }

    loopRange.startSeconds = startSeconds;
    loopRange.endSeconds = endSeconds;
    loopRange.enabled = loopRange.endSeconds > loopRange.startSeconds;
}

void Project::setLoopEnabled(bool enabled)
{
    if (enabled && loopRange.endSeconds <= loopRange.startSeconds)
        loopRange.enabled = false;
    else
        loopRange.enabled = enabled;
}

void Project::clearLoopRange()
{
    loopRange = {};
}
