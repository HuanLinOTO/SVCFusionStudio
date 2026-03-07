#include "BasePitchPreview.h"
#include "BasePitchCurve.h"
#include "Constants.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct PreviewNote {
  int startFrame = 0;
  int endFrame = 0;
  float midiNote = 0.0f;
  bool selected = false;
};

struct PreviewTiming {
  double startSec = 0.0;
  double endSec = 0.0;
};

int clampMsIndex(int idx, int minIdx, int maxIdx) {
  if (idx < minIdx)
    return minIdx;
  if (idx > maxIdx)
    return maxIdx;
  return idx;
}
} // namespace

BasePitchPreviewRange computeBasePitchPreviewRange(
    const std::vector<Note> &notes, int totalFrames,
    const std::function<bool(const Note &)> &isSelected) {
  BasePitchPreviewRange result;
  if (notes.empty() || totalFrames <= 0)
    return result;

  std::vector<PreviewNote> segments;
  segments.reserve(notes.size());
  for (const auto &note : notes) {
    if (note.isRest())
      continue;
    segments.push_back(
        {note.getStartFrame(), note.getEndFrame(), note.getMidiNote(),
         isSelected ? isSelected(note) : false});
  }

  if (segments.empty())
    return result;

  std::sort(segments.begin(), segments.end(),
            [](const PreviewNote &a, const PreviewNote &b) {
              if (a.startFrame != b.startFrame)
                return a.startFrame < b.startFrame;
              return a.endFrame < b.endFrame;
            });

  int lastEndFrame = 0;
  for (const auto &seg : segments)
    lastEndFrame = std::max(lastEndFrame, seg.endFrame);

  const double msPerFrame =
      1000.0 * static_cast<double>(HOP_SIZE) / static_cast<double>(SAMPLE_RATE);
  const double lastNoteEndSec = lastEndFrame * msPerFrame / 1000.0;
  const int totalMs = static_cast<int>(
      std::round(1000.0 * (lastNoteEndSec + BasePitchCurve::smoothWindowSec()))) +
                      1;

  if (totalMs <= 1)
    return result;

  const int noteCount = static_cast<int>(segments.size());
  std::vector<PreviewTiming> timings;
  timings.reserve(noteCount);
  for (const auto &seg : segments) {
    timings.push_back({seg.startFrame * msPerFrame / 1000.0,
                       seg.endFrame * msPerFrame / 1000.0});
  }

  std::vector<double> midpoints;
  midpoints.reserve(std::max(0, noteCount - 1));
  for (int i = 0; i + 1 < noteCount; ++i) {
    midpoints.push_back(0.5 * (timings[i].endSec + timings[i + 1].startSec));
  }

  int minSelectedMs = std::numeric_limits<int>::max();
  int maxSelectedMs = std::numeric_limits<int>::min();
  const double endPaddingSec =
      lastNoteEndSec + BasePitchCurve::smoothWindowSec();
  for (int i = 0; i < noteCount; ++i) {
    if (!segments[i].selected)
      continue;
    const double regionStart =
        (i == 0) ? 0.0 : midpoints[static_cast<size_t>(i - 1)];
    const double regionEnd =
        (i == noteCount - 1) ? endPaddingSec : midpoints[static_cast<size_t>(i)];
    int regionStartMs = static_cast<int>(std::floor(regionStart * 1000.0));
    int regionEndMs = static_cast<int>(std::ceil(regionEnd * 1000.0));
    regionStartMs = std::clamp(regionStartMs, 0, totalMs - 1);
    regionEndMs = std::clamp(regionEndMs, 0, totalMs - 1);
    minSelectedMs = std::min(minSelectedMs, regionStartMs);
    maxSelectedMs = std::max(maxSelectedMs, regionEndMs);
  }

  if (minSelectedMs == std::numeric_limits<int>::max() ||
      maxSelectedMs == std::numeric_limits<int>::min()) {
    return result;
  }

  const int kernelSize = BasePitchCurve::kernelSize();
  const int halfKernel = kernelSize / 2;

  const int bufferStartMs = std::max(0, minSelectedMs - kernelSize);
  const int bufferEndMs = std::min(totalMs - 1, maxSelectedMs + kernelSize);
  const int affectStartMs = std::max(0, minSelectedMs - halfKernel);
  const int affectEndMs = std::min(totalMs - 1, maxSelectedMs + halfKernel);

  if (bufferEndMs <= bufferStartMs || affectEndMs < affectStartMs)
    return result;

  const int bufferLen = bufferEndMs - bufferStartMs + 1;
  std::vector<float> initValues(static_cast<size_t>(bufferLen), 0.0f);

  double startTime = 0.001 * static_cast<double>(bufferStartMs);
  int noteIndex = 0;
  while (noteIndex < static_cast<int>(midpoints.size()) &&
         startTime > midpoints[static_cast<size_t>(noteIndex)]) {
    ++noteIndex;
  }

  for (int ms = bufferStartMs; ms <= bufferEndMs; ++ms) {
    const double time = 0.001 * static_cast<double>(ms);
    while (noteIndex < static_cast<int>(midpoints.size()) &&
           time > midpoints[static_cast<size_t>(noteIndex)]) {
      ++noteIndex;
    }
    const bool selected =
        (noteIndex >= 0 && noteIndex < noteCount) ? segments[noteIndex].selected
                                                  : false;
    initValues[static_cast<size_t>(ms - bufferStartMs)] =
        selected ? 1.0f : 0.0f;
  }

  const auto &kernel = BasePitchCurve::getCosineKernel();
  const int affectLen = affectEndMs - affectStartMs + 1;
  std::vector<float> smoothedMs(static_cast<size_t>(affectLen), 0.0f);

  for (int ms = affectStartMs; ms <= affectEndMs; ++ms) {
    double sum = 0.0;
    for (int j = 0; j < kernelSize; ++j) {
      int srcIdx = ms - halfKernel + j;
      srcIdx = clampMsIndex(srcIdx, 0, totalMs - 1);
      srcIdx = clampMsIndex(srcIdx, bufferStartMs, bufferEndMs);
      const float v = initValues[static_cast<size_t>(srcIdx - bufferStartMs)];
      sum += v * kernel[static_cast<size_t>(j)];
    }
    smoothedMs[static_cast<size_t>(ms - affectStartMs)] =
        static_cast<float>(sum);
  }

  const int frameStart =
      std::max(0, static_cast<int>(std::floor(affectStartMs / msPerFrame)));
  const int frameEnd = std::min(
      totalFrames,
      static_cast<int>(std::ceil((affectEndMs + 1.0) / msPerFrame)) + 1);

  if (frameEnd <= frameStart)
    return result;

  result.startFrame = frameStart;
  result.endFrame = frameEnd;
  result.weights.resize(static_cast<size_t>(frameEnd - frameStart), 0.0f);

  auto sampleSmoothed = [&](int msIdx) -> float {
    if (msIdx < affectStartMs || msIdx > affectEndMs)
      return 0.0f;
    return smoothedMs[static_cast<size_t>(msIdx - affectStartMs)];
  };

  for (int frame = frameStart; frame < frameEnd; ++frame) {
    double ms = frame * msPerFrame;
    int msIdx = static_cast<int>(ms);
    double frac = ms - msIdx;
    float v0 = sampleSmoothed(msIdx);
    float v1 = sampleSmoothed(msIdx + 1);
    float value = static_cast<float>(v0 * (1.0 - frac) + v1 * frac);
    result.weights[static_cast<size_t>(frame - frameStart)] = value;
  }

  return result;
}
