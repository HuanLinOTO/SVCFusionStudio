#pragma once

#include "../JuceHeader.h"
#include "Analysis/AudioAnalyzer.h"
#include "AudioEngine.h"
#include "Engine/PlaybackController.h"
#include "FCPEPitchDetector.h"
#include "GAMEDetector.h"
#include "HNSepModel.h"
#include "PitchDetectorType.h"
#include "RMVPEPitchDetector.h"
#include "SVCInferenceEngine.h"
#include "SVCModelSession.h"
#include "Synthesis/IncrementalSynthesizer.h"
#include "Vocoder.h"
#include "../Models/Project.h"
#include "../Utils/AppLogger.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

class EditorController {
public:
  explicit EditorController(bool enableAudioDevice);
  ~EditorController();

  Project *getProject() const { return project.get(); }
  void setProject(std::unique_ptr<Project> newProject);

  AudioEngine *getAudioEngine() const { return audioEngine.get(); }
  Vocoder *getVocoder() const { return vocoder.get(); }
  AudioAnalyzer *getAudioAnalyzer() const { return audioAnalyzer.get(); }
  IncrementalSynthesizer *getIncrementalSynth() const {
    return incrementalSynth.get();
  }
  PlaybackController *getPlaybackController() const {
    return playbackController.get();
  }

  void setPitchDetectorType(PitchDetectorType type) {
    pitchDetectorType = type;
    if (audioAnalyzer)
      audioAnalyzer->setPitchDetectorType(type);
  }

  void setDeviceConfig(const juce::String &deviceName, int gpuDeviceId) {
    device = deviceName;
    deviceId = gpuDeviceId;
    if (vocoder) {
      vocoder->setExecutionDevice(deviceName);
      vocoder->setExecutionDeviceId(gpuDeviceId);
    }
  }

  void reloadInferenceModels(bool async);
  bool isInferenceBusy() const;
  bool isLoading() const { return isLoadingAudio.load(); }
  bool isRendering() const { return isRenderingFlag.load(); }

  using ProgressCallback =
      std::function<void(double, const juce::String &)>;
  using LoadCompleteCallback =
      std::function<void(const juce::AudioBuffer<float> &)>;
  using CancelCallback = std::function<void()>;

  void loadAudioFileAsync(const juce::File &file,
                          const ProgressCallback &onProgress,
                          const LoadCompleteCallback &onComplete,
                          const CancelCallback &onCancelled);
  void setHostAudioAsync(const juce::AudioBuffer<float> &buffer,
                         double sampleRate,
                         const ProgressCallback &onProgress,
                         const LoadCompleteCallback &onComplete);
  void requestCancelLoading();

  void renderProcessedAudioAsync(const Project &project,
                                 float globalPitchOffset,
                                 const std::function<void(bool)> &onComplete);
  void requestCancelRender();

  void resynthesizeIncrementalAsync(
      Project &project,
      const std::function<void(const juce::String &)> &onProgress,
      const std::function<void(bool)> &onComplete,
      std::atomic<bool> &pendingRerun,
      bool isPluginMode);

  void analyzeAudio(Project &targetProject,
                    const std::function<void(double, const juce::String &)>
                        &onProgress,
                    std::function<void()> onComplete = nullptr);

  void segmentIntoNotes(Project &targetProject,
                        std::function<void()> onStreamingUpdate = nullptr);

  void analyzeAudioAsync(
      const std::function<void(Project &)> &onProjectReady,
      const std::function<void()> &onProjectChanged);

  void segmentIntoNotesAsync(
      const std::function<void(Project &)> &onProjectReady,
      const std::function<void()> &onNotesChanged);

  // --- SVC Model ---
  SVCInferenceEngine *getSVCEngine() const { return svcEngine.get(); }
  SVCModelSession *getSVCModel() const { return svcModel.get(); }

  /** Load an SVC voicebank from an extracted directory (voicebanks/<name>/). */
  bool loadSVCModelFromDirectory(const juce::File& voicebankDir);

  /** Load an SVC voicebank (.sfs_model file). Returns true on success. */
  bool loadSVCModel(const juce::File& sfsModelFile);

  /** Unload the current SVC model. */
  void unloadSVCModel();

  /** Check if an SVC model is active. */
  bool isSVCModelActive() const;

  /** Set SVC inference parameters. */
  void setSVCParams(const SVCInferenceEngine::InferenceParams& params) { svcParams = params; }
  const SVCInferenceEngine::InferenceParams& getSVCParams() const { return svcParams; }

  /**
   * Run full SVC conversion on the entire loaded audio.
   * Runs SVC inference -> vocoder -> blend with original -> replaces waveform.
   * Should be called on a background thread.
   */
  using SVCProgressCallback = std::function<void(const juce::String&)>;
  using SVCCompleteCallback = std::function<void(bool success)>;
  void runFullSVCConversionAsync(SVCProgressCallback onProgress,
                                 SVCCompleteCallback onComplete);

  /** Check if an SVC conversion is currently running. */
  bool isSVCConvertingNow() const { return isSVCConverting.load(); }

  /** Cancel any in-progress SVC conversion so a new one can start.
   *  Safe to call from the message thread (non-blocking).
   *  The old conversion thread will finish on its own; its result is discarded. */
  void cancelSVCConversion();

private:
  GPUProvider getProviderFromDevice(const juce::String &device) const;
  Vocoder *ensureVocoder();

  std::unique_ptr<Project> project;
  std::unique_ptr<AudioEngine> audioEngine;
  std::unique_ptr<FCPEPitchDetector> fcpePitchDetector;
  std::unique_ptr<RMVPEPitchDetector> rmvpePitchDetector;
  std::unique_ptr<GAMEDetector> gameDetector;
  std::unique_ptr<HNSepModel> hnsepModel;
  std::unique_ptr<Vocoder> vocoder;
  std::unique_ptr<AudioAnalyzer> audioAnalyzer;
  std::unique_ptr<IncrementalSynthesizer> incrementalSynth;
  std::unique_ptr<PlaybackController> playbackController;
  std::unique_ptr<SVCInferenceEngine> svcEngine;
  std::unique_ptr<SVCModelSession> svcModel;
  SVCInferenceEngine::InferenceParams svcParams;

  juce::File fcpeModelPath;
  juce::File melFilterbankPath;
  juce::File centTablePath;
  juce::File rmvpeModelPath;
  juce::File gameModelDir;
  juce::File hnsepModelDir;

  PitchDetectorType pitchDetectorType = PitchDetectorType::RMVPE;
  juce::String device = "CPU";
  int deviceId = 0;

  std::atomic<bool> isReloadingModels{false};
  std::thread modelReloadThread;

  // Async load state
  std::thread loaderThread;
  std::thread loaderJoinerThread;
  std::atomic<bool> isLoadingAudio{false};
  std::atomic<bool> cancelLoadingFlag{false};
  std::atomic<std::uint64_t> hostAnalysisJobId{0};

  // Async render state
  std::thread renderThread;
  std::atomic<bool> cancelRenderFlag{false};
  std::atomic<bool> isRenderingFlag{false};

  // Async SVC conversion state
  std::thread svcConversionThread;
  std::vector<std::thread> svcOldThreads;  // cancelled threads awaiting join
  std::atomic<bool> isSVCConverting{false};
  std::atomic<bool> cancelSVCFlag{false};
  std::atomic<uint64_t> svcGeneration{0};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorController)
};
