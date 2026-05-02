#include "NoteSplitter.h"
#include "../../Utils/Constants.h"
#include "../../Utils/CurveResampler.h"
#include <algorithm>
#include <cmath>

Note* NoteSplitter::findNoteAt(float x, float y) {
    if (!project || !coordMapper)
        return nullptr;

    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();

    for (auto& note : project->getNotes()) {
        if (note.isRest())
            continue;

        float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
        float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
        float noteY = coordMapper->midiToY(note.getAdjustedMidiNote());
        float noteH = pixelsPerSemitone;

        if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH) {
            return &note;
        }
    }

    return nullptr;
}

bool NoteSplitter::splitNoteAtFrame(Note* note, int splitFrame) {
    if (!note || !project)
        return false;

    int startFrame = note->getStartFrame();
    int endFrame = note->getEndFrame();
    int srcStartFrame = note->getSrcStartFrame();
    int srcEndFrame = note->getSrcEndFrame();
    // Ensure split point is within note bounds (with margin)
    if (splitFrame <= startFrame + 5 || splitFrame >= endFrame - 5)
        return false;

    // Store original note data for undo
    Note originalNote = *note;

    const int destLength = std::max(1, endFrame - startFrame);
    const int srcLength = std::max(1, srcEndFrame - srcStartFrame);
    const int splitSrcFrame = srcStartFrame +
        static_cast<int>(std::llround(static_cast<double>(splitFrame - startFrame) *
                                      static_cast<double>(srcLength) /
                                      static_cast<double>(destLength)));
    const int clampedSplitSrcFrame = std::max(srcStartFrame,
        std::min(splitSrcFrame, srcEndFrame));

    // Ensure clip waveform exists before splitting
    if (!note->hasClipWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.waveform.getNumSamples() > 0) {
            int startSample = startFrame * HOP_SIZE;
            int endSample = endFrame * HOP_SIZE;
            startSample = std::max(0, std::min(startSample, audioData.waveform.getNumSamples()));
            endSample = std::max(startSample, std::min(endSample, audioData.waveform.getNumSamples()));
            std::vector<float> clip;
            clip.reserve(static_cast<size_t>(endSample - startSample));
            const float* src = audioData.waveform.getReadPointer(0);
            for (int i = startSample; i < endSample; ++i)
                clip.push_back(src[i]);
            note->setClipWaveform(std::move(clip));
        }
    }

    // Ensure clip mel exists before splitting
    if (!note->hasClipMel()) {
        auto& audioData = project->getAudioData();
        if (!audioData.melSpectrogram.empty()) {
            int melSize = static_cast<int>(audioData.melSpectrogram.size());
            int melStart = std::max(0, std::min(startFrame, melSize));
            int melEnd = std::max(melStart, std::min(endFrame, melSize));
            if (melEnd > melStart) {
                std::vector<std::vector<float>> melClip(
                    audioData.melSpectrogram.begin() + melStart,
                    audioData.melSpectrogram.begin() + melEnd);
                note->setClipMel(std::move(melClip));
            }
        }
    }

    auto& audioData = project->getAudioData();

    if (!note->hasClipHarmonicWaveform() && audioData.harmonicWaveform.getNumSamples() > 0) {
        int startSample = note->getSrcStartFrame() * HOP_SIZE;
        int endSample = note->getSrcEndFrame() * HOP_SIZE;
        startSample = std::max(0, std::min(startSample, audioData.harmonicWaveform.getNumSamples()));
        endSample = std::max(startSample, std::min(endSample, audioData.harmonicWaveform.getNumSamples()));
        std::vector<float> clip;
        clip.reserve(static_cast<size_t>(endSample - startSample));
        const float* src = audioData.harmonicWaveform.getReadPointer(0);
        for (int i = startSample; i < endSample; ++i)
            clip.push_back(src[i]);
        note->setClipHarmonicWaveform(std::move(clip));
    }

    if (!note->hasClipNoiseWaveform() && audioData.noiseWaveform.getNumSamples() > 0) {
        int startSample = note->getSrcStartFrame() * HOP_SIZE;
        int endSample = note->getSrcEndFrame() * HOP_SIZE;
        startSample = std::max(0, std::min(startSample, audioData.noiseWaveform.getNumSamples()));
        endSample = std::max(startSample, std::min(endSample, audioData.noiseWaveform.getNumSamples()));
        std::vector<float> clip;
        clip.reserve(static_cast<size_t>(endSample - startSample));
        const float* src = audioData.noiseWaveform.getReadPointer(0);
        for (int i = startSample; i < endSample; ++i)
            clip.push_back(src[i]);
        note->setClipNoiseWaveform(std::move(clip));
    }

    // Create the second note (right part)
    Note secondNote;
    secondNote.setStartFrame(splitFrame);
    secondNote.setEndFrame(endFrame);
    secondNote.setSrcStartFrame(clampedSplitSrcFrame);
    secondNote.setSrcEndFrame(srcEndFrame);
    secondNote.setMidiNote(note->getMidiNote());
    secondNote.setLyric(note->getLyric());
    secondNote.setPitchOffset(0.0f);

    // Split clip waveform if available
    if (note->hasClipWaveform()) {
        const auto& clip = note->getClipWaveform();
        int splitOffset = (clampedSplitSrcFrame - srcStartFrame) * HOP_SIZE;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(clip.size())));
        std::vector<float> leftClip(clip.begin(), clip.begin() + splitOffset);
        std::vector<float> rightClip(clip.begin() + splitOffset, clip.end());
        note->setClipWaveform(std::move(leftClip));
        secondNote.setClipWaveform(std::move(rightClip));
    }

    // Split clip mel if available
    if (note->hasClipMel()) {
        const auto& mel = note->getClipMel();
        int splitOffset = clampedSplitSrcFrame - srcStartFrame;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(mel.size())));
        std::vector<std::vector<float>> leftMel(mel.begin(), mel.begin() + splitOffset);
        std::vector<std::vector<float>> rightMel(mel.begin() + splitOffset, mel.end());
        note->setClipMel(std::move(leftMel));
        secondNote.setClipMel(std::move(rightMel));
    }

    if (note->hasClipHarmonicWaveform()) {
        const auto& clip = note->getClipHarmonicWaveform();
        int splitOffset = (clampedSplitSrcFrame - srcStartFrame) * HOP_SIZE;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(clip.size())));
        std::vector<float> leftClip(clip.begin(), clip.begin() + splitOffset);
        std::vector<float> rightClip(clip.begin() + splitOffset, clip.end());
        note->setClipHarmonicWaveform(std::move(leftClip));
        secondNote.setClipHarmonicWaveform(std::move(rightClip));
    }

    if (note->hasClipNoiseWaveform()) {
        const auto& clip = note->getClipNoiseWaveform();
        int splitOffset = (clampedSplitSrcFrame - srcStartFrame) * HOP_SIZE;
        splitOffset = std::max(0, std::min(splitOffset, static_cast<int>(clip.size())));
        std::vector<float> leftClip(clip.begin(), clip.begin() + splitOffset);
        std::vector<float> rightClip(clip.begin() + splitOffset, clip.end());
        note->setClipNoiseWaveform(std::move(leftClip));
        secondNote.setClipNoiseWaveform(std::move(rightClip));
    }

    const int originalLength = std::max(1, endFrame - startFrame);
    const int leftLength = std::max(1, splitFrame - startFrame);
    const int rightLength = std::max(1, endFrame - splitFrame);

    if (note->hasVoicingCurve()) {
        auto fitted = CurveResampler::resampleLinear(note->getVoicingCurve(), originalLength);
        std::vector<float> leftCurve(fitted.begin(), fitted.begin() + leftLength);
        std::vector<float> rightCurve(fitted.begin() + leftLength, fitted.end());
        note->setVoicingCurve(std::move(leftCurve));
        secondNote.setVoicingCurve(std::move(rightCurve));
    }

    if (note->hasBreathCurve()) {
        auto fitted = CurveResampler::resampleLinear(note->getBreathCurve(), originalLength);
        std::vector<float> leftCurve(fitted.begin(), fitted.begin() + leftLength);
        std::vector<float> rightCurve(fitted.begin() + leftLength, fitted.end());
        note->setBreathCurve(std::move(leftCurve));
        secondNote.setBreathCurve(std::move(rightCurve));
    }

    if (note->hasTensionCurve()) {
        auto fitted = CurveResampler::resampleLinear(note->getTensionCurve(), originalLength);
        std::vector<float> leftCurve(fitted.begin(), fitted.begin() + leftLength);
        std::vector<float> rightCurve(fitted.begin() + leftLength, fitted.end());
        note->setTensionCurve(std::move(leftCurve));
        secondNote.setTensionCurve(std::move(rightCurve));
    }

    // Modify the first note (left part)
    note->setEndFrame(splitFrame);
    note->setSrcEndFrame(clampedSplitSrcFrame);

    // Save first note BEFORE addNote (addNote may invalidate note pointer due to vector reallocation)
    Note firstNote = *note;

    // Add the second note to project
    project->addNote(secondNote);

    // Create undo action - don't pass callback to avoid lifetime issues
    // UI refresh is handled by UndoManager's onUndoRedo callback
    if (undoManager) {
        auto action = std::make_unique<NoteSplitAction>(
            project, originalNote, firstNote, secondNote, nullptr);
        undoManager->addAction(std::move(action));
    }

    if (onNoteSplit)
        onNoteSplit();

    return true;
}

bool NoteSplitter::splitNoteAtX(Note* note, float x) {
    if (!note || !coordMapper)
        return false;

    // Convert X coordinate to frame
    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    double time = x / pixelsPerSecond;
    int frame = static_cast<int>(time * SAMPLE_RATE / HOP_SIZE);

    return splitNoteAtFrame(note, frame);
}
