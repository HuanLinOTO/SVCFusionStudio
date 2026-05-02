#pragma once

#include "../JuceHeader.h"
#include <vector>

/**
 * Applies voicing, breath, and tension edits to a harmonic-noise separated
 * segment, then recombines the result into a waveform suitable for mel regen.
 */
class TensionProcessor {
public:
  TensionProcessor();
  ~TensionProcessor() = default;

  std::vector<float> processSegment(const float *harmonicData,
                                    const float *noiseData,
                                    int numSamples,
                                    const float *voicingCurve,
                                    const float *breathCurve,
                                    const float *tensionCurve,
                                    int numFrames) const;

  bool hasActiveEdits(const float *voicingCurve,
                      const float *breathCurve,
                      const float *tensionCurve,
                      int numFrames) const;

private:
  static constexpr int kFFTSize = 2048;
  static constexpr int kHopSize = 512;
  static constexpr int kWinSize = 2048;
  static constexpr int kSampleRate = 44100;
  static constexpr int kFFTBin = kFFTSize / 2 + 1;

  std::vector<float> hannWindow;

  std::vector<float> preEmphasisBaseTensionSegment(
      const std::vector<float> &scaledHarmonic, const float *tensionCurve,
      int numFrames) const;
  void forwardFFT(const float *frame, float *outReal, float *outImag) const;
  void inverseFFT(const float *inReal, const float *inImag,
                  float *outFrame) const;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TensionProcessor)
};