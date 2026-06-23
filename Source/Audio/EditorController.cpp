#include "EditorController.h"
#include "../Utils/AudioSlicer.h"
#include "../Utils/SHA256Utils.h"
#include "../Utils/Constants.h"
#include "../Utils/F0Smoother.h"

#ifdef _WIN32
#include <eh.h>
static void logSehException(unsigned int code, EXCEPTION_POINTERS *ep) {
  LOG("SEH EXCEPTION code=0x" + juce::String::toHexString(static_cast<int>(code)) +
      " addr=0x" + juce::String::toHexString(
                       reinterpret_cast<intptr_t>(ep->ExceptionRecord->ExceptionAddress)));
}
struct SehTranslatorGuard {
  _se_translator_function oldFn;
  SehTranslatorGuard() { oldFn = _set_se_translator(logSehException); }
  ~SehTranslatorGuard() { _set_se_translator(oldFn); }
};
#endif
#include "../Utils/HNSepCurveProcessor.h"
#include "../Utils/Localization.h"
#include "../Utils/MelSpectrogram.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/PlatformPaths.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <future>

namespace {
bool shouldUnloadHNSepAfterUse() {
  return juce::SystemStats::getEnvironmentVariable(
             "SVCFUSION_HNSEP_UNLOAD_AFTER_USE", "1") == "1";
}

void maybeUnloadHNSepAfterUse(HNSepModel *model) {
  if (model != nullptr && shouldUnloadHNSepAfterUse())
    model->unload();
}

struct AtomicFlagGuard {
  explicit AtomicFlagGuard(std::atomic<bool> &target) : flag(target) {
    flag = true;
  }

  ~AtomicFlagGuard() { flag = false; }

  std::atomic<bool> &flag;
};
} // namespace

EditorController::EditorController(bool enableAudioDevice) {
  LOG("EditorController: constructor start (multi-track)");
  if (enableAudioDevice)
    audioEngine = std::make_unique<AudioEngine>();
  LOG("EditorController: audioEngine created=" +
      juce::String(audioEngine != nullptr ? "true" : "false"));

  fcpePitchDetector = std::make_unique<FCPEPitchDetector>();
  LOG("EditorController: fcpePitchDetector created");
  rmvpePitchDetector = std::make_unique<RMVPEPitchDetector>();
  LOG("EditorController: rmvpePitchDetector created");
  gameDetector = std::make_unique<GAMEDetector>();
  LOG("EditorController: gameDetector created");
  hnsepModel = std::make_unique<HNSepModel>();
  LOG("EditorController: hnsepModel created");
  audioAnalyzer = std::make_unique<AudioAnalyzer>();
  LOG("EditorController: audioAnalyzer created");
  incrementalSynth = std::make_unique<IncrementalSynthesizer>();
  LOG("EditorController: incrementalSynth created");
  playbackController = std::make_unique<PlaybackController>();
  LOG("EditorController: playbackController created");
  svcEngine = std::make_unique<SVCInferenceEngine>();
  LOG("EditorController: svcEngine created");
  svcModel = std::make_unique<SVCModelSession>();
  LOG("EditorController: svcModel created");

  fcpeModelPath = PlatformPaths::getModelFile("fcpe.onnx");
  LOG("EditorController: fcpeModelPath=" + fcpeModelPath.getFullPathName());
  melFilterbankPath = PlatformPaths::getModelFile("mel_filterbank.bin");
  LOG("EditorController: melFilterbankPath=" + melFilterbankPath.getFullPathName());
  centTablePath = PlatformPaths::getModelFile("cent_table.bin");
  LOG("EditorController: centTablePath=" + centTablePath.getFullPathName());
  rmvpeModelPath = PlatformPaths::getModelFile("rmvpe.onnx");
  LOG("EditorController: rmvpeModelPath=" + rmvpeModelPath.getFullPathName());
  gameModelDir = PlatformPaths::getModelSubDir("GAME", "encoder.onnx");
  LOG("EditorController: gameModelDir=" + gameModelDir.getFullPathName());
  hnsepModelDir = PlatformPaths::getModelSubDir("hnsep", "hnsep_VR.onnx");
  LOG("EditorController: hnsepModelDir=" + hnsepModelDir.getFullPathName());

  audioAnalyzer->setFCPEDetector(fcpePitchDetector.get());
  audioAnalyzer->setRMVPEDetector(rmvpePitchDetector.get());
  audioAnalyzer->setGAMEDetector(gameDetector.get());
  audioAnalyzer->setPitchDetectorType(pitchDetectorType);

  if (audioEngine)
    playbackController->setAudioEngine(audioEngine.get());
  LOG("EditorController: constructor complete");
}

EditorController::~EditorController() {
  if (modelReloadThread.joinable())
    modelReloadThread.join();
  cancelLoadingFlag = true;
  if (loaderThread.joinable())
    loaderThread.join();
  if (loaderJoinerThread.joinable())
    loaderJoinerThread.join();
  cancelRenderFlag = true;
  if (renderThread.joinable())
    renderThread.join();
  if (incrementalSynth)
    incrementalSynth->cancel();
  if (incrementalSynthThread.joinable())
    incrementalSynthThread.join();
  ++hnsepPrewarmGeneration;
  if (hnsepPrewarmThread.joinable())
    hnsepPrewarmThread.join();
  cancelSVCFlag = true;
  if (svcConversionThread.joinable())
    svcConversionThread.join();
  for (auto& t : svcOldThreads)
    if (t.joinable()) t.join();
}

Project* EditorController::getProject() const {
  if (activeTrackIndex >= 0 && activeTrackIndex < static_cast<int>(tracks.size()))
    return tracks[static_cast<size_t>(activeTrackIndex)]->project.get();
  return nullptr;
}

Track* EditorController::getActiveTrack() const {
  if (activeTrackIndex >= 0 && activeTrackIndex < static_cast<int>(tracks.size()))
    return tracks[static_cast<size_t>(activeTrackIndex)].get();
  return nullptr;
}

Track* EditorController::getTrack(int index) const {
  if (index >= 0 && index < static_cast<int>(tracks.size()))
    return tracks[static_cast<size_t>(index)].get();
  return nullptr;
}

void EditorController::setActiveTrack(int index) {
  if (index >= 0 && index < static_cast<int>(tracks.size())) {
    activeTrackIndex = index;
    auto* track = tracks[static_cast<size_t>(index)].get();
    if (track && track->project) {
      ++hnsepPrewarmGeneration;
      if (hnsepPrewarmThread.joinable())
        hnsepPrewarmThread.join();
      prewarmHNSepBasesAsync(*track->project);
    }
  }
}

void EditorController::setProject(std::unique_ptr<Project> newProject) {
  // Legacy: wrap in a single vocal track
  auto track = std::make_unique<Track>();
  track->type = TrackType::Vocal;
  track->name = newProject ? newProject->getName() : "Untitled";
  track->project = std::move(newProject);
  if (!track->project)
    track->project = std::make_unique<Project>();

  ++hnsepPrewarmGeneration;
  if (hnsepPrewarmThread.joinable())
    hnsepPrewarmThread.join();

  tracks.clear();
  tracks.push_back(std::move(track));
  activeTrackIndex = 0;

  if (tracks.back()->project)
    prewarmHNSepBasesAsync(*tracks.back()->project);

  if (audioEngine) {
    std::vector<Track*> trackPtrs;
    for (auto& t : tracks)
      trackPtrs.push_back(t.get());
    audioEngine->setTracks(trackPtrs);
    audioEngine->rebuildMixedWaveform();
  }
}

void EditorController::setSessionLoopRange(double startSeconds, double endSeconds) {
  if (startSeconds > endSeconds)
    std::swap(startSeconds, endSeconds);

  double duration = 0.0;
  for (const auto& t : tracks) {
    if (t && t->project)
      duration = juce::jmax(duration, static_cast<double>(t->project->getAudioData().getDuration()));
  }
  if (duration > 0.0) {
    startSeconds = juce::jlimit(0.0, duration, startSeconds);
    endSeconds = juce::jlimit(0.0, duration, endSeconds);
  }

  sessionLoopRange.startSeconds = startSeconds;
  sessionLoopRange.endSeconds = endSeconds;
  sessionLoopRange.enabled = sessionLoopRange.endSeconds > sessionLoopRange.startSeconds;

  for (auto& t : tracks) {
    if (t && t->project)
      t->project->setLoopRange(startSeconds, endSeconds);
  }

  if (audioEngine)
    audioEngine->setLoopRange(startSeconds, endSeconds);
}

void EditorController::setSessionLoopEnabled(bool enabled) {
  sessionLoopRange.enabled = enabled;
  for (auto& t : tracks) {
    if (t && t->project)
      t->project->setLoopEnabled(enabled);
  }
  if (audioEngine)
    audioEngine->setLoopEnabled(enabled);
}

void EditorController::clearSessionLoopRange() {
  sessionLoopRange = {};
  for (auto& t : tracks) {
    if (t && t->project)
      t->project->clearLoopRange();
  }
  if (audioEngine)
    audioEngine->clearLoopRange();
}

void EditorController::refreshAudioEngine(bool preservePosition) {
  if (audioEngine) {
    std::vector<Track*> trackPtrs;
    for (auto& t : tracks)
      trackPtrs.push_back(t.get());
    audioEngine->setTracks(trackPtrs);
    audioEngine->rebuildMixedWaveform(preservePosition);
  }
}

void EditorController::removeTrack(int trackIndex) {
  if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
    return;

  tracks.erase(tracks.begin() + trackIndex);

  if (activeTrackIndex >= static_cast<int>(tracks.size()))
    activeTrackIndex = static_cast<int>(tracks.size()) - 1;

  if (audioEngine) {
    std::vector<Track*> trackPtrs;
    for (auto& t : tracks)
      trackPtrs.push_back(t.get());
    audioEngine->setTracks(trackPtrs);
    audioEngine->rebuildMixedWaveform();
  }
}

Vocoder *EditorController::ensureVocoder() {
  if (!vocoder) {
    LOG("EditorController: creating vocoder on demand");
    vocoder = std::make_unique<Vocoder>();
    vocoder->setExecutionDevice(device);
    vocoder->setExecutionDeviceId(deviceId);
    if (incrementalSynth)
      incrementalSynth->setVocoder(vocoder.get());
  }
  return vocoder.get();
}

// ═══════════════════════════════════════════════════════════════════════
// SVC Model Management
// ═══════════════════════════════════════════════════════════════════════

bool EditorController::loadSVCModel(const juce::File& sfsModelFile) {
  if (!svcEngine || !svcModel) return false;

  // Load ContentVec if not already loaded
  if (!svcEngine->isContentVecLoaded()) {
    auto contentVecPath = PlatformPaths::getModelFile("contentvec768l12.onnx");
    if (!contentVecPath.existsAsFile()) {
      LOG("EditorController: ContentVec ONNX not found at: " + contentVecPath.getFullPathName());
      return false;
    }
    LOG("EditorController: Loading ContentVec from " + contentVecPath.getFullPathName());
    if (!svcEngine->loadContentVec(contentVecPath, device, deviceId)) {
      LOG("EditorController: Failed to load ContentVec");
      return false;
    }
    LOG("EditorController: ContentVec loaded successfully");
  }

  // Load the SVC model from .sfs_model ZIP
  LOG("EditorController: Loading SVC model from " + sfsModelFile.getFullPathName());
  if (!svcModel->loadFromSfsModel(sfsModelFile, device, deviceId)) {
    LOG("EditorController: Failed to load SVC model");
    return false;
  }

  auto& cfg = svcModel->getConfig();
  LOG("EditorController: SVC model loaded -- " + cfg.modelTypeName +
      " (" + cfg.name + ") sr=" + juce::String(cfg.sampleRate) +
      " hop=" + juce::String(cfg.blockSize) +
      " spk=" + juce::String(cfg.nSpk));
  lastSVCModelPath = sfsModelFile;
  lastSVCModelWasDirectory = false;
  if (auto* p = getProject()) {
    auto &audioData = p->getAudioData();
    audioData.svcEnabled = true;
    audioData.svcVoicebankWasDirectory = false;
    audioData.svcVoicebankName = sfsModelFile.getFileNameWithoutExtension();
    audioData.svcVoicebankPath = sfsModelFile.getFullPathName();
  }
  return true;
}

bool EditorController::loadSVCModelFromDirectory(const juce::File& voicebankDir) {
  if (!svcEngine || !svcModel) return false;

  // Load ContentVec if not already loaded
  if (!svcEngine->isContentVecLoaded()) {
    auto contentVecPath = PlatformPaths::getModelFile("contentvec768l12.onnx");
    if (!contentVecPath.existsAsFile()) {
      LOG("EditorController: ContentVec ONNX not found at: " + contentVecPath.getFullPathName());
      return false;
    }
    LOG("EditorController: Loading ContentVec from " + contentVecPath.getFullPathName());
    if (!svcEngine->loadContentVec(contentVecPath, device, deviceId)) {
      LOG("EditorController: Failed to load ContentVec");
      return false;
    }
    LOG("EditorController: ContentVec loaded successfully");
  }

  // Load the SVC model from extracted directory
  LOG("EditorController: Loading SVC model from directory " + voicebankDir.getFullPathName());
  if (!svcModel->loadFromDirectory(voicebankDir, device, deviceId)) {
    LOG("EditorController: Failed to load SVC model from directory");
    return false;
  }

  auto& cfg = svcModel->getConfig();
  LOG("EditorController: SVC model loaded -- " + cfg.modelTypeName +
      " (" + cfg.name + ") sr=" + juce::String(cfg.sampleRate) +
      " hop=" + juce::String(cfg.blockSize) +
      " spk=" + juce::String(cfg.nSpk));
  lastSVCModelPath = voicebankDir;
  lastSVCModelWasDirectory = true;
  if (auto* p = getProject()) {
    auto &audioData = p->getAudioData();
    audioData.svcEnabled = true;
    audioData.svcVoicebankWasDirectory = true;
    audioData.svcVoicebankName = voicebankDir.getFileName();
    audioData.svcVoicebankPath = voicebankDir.getFullPathName();
  }
  return true;
}

void EditorController::unloadSVCModel() {
  if (svcModel) svcModel->unload();
}

bool EditorController::isSVCModelActive() const {
  return svcModel && svcModel->isLoaded();
}

bool EditorController::ensureSVCModelReady() {
  if (!svcEngine || !svcModel)
    return false;

  if (svcModel->isLoaded() && svcEngine->isContentVecLoaded())
    return true;

  if (!lastSVCModelPath.exists()) {
    LOG("EditorController: cannot restore SVC model, no previous voicebank path");
    return false;
  }

  LOG("EditorController: restoring SVC model on demand from " +
      lastSVCModelPath.getFullPathName());

  auto contentVecPath = PlatformPaths::getModelFile("contentvec768l12.onnx");
  if (!contentVecPath.existsAsFile()) {
    LOG("EditorController: ContentVec ONNX not found at: " +
        contentVecPath.getFullPathName());
    return false;
  }

  if (!svcEngine->isContentVecLoaded() &&
      !svcEngine->loadContentVec(contentVecPath, device, deviceId)) {
    LOG("EditorController: Failed to restore ContentVec");
    return false;
  }

  if (svcModel->isLoaded())
    return true;

  const bool ok = lastSVCModelWasDirectory
                      ? svcModel->loadFromDirectory(lastSVCModelPath, device,
                                                    deviceId)
                      : svcModel->loadFromSfsModel(lastSVCModelPath, device,
                                                   deviceId);
  if (!ok) {
    LOG("EditorController: Failed to restore SVC model");
    return false;
  }

  auto &cfg = svcModel->getConfig();
  LOG("EditorController: SVC model restored -- " + cfg.modelTypeName +
      " (" + cfg.name + ")");
  if (auto* p = getProject()) {
    auto &audioData = p->getAudioData();
    audioData.svcEnabled = true;
    audioData.svcVoicebankWasDirectory = lastSVCModelWasDirectory;
    audioData.svcVoicebankName = lastSVCModelPath.getFileNameWithoutExtension();
    if (lastSVCModelWasDirectory)
      audioData.svcVoicebankName = lastSVCModelPath.getFileName();
    audioData.svcVoicebankPath = lastSVCModelPath.getFullPathName();
  }
  return true;
}

void EditorController::cancelSVCConversion() {
  if (!isSVCConverting.load())
    return;

  LOG("EditorController: Cancelling SVC conversion");
  cancelSVCFlag = true;
  ++svcGeneration;          // Invalidates any pending callAsync from old thread
  isSVCConverting = false;  // Allow new conversion to start
}

void EditorController::runFullSVCConversionAsync(SVCProgressCallback onProgress,
                                                  SVCCompleteCallback onComplete) {
  auto *voc = ensureVocoder();
  auto* activeProject = getProject();
  if (!activeProject || !svcEngine || !svcModel || !voc) {
    if (onComplete) onComplete(false);
    return;
  }

  if (!ensureSVCModelReady()) {
    LOG("EditorController::runFullSVCConversion: SVC not ready");
    if (onComplete) onComplete(false);
    return;
  }

  auto& audioData = activeProject->getAudioData();
  if (audioData.waveform.getNumSamples() == 0 || audioData.f0.empty() || audioData.melSpectrogram.empty()) {
    LOG("EditorController::runFullSVCConversion: No audio data");
    if (onComplete) onComplete(false);
    return;
  }

  if (!voc->isLoaded()) {
    auto modelPath = PlatformPaths::getModelFile("pc_nsf_hifigan.onnx");
    if (modelPath.existsAsFile()) {
      if (!voc->loadModel(modelPath)) {
        LOG("EditorController: Failed to load vocoder for SVC conversion");
        if (onComplete) onComplete(false);
        return;
      }
    } else {
      LOG("EditorController: Vocoder model not found");
      if (onComplete) onComplete(false);
      return;
    }
  }

  // If a previous conversion is still running, cancel it
  if (isSVCConverting.load()) {
    LOG("EditorController: SVC conversion already in progress, cancelling");
    cancelSVCConversion();
  }

  // Move old thread to pending list (non-blocking).
  // It will be joined in the destructor.
  if (svcConversionThread.joinable())
    svcOldThreads.push_back(std::move(svcConversionThread));

  cancelSVCFlag = false;
  isSVCConverting = true;
  auto myGen = svcGeneration.load();

  // ── Copy all project data into locals so the thread never touches `project` ──
  juce::AudioBuffer<float> localOrigWaveform;
  localOrigWaveform.makeCopyOf(
      audioData.originalWaveform.getNumSamples() > 0
          ? audioData.originalWaveform
          : audioData.waveform);

  std::vector<bool> localVoicedMask = audioData.voicedMask;
  int sampleRate = audioData.sampleRate;
  int totalFrames = static_cast<int>(audioData.f0.size());
  int hopSize = voc->getHopSize();
  int totalSamples = localOrigWaveform.getNumSamples();

  auto adjustedF0 = activeProject->getAdjustedF0();
  bool pitchOffsetPreSVC = activeProject->isPitchOffsetBeforeSVC();
  auto f0ForSVC = pitchOffsetPreSVC ? adjustedF0
                                    : activeProject->getAdjustedF0NoGlobalOffset();
  f0ForSVC = HNSepCurveProcessor::applyShfcToF0(f0ForSVC,
                                                audioData.shfcCurve);
  auto& cfg = svcModel->getConfig();
  bool isSoVITS = (cfg.modelTypeIndex == 2);
  bool isRVC = (cfg.modelTypeIndex == 5);
  bool isDirectAudioSVC = isSoVITS || isRVC;
  // ── End of project data copying ──

  svcConversionThread = std::thread([this, myGen, onProgress, onComplete,
                                     localOrig = std::move(localOrigWaveform),
                                     localVoiced = std::move(localVoicedMask),
                                     adjustedF0 = std::move(adjustedF0),
                                     f0ForSVC = std::move(f0ForSVC),
                                      sampleRate, totalFrames, hopSize,
                                       totalSamples, pitchOffsetPreSVC, voc,
                                       isSoVITS, isRVC, isDirectAudioSVC]() {
    const float* origPtr = localOrig.getReadPointer(0);

    if (cancelSVCFlag.load()) {
      isSVCConverting = false;
      if (onComplete) juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
      return;
    }

    if (onProgress)
      juce::MessageManager::callAsync([onProgress]() { onProgress("Running SVC inference..."); });

    LOG("EditorController: Running full SVC conversion -- " +
        juce::String(totalFrames) + " frames, " +
        juce::String(totalSamples) + " samples" +
        " pitchOffsetMode=" + (pitchOffsetPreSVC ? "pre-SVC" : "post-SVC"));

    auto tConvStart = std::chrono::steady_clock::now();

    std::vector<float> finalAudio;

    // SVC mel storage: collected during inference for later writeback to audioData.melSpectrogram
    // This allows stretch / pitch-edit to reuse the SVC mel without re-running inference.
    // For sliced path: vector of (f0Start, segMel) pairs
    // For non-sliced path: single full-length mel
    struct SegMelEntry { int f0Start; std::vector<std::vector<float>> mel; };
    std::vector<SegMelEntry> collectedSegMels;
    std::vector<std::vector<float>> fullSvcMel;  // non-sliced path only
    bool anySegmentFailed = false;

    // ── Decide whether to slice (only for audio > 10 seconds) ──
    bool shouldSlice = totalSamples > sampleRate * 10;

    if (shouldSlice) {
      auto tSlice = std::chrono::steady_clock::now();
      auto segments = AudioSlicer::slice(origPtr, totalSamples, sampleRate, hopSize, -40.f);
      { auto now = std::chrono::steady_clock::now();
        LOG("  [Timer] Audio slicing: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tSlice).count()) + " ms -> " + juce::String(segments.size()) + " segments"); }

      if (segments.empty()) {
        LOG("EditorController: Slicer returned 0 segments, falling back to full audio");
        shouldSlice = false;
      } else {
        // ── Sliced inference loop (matches SVCFusion Python) ──
        std::vector<float> resultAudio;
        int currentLength = 0;

        for (int segIdx = 0; segIdx < static_cast<int>(segments.size()); ++segIdx) {
          if (cancelSVCFlag.load()) break;

          auto& seg = segments[segIdx];
          int segFrames = (seg.numSamples + hopSize - 1) / hopSize;
          int f0Start = seg.startFrame;
          int f0End = std::min(f0Start + segFrames, totalFrames);
          if (f0End <= f0Start) continue;
          segFrames = f0End - f0Start;

          LOG("EditorController: Segment " + juce::String(segIdx + 1) + "/" +
              juce::String(segments.size()) + " -- startFrame=" + juce::String(f0Start) +
              " frames=" + juce::String(segFrames) +
              " samples=" + juce::String(seg.numSamples));

          if (onProgress) {
            int idx = segIdx; int tot = static_cast<int>(segments.size());
            juce::MessageManager::callAsync([onProgress, idx, tot]() {
              onProgress("SVC segment " + juce::String(idx + 1) + "/" + juce::String(tot) + "...");
            });
          }

          auto tSeg = std::chrono::steady_clock::now();
          std::vector<float> segAudio;

          if (isSoVITS) {
            std::vector<float> segF0(f0ForSVC.begin() + f0Start,
                                     f0ForSVC.begin() + f0End);

            auto directAudio = svcEngine->inferSoVITS(
                *svcModel, origPtr + seg.startSample, seg.numSamples,
                sampleRate, segF0, svcParams);

            if (!directAudio.empty()) {
              MelSpectrogram melComp(sampleRate, N_FFT, hopSize, NUM_MELS,
                                     FMIN, FMAX);
              auto segMel = melComp.compute(directAudio.data(),
                                            static_cast<int>(directAudio.size()));
              std::vector<float> segF0Voc(adjustedF0.begin() + f0Start,
                                          adjustedF0.begin() + f0End);
              if (static_cast<int>(segMel.size()) > segFrames) {
                segMel.resize(static_cast<size_t>(segFrames));
              } else if (static_cast<int>(segMel.size()) < segFrames &&
                         !segMel.empty()) {
                segMel.resize(static_cast<size_t>(segFrames), segMel.back());
              }
              segAudio = voc->infer(segMel, segF0Voc);
            }
          } else if (isRVC) {
            std::vector<float> segF0(f0ForSVC.begin() + f0Start,
                                     f0ForSVC.begin() + f0End);

            auto directAudio = svcEngine->inferRVC(
                *svcModel, origPtr + seg.startSample, seg.numSamples,
                sampleRate, segF0, svcParams);

            if (!directAudio.empty()) {
              MelSpectrogram melComp(sampleRate, N_FFT, hopSize, NUM_MELS,
                                     FMIN, FMAX);
              auto segMel = melComp.compute(directAudio.data(),
                                            static_cast<int>(directAudio.size()));
              std::vector<float> segF0Voc(adjustedF0.begin() + f0Start,
                                          adjustedF0.begin() + f0End);
              if (static_cast<int>(segMel.size()) > segFrames) {
                segMel.resize(static_cast<size_t>(segFrames));
              } else if (static_cast<int>(segMel.size()) < segFrames &&
                         !segMel.empty()) {
                segMel.resize(static_cast<size_t>(segFrames), segMel.back());
              }
              segAudio = voc->infer(segMel, segF0Voc);
            }
          } else {
            // Slice F0 for SVC
            std::vector<float> segF0SVC(f0ForSVC.begin() + f0Start,
                                        f0ForSVC.begin() + f0End);

            auto segMel = svcEngine->infer(
                *svcModel, origPtr + seg.startSample, seg.numSamples,
                sampleRate, segF0SVC, svcParams);

            if (!segMel.empty() && !cancelSVCFlag.load()) {
              // Save SVC mel for later writeback to audioData.melSpectrogram
              collectedSegMels.push_back({ f0Start, segMel });

              // Slice F0 for vocoder
              std::vector<float> segF0Voc(adjustedF0.begin() + f0Start,
                                          adjustedF0.begin() + f0End);
              segAudio = voc->infer(segMel, segF0Voc);
            }
          }

          { auto now = std::chrono::steady_clock::now();
            LOG("  [Timer] Segment " + juce::String(segIdx + 1) + " total: " +
                juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tSeg).count()) + " ms" +
                " -> " + juce::String(segAudio.size()) + " samples"); }

          if (segAudio.empty()) {
            anySegmentFailed = true;
            const int fallbackStart = juce::jlimit(0, totalSamples, seg.startSample);
            const int fallbackLen = juce::jlimit(0, totalSamples - fallbackStart,
                                                seg.numSamples);
            if (fallbackLen > 0) {
              segAudio.assign(origPtr + fallbackStart,
                              origPtr + fallbackStart + fallbackLen);
              LOG("EditorController: Segment " + juce::String(segIdx + 1) +
                  " SVC inference failed; falling back to original audio [" +
                  juce::String(fallbackLen) + " samples]");
            }
          }

          if (segAudio.empty()) continue;

          // ── Reassemble using cross_fade / zero-fill (matches Python) ──
          int silentLength = seg.startSample - currentLength;
          if (silentLength >= 0) {
            // Gap: fill with zeros, then append segment
            resultAudio.resize(resultAudio.size() + silentLength, 0.f);
            resultAudio.insert(resultAudio.end(), segAudio.begin(), segAudio.end());
          } else {
            // Overlap: cross-fade
            resultAudio = AudioSlicer::crossFade(
                resultAudio, segAudio, currentLength + silentLength);
          }
          currentLength = currentLength + silentLength + static_cast<int>(segAudio.size());
        }

        if (cancelSVCFlag.load()) {
          isSVCConverting = false;
          if (onComplete) juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
          return;
        }

        finalAudio = std::move(resultAudio);
        // Pad or trim to match original length
        finalAudio.resize(totalSamples, 0.f);

        LOG("EditorController: Sliced inference complete -- " +
            juce::String(finalAudio.size()) + " samples from " +
            juce::String(segments.size()) + " segments");
      }
    }

    // ── Fallback: process entire audio at once (short audio or slicer returned 0) ──
    if (!shouldSlice) {
      if (isDirectAudioSVC) {
        auto directAudio = isSoVITS
            ? svcEngine->inferSoVITS(
                *svcModel, origPtr, totalSamples, sampleRate, f0ForSVC, svcParams)
            : svcEngine->inferRVC(
                *svcModel, origPtr, totalSamples, sampleRate, f0ForSVC, svcParams);

        if (cancelSVCFlag.load() || directAudio.empty()) {
          if (directAudio.empty()) LOG("EditorController: direct-audio SVC inference failed");
          isSVCConverting = false;
          if (onComplete) juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
          return;
        }

        LOG("EditorController: direct-audio SVC inference OK -- " + juce::String(directAudio.size()) + " samples");

        MelSpectrogram melComp(sampleRate, N_FFT, hopSize, NUM_MELS, FMIN,
                               FMAX);
        auto directMel = melComp.compute(directAudio.data(),
                                         static_cast<int>(directAudio.size()));
        if (static_cast<int>(directMel.size()) > totalFrames) {
          directMel.resize(static_cast<size_t>(totalFrames));
        } else if (static_cast<int>(directMel.size()) < totalFrames &&
                   !directMel.empty()) {
          directMel.resize(static_cast<size_t>(totalFrames), directMel.back());
        }

        finalAudio = voc->infer(directMel, adjustedF0);
        if (cancelSVCFlag.load() || finalAudio.empty()) {
          if (finalAudio.empty()) LOG("EditorController: Vocoder inference failed on direct-audio SVC mel");
          isSVCConverting = false;
          if (onComplete) juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
          return;
        }

        LOG("EditorController: Vocoder OK on direct-audio SVC mel -- " + juce::String(finalAudio.size()) + " samples");
      }
      else {
        auto svcMel = svcEngine->infer(
            *svcModel, origPtr, totalSamples, sampleRate, f0ForSVC, svcParams);

        if (cancelSVCFlag.load() || svcMel.empty()) {
          if (svcMel.empty()) LOG("EditorController: SVC mel inference failed");
          isSVCConverting = false;
          if (onComplete) juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
          return;
        }

        LOG("EditorController: SVC mel OK -- [" +
            juce::String(svcMel.size()) + "][" +
            juce::String(svcMel.empty() ? 0 : svcMel[0].size()) + "]");

        // Save SVC mel for later writeback to audioData.melSpectrogram
        fullSvcMel = svcMel;

        if (onProgress)
          juce::MessageManager::callAsync([onProgress]() { onProgress("Running vocoder..."); });

        finalAudio = voc->infer(svcMel, adjustedF0);

        if (cancelSVCFlag.load() || finalAudio.empty()) {
          if (finalAudio.empty()) LOG("EditorController: Vocoder inference failed on SVC mel");
          isSVCConverting = false;
          if (onComplete) juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
          return;
        }

        LOG("EditorController: Vocoder OK -- " + juce::String(finalAudio.size()) + " samples");
      }
    }

    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] Total SVC conversion (inference+vocoder): " +
          juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tConvStart).count()) + " ms"); }

    if (cancelSVCFlag.load()) {
      isSVCConverting = false;
      if (onComplete) juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
      return;
    }

    // Blend: use SVC audio for voiced segments, original for unvoiced
    int numSynthSamples = static_cast<int>(finalAudio.size());
    int writeLen = std::min(numSynthSamples, totalSamples);

    // Build blend mask: voiced=SVC, long-unvoiced=original
    constexpr int kKeepOriginalUnvoicedFrames = 24;
    std::vector<float> frameMask(totalFrames, 1.0f);
    {
      int i = 0;
      while (i < totalFrames) {
        bool voiced = i < static_cast<int>(localVoiced.size()) && localVoiced[i];
        if (voiced) { ++i; continue; }

        int runStart = i;
        while (i < totalFrames) {
          bool v = i < static_cast<int>(localVoiced.size()) && localVoiced[i];
          if (v) break;
          ++i;
        }
        int runLen = i - runStart;
        if (runLen >= kKeepOriginalUnvoicedFrames) {
          for (int k = runStart; k < i; ++k)
            frameMask[k] = 0.f;
        }
      }
    }

    // Expand to per-sample with smooth transitions
    std::vector<float> blendMask(writeLen, 1.0f);
    for (int f = 0; f < totalFrames; ++f) {
      int ss = f * hopSize;
      int se = std::min(ss + hopSize, writeLen);
      for (int s = ss; s < se; ++s)
        blendMask[s] = frameMask[f];
    }

    // Smooth transitions
    const int kRampSamples = std::max(512, hopSize * 2);
    for (int f = 0; f < totalFrames - 1; ++f) {
      if (frameMask[f] == frameMask[f + 1]) continue;
      int center = (f + 1) * hopSize;
      int rampStart = std::max(0, center - kRampSamples / 2);
      int rampEnd = std::min(writeLen, center + kRampSamples / 2);
      float fromVal = frameMask[f];
      float toVal = frameMask[f + 1];
      for (int s = rampStart; s < rampEnd; ++s) {
        float t = static_cast<float>(s - rampStart) / static_cast<float>(rampEnd - rampStart);
        blendMask[s] = fromVal + (toVal - fromVal) * t;
      }
    }

    // Apply blend: result = blend * SVC + (1-blend) * original
    std::vector<float> blendedAudio(writeLen, 0.f);
    for (int i = 0; i < writeLen; ++i) {
      float svc = (i < numSynthSamples) ? finalAudio[i] : 0.f;
      float orig = origPtr[i];
      blendedAudio[i] = blendMask[i] * svc + (1.f - blendMask[i]) * orig;
    }

    // For direct-audio SVC models: recompute mel from blended waveform so that subsequent
    // stretch/pitch-edit operations (which read audioData.melSpectrogram) use
    // SoVITS-derived mel through vocoder, preserving SoVITS timbre.
    // SoVITS produces audio directly (not mel), so we must analyze mel here.
    std::vector<std::vector<float>> sovitsMel;
    if (isDirectAudioSVC && !blendedAudio.empty()) {
      MelSpectrogram melComp(sampleRate, N_FFT, HOP_SIZE, NUM_MELS, FMIN, FMAX);
      sovitsMel = melComp.compute(blendedAudio.data(),
                                  static_cast<int>(blendedAudio.size()));
      LOG("EditorController: direct-audio SVC mel recomputed from blended waveform [" +
          juce::String(sovitsMel.size()) + " frames]");
    }

    std::vector<float> svcHarmonic;
    std::vector<float> svcNoise;
    bool refreshedSVCBases = false;
    bool clearStaleHNSepBases = false;
    if (!blendedAudio.empty()) {
      std::lock_guard<std::mutex> lock(hnsepBasesMutex);
      notifyBackgroundStatus("Preparing HNSep harmonic/noise bases...", true);
      if (ensureHNSepModelLoadedForAnalysis()) {
        refreshedSVCBases = hnsepModel->separate(
            blendedAudio.data(), static_cast<int>(blendedAudio.size()),
            svcHarmonic, svcNoise);
        if (refreshedSVCBases) {
          LOG("EditorController: refreshed HNSep bases from SVC result [" +
              juce::String(svcHarmonic.size()) + " samples]");
        } else {
          clearStaleHNSepBases = true;
          LOG("EditorController: failed to refresh HNSep bases from SVC result");
        }
        maybeUnloadHNSepAfterUse(hnsepModel.get());
      } else {
        clearStaleHNSepBases = true;
      }
      if (lastSVCModelPath.exists())
        ensureSVCModelReady();
      notifyBackgroundStatus({}, false);
    }

    if (onProgress)
      juce::MessageManager::callAsync([onProgress]() { onProgress("Applying SVC audio..."); });

    // Update the waveform on the message thread
    // Check generation to discard stale results (project may have been replaced)
    juce::MessageManager::callAsync([this, myGen,
                                     blendedAudio = std::move(blendedAudio),
                                      writeLen, onComplete, isDirectAudioSVC, totalFrames,
                                      collectedSegMels = std::move(collectedSegMels),
                                        fullSvcMel = std::move(fullSvcMel),
                                        anySegmentFailed,
                                        sovitsMel = std::move(sovitsMel),
                                        svcHarmonic = std::move(svcHarmonic),
                                        svcNoise = std::move(svcNoise),
                                        refreshedSVCBases,
                                        clearStaleHNSepBases]() {
      // If generation doesn't match, a newer conversion superseded us — discard
      if (svcGeneration.load() != myGen) {
        LOG("EditorController: Discarding stale SVC result (generation mismatch)");
        // Don't touch isSVCConverting — the newer conversion owns it
        if (svcEngine && svcEngine->isContentVecLoaded())
          svcEngine->unloadContentVec();
        if (onComplete) onComplete(false);
        return;
      }

      if (!getProject()) {
        isSVCConverting = false;
        if (svcEngine && svcEngine->isContentVecLoaded())
          svcEngine->unloadContentVec();
        if (onComplete) onComplete(false);
        return;
      }

      auto& audioData = getProject()->getAudioData();
      int numChannels = audioData.waveform.getNumChannels();

      for (int ch = 0; ch < numChannels; ++ch) {
        float* dst = audioData.waveform.getWritePointer(ch);
        int writeable = std::min(writeLen, audioData.waveform.getNumSamples());
        for (int i = 0; i < writeable; ++i)
          dst[i] = blendedAudio[i];
      }
      audioData.waveformFromSVC = true;
      audioData.svcRendered = true;
      audioData.hnsepCurvesTargetSVC = true;

      // ── Store SVC mel in audioData.melSpectrogram so that subsequent
      //    stretch / pitch edits use SVC mel through vocoder (no re-inference) ──
      if (isDirectAudioSVC) {
        audioData.melFromSVC = false;
        // SoVITS produces audio directly (no mel), so we recomputed mel from
        // the blended SoVITS waveform in the worker thread.  Store it so that
        // incremental synthesis (stretch / pitch-edit) keeps SoVITS timbre.
        if (!sovitsMel.empty()) {
          const int melSize = static_cast<int>(audioData.melSpectrogram.size());
          int copyLen = std::min(static_cast<int>(sovitsMel.size()), melSize);
          for (int i = 0; i < copyLen; ++i)
            audioData.melSpectrogram[i] = sovitsMel[i];
          LOG("EditorController: Stored direct-audio SVC-derived mel [" + juce::String(copyLen) + " frames]");
        }
        // melFromSVC stays false for SoVITS — the mel is analysis-derived,
        // not direct SVC output; finishStretchDrag will recompute from waveform.
      } else {
        const int melSize = static_cast<int>(audioData.melSpectrogram.size());
        audioData.melFromSVC = false;
        if (!fullSvcMel.empty()) {
          // Non-sliced path: replace entire melSpectrogram
          int copyLen = std::min(static_cast<int>(fullSvcMel.size()), melSize);
          for (int i = 0; i < copyLen; ++i)
            audioData.melSpectrogram[i] = fullSvcMel[i];
          LOG("EditorController: Stored full SVC mel [" + juce::String(copyLen) + " frames]");
          audioData.melFromSVC = true;
        } else if (!collectedSegMels.empty()) {
          // Sliced path: write each segment's mel at its frame position
          for (auto& entry : collectedSegMels) {
            int segLen = static_cast<int>(entry.mel.size());
            for (int i = 0; i < segLen && (entry.f0Start + i) < melSize; ++i)
              audioData.melSpectrogram[entry.f0Start + i] = entry.mel[i];
          }
          LOG("EditorController: Stored sliced SVC mel [" +
              juce::String(collectedSegMels.size()) + " segments]");
          audioData.melFromSVC = !anySegmentFailed;
          if (anySegmentFailed) {
            LOG("EditorController: Sliced SVC had failed segments; melFromSVC disabled to avoid stale-mel fast path");
          }
        } else if (anySegmentFailed) {
          LOG("EditorController: No sliced SVC mel was stored after segment failures; melFromSVC disabled");
        }
      }

      if (refreshedSVCBases) {
        const int totalWaveSamples = audioData.waveform.getNumSamples();
        const int harmonicCopyLen = std::min(
            totalWaveSamples, static_cast<int>(svcHarmonic.size()));
        const int noiseCopyLen = std::min(totalWaveSamples,
                                          static_cast<int>(svcNoise.size()));
        audioData.harmonicWaveform.setSize(1, totalWaveSamples,
                                           false, false, true);
        audioData.harmonicWaveform.clear();
        if (harmonicCopyLen > 0) {
          juce::FloatVectorOperations::copy(
              audioData.harmonicWaveform.getWritePointer(0),
              svcHarmonic.data(), harmonicCopyLen);
        }
        audioData.noiseWaveform.setSize(1, totalWaveSamples,
                                        false, false, true);
        audioData.noiseWaveform.clear();
        if (noiseCopyLen > 0) {
          juce::FloatVectorOperations::copy(
              audioData.noiseWaveform.getWritePointer(0), svcNoise.data(),
              noiseCopyLen);
        }
        audioData.hnsepBasesFromSVC = true;
      } else if (clearStaleHNSepBases) {
        audioData.harmonicWaveform.setSize(1, 0);
        audioData.noiseWaveform.setSize(1, 0);
        audioData.hnsepBasesFromSVC = false;
      }

      // Load into audio engine for playback
      if (audioEngine) {
        try {
          audioEngine->loadWaveform(audioData.waveform, audioData.sampleRate, true);
        } catch (...) {
          LOG("EditorController: EXCEPTION in loadWaveform after SVC conversion");
        }
      }

      LOG("EditorController: Full SVC conversion complete -- " + juce::String(writeLen) + " samples replaced");
      isSVCConverting = false;
      if (svcEngine && svcEngine->isContentVecLoaded())
        svcEngine->unloadContentVec();
      if (onComplete) onComplete(true);
    });
  });
}

GPUProvider EditorController::getProviderFromDevice(
    const juce::String &deviceName) const {
  if (deviceName == "CUDA")
    return GPUProvider::CUDA;
  if (deviceName == "DirectML")
    return GPUProvider::DirectML;
  if (deviceName == "CoreML")
    return GPUProvider::CoreML;
  if (deviceName.isNotEmpty() && deviceName != "CPU")
    LOG("Unsupported pitch detector device: " + deviceName + ", using CPU");
  return GPUProvider::CPU;
}

void EditorController::reloadInferenceModels(bool async) {
  auto provider = getProviderFromDevice(device);
  int resolvedDeviceId = deviceId < 0 ? 0 : deviceId;

  auto fcpePath = fcpeModelPath;
  auto melPath = melFilterbankPath;
  auto centPath = centTablePath;
  auto rmvpePath = rmvpeModelPath;
  auto selectedPitchDetector = pitchDetectorType;

  auto reloadTask = [device = device,
                      provider,
                      resolvedDeviceId,
                      fcpePath,
                      melPath,
                      centPath,
                      rmvpePath,
                      selectedPitchDetector](EditorController *self) {
    if (!self)
      return;

    if (self->gameDetector && self->gameDetector->isLoaded())
      self->gameDetector->unload();
    if (self->hnsepModel && self->hnsepModel->isLoaded())
      self->hnsepModel->unload();

    if (selectedPitchDetector == PitchDetectorType::FCPE &&
        self->rmvpePitchDetector && self->rmvpePitchDetector->isLoaded()) {
      self->rmvpePitchDetector->unload();
    }
    if (selectedPitchDetector == PitchDetectorType::RMVPE &&
        self->fcpePitchDetector && self->fcpePitchDetector->isLoaded()) {
      self->fcpePitchDetector->unload();
    }

    if (selectedPitchDetector == PitchDetectorType::FCPE &&
        self->fcpePitchDetector && fcpePath.existsAsFile()) {
      LOG("EditorController: loading FCPE model (device " + device +
          ", id " + juce::String(resolvedDeviceId) + ")...");
      if (self->fcpePitchDetector->loadModel(fcpePath, melPath, centPath,
                                             provider, resolvedDeviceId)) {
        LOG("FCPE pitch detector loaded successfully");
      } else {
        LOG("Failed to load FCPE model");
      }
    } else if (selectedPitchDetector == PitchDetectorType::FCPE &&
               self->fcpePitchDetector) {
      LOG("FCPE model not found at: " + fcpePath.getFullPathName());
    }

    if (selectedPitchDetector == PitchDetectorType::RMVPE &&
        self->rmvpePitchDetector && rmvpePath.existsAsFile()) {
      LOG("EditorController: loading RMVPE model (device " + device +
          ", id " + juce::String(resolvedDeviceId) + ")...");
      if (self->rmvpePitchDetector->loadModel(rmvpePath, provider,
                                              resolvedDeviceId)) {
        LOG("RMVPE pitch detector loaded successfully");
      } else {
        LOG("Failed to load RMVPE model");
      }
    } else if (selectedPitchDetector == PitchDetectorType::RMVPE &&
               self->rmvpePitchDetector) {
      LOG("RMVPE model not found at: " + rmvpePath.getFullPathName());
    }
  };

  if (!async) {
    if (modelReloadThread.joinable())
      modelReloadThread.join();
    isReloadingModels = true;
    std::thread syncReloadThread([this, reloadTask]() mutable {
      reloadTask(this);
    });
    syncReloadThread.join();
    isReloadingModels = false;
    return;
  }

  if (isReloadingModels.exchange(true))
    return;

  if (modelReloadThread.joinable())
    modelReloadThread.join();

  modelReloadThread = std::thread([this, reloadTask]() mutable {
    reloadTask(this);
    isReloadingModels = false;
  });
}

bool EditorController::isInferenceBusy() const {
  if (isAnalyzingAudio.load())
    return true;
  if (audioAnalyzer && audioAnalyzer->isAnalyzing())
    return true;
  if (incrementalSynth && incrementalSynth->isSynthesizing())
    return true;
  if (isReloadingModels.load())
    return true;
  return false;
}

juce::String EditorController::getModelDebugStatusText() const {
  auto loadedText = [](bool loaded) { return loaded ? "loaded" : "not loaded"; };
  auto deviceText = [this]() {
    return device + " #" + juce::String(deviceId);
  };

  juce::StringArray lines;
  lines.add("Model Debug");
  lines.add("Device: " + deviceText());
  lines.add("Reloading: " + juce::String(isReloadingModels.load() ? "yes" : "no"));
  lines.add(juce::String());
  lines.add("FCPE: " + juce::String(loadedText(fcpePitchDetector && fcpePitchDetector->isLoaded())));
  lines.add("RMVPE: " + juce::String(loadedText(rmvpePitchDetector && rmvpePitchDetector->isLoaded())));
  lines.add("GAME: " + juce::String(loadedText(gameDetector && gameDetector->isLoaded())));
  lines.add("HNSep: " + juce::String(loadedText(hnsepModel && hnsepModel->isLoaded())));

  const bool vocoderLoaded = vocoder && vocoder->isLoaded();
  juce::String vocoderDevice = deviceText();
  if (vocoder) {
    vocoderDevice = vocoder->getExecutionDevice() + " #" +
                    juce::String(vocoder->getExecutionDeviceId());
  }
  lines.add("Vocoder: " + juce::String(loadedText(vocoderLoaded)) +
            " (" + vocoderDevice + ")");

  lines.add("ContentVec: " + juce::String(loadedText(svcEngine && svcEngine->isContentVecLoaded())));

  if (svcModel && svcModel->isLoaded()) {
    const auto &cfg = svcModel->getConfig();
    lines.add("SVC: loaded (" + cfg.modelTypeName + ", " + cfg.name + ")");
  } else {
    lines.add("SVC: not loaded");
  }

  return lines.joinIntoString("\n");
}

void EditorController::notifyBackgroundStatus(const juce::String &message,
                                              bool active) {
  if (!backgroundStatusCallback)
    return;
  juce::MessageManager::callAsync(
      [callback = backgroundStatusCallback, message, active]() {
        callback(message, active);
      });
}

bool EditorController::ensureHNSepModelLoadedForAnalysis() {
  if (!hnsepModel)
    return false;
  if (hnsepModel->isLoaded())
    return true;

  if (svcModel && svcModel->isLoaded())
    svcModel->unload();
  if (svcEngine && svcEngine->isContentVecLoaded())
    svcEngine->unloadContentVec();

  if (!hnsepModelDir.isDirectory()) {
    LOG("hnsep model directory not found: " + hnsepModelDir.getFullPathName());
    return false;
  }

  const auto provider = getProviderFromDevice(device);
  const int resolvedDeviceId = deviceId < 0 ? 0 : deviceId;
  const auto hnsepCpuFile = hnsepModelDir.getChildFile("hnsep_VR.onnx");
  const auto hnsepDmlFile = hnsepModelDir.getChildFile("hnsep_VR_convstft.onnx");
  auto hnsepFile = provider == GPUProvider::DirectML && hnsepDmlFile.existsAsFile()
                       ? hnsepDmlFile
                       : hnsepCpuFile;
  if (!hnsepFile.existsAsFile()) {
    LOG("hnsep model file not found: " + hnsepFile.getFullPathName());
    return false;
  }

  LOG("EditorController: loading hnsep model on demand from " +
      hnsepFile.getFullPathName() + " (device " + device + ", id " +
      juce::String(resolvedDeviceId) + ")...");
  if (hnsepModel->loadModel(hnsepFile, provider, resolvedDeviceId))
    return true;

  if (provider == GPUProvider::DirectML && hnsepFile != hnsepCpuFile &&
      hnsepCpuFile.existsAsFile()) {
    LOG("Retrying hnsep with CPU fallback model: " +
        hnsepCpuFile.getFullPathName());
    return hnsepModel->loadModel(hnsepCpuFile, GPUProvider::CPU, 0);
  }

  return false;
}

bool EditorController::ensureHNSepBases(Project &targetProject) {
  std::lock_guard<std::mutex> lock(hnsepBasesMutex);

  auto &audioData = targetProject.getAudioData();
  const int numSamples = audioData.waveform.getNumSamples();
  if (numSamples <= 0)
    return false;
  if (audioData.hnsepCurvesTargetSVC && !audioData.waveformFromSVC) {
    LOG("HNSep bases deferred until SVC waveform is restored");
    return false;
  }
  if (audioData.harmonicWaveform.getNumSamples() > 0 &&
      audioData.noiseWaveform.getNumSamples() > 0 &&
      (!audioData.waveformFromSVC || audioData.hnsepBasesFromSVC))
    return true;
  notifyBackgroundStatus("Preparing HNSep harmonic/noise bases...", true);
  if (!ensureHNSepModelLoadedForAnalysis()) {
    notifyBackgroundStatus({}, false);
    return false;
  }

  std::vector<float> harmonic;
  std::vector<float> noise;
  const float *samples = audioData.waveform.getReadPointer(0);
  if (!hnsepModel->separate(samples, numSamples, harmonic, noise)) {
    LOG("HNSep on-demand separation failed");
    maybeUnloadHNSepAfterUse(hnsepModel.get());
    notifyBackgroundStatus({}, false);
    return false;
  }

  const int harmonicCopyLen = std::min(numSamples, static_cast<int>(harmonic.size()));
  const int noiseCopyLen = std::min(numSamples, static_cast<int>(noise.size()));
  audioData.harmonicWaveform.setSize(1, numSamples);
  audioData.harmonicWaveform.clear();
  if (harmonicCopyLen > 0) {
    juce::FloatVectorOperations::copy(
        audioData.harmonicWaveform.getWritePointer(0), harmonic.data(),
        harmonicCopyLen);
  }
  audioData.noiseWaveform.setSize(1, numSamples);
  audioData.noiseWaveform.clear();
  if (noiseCopyLen > 0) {
    juce::FloatVectorOperations::copy(
        audioData.noiseWaveform.getWritePointer(0), noise.data(), noiseCopyLen);
  }

  LOG("HNSep on-demand separation complete: " + juce::String(numSamples) +
      " samples separated into harmonic + noise");
  maybeUnloadHNSepAfterUse(hnsepModel.get());
  audioData.hnsepBasesFromSVC = audioData.waveformFromSVC;
  notifyBackgroundStatus({}, false);
  return true;
}

void EditorController::prewarmHNSepBasesAsync(Project &targetProject) {
  auto &audioData = targetProject.getAudioData();
  const auto curveFrameCount = std::max(
      audioData.voicingCurve.size(),
      std::max(audioData.breathCurve.size(), audioData.tensionCurve.size()));
  const int frameCount = static_cast<int>(curveFrameCount);
  if (frameCount <= 0)
    return;

  if (audioData.hnsepCurvesTargetSVC && !audioData.waveformFromSVC)
    return;

  if (!HNSepCurveProcessor::hasActiveEdits(targetProject, 0, frameCount))
    return;

  if (audioData.harmonicWaveform.getNumSamples() > 0 &&
      audioData.noiseWaveform.getNumSamples() > 0)
    return;

  ++hnsepPrewarmGeneration;
  if (hnsepPrewarmThread.joinable())
    hnsepPrewarmThread.join();

  auto generation = hnsepPrewarmGeneration.load();
  auto *projectPtr = &targetProject;
  hnsepPrewarmThread = std::thread([this, projectPtr, generation]() {
    if (hnsepPrewarmGeneration.load() != generation)
      return;

    LOG("EditorController: prewarming HNSep bases for restored project");
    if (!ensureHNSepBases(*projectPtr)) {
      LOG("EditorController: HNSep bases prewarm failed");
      return;
    }
    LOG("EditorController: HNSep bases prewarm complete");
  });
}

void EditorController::requestCancelLoading() {
  cancelLoadingFlag = true;
}

void EditorController::loadAudioFileAsync(
    const juce::File &file,
    const ProgressCallback &onProgress,
    const LoadCompleteCallback &onComplete,
    const CancelCallback &onCancelled) {
  loadAudioFileAsTrack(file, onProgress, onComplete, onCancelled);
}

void EditorController::loadAudioFileAsTrack(
    const juce::File &file,
    const ProgressCallback &onProgress,
    const LoadCompleteCallback &onComplete,
    const CancelCallback &onCancelled) {
  if (isLoadingAudio.load())
    return;

  cancelLoadingFlag = false;
  isLoadingAudio = true;

  if (loaderThread.joinable())
    loaderThread.join();

  loaderThread = std::thread([this, file, onProgress, onComplete, onCancelled]() {
    auto updateProgress = [&](double p, const juce::String &msg) {
      if (onProgress)
        onProgress(p, msg);
    };

    updateProgress(0.05, TR("progress.loading_audio"));

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(file));
    if (reader == nullptr || cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    auto shaFuture = std::async(std::launch::async, [file]() {
      return SHA256Utils::fileSHA256(file);
    });

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const int srcSampleRate = static_cast<int>(reader->sampleRate);

    juce::AudioBuffer<float> buffer(1, numSamples);

    updateProgress(0.10, "Reading audio...");
    if (reader->numChannels == 1) {
      reader->read(&buffer, 0, numSamples, 0, true, false);
    } else {
      juce::AudioBuffer<float> stereoBuffer(2, numSamples);
      reader->read(&stereoBuffer, 0, numSamples, 0, true, true);

      const float *left = stereoBuffer.getReadPointer(0);
      const float *right = stereoBuffer.getReadPointer(1);
      float *mono = buffer.getWritePointer(0);

      for (int i = 0; i < numSamples; ++i)
        mono[i] = (left[i] + right[i]) * 0.5f;
    }

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    if (srcSampleRate != SAMPLE_RATE) {
      updateProgress(0.18, "Resampling...");
      const double ratio = static_cast<double>(srcSampleRate) / SAMPLE_RATE;
      const int newNumSamples = static_cast<int>(numSamples / ratio);

      juce::AudioBuffer<float> resampledBuffer(1, newNumSamples);
      const float *src = buffer.getReadPointer(0);
      float *dst = resampledBuffer.getWritePointer(0);

      for (int i = 0; i < newNumSamples; ++i) {
        const double srcPos = i * ratio;
        const int srcIndex = static_cast<int>(srcPos);
        const double frac = srcPos - srcIndex;

        if (srcIndex + 1 < numSamples)
          dst[i] = static_cast<float>(src[srcIndex] * (1.0 - frac) +
                                      src[srcIndex + 1] * frac);
        else
          dst[i] = src[srcIndex];
      }

      buffer = std::move(resampledBuffer);
    }

    updateProgress(0.80, "Preparing track...");
    auto newProject = std::make_unique<Project>();
    newProject->setFilePath(file);
    newProject->setAudioSha256(shaFuture.get());
    auto &audioData = newProject->getAudioData();
    audioData.waveform = std::move(buffer);
    audioData.sampleRate = SAMPLE_RATE;
    audioData.originalWaveform.makeCopyOf(audioData.waveform);

    if (cancelLoadingFlag.load()) {
      isLoadingAudio = false;
      if (onCancelled)
        juce::MessageManager::callAsync(onCancelled);
      return;
    }

    updateProgress(0.95, "Finalizing...");

    juce::AudioBuffer<float> originalWaveform;
    originalWaveform.makeCopyOf(audioData.waveform);

    juce::MessageManager::callAsync(
        [this, project = std::move(newProject),
         original = std::move(originalWaveform), file, onComplete]() mutable {
          cancelSVCConversion();

          auto track = std::make_unique<Track>();
          track->type = TrackType::Accompaniment;
          track->name = file.getFileNameWithoutExtension();
          track->project = std::move(project);

          int newIndex = static_cast<int>(tracks.size());
          tracks.push_back(std::move(track));

          // If this is the first track or no active track, set as active
          if (activeTrackIndex < 0)
            activeTrackIndex = newIndex;

          if (audioEngine) {
            std::vector<Track*> trackPtrs;
            for (auto& t : tracks)
              trackPtrs.push_back(t.get());
            audioEngine->setTracks(trackPtrs);
            audioEngine->rebuildMixedWaveform();
          }

          isLoadingAudio = false;
          if (onComplete)
            onComplete(original);
        });
  });
}

void EditorController::convertTrackToVocal(
    int trackIndex,
    const ProgressCallback &onProgress,
    const TrackConvertedCallback &onComplete) {
  if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
    if (onComplete)
      onComplete(trackIndex, false);
    return;
  }

  auto* track = tracks[static_cast<size_t>(trackIndex)].get();
  if (!track || !track->project) {
    if (onComplete)
      onComplete(trackIndex, false);
    return;
  }

  if (track->isVocal()) {
    if (onComplete)
      onComplete(trackIndex, true);
    return;
  }

  auto* projectPtr = track->project.get();
  auto updateProgress = [onProgress](double p, const juce::String &msg) {
    if (onProgress)
      onProgress(p, msg);
  };

  auto completeOnVocal = [this, trackIndex, onComplete](bool success) {
    if (success && trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())) {
      auto* t = tracks[static_cast<size_t>(trackIndex)].get();
      if (t) {
        t->type = TrackType::Vocal;
        t->project->setName(t->name);
        if (audioEngine) {
          std::vector<Track*> trackPtrs;
          for (auto& tr : tracks)
            trackPtrs.push_back(tr.get());
          audioEngine->setTracks(trackPtrs);
          audioEngine->rebuildMixedWaveform(true);
        }
      }
    }
    if (onComplete)
      juce::MessageManager::callAsync([onComplete, trackIndex, success]() {
        onComplete(trackIndex, success);
      });
  };

  std::thread([this, projectPtr, updateProgress, completeOnVocal]() {
    analyzeAudio(*projectPtr, updateProgress, [completeOnVocal]() {
      completeOnVocal(true);
    });
  }).detach();
}

void EditorController::setHostAudioAsync(
    const juce::AudioBuffer<float> &buffer,
    double sampleRate,
    const ProgressCallback &onProgress,
    const LoadCompleteCallback &onComplete) {
  isLoadingAudio = true;
  cancelLoadingFlag.store(true);
  if (loaderThread.joinable()) {
    if (loaderJoinerThread.joinable())
      loaderJoinerThread.join();
    auto old = std::move(loaderThread);
    loaderJoinerThread = std::thread([t = std::move(old)]() mutable {
      if (t.joinable())
        t.join();
    });
  }
  cancelLoadingFlag.store(false);

  const auto jobId = hostAnalysisJobId.fetch_add(1) + 1;

  loaderThread = std::thread([this, buffer, sampleRate, onProgress, onComplete, jobId]() mutable {
    if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
    {
      isLoadingAudio = false;
      return;
    }

    juce::AudioBuffer<float> resampledBuffer;
    const double inputSampleRate = sampleRate;
    if (inputSampleRate > 0.0 &&
        std::abs(inputSampleRate - static_cast<double>(SAMPLE_RATE)) > 1e-6) {
      const int inSamples = buffer.getNumSamples();
      const int outSamples = static_cast<int>(
          std::llround(static_cast<double>(inSamples) *
                       (static_cast<double>(SAMPLE_RATE) / inputSampleRate)));
      const int channels = buffer.getNumChannels();
      resampledBuffer.setSize(channels, std::max(0, outSamples), false, false,
                              true);
      resampledBuffer.clear();

      const double ratio = inputSampleRate / static_cast<double>(SAMPLE_RATE);
      for (int ch = 0; ch < channels; ++ch) {
        juce::LagrangeInterpolator interp;
        interp.reset();
        interp.process(ratio, buffer.getReadPointer(ch),
                       resampledBuffer.getWritePointer(ch),
                       resampledBuffer.getNumSamples());
      }
    }

    const juce::AudioBuffer<float> &stored =
        resampledBuffer.getNumSamples() > 0 ? resampledBuffer : buffer;
    const double storedSampleRate = resampledBuffer.getNumSamples() > 0
                                        ? static_cast<double>(SAMPLE_RATE)
                                        : inputSampleRate;

    auto projectCopy = std::make_unique<Project>();
    projectCopy->getAudioData().waveform = stored;
    projectCopy->getAudioData().sampleRate = static_cast<int>(storedSampleRate);

    auto updateProgress = [&](double p, const juce::String &msg) {
      if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
        return;
      if (onProgress)
        onProgress(p, msg);
    };

    analyzeAudio(*projectCopy, updateProgress);

    if (cancelLoadingFlag.load() || hostAnalysisJobId.load() != jobId)
    {
      isLoadingAudio = false;
      return;
    }

    // Store pristine original waveform in AudioData for blend-based synthesis
    projectCopy->getAudioData().originalWaveform.makeCopyOf(projectCopy->getAudioData().waveform);

    juce::AudioBuffer<float> originalWaveform;
    originalWaveform.makeCopyOf(projectCopy->getAudioData().waveform);

    juce::MessageManager::callAsync(
        [this, project = std::move(projectCopy),
         original = std::move(originalWaveform), onComplete, jobId]() mutable {
          if (hostAnalysisJobId.load() != jobId)
            return;
          setProject(std::move(project));
          isLoadingAudio = false;
          if (onComplete)
            onComplete(original);
        });
  });
}

void EditorController::requestCancelRender() {
  cancelRenderFlag = true;
}

void EditorController::renderProcessedAudioAsync(
    const Project &project,
    float globalPitchOffset,
    const std::function<void(bool)> &onComplete) {
  cancelRenderFlag = true;
  if (renderThread.joinable())
    renderThread.join();
  isRenderingFlag = false;
  cancelRenderFlag = false;

  auto f0Snapshot = project.getAudioData().f0;
  auto voicedMaskSnapshot = project.getAudioData().voicedMask;
  auto melSpecSnapshot = project.getAudioData().melSpectrogram;
  Vocoder *voc = ensureVocoder();

  renderThread = std::thread(
      [this, f0Snapshot = std::move(f0Snapshot),
       voicedMaskSnapshot = std::move(voicedMaskSnapshot),
       melSpecSnapshot = std::move(melSpecSnapshot), globalPitchOffset, voc,
       onComplete]() mutable {
        isRenderingFlag = true;

        auto finishRendering = [this]() { isRenderingFlag = false; };

        if (cancelRenderFlag.load())
          return finishRendering();

        if (f0Snapshot.empty() || melSpecSnapshot.empty()) {
          if (onComplete)
            juce::MessageManager::callAsync([onComplete]() { onComplete(false); });
          return finishRendering();
        }

        if (voicedMaskSnapshot.size() < f0Snapshot.size())
          voicedMaskSnapshot.resize(f0Snapshot.size(), true);

        for (size_t i = 0; i < f0Snapshot.size(); ++i) {
          if (cancelRenderFlag.load())
            return finishRendering();
          if (voicedMaskSnapshot[i] && f0Snapshot[i] > 0)
            f0Snapshot[i] *= std::pow(2.0f, globalPitchOffset / 12.0f);
        }

        if (cancelRenderFlag.load())
          return finishRendering();

        auto synthesized = voc ? voc->infer(melSpecSnapshot, f0Snapshot)
                               : std::vector<float>{};

        if (cancelRenderFlag.load())
          return finishRendering();

        if (onComplete)
          juce::MessageManager::callAsync(
              [onComplete, ok = !synthesized.empty()]() { onComplete(ok); });
        finishRendering();
      });
}

void EditorController::resynthesizeIncrementalAsync(
    Project &project,
    const std::function<void(const juce::String &)> &onProgress,
    const std::function<void(bool)> &onComplete,
    std::atomic<bool> &pendingRerun,
    bool isPluginMode) {
  LOG("[STRETCH-DBG] resynthesizeIncrementalAsync() ENTER");
  auto *synth = incrementalSynth.get();
  auto *voc = ensureVocoder();
  if (!synth || !voc) {
    LOG("[STRETCH-DBG] resynthIncrAsync: BAIL — synth(" + juce::String((int)(synth != nullptr)) + ") vocoder(" + juce::String((int)(voc != nullptr)) + ")");
    if (onComplete)
      onComplete(false);
    return;
  }

  if (synth->isSynthesizing()) {
    LOG("[STRETCH-DBG] resynthIncrAsync: already synthesizing, queuing rerun");
    pendingRerun.store(true);
    synth->cancel();
    return;
  }

  auto &audioData = project.getAudioData();
  if (audioData.melSpectrogram.empty() || audioData.f0.empty()) {
    LOG("[STRETCH-DBG] resynthIncrAsync: BAIL — mel/f0 empty");
    if (onComplete)
      onComplete(false);
    return;
  }
  if (!voc->isLoaded()) {
    auto modelPath = PlatformPaths::getModelFile("pc_nsf_hifigan.onnx");
    if (!modelPath.existsAsFile()) {
      LOG("[STRETCH-DBG] resynthIncrAsync: BAIL — vocoder model not found");
      if (onComplete)
        onComplete(false);
      return;
    }

    if (!voc->loadModel(modelPath)) {
      LOG("[STRETCH-DBG] resynthIncrAsync: BAIL — vocoder load failed");
      if (onComplete)
        onComplete(false);
      return;
    }
  }

  if (!project.hasDirtyNotes() && !project.hasF0DirtyRange()) {
    LOG("[STRETCH-DBG] resynthIncrAsync: BAIL — no dirty: notes=" + juce::String((int)project.hasDirtyNotes()) + " f0=" + juce::String((int)project.hasF0DirtyRange()));
    if (onComplete)
      onComplete(false);
    return;
  }

  auto dirtyRange = project.getDirtyFrameRange();
  auto dirtyStart = dirtyRange.first;
  auto dirtyEnd = dirtyRange.second;
  LOG("[STRETCH-DBG] resynthIncrAsync: dirtyRange=[" + juce::String(dirtyStart) + ", " + juce::String(dirtyEnd) + "]");
  if (dirtyStart < 0 || dirtyEnd < 0) {
    if (onComplete)
      onComplete(false);
    return;
  }

  synth->setProject(&project);
  synth->setVocoder(voc);

  synth->setSVCEngine(nullptr);
  synth->setSVCModel(nullptr);

  pendingRerun.store(false);

  if (onProgress)
    onProgress(TR("progress.synthesizing"));

  AudioEngine *audioEnginePtr = nullptr;
  if (!isPluginMode && audioEngine)
    audioEnginePtr = audioEngine.get();

  if (incrementalSynthThread.joinable())
    incrementalSynthThread.join();

  auto progressOnMessageThread = [onProgress](const juce::String &message) {
    if (onProgress) {
      juce::MessageManager::callAsync(
          [onProgress, message]() { onProgress(message); });
    }
  };

  auto completeOnMessageThread = [this, projectPtr = &project,
                                  pending = &pendingRerun, onComplete,
                                  audioEnginePtr, isPluginMode](bool success) {
    juce::MessageManager::callAsync(
        [this, projectPtr, pending, onComplete, audioEnginePtr, isPluginMode,
         success]() {
        if (!success) {
          if (pending->exchange(false)) {
            juce::MessageManager::callAsync([this, projectPtr, pending, onComplete,
                                             audioEnginePtr, isPluginMode]() {
              resynthesizeIncrementalAsync(*projectPtr, nullptr, onComplete,
                                           *pending, isPluginMode);
            });
          } else if (onComplete) {
            onComplete(false);
          }
          return;
        }

        if (audioEnginePtr && !isPluginMode) {
          auto &audioData = projectPtr->getAudioData();
          try {
            audioEnginePtr->loadWaveform(audioData.waveform,
                                         audioData.sampleRate, true);
          } catch (...) {
            DBG("resynthesizeIncrementalAsync: EXCEPTION in loadWaveform!");
          }
        }

        if (onComplete)
          onComplete(true);

        if (pending->exchange(false)) {
          juce::MessageManager::callAsync([this, projectPtr, pending, onComplete,
                                           audioEnginePtr, isPluginMode]() {
            resynthesizeIncrementalAsync(*projectPtr, nullptr, onComplete,
                                          *pending, isPluginMode);
          });
        }
      });
  };

  incrementalSynthThread = std::thread(
      [this, synth, projectPtr = &project, dirtyStart, dirtyEnd,
       progress = std::move(progressOnMessageThread),
       complete = std::move(completeOnMessageThread)]() mutable {
        const bool hasHNSepEdits =
            HNSepCurveProcessor::hasActiveEdits(*projectPtr, dirtyStart,
                                                dirtyEnd);
        if (hasHNSepEdits)
          progress("Preparing HNSep harmonic/noise bases...");
        if (hasHNSepEdits && !ensureHNSepBases(*projectPtr)) {
          LOG("[STRETCH-DBG] resynthIncrAsync: HNSep edits present but bases unavailable");
        }
        const auto &audioData = projectPtr->getAudioData();
        const bool shouldRestoreSVC = projectPtr->hasSvcConditioningDirtyRange();
        if (shouldRestoreSVC)
          ensureSVCModelReady();
        if (isSVCModelActive()) {
          synth->setSVCEngine(svcEngine.get());
          synth->setSVCModel(svcModel.get());
          synth->setSVCParams(svcParams);
        } else {
          synth->setSVCEngine(nullptr);
          synth->setSVCModel(nullptr);
        }
        synth->synthesizeRegion(std::move(progress), std::move(complete));
        if (shouldRestoreSVC && svcEngine && svcEngine->isContentVecLoaded())
          svcEngine->unloadContentVec();
      });
}

void EditorController::analyzeAudio(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress,
    std::function<void()> onComplete) {
  if (modelReloadThread.joinable()) {
    modelReloadThread.join();
    isReloadingModels = false;
  }

  auto &audioData = targetProject.getAudioData();
  if (audioData.waveform.getNumSamples() == 0)
    return;

  if (isAnalyzingAudio.exchange(true)) {
    LOG("EditorController::analyzeAudio skipped: analysis already running");
    return;
  }
  AtomicFlagGuard analyzingGuard(isAnalyzingAudio);

  auto showMissingModelAndAbort = [](const juce::String &modelName,
                                     const juce::File &path) {
    juce::MessageManager::callAsync([modelName, path]() {
      juce::AlertWindow::showMessageBoxAsync(
          juce::AlertWindow::WarningIcon, "Missing model file",
          modelName + " was not found at:\n" + path.getFullPathName() +
              "\n\nPlease install the required model files and try again.");
    });
  };
  auto showModelLoadFailedAndAbort = [](const juce::String &modelName,
                                        const juce::File &path) {
    juce::MessageManager::callAsync([modelName, path]() {
      juce::AlertWindow::showMessageBoxAsync(
          juce::AlertWindow::WarningIcon, "Model load failed",
          modelName + " exists but failed to load:\n" + path.getFullPathName() +
              "\n\nPlease check inference device settings (CPU/CUDA/DirectML) "
              "or model compatibility.");
    });
  };

  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();

  const auto selectedPitchDetector = pitchDetectorType;
  if (selectedPitchDetector == PitchDetectorType::RMVPE) {
    if (!rmvpeModelPath.existsAsFile()) {
      showMissingModelAndAbort("rmvpe.onnx", rmvpeModelPath);
      return;
    }
    if (!rmvpePitchDetector || !rmvpePitchDetector->isLoaded()) {
      showModelLoadFailedAndAbort("rmvpe.onnx", rmvpeModelPath);
      return;
    }
  } else if (selectedPitchDetector == PitchDetectorType::FCPE) {
    if (!fcpeModelPath.existsAsFile()) {
      showMissingModelAndAbort("fcpe.onnx", fcpeModelPath);
      return;
    }
    if (!fcpePitchDetector || !fcpePitchDetector->isLoaded()) {
      showModelLoadFailedAndAbort("fcpe.onnx", fcpeModelPath);
      return;
    }
    if (!melFilterbankPath.existsAsFile()) {
      showMissingModelAndAbort("mel_filterbank.bin", melFilterbankPath);
      return;
    }
    if (!centTablePath.existsAsFile()) {
      showMissingModelAndAbort("cent_table.bin", centTablePath);
      return;
    }
  }

  LOG("========== PITCH DETECTOR SELECTION ==========");
  LOG("Selected detector: " +
      juce::String(pitchDetectorTypeToString(selectedPitchDetector)));
  LOG("RMVPE loaded: " +
      juce::String(
          rmvpePitchDetector && rmvpePitchDetector->isLoaded() ? "YES" : "NO"));
  LOG("FCPE loaded: " +
      juce::String(fcpePitchDetector && fcpePitchDetector->isLoaded() ? "YES"
                                                                      : "NO"));

  const int sampleRate = audioData.sampleRate;
  const auto provider = getProviderFromDevice(device);
  const bool allowConcurrentModelInference = provider == GPUProvider::CPU;
  const int predictedFrames = std::max(1, numSamples / HOP_SIZE + 1);
  const int resolvedDeviceId = deviceId < 0 ? 0 : deviceId;

  auto loadGameForAnalysis = [&]() {
    if (!gameDetector || gameDetector->isLoaded())
      return;
    if (!gameModelDir.isDirectory()) {
      LOG("GAME model directory not found: " + gameModelDir.getFullPathName());
      return;
    }

    LOG("EditorController: loading GAME models for analysis from " +
        gameModelDir.getFullPathName() + " (device " + device + ", id " +
        juce::String(resolvedDeviceId) + ")...");
    if (!gameDetector->loadModels(gameModelDir, provider, resolvedDeviceId))
      LOG("Failed to load GAME models from " + gameModelDir.getFullPathName());
  };

  ensureHNSepModelLoadedForAnalysis();
  if (allowConcurrentModelInference)
    loadGameForAnalysis();

  struct HNSepResult {
    bool attempted = false;
    bool ok = false;
    std::vector<float> harmonic;
    std::vector<float> noise;
  };

  auto computeVadMaskForFrames = [samples, numSamples](int frameCount) {
    std::vector<bool> mask(static_cast<size_t>(std::max(0, frameCount)), false);
    constexpr float kVadThreshold = 0.008f;
    for (int vi = 0; vi < frameCount; ++vi) {
      int ss = vi * HOP_SIZE;
      int se = std::min(ss + HOP_SIZE, numSamples);
      if (ss >= numSamples)
        continue;
      float sumSq = 0.0f;
      for (int vj = ss; vj < se; ++vj)
        sumSq += samples[vj] * samples[vj];
      float rms = std::sqrt(sumSq / static_cast<float>(se - ss));
      mask[static_cast<size_t>(vi)] = rms > kVadThreshold;
    }
    return mask;
  };

  auto runF0Extraction = [this, selectedPitchDetector, samples, numSamples,
                          sampleRate]() {
    if (selectedPitchDetector == PitchDetectorType::RMVPE)
      return rmvpePitchDetector->extractF0(samples, numSamples, sampleRate);
    if (selectedPitchDetector == PitchDetectorType::FCPE)
      return fcpePitchDetector->extractF0(samples, numSamples, sampleRate);
    return std::vector<float>{};
  };

  auto runHNSep = [this, samples, numSamples](std::function<void(double)> progress) {
    HNSepResult result;
    result.attempted = true;
    if (!hnsepModel || !hnsepModel->isLoaded())
      return result;
    result.ok = hnsepModel->separateWithProgress(
        samples, numSamples, result.harmonic, result.noise, std::move(progress));
    return result;
  };

  auto runGameDetection = [this, samples, numSamples]() {
    GameSegmentationResult result;
    result.attempted = true;
    if (!gameDetector || !gameDetector->isLoaded())
      return result;
    result.notes = gameDetector->detectNotesWithProgress(
        samples, numSamples, GAMEDetector::SAMPLE_RATE, nullptr);
    result.chunks = gameDetector->getLastChunkRanges();
    return result;
  };

  onProgress(0.35, "Computing mel spectrogram...");
  LOG("analyzeAudio: starting mel spectrogram");
  auto melFuture = std::async(std::launch::async, [samples, numSamples, sampleRate]() {
    MelSpectrogram melComputer(sampleRate, N_FFT, HOP_SIZE, NUM_MELS, FMIN, FMAX);
    return melComputer.compute(samples, numSamples);
  });

  onProgress(0.55, "Extracting pitch (F0)...");
  LOG("analyzeAudio: starting F0 extraction");
  std::future<std::vector<float>> f0Future;
  std::vector<float> extractedF0;
  if (allowConcurrentModelInference) {
    f0Future = std::async(std::launch::async, runF0Extraction);
  } else {
    try {
#ifdef _WIN32
      SehTranslatorGuard sehGuard;
#endif
      extractedF0 = runF0Extraction();
    } catch (...) {
      LOG("analyzeAudio: EXCEPTION during F0 extraction");
      throw;
    }
  }
  LOG("analyzeAudio: F0 extraction done, frames=" + juce::String(extractedF0.size()));
  auto vadFuture = std::async(std::launch::async, computeVadMaskForFrames,
                              predictedFrames);

  auto modelPath = PlatformPaths::getModelFile("pc_nsf_hifigan.onnx");
  auto *voc = ensureVocoder();
  LOG("analyzeAudio: vocoder ready, loaded=" + juce::String(voc && voc->isLoaded() ? "yes" : "no"));
  const bool vocoderMissing = !voc || (!modelPath.existsAsFile() && !voc->isLoaded());
  const bool shouldLoadVocoder = voc && modelPath.existsAsFile() && !voc->isLoaded();
  std::future<bool> vocoderLoadFuture;
  if (shouldLoadVocoder && allowConcurrentModelInference) {
    vocoderLoadFuture = std::async(std::launch::async, [voc, modelPath]() {
      return voc->loadModel(modelPath);
    });
  } else if (shouldLoadVocoder && !allowConcurrentModelInference) {
    LOG("analyzeAudio: loading vocoder synchronously");
    if (!voc->loadModel(modelPath)) {
      juce::MessageManager::callAsync([modelPath]() {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "Inference failed",
            "Failed to load vocoder model at:\n" + modelPath.getFullPathName() +
                "\n\nPlease check your model installation and try again.");
      });
      return;
    }
  }

  std::future<HNSepResult> hnsepFuture;
  if (allowConcurrentModelInference && hnsepModel && hnsepModel->isLoaded())
    hnsepFuture = std::async(std::launch::async, runHNSep,
                             std::function<void(double)>{});

  std::future<GameSegmentationResult> gameFuture;
  if (allowConcurrentModelInference && gameDetector && gameDetector->isLoaded())
    gameFuture = std::async(std::launch::async, runGameDetection);

  audioData.melSpectrogram = melFuture.get();
  int targetFrames = static_cast<int>(audioData.melSpectrogram.size());

  if (f0Future.valid())
    extractedF0 = f0Future.get();

  if (extractedF0.empty() || targetFrames <= 0) {
    juce::MessageManager::callAsync([]() {
      juce::AlertWindow::showMessageBoxAsync(
          juce::AlertWindow::WarningIcon, "Inference failed",
          "Failed to extract pitch (F0). Please check your model installation "
          "and settings.");
    });
    return;
  }

  {
    audioData.f0.resize(targetFrames);

    const double neuralFrameTime = 160.0 / 16000.0;
    const double vocoderFrameTime =
        static_cast<double>(HOP_SIZE) /
        static_cast<double>(std::max(1, audioData.sampleRate));

    for (int i = 0; i < targetFrames; ++i) {
      double vocoderTime = i * vocoderFrameTime;
      double neuralFramePos = vocoderTime / neuralFrameTime;
      int srcIdx = static_cast<int>(neuralFramePos);
      double frac = neuralFramePos - srcIdx;

      if (srcIdx + 1 < static_cast<int>(extractedF0.size())) {
        float f0_a = extractedF0[srcIdx];
        float f0_b = extractedF0[srcIdx + 1];

        if (f0_a > 0.0f && f0_b > 0.0f) {
          float logF0_a = std::log(f0_a);
          float logF0_b = std::log(f0_b);
          float logF0_interp = logF0_a * (1.0 - frac) + logF0_b * frac;
          audioData.f0[i] = std::exp(logF0_interp);
        } else if (f0_a > 0.0f) {
          audioData.f0[i] = f0_a;
        } else if (f0_b > 0.0f) {
          audioData.f0[i] = f0_b;
        } else {
          audioData.f0[i] = 0.0f;
        }
      } else if (srcIdx < static_cast<int>(extractedF0.size())) {
        audioData.f0[i] = extractedF0[srcIdx];
      } else {
        audioData.f0[i] = extractedF0.back() > 0.0f ? extractedF0.back() : 0.0f;
      }
    }

    audioData.voicedMask.resize(audioData.f0.size());
    for (size_t i = 0; i < audioData.f0.size(); ++i) {
      audioData.voicedMask[i] = audioData.f0[i] > 0;
    }

    audioData.vadMask = vadFuture.get();
    audioData.vadMask.resize(audioData.f0.size(), false);

    onProgress(0.65, "Smoothing pitch curve...");
    audioData.f0 = F0Smoother::smoothF0(audioData.f0, audioData.voicedMask);
    audioData.f0 = PitchCurveProcessor::interpolateWithUvMask(
        audioData.f0, audioData.voicedMask);
  }

  auto applyHNSepResult = [&audioData, numSamples](const HNSepResult &result) {
    if (!result.attempted)
      return;
    if (result.ok) {
      const int harmonicCopyLen = std::min(
          numSamples, static_cast<int>(result.harmonic.size()));
      const int noiseCopyLen = std::min(numSamples,
                                        static_cast<int>(result.noise.size()));

      audioData.harmonicWaveform.setSize(1, numSamples);
      audioData.harmonicWaveform.clear();
      if (harmonicCopyLen > 0) {
        juce::FloatVectorOperations::copy(
            audioData.harmonicWaveform.getWritePointer(0),
            result.harmonic.data(), harmonicCopyLen);
      }

      audioData.noiseWaveform.setSize(1, numSamples);
      audioData.noiseWaveform.clear();
      if (noiseCopyLen > 0) {
        juce::FloatVectorOperations::copy(
            audioData.noiseWaveform.getWritePointer(0), result.noise.data(),
            noiseCopyLen);
      }

      LOG("hnsep separation complete: " + juce::String(numSamples) +
          " samples separated into harmonic + noise");
    } else {
      LOG("hnsep separation failed - harmonic/noise buffers left empty");
    }
  };

  if (hnsepModel && hnsepModel->isLoaded() && numSamples > 0) {
    onProgress(0.70, "Separating harmonic/noise...");
    if (hnsepFuture.valid()) {
      applyHNSepResult(hnsepFuture.get());
    } else {
      applyHNSepResult(runHNSep([&onProgress](double progress) {
        onProgress(0.70 + progress * 0.05,
                   "Separating harmonic/noise...");
      }));
    }
  } else if (hnsepModel && !hnsepModel->isLoaded()) {
    LOG("hnsep model not loaded - skipping harmonic-noise separation");
  }

  if (!allowConcurrentModelInference) {
    if (hnsepModel && hnsepModel->isLoaded())
      maybeUnloadHNSepAfterUse(hnsepModel.get());
    loadGameForAnalysis();
  }

  onProgress(0.75, TR("progress.loading_vocoder"));
  if (!voc) {
    juce::MessageManager::callAsync([]() {
      juce::AlertWindow::showMessageBoxAsync(
          juce::AlertWindow::WarningIcon, "Inference failed",
          "Failed to create vocoder instance.");
    });
    return;
  }

  if (vocoderMissing) {
    showMissingModelAndAbort("pc_nsf_hifigan.onnx", modelPath);
    return;
  }

  if (vocoderLoadFuture.valid()) {
    if (vocoderLoadFuture.get()) {
      DBG("Vocoder model loaded successfully: " + modelPath.getFullPathName());
    } else {
      juce::MessageManager::callAsync([modelPath]() {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "Inference failed",
            "Failed to load vocoder model at:\n" + modelPath.getFullPathName() +
                "\n\nPlease check your model installation and try again.");
      });
      return;
    }
  } else if (shouldLoadVocoder) {
    if (voc->loadModel(modelPath)) {
      DBG("Vocoder model loaded successfully: " + modelPath.getFullPathName());
    } else {
      juce::MessageManager::callAsync([modelPath]() {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "Inference failed",
            "Failed to load vocoder model at:\n" + modelPath.getFullPathName() +
                "\n\nPlease check your model installation and try again.");
      });
      return;
    }
  }

  onProgress(0.90, "Segmenting notes...");
  GameSegmentationResult gameResult;
  const GameSegmentationResult *gameResultPtr = nullptr;
  if (gameFuture.valid()) {
    gameResult = gameFuture.get();
    gameResultPtr = &gameResult;
  }
  segmentIntoNotesInternal(targetProject, nullptr, gameResultPtr);

  PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);
  HNSepCurveProcessor::initializeCurves(targetProject);

  if (hnsepModel && hnsepModel->isLoaded())
    maybeUnloadHNSepAfterUse(hnsepModel.get());
  if (gameDetector && gameDetector->isLoaded())
    gameDetector->unload();

  if (onComplete)
    onComplete();
}

void EditorController::analyzeAudioAsync(
    const std::function<void(Project &)> &onProjectReady,
    const std::function<void()> &onProjectChanged) {
  if (loaderThread.joinable())
    loaderThread.join();

  loaderThread = std::thread([this, onProjectReady, onProjectChanged]() {
    auto* activeProject = getProject();
    if (!activeProject)
      return;

    auto projectCopy = std::make_shared<Project>(*activeProject);

    analyzeAudio(*projectCopy, [](double, const juce::String &) {});

    juce::MessageManager::callAsync([this, projectCopy, onProjectReady,
                                     onProjectChanged]() {
      auto* proj = getProject();
      if (!proj)
        return;

      proj->getAudioData().melSpectrogram =
          projectCopy->getAudioData().melSpectrogram;
      proj->getAudioData().f0 = projectCopy->getAudioData().f0;
      proj->getAudioData().voicedMask =
          projectCopy->getAudioData().voicedMask;
      proj->getAudioData().vadMask =
          projectCopy->getAudioData().vadMask;
      proj->getAudioData().originalWaveform.makeCopyOf(
          projectCopy->getAudioData().originalWaveform);
      proj->getAudioData().basePitch =
          projectCopy->getAudioData().basePitch;
      proj->getAudioData().deltaPitch =
          projectCopy->getAudioData().deltaPitch;
      proj->getAudioData().voicingCurve =
          projectCopy->getAudioData().voicingCurve;
      proj->getAudioData().breathCurve =
          projectCopy->getAudioData().breathCurve;
      proj->getAudioData().tensionCurve =
          projectCopy->getAudioData().tensionCurve;
      proj->getAudioData().harmonicWaveform.makeCopyOf(
          projectCopy->getAudioData().harmonicWaveform);
      proj->getAudioData().noiseWaveform.makeCopyOf(
          projectCopy->getAudioData().noiseWaveform);

      if (onProjectReady)
        onProjectReady(*proj);
      if (onProjectChanged)
        onProjectChanged();
    });
  });
}

void EditorController::segmentIntoNotesAsync(
    const std::function<void(Project &)> &onProjectReady,
    const std::function<void()> &onNotesChanged) {
  if (loaderThread.joinable())
    loaderThread.join();

  loaderThread = std::thread([this, onProjectReady, onNotesChanged]() {
    auto* activeProject = getProject();
    if (!activeProject)
      return;

    auto projectCopy = std::make_shared<Project>(*activeProject);
    segmentIntoNotes(*projectCopy);

    juce::MessageManager::callAsync([this, projectCopy, onProjectReady,
                                     onNotesChanged]() {
      auto* proj = getProject();
      if (!proj)
        return;

      proj->getNotes() = projectCopy->getNotes();
      proj->getAudioData().voicingCurve =
          projectCopy->getAudioData().voicingCurve;
      proj->getAudioData().breathCurve =
          projectCopy->getAudioData().breathCurve;
      proj->getAudioData().tensionCurve =
          projectCopy->getAudioData().tensionCurve;

      if (onProjectReady)
        onProjectReady(*proj);
      if (onNotesChanged)
        onNotesChanged();
    });
  });
}

void EditorController::segmentIntoNotes(Project &targetProject,
                                         std::function<void()> onStreamingUpdate) {
  segmentIntoNotesInternal(targetProject, onStreamingUpdate, nullptr);
}

void EditorController::segmentIntoNotesInternal(
    Project &targetProject,
    std::function<void()> onStreamingUpdate,
    const GameSegmentationResult *gameResult) {
  auto &audioData = targetProject.getAudioData();
  auto &notes = targetProject.getNotes();
  notes.clear();
  audioData.someChunkRanges.clear();
  audioData.someDebugChunks.clear();

  const int melSize = static_cast<int>(audioData.melSpectrogram.size());

  auto fillNoteClips = [&](Note &note) {
    const int clipStartFrame = std::max(0, note.getSrcStartFrame());
    const int clipEndFrame = std::max(clipStartFrame, note.getSrcEndFrame());

    if (audioData.waveform.getNumSamples() > 0) {
      int startSample = clipStartFrame * HOP_SIZE;
      int endSample = clipEndFrame * HOP_SIZE;
      startSample =
          std::max(0, std::min(startSample, audioData.waveform.getNumSamples()));
      endSample = std::max(startSample,
                           std::min(endSample, audioData.waveform.getNumSamples()));
      std::vector<float> clip;
      clip.reserve(static_cast<size_t>(endSample - startSample));
      const float *src = audioData.waveform.getReadPointer(0);
      for (int i = startSample; i < endSample; ++i)
        clip.push_back(src[i]);
      note.setClipWaveform(std::move(clip));
    }

    if (audioData.harmonicWaveform.getNumSamples() > 0) {
      int startSample = clipStartFrame * HOP_SIZE;
      int endSample = clipEndFrame * HOP_SIZE;
      startSample = std::max(
          0, std::min(startSample, audioData.harmonicWaveform.getNumSamples()));
      endSample =
          std::max(startSample,
                   std::min(endSample, audioData.harmonicWaveform.getNumSamples()));
      std::vector<float> clip;
      clip.reserve(static_cast<size_t>(endSample - startSample));
      const float *src = audioData.harmonicWaveform.getReadPointer(0);
      for (int i = startSample; i < endSample; ++i)
        clip.push_back(src[i]);
      note.setClipHarmonicWaveform(std::move(clip));
    }

    if (audioData.noiseWaveform.getNumSamples() > 0) {
      int startSample = clipStartFrame * HOP_SIZE;
      int endSample = clipEndFrame * HOP_SIZE;
      startSample = std::max(
          0, std::min(startSample, audioData.noiseWaveform.getNumSamples()));
      endSample =
          std::max(startSample,
                   std::min(endSample, audioData.noiseWaveform.getNumSamples()));
      std::vector<float> clip;
      clip.reserve(static_cast<size_t>(endSample - startSample));
      const float *src = audioData.noiseWaveform.getReadPointer(0);
      for (int i = startSample; i < endSample; ++i)
        clip.push_back(src[i]);
      note.setClipNoiseWaveform(std::move(clip));
    }

    if (!audioData.melSpectrogram.empty() && clipStartFrame < melSize) {
      const int melStart = std::max(0, clipStartFrame);
      const int melEnd = std::min(clipEndFrame, melSize);
      if (melEnd > melStart) {
        std::vector<std::vector<float>> melClip(
            audioData.melSpectrogram.begin() + melStart,
            audioData.melSpectrogram.begin() + melEnd);
        note.setClipMel(std::move(melClip));
      }
    }
  };

  if (audioData.f0.empty())
    return;

  const bool hasPrecomputedGame = gameResult != nullptr && gameResult->attempted;

  if (!hasPrecomputedGame && (!gameDetector || !gameDetector->isLoaded())) {
    auto searchedPath = gameModelDir.getFullPathName();
    auto bundlePath =
        PlatformPaths::getModelsDirectory().getChildFile("GAME").getFullPathName();

    juce::String detail = "GAME models were not found.\n\n"
                          "Searched path: " + searchedPath + "\n"
                          "Bundle path: " + bundlePath + "\n"
                          "gameDetector: " +
                          juce::String(gameDetector ? "created" : "null") +
                          "\n"
                          "isLoaded: " +
                          juce::String(gameDetector ? (gameDetector->isLoaded() ? "true" : "false") : "N/A") +
                          "\n"
                          "isDirectory: " +
                          juce::String(gameModelDir.isDirectory() ? "true" : "false") +
                          "\n\n"
                          "Required files: encoder.onnx, segmenter.onnx, estimator.onnx, bd2dur.onnx, config.json\n\n";

    for (auto *name : {"encoder.onnx", "segmenter.onnx", "estimator.onnx",
                       "bd2dur.onnx", "config.json"}) {
      auto file = gameModelDir.getChildFile(name);
      detail += juce::String(name) + ": " +
                (file.existsAsFile() ? "OK" : "MISSING") + "\n";
    }

    juce::MessageManager::callAsync([detail]() {
      juce::AlertWindow::showMessageBoxAsync(
          juce::AlertWindow::WarningIcon, "GAME Error", detail);
    });
    return;
  }

  if ((hasPrecomputedGame || (gameDetector && gameDetector->isLoaded())) &&
      audioData.waveform.getNumSamples() > 0) {

    const float *samples = audioData.waveform.getReadPointer(0);
    int numSamples = audioData.waveform.getNumSamples();
    const int f0Size = static_cast<int>(audioData.f0.size());

    std::vector<GAMEDetector::NoteEvent> detectedGameNotes;
    std::vector<GAMEDetector::ChunkRange> detectedSlicerChunks;
    const std::vector<GAMEDetector::NoteEvent> *gameNotesPtr = nullptr;
    const std::vector<GAMEDetector::ChunkRange> *slicerChunksPtr = nullptr;
    if (hasPrecomputedGame) {
      gameNotesPtr = &gameResult->notes;
      slicerChunksPtr = &gameResult->chunks;
    } else {
      detectedGameNotes = gameDetector->detectNotesWithProgress(
          samples, numSamples, GAMEDetector::SAMPLE_RATE, nullptr);
      detectedSlicerChunks = gameDetector->getLastChunkRanges();
      gameNotesPtr = &detectedGameNotes;
      slicerChunksPtr = &detectedSlicerChunks;
    }

    const auto &gameNotes = *gameNotesPtr;
    const auto &slicerChunks = *slicerChunksPtr;
    for (int chunkIndex = 0; chunkIndex < static_cast<int>(slicerChunks.size());
         ++chunkIndex) {
      const auto &slicerChunk = slicerChunks[chunkIndex];
      int chunkStartFrame = slicerChunk.startSample / GAMEDetector::HOP_SIZE;
      int chunkEndFrame =
          (slicerChunk.endSample + GAMEDetector::HOP_SIZE - 1) /
          GAMEDetector::HOP_SIZE;
      chunkStartFrame = std::max(0, std::min(chunkStartFrame, f0Size));
      chunkEndFrame = std::max(chunkStartFrame, std::min(chunkEndFrame, f0Size));

      audioData.someChunkRanges.emplace_back(chunkStartFrame, chunkEndFrame);

      AudioData::SomeDebugChunk debugChunk;
      debugChunk.chunkIndex = chunkIndex;
      debugChunk.startFrame = chunkStartFrame;
      debugChunk.endFrame = chunkEndFrame;
      debugChunk.shortRestThreshold = 0;

      for (const auto &gameNote : gameNotes) {
        if (gameNote.startFrame < chunkStartFrame ||
            gameNote.startFrame >= chunkEndFrame)
          continue;

        AudioData::SomeDebugEvent event;
        event.startFrame = gameNote.startFrame;
        event.endFrame = gameNote.endFrame;
        event.attachedStartFrame = gameNote.startFrame;
        event.midiNote = gameNote.midiNote;
        event.isRest = gameNote.isRest;
        event.durationSeconds =
            static_cast<float>(gameNote.endFrame - gameNote.startFrame) *
            GAMEDetector::HOP_SIZE /
            static_cast<float>(GAMEDetector::SAMPLE_RATE);
        event.durationFrames = gameNote.endFrame - gameNote.startFrame;
        debugChunk.events.push_back(event);
      }
      audioData.someDebugChunks.push_back(std::move(debugChunk));
    }

    if (audioData.someChunkRanges.empty())
      audioData.someChunkRanges.emplace_back(0, f0Size);

    for (const auto &gameNote : gameNotes) {
      if (gameNote.isRest)
        continue;

      int f0Start = std::max(0, std::min(gameNote.startFrame, f0Size - 1));
      int f0End = std::max(f0Start + 1, std::min(gameNote.endFrame, f0Size));

      if (f0End - f0Start < 3)
        continue;

      Note note(f0Start, f0End, gameNote.midiNote);
      std::vector<float> f0Values(audioData.f0.begin() + f0Start,
                                  audioData.f0.begin() + f0End);
      note.setF0Values(std::move(f0Values));
      notes.push_back(note);
    }

    if (onStreamingUpdate)
      juce::MessageManager::callAsync(onStreamingUpdate);

    // VAD + SOME rest guided boundary refinement:
    // expand note heads/tails into energetic consonant regions so note lengths
    // better cover pre/post-consonants.
    if (!notes.empty() && !audioData.vadMask.empty()) {
      struct RestRange {
        int start = 0;
        int end = 0;
      };
      std::vector<RestRange> rests;
      for (const auto &chunk : audioData.someDebugChunks) {
        for (const auto &ev : chunk.events) {
          if (!ev.isRest)
            continue;
          if (ev.endFrame <= ev.startFrame)
            continue;
          rests.push_back({ev.startFrame, ev.endFrame});
        }
      }

      auto vadRatioInRange = [&](int s, int e) -> float {
        if (e <= s || audioData.vadMask.empty())
          return 0.0f;
        s = std::max(0, s);
        e = std::min(e, static_cast<int>(audioData.vadMask.size()));
        if (e <= s)
          return 0.0f;
        int voiced = 0;
        for (int i = s; i < e; ++i) {
          if (audioData.vadMask[static_cast<size_t>(i)])
            ++voiced;
        }
        return static_cast<float>(voiced) / static_cast<float>(e - s);
      };

      constexpr int kMaxHeadFrames = 14;     // ~162ms
      constexpr int kMaxTailFrames = 10;     // ~116ms
      constexpr int kRestBridgeGap = 4;      // allow tiny gap
      constexpr int kMaxRestAttach = 18;     // max attached rest length
      constexpr float kVadAttachRatio = 0.30f; // energetic enough to attach

      const int f0Size = static_cast<int>(audioData.f0.size());

      for (size_t ni = 0; ni < notes.size(); ++ni) {
        auto &note = notes[ni];
        int start = note.getStartFrame();
        int end = note.getEndFrame();
        if (end <= start)
          continue;

        int prevEnd = 0;
        if (ni > 0)
          prevEnd = notes[ni - 1].getEndFrame();
        int nextStart = static_cast<int>(audioData.vadMask.size());
        if (ni + 1 < notes.size())
          nextStart = notes[ni + 1].getStartFrame();

        // 1) Plain VAD backward/forward expansion.
        int newStart = start;
        for (int i = start - 1; i >= std::max(prevEnd, start - kMaxHeadFrames);
             --i) {
          if (i >= 0 && i < static_cast<int>(audioData.vadMask.size()) &&
              audioData.vadMask[static_cast<size_t>(i)])
            newStart = i;
          else
            break;
        }

        int newEnd = end;
        for (int i = end; i < std::min(nextStart, end + kMaxTailFrames); ++i) {
          if (i >= 0 && i < static_cast<int>(audioData.vadMask.size()) &&
              audioData.vadMask[static_cast<size_t>(i)])
            newEnd = i + 1;
          else
            break;
        }

        // 2) Rest-guided attach (front/back) gated by VAD ratio.
        for (const auto &rr : rests) {
          const int restLen = rr.end - rr.start;
          if (restLen <= 0 || restLen > kMaxRestAttach)
            continue;

          // Front rest -> note head
          if (rr.end <= start && start - rr.end <= kRestBridgeGap) {
            const int candStart = std::max(prevEnd, rr.start);
            if (candStart < newStart &&
                vadRatioInRange(candStart, start) >= kVadAttachRatio) {
              newStart = std::max(candStart, start - kMaxHeadFrames);
            }
          }

          // Back rest -> note tail
          if (rr.start >= end && rr.start - end <= kRestBridgeGap) {
            const int candEnd = std::min(nextStart, rr.end);
            if (candEnd > newEnd &&
                vadRatioInRange(end, candEnd) >= kVadAttachRatio) {
              newEnd = std::min(candEnd, end + kMaxTailFrames);
            }
          }
        }

        newStart = std::max(prevEnd, newStart);
        newEnd = std::max(newStart + 1, std::min(nextStart, newEnd));
        newStart = std::max(0, std::min(newStart, f0Size - 1));
        newEnd = std::max(newStart + 1, std::min(newEnd, f0Size));

        note.setStartFrame(newStart);
        note.setEndFrame(newEnd);
        std::vector<float> f0Values(audioData.f0.begin() + newStart,
                                    audioData.f0.begin() + newEnd);
        note.setF0Values(std::move(f0Values));
        fillNoteClips(note);
      }
    }

    for (auto &note : notes) {
      if (!note.hasClipWaveform())
        fillNoteClips(note);
    }

    juce::Thread::sleep(100);

    DBG("SOME segmented into " << notes.size() << " notes");

    if (!audioData.f0.empty())
      PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);
    HNSepCurveProcessor::initializeCurves(targetProject);

    return;
  }

  auto finalizeNote = [&](int start, int end) {
    if (end - start < 5)
      return;

    float midiSum = 0.0f;
    int midiCount = 0;
    for (int j = start; j < end; ++j) {
      if (j < static_cast<int>(audioData.voicedMask.size()) &&
          audioData.voicedMask[j] && audioData.f0[j] > 0) {
        midiSum += freqToMidi(audioData.f0[j]);
        midiCount++;
      }
    }
    if (midiCount == 0)
      return;

    float midi = midiSum / midiCount;

    Note note(start, end, midi);
    std::vector<float> f0Values(audioData.f0.begin() + start,
                                audioData.f0.begin() + end);
    note.setF0Values(std::move(f0Values));
    fillNoteClips(note);
    notes.push_back(note);
  };

  constexpr float pitchSplitThreshold = 0.5f;
  constexpr int minFramesForSplit = 3;
  constexpr int maxUnvoicedGap = INT_MAX;

  bool inNote = false;
  int noteStart = 0;
  int currentMidiNote = 0;
  int pitchChangeCount = 0;
  int pitchChangeStart = 0;
  int unvoicedCount = 0;

  for (size_t i = 0; i < audioData.f0.size(); ++i) {
    bool voiced = i < audioData.voicedMask.size() && audioData.voicedMask[i];

    if (voiced && !inNote) {
      inNote = true;
      noteStart = static_cast<int>(i);
      currentMidiNote =
          static_cast<int>(std::round(freqToMidi(audioData.f0[i])));
      pitchChangeCount = 0;
      unvoicedCount = 0;
    } else if (voiced && inNote) {
      unvoicedCount = 0;

      float currentMidi = freqToMidi(audioData.f0[i]);
      int quantizedMidi = static_cast<int>(std::round(currentMidi));

      if (quantizedMidi != currentMidiNote &&
          std::abs(currentMidi - currentMidiNote) > pitchSplitThreshold) {
        if (pitchChangeCount == 0)
          pitchChangeStart = static_cast<int>(i);
        pitchChangeCount++;

        if (pitchChangeCount >= minFramesForSplit) {
          finalizeNote(noteStart, pitchChangeStart);

          noteStart = pitchChangeStart;
          currentMidiNote = quantizedMidi;
          pitchChangeCount = 0;
        }
      } else {
        pitchChangeCount = 0;
      }
    } else if (!voiced && inNote) {
      unvoicedCount++;
      if (unvoicedCount > maxUnvoicedGap) {
        finalizeNote(noteStart, static_cast<int>(i) - unvoicedCount);
        inNote = false;
        pitchChangeCount = 0;
        unvoicedCount = 0;
      }
    }
  }

  if (inNote) {
    finalizeNote(noteStart, static_cast<int>(audioData.f0.size()));
  }

  if (!audioData.f0.empty())
    PitchCurveProcessor::rebuildCurvesFromSource(targetProject, audioData.f0);
  HNSepCurveProcessor::initializeCurves(targetProject);
}
