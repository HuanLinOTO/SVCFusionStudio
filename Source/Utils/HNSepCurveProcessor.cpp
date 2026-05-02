#include "HNSepCurveProcessor.h"
#include "CurveResampler.h"

#include <algorithm>
#include <cmath>

namespace {
void ensureCurveSizes(AudioData &audioData, int totalFrames) {
  if (totalFrames <= 0)
    return;

  if (audioData.voicingCurve.size() != static_cast<size_t>(totalFrames)) {
    audioData.voicingCurve.assign(static_cast<size_t>(totalFrames),
                                  HNSepCurveProcessor::kDefaultVoicing);
  }
  if (audioData.breathCurve.size() != static_cast<size_t>(totalFrames)) {
    audioData.breathCurve.assign(static_cast<size_t>(totalFrames),
                                 HNSepCurveProcessor::kDefaultBreath);
  }
  if (audioData.tensionCurve.size() != static_cast<size_t>(totalFrames)) {
    audioData.tensionCurve.assign(static_cast<size_t>(totalFrames),
                                  HNSepCurveProcessor::kDefaultTension);
  }
}

std::vector<float> fitCurveToLength(const std::vector<float> &source,
                                    int targetLength, float defaultValue) {
  if (targetLength <= 0)
    return {};
  if (source.empty())
    return std::vector<float>(static_cast<size_t>(targetLength), defaultValue);
  if (static_cast<int>(source.size()) == targetLength)
    return source;
  return CurveResampler::resampleLinear(source, targetLength);
}

void writeCurveRange(std::vector<float> &dest, const std::vector<float> &source,
                     int startFrame, int endFrame, float defaultValue) {
  const int totalFrames = static_cast<int>(dest.size());
  const int clampedStart = std::max(0, startFrame);
  const int clampedEnd = std::min(totalFrames, endFrame);
  if (clampedEnd <= clampedStart)
    return;

  std::fill(dest.begin() + clampedStart, dest.begin() + clampedEnd,
            defaultValue);

  const int length = clampedEnd - clampedStart;
  const auto fitted = fitCurveToLength(source, length, defaultValue);
  for (int index = 0; index < length; ++index) {
    dest[static_cast<size_t>(clampedStart + index)] =
        fitted[static_cast<size_t>(index)];
  }
}

bool curveDiffersFrom(const std::vector<float> &curve, int startFrame,
                      int endFrame, float defaultValue) {
  const int totalFrames = static_cast<int>(curve.size());
  const int clampedStart = std::max(0, startFrame);
  const int clampedEnd = std::min(totalFrames, endFrame);
  for (int frame = clampedStart; frame < clampedEnd; ++frame) {
    if (std::abs(curve[static_cast<size_t>(frame)] - defaultValue) > 0.001f)
      return true;
  }
  return false;
}
} // namespace

namespace HNSepCurveProcessor {
void initializeCurves(Project &project) {
  auto &audioData = project.getAudioData();
  const int totalFrames = audioData.getNumFrames();
  ensureCurveSizes(audioData, totalFrames);

  for (auto &note : project.getNotes()) {
    if (note.isRest())
      continue;

    const int noteLength = note.getDurationFrames();
    if (noteLength <= 0)
      continue;

    const int startFrame = note.getStartFrame();
    const int endFrame = std::min(startFrame + noteLength, totalFrames);
    const int sliceLength = std::max(0, endFrame - startFrame);

    if (!note.hasVoicingCurve()) {
      if (sliceLength > 0 && startFrame >= 0 &&
          endFrame <= static_cast<int>(audioData.voicingCurve.size())) {
        note.setVoicingCurve(std::vector<float>(
            audioData.voicingCurve.begin() + startFrame,
            audioData.voicingCurve.begin() + endFrame));
      } else {
        note.setVoicingCurve(std::vector<float>(static_cast<size_t>(noteLength),
                                                kDefaultVoicing));
      }
    }

    if (!note.hasBreathCurve()) {
      if (sliceLength > 0 && startFrame >= 0 &&
          endFrame <= static_cast<int>(audioData.breathCurve.size())) {
        note.setBreathCurve(std::vector<float>(
            audioData.breathCurve.begin() + startFrame,
            audioData.breathCurve.begin() + endFrame));
      } else {
        note.setBreathCurve(std::vector<float>(static_cast<size_t>(noteLength),
                                               kDefaultBreath));
      }
    }

    if (!note.hasTensionCurve()) {
      if (sliceLength > 0 && startFrame >= 0 &&
          endFrame <= static_cast<int>(audioData.tensionCurve.size())) {
        note.setTensionCurve(std::vector<float>(
            audioData.tensionCurve.begin() + startFrame,
            audioData.tensionCurve.begin() + endFrame));
      } else {
        note.setTensionCurve(std::vector<float>(static_cast<size_t>(noteLength),
                                                kDefaultTension));
      }
    }
  }

  rebuildCurvesFromNotes(project);
}

void rebuildCurvesFromNotes(Project &project) {
  auto &audioData = project.getAudioData();
  const int totalFrames = audioData.getNumFrames();
  ensureCurveSizes(audioData, totalFrames);

  std::fill(audioData.voicingCurve.begin(), audioData.voicingCurve.end(),
            kDefaultVoicing);
  std::fill(audioData.breathCurve.begin(), audioData.breathCurve.end(),
            kDefaultBreath);
  std::fill(audioData.tensionCurve.begin(), audioData.tensionCurve.end(),
            kDefaultTension);

  for (const auto &note : project.getNotes()) {
    if (note.isRest())
      continue;

    writeCurveRange(audioData.voicingCurve, note.getVoicingCurve(),
                    note.getStartFrame(), note.getEndFrame(), kDefaultVoicing);
    writeCurveRange(audioData.breathCurve, note.getBreathCurve(),
                    note.getStartFrame(), note.getEndFrame(), kDefaultBreath);
    writeCurveRange(audioData.tensionCurve, note.getTensionCurve(),
                    note.getStartFrame(), note.getEndFrame(), kDefaultTension);
  }
}

void rebuildCurvesForRange(Project &project, int startFrame, int endFrame) {
  auto &audioData = project.getAudioData();
  const int totalFrames = audioData.getNumFrames();
  ensureCurveSizes(audioData, totalFrames);

  const int clampedStart = std::max(0, startFrame);
  const int clampedEnd = std::min(totalFrames, endFrame);
  if (clampedEnd <= clampedStart)
    return;

  std::fill(audioData.voicingCurve.begin() + clampedStart,
            audioData.voicingCurve.begin() + clampedEnd, kDefaultVoicing);
  std::fill(audioData.breathCurve.begin() + clampedStart,
            audioData.breathCurve.begin() + clampedEnd, kDefaultBreath);
  std::fill(audioData.tensionCurve.begin() + clampedStart,
            audioData.tensionCurve.begin() + clampedEnd, kDefaultTension);

  for (const auto &note : project.getNotes()) {
    if (note.isRest())
      continue;

    const int overlapStart = std::max(clampedStart, note.getStartFrame());
    const int overlapEnd = std::min(clampedEnd, note.getEndFrame());
    if (overlapEnd <= overlapStart)
      continue;

    const int noteLength = note.getDurationFrames();
    if (noteLength <= 0)
      continue;

    const auto voicing =
        fitCurveToLength(note.getVoicingCurve(), noteLength, kDefaultVoicing);
    const auto breath =
        fitCurveToLength(note.getBreathCurve(), noteLength, kDefaultBreath);
    const auto tension =
        fitCurveToLength(note.getTensionCurve(), noteLength, kDefaultTension);

    for (int frame = overlapStart; frame < overlapEnd; ++frame) {
      const int localFrame = frame - note.getStartFrame();
      audioData.voicingCurve[static_cast<size_t>(frame)] =
          voicing[static_cast<size_t>(localFrame)];
      audioData.breathCurve[static_cast<size_t>(frame)] =
          breath[static_cast<size_t>(localFrame)];
      audioData.tensionCurve[static_cast<size_t>(frame)] =
          tension[static_cast<size_t>(localFrame)];
    }
  }
}

void extractNoteCurvesFromMaster(Project &project) {
  auto &audioData = project.getAudioData();
  const int totalFrames = audioData.getNumFrames();
  ensureCurveSizes(audioData, totalFrames);

  for (auto &note : project.getNotes()) {
    if (note.isRest())
      continue;

    const int startFrame = std::max(0, note.getStartFrame());
    const int endFrame = std::min(totalFrames, note.getEndFrame());
    const int length = std::max(0, endFrame - startFrame);
    if (length <= 0)
      continue;

    note.setVoicingCurve(std::vector<float>(audioData.voicingCurve.begin() +
                                                startFrame,
                                            audioData.voicingCurve.begin() +
                                                endFrame));
    note.setBreathCurve(std::vector<float>(audioData.breathCurve.begin() +
                                               startFrame,
                                           audioData.breathCurve.begin() +
                                               endFrame));
    note.setTensionCurve(std::vector<float>(audioData.tensionCurve.begin() +
                                                startFrame,
                                            audioData.tensionCurve.begin() +
                                                endFrame));
  }
}

bool hasActiveEdits(const Project &project, int startFrame, int endFrame) {
  const auto &audioData = project.getAudioData();
  return curveDiffersFrom(audioData.voicingCurve, startFrame, endFrame,
                          kDefaultVoicing) ||
         curveDiffersFrom(audioData.breathCurve, startFrame, endFrame,
                          kDefaultBreath) ||
         curveDiffersFrom(audioData.tensionCurve, startFrame, endFrame,
                          kDefaultTension);
}
} // namespace HNSepCurveProcessor