#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../SVCInferenceEngine.h"
#include "../SVCModelSession.h"
#include "../Vocoder.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

/**
 * Voiced-Only Blend synthesizer.
 * Resynthesizes dirty regions using vocoder, then blends:
 *   voiced frames  -> synthesized audio
 *   unvoiced frames -> original audio (preserves breathing)
 *
 * When an SVC model is active, runs the SVC inference engine to generate
 * the mel spectrogram instead of using the original analysis mel.
 */
class IncrementalSynthesizer {
public:
  using ProgressCallback = std::function<void(const juce::String &message)>;
  using CompleteCallback = std::function<void(bool success)>;

  IncrementalSynthesizer();
  ~IncrementalSynthesizer();

  void setVocoder(Vocoder *v) { vocoder = v; }
  void setProject(Project *p) { project = p; }

  /** Set SVC engine and model for voice conversion mode. */
  void setSVCEngine(SVCInferenceEngine *engine) { svcEngine = engine; }
  void setSVCModel(SVCModelSession *model) { svcModel = model; }
  void setSVCParams(const SVCInferenceEngine::InferenceParams& params) { svcParams = params; }

  void synthesizeRegion(ProgressCallback onProgress,
                        CompleteCallback onComplete);

  void cancel();
  bool isSynthesizing() const { return isBusy.load(); }

private:
  /// Compute synthesis range: find voiced segments overlapping dirty range,
  /// expand to include complete segments + padding frames.
  std::pair<int, int> computeSynthesisRange(int dirtyStart, int dirtyEnd);

  /// Generate per-sample blend mask from voicedMask.
  /// 1.0 = use synthesized, 0.0 = use original, smooth ramps at transitions.
  std::vector<float> generateBlendMask(int startFrame, int endFrame,
                                       int hopSize);

  /// Apply synthesized audio (from vocoder or SoVITS direct) with blending.
  /// Shared by both the vocoder callback path and SoVITS direct audio path.
  void applySynthesizedAudio(
      std::vector<float> synthesizedAudio,
      std::vector<float> blendMask,
      std::vector<float> originalSegment,
      Project* capturedProject,
      int capturedStartFrame,
      int capturedEndFrame,
      int hopSize,
      uint64_t currentJobId,
      std::shared_ptr<std::atomic<bool>> capturedCancelFlag,
      CompleteCallback onComplete);

  Vocoder *vocoder = nullptr;
  Project *project = nullptr;
  SVCInferenceEngine *svcEngine = nullptr;
  SVCModelSession *svcModel = nullptr;
  SVCInferenceEngine::InferenceParams svcParams;

  std::shared_ptr<std::atomic<bool>> cancelFlag;
  std::atomic<uint64_t> jobId{0};
  std::atomic<bool> isBusy{false};

  std::thread applyThread;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IncrementalSynthesizer)
};
