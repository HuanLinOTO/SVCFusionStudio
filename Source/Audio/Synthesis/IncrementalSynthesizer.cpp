#include "IncrementalSynthesizer.h"
#include "../../Utils/Localization.h"
#include "../../Utils/AppLogger.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

IncrementalSynthesizer::IncrementalSynthesizer() = default;

IncrementalSynthesizer::~IncrementalSynthesizer() { cancel(); }

void IncrementalSynthesizer::cancel() {
  if (cancelFlag)
    cancelFlag->store(true);
}

// ---------------------------------------------------------------------------
// computeSynthesisRange: find voiced segments overlapping dirty range,
// expand to include complete segments + padding.
// ---------------------------------------------------------------------------
std::pair<int, int>
IncrementalSynthesizer::computeSynthesisRange(int dirtyStart, int dirtyEnd) {
  if (!project)
    return {dirtyStart, dirtyEnd};

  auto &voicedMask = project->getAudioData().voicedMask;
  const int totalFrames = static_cast<int>(voicedMask.size());
  if (totalFrames == 0)
    return {dirtyStart, dirtyEnd};

  dirtyStart = std::max(0, dirtyStart);
  dirtyEnd = std::min(totalFrames, dirtyEnd);

  // Give the vocoder enough temporal context to stabilize local phase when
  // doing chunked re-synthesis.
  constexpr int kPadFrames = 24;
  // Bridge short UV gaps so adjacent notes around consonants are synthesized
  // together; this avoids junction phase resets between neighboring notes.
  constexpr int kGapBridgeFrames = 16;

  auto isVoiced = [&](int idx) -> bool {
    return idx >= 0 && idx < totalFrames && static_cast<bool>(voicedMask[idx]);
  };

  // Expand backward to include neighboring voiced segments across short gaps.
  int start = dirtyStart;
  int backGap = 0;
  while (start > 0) {
    if (isVoiced(start - 1)) {
      --start;
      backGap = 0;
      continue;
    }
    if (backGap < kGapBridgeFrames) {
      --start;
      ++backGap;
      continue;
    }
    break;
  }
  start = std::max(0, start - kPadFrames);

  // Expand forward to include neighboring voiced segments across short gaps.
  int end = dirtyEnd;
  int fwdGap = 0;
  while (end < totalFrames) {
    if (isVoiced(end)) {
      ++end;
      fwdGap = 0;
      continue;
    }
    if (fwdGap < kGapBridgeFrames) {
      ++end;
      ++fwdGap;
      continue;
    }
    break;
  }
  end = std::min(totalFrames, end + kPadFrames);

  DBG("computeSynthesisRange: [" << dirtyStart << ", " << dirtyEnd
                                  << "] -> [" << start << ", " << end << "]");
  return {start, end};
}

// ---------------------------------------------------------------------------
// generateBlendMask: per-sample blend factor from voicedMask.
// 1.0 = synthesized, 0.0 = original, smooth ramps at transitions.
// ---------------------------------------------------------------------------
std::vector<float>
IncrementalSynthesizer::generateBlendMask(int startFrame, int endFrame,
                                          int hopSize) {
  auto &voicedMask = project->getAudioData().voicedMask;
  const int totalFrames = static_cast<int>(voicedMask.size());
  const int numFrames = endFrame - startFrame;
  const int numSamples = numFrames * hopSize;

  // Step 1: stability-first frame mask.
  // Default to synthesized audio in the whole region to avoid internal
  // orig/synth combing artifacts at note junctions.
  std::vector<float> frameMask(numFrames, 1.0f);

  // Keep original audio only for long unvoiced runs (e.g. clear breaths/silence),
  // not for short UV gaps between notes.
  constexpr int kKeepOriginalUnvoicedFrames = 24;
  if (numFrames > 0 && totalFrames > 0) {
    int i = 0;
    while (i < numFrames) {
      const int gf = startFrame + i;
      const bool voiced =
          gf >= 0 && gf < totalFrames && static_cast<bool>(voicedMask[gf]);
      if (voiced) {
        ++i;
        continue;
      }

      const int runStart = i;
      while (i < numFrames) {
        const int g = startFrame + i;
        const bool v =
            g >= 0 && g < totalFrames && static_cast<bool>(voicedMask[g]);
        if (v)
          break;
        ++i;
      }
      const int runEnd = i;
      const int runLen = runEnd - runStart;
      if (runLen >= kKeepOriginalUnvoicedFrames) {
        for (int k = runStart; k < runEnd; ++k)
          frameMask[k] = 0.0f;
      }
    }
  }

  // Step 2: expand to per-sample (sample-and-hold)
  std::vector<float> mask(numSamples, 0.0f);
  for (int i = 0; i < numFrames; ++i) {
    int ss = i * hopSize;
    int se = std::min(ss + hopSize, numSamples);
    for (int s = ss; s < se; ++s)
      mask[s] = frameMask[i];
  }

  // Step 3: smooth transitions with linear ramp at frame boundaries
  constexpr int kMinRampSamples = 512;
  const int kRampSamples = std::max(kMinRampSamples, hopSize * 2);
  for (int i = 0; i < numFrames - 1; ++i) {
    if (frameMask[i] == frameMask[i + 1])
      continue;
    // Transition at frame boundary
    int center = (i + 1) * hopSize;
    int rampStart = std::max(0, center - kRampSamples / 2);
    int rampEnd = std::min(numSamples, center + kRampSamples / 2);
    float fromVal = frameMask[i];
    float toVal = frameMask[i + 1];
    for (int s = rampStart; s < rampEnd; ++s) {
      float t = static_cast<float>(s - rampStart) /
                static_cast<float>(rampEnd - rampStart);
      mask[s] = fromVal + (toVal - fromVal) * t;
    }
  }

  return mask;
}

// ---------------------------------------------------------------------------
// synthesizeRegion: Voiced-Only Blend approach.
// ---------------------------------------------------------------------------
void IncrementalSynthesizer::synthesizeRegion(ProgressCallback onProgress,
                                              CompleteCallback onComplete) {
  LOG("[STRETCH-DBG] synthesizeRegion() ENTER");
  if (!project || !vocoder) {
    LOG("[STRETCH-DBG] synthesizeRegion: BAIL — no project(" + juce::String((int)(project != nullptr)) + ") or vocoder(" + juce::String((int)(vocoder != nullptr)) + ")");
    if (onComplete)
      onComplete(false);
    return;
  }

  auto &audioData = project->getAudioData();
  if (audioData.melSpectrogram.empty() || audioData.f0.empty()) {
    LOG("[STRETCH-DBG] synthesizeRegion: BAIL — mel empty(" + juce::String((int)audioData.melSpectrogram.empty()) + ") or f0 empty(" + juce::String((int)audioData.f0.empty()) + ")");
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!vocoder->isLoaded()) {
    LOG("[STRETCH-DBG] synthesizeRegion: BAIL — vocoder not loaded");
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!project->hasDirtyNotes() && !project->hasF0DirtyRange()) {
    LOG("[STRETCH-DBG] synthesizeRegion: BAIL — no dirty notes and no F0 dirty range");
    if (onComplete)
      onComplete(false);
    return;
  }

  auto [dirtyStart, dirtyEnd] = project->getDirtyFrameRange();
  LOG("[STRETCH-DBG] dirtyRange=[" + juce::String(dirtyStart) + ", " + juce::String(dirtyEnd) + "]");
  if (dirtyStart < 0 || dirtyEnd < 0) {
    LOG("[STRETCH-DBG] synthesizeRegion: BAIL — invalid dirty range");
    if (onComplete)
      onComplete(false);
    return;
  }

  // Compute synthesis range (voiced segments + padding)
  auto [startFrame, endFrame] = computeSynthesisRange(dirtyStart, dirtyEnd);
  startFrame = std::max(0, startFrame);
  endFrame =
      std::min(static_cast<int>(audioData.melSpectrogram.size()), endFrame);
  LOG("[STRETCH-DBG] synthesisRange=[" + juce::String(startFrame) + ", " + juce::String(endFrame) + "]");

  if (startFrame >= endFrame) {
    LOG("[STRETCH-DBG] synthesizeRegion: BAIL — startFrame >= endFrame");
    if (onComplete)
      onComplete(false);
    return;
  }

  // Generate blend mask before async call (voicedMask is stable here)
  int hopSize = vocoder->getHopSize();
  std::vector<float> blendMask = generateBlendMask(startFrame, endFrame, hopSize);

  // Early exit: if blend mask is all-zero, nothing to synthesize
  bool hasVoiced = std::any_of(blendMask.begin(), blendMask.end(),
                               [](float v) { return v > 0.0f; });
  LOG("[STRETCH-DBG] blendMask size=" + juce::String((int)blendMask.size()) + " hasVoiced=" + juce::String((int)hasVoiced));
  if (!hasVoiced) {
    LOG("[STRETCH-DBG] synthesizeRegion: BAIL — blend mask all-zero (no voiced frames)");
    project->clearAllDirty();
    if (onComplete)
      onComplete(true);
    return;
  }

  // Extract mel + adjusted F0
  std::vector<std::vector<float>> melRange;
  // adjustedF0Range: always includes global pitch offset (for vocoder / final output)
  std::vector<float> adjustedF0Range =
      project->getAdjustedF0ForRange(startFrame, endFrame);

  // Check if SVC model is active — if so, run SVC inference
  bool svcActive = svcEngine && svcModel && svcModel->isLoaded()
                   && svcEngine->isContentVecLoaded();
  bool isSoVITS = svcActive && (svcModel->getConfig().modelTypeIndex == 2);

  // When melFromSVC is true, audioData.melSpectrogram already contains SVC mel.
  // We can skip expensive SVC re-inference and just use the stored mel through
  // vocoder. This makes stretch/pitch-edit fast (vocoder-only, no ContentVec/Encoder).
  bool useSvcMelDirect = svcActive && !isSoVITS && audioData.melFromSVC;

  // Copy waveform segment for blending.
  // When SVC is active, blend against the *current* waveform (which already
  // contains SVC audio) to avoid timbral/pitch seams at region boundaries.
  // For non-SVC paths, use the pristine originalWaveform.
  const auto &origWaveform =
      svcActive
          ? audioData.waveform
          : (audioData.originalWaveform.getNumSamples() > 0
                 ? audioData.originalWaveform
                 : audioData.waveform);
  int startSample = startFrame * hopSize;
  int numSynthSamples = (endFrame - startFrame) * hopSize;
  int totalOrigSamples = origWaveform.getNumSamples();

  std::vector<float> originalSegment(numSynthSamples, 0.0f);
  {
    const float *origPtr = origWaveform.getReadPointer(0);
    int copyLen = std::min(numSynthSamples,
                           std::max(0, totalOrigSamples - startSample));
    if (copyLen > 0 && startSample >= 0)
      std::copy(origPtr + startSample, origPtr + startSample + copyLen,
                originalSegment.begin());
  }
  LOG("[STRETCH-DBG] svcActive=" + juce::String((int)svcActive)
      + " isSoVITS=" + juce::String((int)isSoVITS)
      + " melFromSVC=" + juce::String((int)audioData.melFromSVC)
      + " useSvcMelDirect=" + juce::String((int)useSvcMelDirect));

  // F0 for SVC: may exclude global pitch offset in post-SVC mode
  bool pitchOffsetPreSVC = project->isPitchOffsetBeforeSVC();
  std::vector<float> f0ForSVC;
  if (svcActive && !useSvcMelDirect && !isSoVITS) {
    f0ForSVC = pitchOffsetPreSVC ? adjustedF0Range
                                 : project->getAdjustedF0ForRangeNoGlobalOffset(startFrame, endFrame);
  }

  // For SoVITS incremental (stretch/pitch-edit):
  //   Re-running inferSoVITS on the original audio with updated F0 only changes
  //   the pitch, NOT the timing, because ContentVec keeps the original content
  //   positions.  Instead, we use mel analyzed from the existing SoVITS waveform
  //   (computed by centeredMelComputer in finishStretchDrag) and send it through
  //   the vocoder with the updated F0.  This correctly adjusts both timing
  //   (via mel resampling) and pitch (via F0).
  // For DDSP/Reflow: infer() returns mel, then vocoder generates audio.
  std::vector<float> svcDirectAudio; // only used for SoVITS full inference (not stretch)

  if (useSvcMelDirect)
  {
    // Fast path: SVC mel already stored — just use it through vocoder
    DBG("IncrementalSynthesizer: Using stored SVC mel (melFromSVC) — skipping inference");
    melRange.assign(
        audioData.melSpectrogram.begin() + startFrame,
        audioData.melSpectrogram.begin() + endFrame);
  }
  else if (isSoVITS)
  {
    // SoVITS stretch: use mel from SoVITS waveform analysis through vocoder.
    // This preserves SoVITS timbre while correctly time-stretching.
    LOG("[STRETCH-DBG] SoVITS: using mel+vocoder path for stretch (skip re-inference)");
    melRange.assign(
        audioData.melSpectrogram.begin() + startFrame,
        audioData.melSpectrogram.begin() + endFrame);
  }
  else if (svcActive)
  {
    DBG("IncrementalSynthesizer: SVC mode — type="
        + juce::String(svcModel->getConfig().modelTypeIndex)
        + " (mel+vocoder)");

    const auto& origWF = audioData.originalWaveform.getNumSamples() > 0
                            ? audioData.originalWaveform
                            : audioData.waveform;
    int origSR = static_cast<int>(audioData.sampleRate);
    int origStartSample = startFrame * hopSize;
    int origNumSamples = (endFrame - startFrame) * hopSize;
    int origAvail = origWF.getNumSamples();

    // Get the audio segment for SVC
    std::vector<float> audioSegment(origNumSamples, 0.f);
    if (origStartSample < origAvail)
    {
      int copyLen = std::min(origNumSamples, origAvail - origStartSample);
      const float* ptr = origWF.getReadPointer(0);
      std::copy(ptr + origStartSample, ptr + origStartSample + copyLen,
                audioSegment.begin());
    }

    // DDSP / Reflow-VAE: returns mel for vocoder
    melRange = svcEngine->infer(
        *svcModel,
        audioSegment.data(),
        origNumSamples,
        origSR,
        f0ForSVC,
        svcParams);

    if (melRange.empty())
    {
      DBG("IncrementalSynthesizer: SVC mel inference failed, falling back to original mel");
      melRange.assign(
          audioData.melSpectrogram.begin() + startFrame,
          audioData.melSpectrogram.begin() + endFrame);
    }
    else
    {
      DBG("IncrementalSynthesizer: SVC mel OK — ["
          + juce::String(melRange.size()) + "]["
          + juce::String(melRange.empty() ? 0 : melRange[0].size()) + "]");
    }
  }
  else
  {
    // No SVC model — use original analysis mel
    melRange.assign(
        audioData.melSpectrogram.begin() + startFrame,
        audioData.melSpectrogram.begin() + endFrame);
  }

  // For SoVITS direct audio path, skip vocoder entirely
  if (isSoVITS && !svcDirectAudio.empty())
  {
    if (onProgress)
      onProgress(TR("progress.synthesizing"));

    // Cancel previous job
    if (cancelFlag)
      cancelFlag->store(true);
    cancelFlag = std::make_shared<std::atomic<bool>>(false);
    uint64_t currentJobId = ++jobId;
    isBusy = true;

    int capturedStartFrame = startFrame;
    int capturedEndFrame = endFrame;
    auto capturedCancelFlag = cancelFlag;
    auto capturedProject = project;

    DBG("synthesizeRegion (SoVITS direct): frames [" << startFrame << ", " << endFrame << "]");

    // Go directly to the blend+write thread — no vocoder needed
    applySynthesizedAudio(std::move(svcDirectAudio), std::move(blendMask),
                          std::move(originalSegment), capturedProject,
                          capturedStartFrame, capturedEndFrame, hopSize,
                          currentJobId, capturedCancelFlag, onComplete);
    return;
  }

  if (melRange.empty() || adjustedF0Range.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (onProgress)
    onProgress(TR("progress.synthesizing"));

  // Cancel previous job
  if (cancelFlag)
    cancelFlag->store(true);
  cancelFlag = std::make_shared<std::atomic<bool>>(false);
  uint64_t currentJobId = ++jobId;
  isBusy = true;

  int capturedStartFrame = startFrame;
  int capturedEndFrame = endFrame;
  auto capturedCancelFlag = cancelFlag;
  auto capturedProject = project;

  DBG("synthesizeRegion: frames [" << startFrame << ", " << endFrame << "]");

  vocoder->inferAsync(
      melRange, adjustedF0Range,
      [this, capturedCancelFlag, capturedProject, capturedStartFrame,
       capturedEndFrame, hopSize, currentJobId, onComplete,
       blendMask = std::move(blendMask),
       originalSegment = std::move(originalSegment)](
          std::vector<float> synthesizedAudio) mutable {
        if (currentJobId != jobId.load())
          return;
        if (capturedCancelFlag->load()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }
        if (synthesizedAudio.empty()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }

        applySynthesizedAudio(std::move(synthesizedAudio), std::move(blendMask),
                              std::move(originalSegment), capturedProject,
                              capturedStartFrame, capturedEndFrame, hopSize,
                              currentJobId, capturedCancelFlag, onComplete);
      });
}

void IncrementalSynthesizer::applySynthesizedAudio(
    std::vector<float> synthesizedAudio,
    std::vector<float> blendMask,
    std::vector<float> originalSegment,
    Project* capturedProject,
    int capturedStartFrame,
    int capturedEndFrame,
    int hopSize,
    uint64_t currentJobId,
    std::shared_ptr<std::atomic<bool>> capturedCancelFlag,
    CompleteCallback onComplete)
{
  std::thread([this, capturedCancelFlag, capturedProject,
               capturedStartFrame, capturedEndFrame, hopSize,
               currentJobId, onComplete,
               blendMask = std::move(blendMask),
               originalSegment = std::move(originalSegment),
               synthesizedAudio = std::move(synthesizedAudio)]() mutable {
    if (currentJobId != jobId.load())
      return;
    if (capturedCancelFlag->load()) {
      isBusy = false;
      if (onComplete)
        juce::MessageManager::callAsync(
            [onComplete]() { onComplete(false); });
      return;
    }

    auto &audioData = capturedProject->getAudioData();
    int totalSamples = audioData.waveform.getNumSamples();
    int numChannels = audioData.waveform.getNumChannels();
    int startSample = capturedStartFrame * hopSize;
    int expectedSamples =
        (capturedEndFrame - capturedStartFrame) * hopSize;

    if (expectedSamples <= 0) {
      isBusy = false;
      if (onComplete)
        juce::MessageManager::callAsync(
            [onComplete]() { onComplete(false); });
      return;
    }

    // Resize synthesized audio to match expected
    synthesizedAudio.resize(static_cast<size_t>(expectedSamples), 0.0f);

    int samplesToWrite =
        std::min(expectedSamples, totalSamples - startSample);
    if (samplesToWrite <= 0) {
      isBusy = false;
      if (onComplete)
        juce::MessageManager::callAsync(
            [onComplete]() { onComplete(false); });
      return;
    }

    constexpr int kMinBoundaryBlendSamples = 128;
    constexpr int kMaxBoundaryBlendSamples = 1024;
    const int preferredBlend =
        std::max(kMinBoundaryBlendSamples, hopSize);
    const int boundaryBlendLen = std::min(
        std::min(kMaxBoundaryBlendSamples, preferredBlend),
        std::max(1, samplesToWrite / 8));

    // Build target from model/original blend once.
    std::vector<float> targetSegment(samplesToWrite, 0.0f);
    for (int i = 0; i < samplesToWrite; ++i) {
      const float b =
          (i < static_cast<int>(blendMask.size())) ? blendMask[i] : 0.0f;
      const float synth = synthesizedAudio[static_cast<size_t>(i)];
      const float orig = originalSegment[static_cast<size_t>(i)];
      targetSegment[static_cast<size_t>(i)] =
          b * synth + (1.0f - b) * orig;
    }

    // Apply per-note gain on top of the blended target.
    std::vector<float> sampleGain(static_cast<size_t>(samplesToWrite),
                                  1.0f);
    for (const auto &note : capturedProject->getNotes()) {
      if (note.isRest())
        continue;
      if (std::abs(note.getVolumeDb()) < 0.001f)
        continue;

      const int noteStart = note.getStartFrame();
      const int noteEnd = note.getEndFrame();
      const int overlapStart = std::max(capturedStartFrame, noteStart);
      const int overlapEnd = std::min(capturedEndFrame, noteEnd);
      if (overlapEnd <= overlapStart)
        continue;

      const int localStart = (overlapStart - capturedStartFrame) * hopSize;
      const int localEnd = (overlapEnd - capturedStartFrame) * hopSize;
      if (localStart >= samplesToWrite)
        continue;

      const float gain =
          juce::Decibels::decibelsToGain(note.getVolumeDb(), -60.0f);
      const int clampedStart = std::max(0, localStart);
      const int clampedEnd = std::min(samplesToWrite, localEnd);
      for (int i = clampedStart; i < clampedEnd; ++i) {
        sampleGain[static_cast<size_t>(i)] *= gain;
      }
    }
    for (int i = 0; i < samplesToWrite; ++i) {
      targetSegment[static_cast<size_t>(i)] *=
          sampleGain[static_cast<size_t>(i)];
    }

    // Stitch target using original segment as reference:
    // final = original + edgeEnv * (target - original)
    for (int ch = 0; ch < numChannels; ++ch) {
      float *dst = audioData.waveform.getWritePointer(ch);
      for (int i = 0; i < samplesToWrite; ++i) {
        float edgeEnv = 1.0f;
        if (boundaryBlendLen > 1) {
          if (i < boundaryBlendLen) {
            const float t = static_cast<float>(i) /
                            static_cast<float>(boundaryBlendLen - 1);
            edgeEnv = t * t * (3.0f - 2.0f * t);
          } else if (i >= samplesToWrite - boundaryBlendLen) {
            const int fromEnd = samplesToWrite - 1 - i;
            const float t = static_cast<float>(fromEnd) /
                            static_cast<float>(boundaryBlendLen - 1);
            edgeEnv = t * t * (3.0f - 2.0f * t);
          }
        }

        const float cur = originalSegment[static_cast<size_t>(i)];
        const float target = targetSegment[static_cast<size_t>(i)];
        dst[startSample + i] = cur + edgeEnv * (target - cur);
      }
    }

    DBG("synthesizeRegion: blended " << samplesToWrite
                                      << " samples at " << startSample);

    capturedProject->clearAllDirty();
    isBusy = false;
    if (onComplete)
      juce::MessageManager::callAsync(
          [onComplete]() { onComplete(true); });
  }).detach();
}
