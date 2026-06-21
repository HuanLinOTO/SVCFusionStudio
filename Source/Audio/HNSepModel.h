#pragma once

#include "../JuceHeader.h"
#include "FCPEPitchDetector.h"
#include <array>
#include <functional>
#include <memory>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

/**
 * Harmonic-noise separation model.
 *
 * Splits a mono waveform into harmonic and noise components using ONNX Runtime.
 * Long audio is processed in overlapping chunks and blended at chunk boundaries.
 */
class HNSepModel {
public:
  static constexpr int SAMPLE_RATE = 44100;
  static constexpr int MAX_CHUNK_SAMPLES = SAMPLE_RATE * 30;
  static constexpr int DML_PADDED_CHUNK_SAMPLES = SAMPLE_RATE * 10;
  static constexpr int OVERLAP_SAMPLES = SAMPLE_RATE * 1;

  HNSepModel();
  ~HNSepModel();

  bool loadModel(const juce::File &modelPath,
                 GPUProvider provider = GPUProvider::CPU,
                 int deviceId = 0);
  void unload();
  bool isLoaded() const { return loaded; }

  bool separate(const float *audio, int numSamples,
                std::vector<float> &harmonic,
                std::vector<float> &noise);
  bool separateWithProgress(const float *audio, int numSamples,
                            std::vector<float> &harmonic,
                            std::vector<float> &noise,
                            std::function<void(double)> progressCallback);

  struct ChunkTiming {
    double stftMs = 0.0;
    double preMs = 0.0;
    double coreMs = 0.0;
    double postMs = 0.0;
    double totalMs = 0.0;
    int numSamples = 0;
  };
  ChunkTiming lastChunkTiming;

private:
  bool loaded = false;
  GPUProvider activeProvider = GPUProvider::CPU;

  bool separateChunk(const float *audio, int numSamples,
                     std::vector<float> &harmonic,
                     std::vector<float> &noise);
  int getRuntimeMaxChunkSamples() const;
  int getRuntimeOverlapSamples(int maxChunkSamples) const;

#ifdef HAVE_ONNXRUNTIME
  struct SplitSession {
    std::unique_ptr<Ort::Session> session;
    std::vector<const char *> inputNames;
    std::vector<const char *> outputNames;
    std::vector<std::string> inputNameStrings;
    std::vector<std::string> outputNameStrings;
  };

  std::unique_ptr<Ort::Env> onnxEnv;
  std::unique_ptr<Ort::Session> onnxSession;
  std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;
  std::unique_ptr<SplitSession> preSession;
  std::unique_ptr<SplitSession> coreSession;
  std::unique_ptr<SplitSession> postSession;

  std::vector<const char *> inputNames;
  std::vector<const char *> outputNames;
  std::vector<std::string> inputNameStrings;
  std::vector<std::string> outputNameStrings;
  std::array<int64_t, 2> waveformShapeScratch{1, 0};
  std::array<int64_t, 3> stftConvShapeScratch{1, 2050, 0};
  std::unique_ptr<juce::dsp::FFT> stftFft;
  std::vector<float> stftWindow;
  std::vector<float> stftFrameScratch;
  std::vector<float> stftConvScratch;
  std::vector<float> paddedAudioScratch;
  bool splitPreUsesExternalStft = false;
  static constexpr int STFT_FFT_SIZE = 2048;
  static constexpr int STFT_HOP_SIZE = 512;
  static constexpr int STFT_NUM_BINS = STFT_FFT_SIZE / 2 + 1;
  static constexpr int STFT_NUM_CHANNELS = STFT_NUM_BINS * 2;

  bool splitPipelineEnabled = false;
  int computeExternalStftFrameCount(int numSamples) const;
  bool computeExternalConvStft(const float *audio, int numSamples,
                               std::vector<float> &stftConv);
  bool loadSingleSessionModel(const juce::File &modelPath,
                              GPUProvider provider, int deviceId);
  bool loadSplitPipelineModels(const juce::File &modelPath,
                               GPUProvider provider, int deviceId);
  bool initializeSessionMetadata(Ort::Session &session,
                                 std::vector<std::string> &inputNameStrings,
                                 std::vector<const char *> &inputNames,
                                 std::vector<std::string> &outputNameStrings,
                                 std::vector<const char *> &outputNames);
#endif

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HNSepModel)
};
