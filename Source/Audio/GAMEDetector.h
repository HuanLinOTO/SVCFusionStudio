#pragma once

#include "../JuceHeader.h"
#include "FCPEPitchDetector.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

/**
 * GAME (General Audio-to-MIDI Estimation) note detector.
 *
 * Pipeline:
 *   encoder  -> x_seg, x_est, maskT
 *   segmenter (D3PM loop) -> boundaries
 *   bd2dur   -> durations
 *   estimator -> presence, scores, maskN
 */
class GAMEDetector {
public:
  static constexpr int SAMPLE_RATE = 44100;
  static constexpr int HOP_SIZE = 512;

  struct NoteEvent {
    int startFrame;
    int endFrame;
    float midiNote;
    bool isRest;
  };

  struct ChunkRange {
    int startSample;
    int endSample;
  };

  GAMEDetector();
  ~GAMEDetector();

  bool loadModels(const juce::File& gameDir,
                  GPUProvider provider = GPUProvider::CPU,
                  int deviceId = 0);
  void unload();
  bool isLoaded() const { return loaded; }

  std::vector<NoteEvent> detectNotes(const float* audio, int numSamples,
                                     int sampleRate);
  std::vector<NoteEvent> detectNotesWithProgress(
      const float* audio, int numSamples, int sampleRate,
      std::function<void(double)> progressCallback);

  void setNumD3PMSteps(int steps) { numD3PMSteps = steps; }
  void setSegThreshold(float threshold) { segThreshold = threshold; }
  void setSegRadius(int radius) { segRadius = radius; }
  void setEstThreshold(float threshold) { estThreshold = threshold; }

  int getFrameForSample(int sampleIndex) const { return sampleIndex / HOP_SIZE; }
  int getSampleForFrame(int frameIndex) const { return frameIndex * HOP_SIZE; }

  const std::vector<ChunkRange>& getLastChunkRanges() const {
    return lastChunkRanges;
  }

private:
  bool loaded = false;

  int numD3PMSteps = 8;
  float segThreshold = 0.2f;
  int segRadius = 2;
  float estThreshold = 0.2f;

  int modelSampleRate = 44100;
  float timestep = 0.01f;
  bool supportsLoop = true;
  int embeddingDim = 256;

  bool loadConfig(const juce::File& configPath);
  std::vector<float> resampleToModelRate(const float* audio, int numSamples,
                                         int srcRate);

  static constexpr int MAX_ENCODER_FRAMES = 5000;
  int samplesPerEncoderFrame() const {
    return static_cast<int>(modelSampleRate * timestep);
  }
  int maxChunkSamples() const {
    return MAX_ENCODER_FRAMES * samplesPerEncoderFrame();
  }

  std::vector<ChunkRange> findSilenceChunks(
      const std::vector<float>& waveform) const;
  std::vector<ChunkRange> lastChunkRanges;

  std::vector<NoteEvent> processChunk(
      const std::vector<float>& waveform, int chunkStartSample,
      std::function<void(double)> progressCallback, double progressBase,
      double progressSpan);

#ifdef HAVE_ONNXRUNTIME
  struct ModelSession {
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNameStrings;
    std::vector<std::string> outputNameStrings;
    std::vector<const char*> inputNames;
    std::vector<const char*> outputNames;

    bool load(Ort::Env& env, const juce::File& path,
              const Ort::SessionOptions& options);
    int findInput(const std::string& name) const;
    int findOutput(const std::string& name) const;
  };

  std::unique_ptr<Ort::Env> onnxEnv;
  ModelSession encoder;
  ModelSession segmenter;
  ModelSession estimator;
  ModelSession bd2dur;

  Ort::SessionOptions createSessionOptions(GPUProvider provider, int deviceId);
#endif
};
