#include "TensionProcessor.h"

#include <algorithm>
#include <cmath>

namespace {
int bitReverse(int value, int log2n) {
  int result = 0;
  for (int index = 0; index < log2n; ++index) {
    result = (result << 1) | (value & 1);
    value >>= 1;
  }
  return result;
}
} // namespace

TensionProcessor::TensionProcessor() {
  hannWindow.resize(static_cast<size_t>(kWinSize));
  const double twoPi = 2.0 * 3.14159265358979323846;
  for (int sampleIndex = 0; sampleIndex < kWinSize; ++sampleIndex) {
    hannWindow[static_cast<size_t>(sampleIndex)] =
        static_cast<float>(0.5 * (1.0 - std::cos(twoPi * sampleIndex / kWinSize)));
  }
}

bool TensionProcessor::hasActiveEdits(const float *voicingCurve,
                                      const float *breathCurve,
                                      const float *tensionCurve,
                                      int numFrames) const {
  for (int frameIndex = 0; frameIndex < numFrames; ++frameIndex) {
    const float voicing = voicingCurve ? voicingCurve[frameIndex] : 100.0f;
    const float breath = breathCurve ? breathCurve[frameIndex] : 100.0f;
    const float tension = tensionCurve ? tensionCurve[frameIndex] : 0.0f;
    if (std::abs(voicing - 100.0f) > 0.001f ||
        std::abs(breath - 100.0f) > 0.001f ||
        std::abs(tension) > 0.001f) {
      return true;
    }
  }

  return false;
}

std::vector<float> TensionProcessor::processSegment(
    const float *harmonicData, const float *noiseData, int numSamples,
    const float *voicingCurve, const float *breathCurve,
    const float *tensionCurve, int numFrames) const {
  if (numSamples <= 0 || numFrames <= 0)
    return {};

  std::vector<float> scaledHarmonic(static_cast<size_t>(numSamples), 0.0f);
  std::vector<float> scaledNoise(static_cast<size_t>(numSamples), 0.0f);

  bool hasAnyTension = false;
  for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex) {
    const int frame = std::clamp(sampleIndex / kHopSize, 0, numFrames - 1);
    const float voicingPct = voicingCurve ? voicingCurve[frame] : 100.0f;
    const float breathPct = breathCurve ? breathCurve[frame] : 100.0f;
    const float tension = tensionCurve ? tensionCurve[frame] : 0.0f;

    scaledHarmonic[static_cast<size_t>(sampleIndex)] =
        harmonicData[sampleIndex] * (voicingPct / 100.0f);
    scaledNoise[static_cast<size_t>(sampleIndex)] =
        noiseData[sampleIndex] * (breathPct / 100.0f);
    hasAnyTension = hasAnyTension || std::abs(tension) > 0.001f;
  }

  std::vector<float> processedHarmonic =
      hasAnyTension ? preEmphasisBaseTensionSegment(scaledHarmonic,
                                                    tensionCurve, numFrames)
                    : scaledHarmonic;

  std::vector<float> result(static_cast<size_t>(numSamples), 0.0f);
  for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex) {
    result[static_cast<size_t>(sampleIndex)] =
        scaledNoise[static_cast<size_t>(sampleIndex)] +
        processedHarmonic[static_cast<size_t>(sampleIndex)];
  }

  return result;
}

std::vector<float> TensionProcessor::preEmphasisBaseTensionSegment(
    const std::vector<float> &scaledHarmonic, const float *tensionCurve,
    int numFrames) const {
  const int numSamples = static_cast<int>(scaledHarmonic.size());
  if (numSamples <= 0)
    return {};

  float originalMax = 0.0f;
  double originalEnergySum = 0.0;
  for (float sample : scaledHarmonic) {
    originalMax = std::max(originalMax, std::abs(sample));
    originalEnergySum += static_cast<double>(sample) *
                         static_cast<double>(sample);
  }
  const float originalRms = static_cast<float>(
      std::sqrt(originalEnergySum / std::max(1, numSamples)));

  if (originalMax < 1.0e-10f)
    return scaledHarmonic;

  const float nyquist = static_cast<float>(kSampleRate) / 2.0f;
  const float x0 = static_cast<float>(kFFTBin) / (nyquist / 1500.0f);

  const int stftFrames = (numSamples + kHopSize - 1) / kHopSize;
  const int paddedLen = stftFrames * kHopSize + kWinSize;
  const int offset = kWinSize / 2;

  std::vector<float> padded(static_cast<size_t>(paddedLen), 0.0f);
  for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex) {
    padded[static_cast<size_t>(offset + sampleIndex)] =
        scaledHarmonic[static_cast<size_t>(sampleIndex)];
  }

  std::vector<float> output(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> windowSum(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> frame(static_cast<size_t>(kFFTSize), 0.0f);
  std::vector<float> fftReal(static_cast<size_t>(kFFTBin), 0.0f);
  std::vector<float> fftImag(static_cast<size_t>(kFFTBin), 0.0f);
  std::vector<float> outFrame(static_cast<size_t>(kFFTSize), 0.0f);

  constexpr float maxTiltDb = 12.0f;

  for (int frameIndex = 0; frameIndex < stftFrames; ++frameIndex) {
    const int frameStart = frameIndex * kHopSize;
    const int curveFrame = std::clamp(frameIndex, 0, numFrames - 1);
    const float userTension = tensionCurve ? tensionCurve[curveFrame] : 0.0f;
    const float clampedTension = juce::jlimit(-100.0f, 100.0f, userTension);
    const float b = -clampedTension / 100.0f * maxTiltDb;

    for (int sampleIndex = 0; sampleIndex < kFFTSize; ++sampleIndex) {
      const int index = frameStart + sampleIndex;
      frame[static_cast<size_t>(sampleIndex)] =
          index < paddedLen ? padded[static_cast<size_t>(index)] *
                                 hannWindow[static_cast<size_t>(sampleIndex)]
                           : 0.0f;
    }

    forwardFFT(frame.data(), fftReal.data(), fftImag.data());

    for (int bin = 0; bin < kFFTBin; ++bin) {
      float filterDb = (-b / x0) * static_cast<float>(bin) + b;
      filterDb = juce::jlimit(-maxTiltDb, maxTiltDb, filterDb);
      const float filterGain = std::pow(10.0f, filterDb / 20.0f);
      fftReal[static_cast<size_t>(bin)] *= filterGain;
      fftImag[static_cast<size_t>(bin)] *= filterGain;
    }

    inverseFFT(fftReal.data(), fftImag.data(), outFrame.data());

    for (int sampleIndex = 0; sampleIndex < kFFTSize; ++sampleIndex) {
      const int index = frameStart + sampleIndex;
      if (index >= paddedLen)
        continue;

      const float window = hannWindow[static_cast<size_t>(sampleIndex)];
      output[static_cast<size_t>(index)] +=
          outFrame[static_cast<size_t>(sampleIndex)] * window;
      windowSum[static_cast<size_t>(index)] += window * window;
    }
  }

  for (int sampleIndex = 0; sampleIndex < paddedLen; ++sampleIndex) {
    if (windowSum[static_cast<size_t>(sampleIndex)] > 1.0e-8f)
      output[static_cast<size_t>(sampleIndex)] /=
          windowSum[static_cast<size_t>(sampleIndex)];
  }

  std::vector<float> result(static_cast<size_t>(numSamples), 0.0f);
  for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex) {
    result[static_cast<size_t>(sampleIndex)] =
        output[static_cast<size_t>(offset + sampleIndex)];
  }

  double filteredEnergySum = 0.0;
  float filteredMax = 0.0f;
  for (float sample : result) {
    filteredMax = std::max(filteredMax, std::abs(sample));
    filteredEnergySum += static_cast<double>(sample) *
                         static_cast<double>(sample);
  }
  const float filteredRms = static_cast<float>(
      std::sqrt(filteredEnergySum / std::max(1, numSamples)));

  if (filteredRms > 1.0e-10f) {
    const float scale = originalRms / filteredRms;
    for (float &sample : result)
      sample *= scale;
  }

  float renormalizedMax = 0.0f;
  for (float sample : result)
    renormalizedMax = std::max(renormalizedMax, std::abs(sample));

  if (originalMax > 1.0e-10f && renormalizedMax > originalMax * 1.5f) {
    const float peakScale = (originalMax * 1.5f) / renormalizedMax;
    for (float &sample : result)
      sample *= peakScale;
  }

  juce::ignoreUnused(filteredMax);
  return result;
}

void TensionProcessor::forwardFFT(const float *frame, float *outReal,
                                  float *outImag) const {
  const int log2N = static_cast<int>(
      std::round(std::log2(static_cast<double>(kFFTSize))));

  std::vector<float> re(static_cast<size_t>(kFFTSize));
  std::vector<float> im(static_cast<size_t>(kFFTSize), 0.0f);

  for (int index = 0; index < kFFTSize; ++index) {
    const int reversed = bitReverse(index, log2N);
    re[static_cast<size_t>(reversed)] = frame[index];
  }

  for (int stage = 1; stage <= log2N; ++stage) {
    const int blockSize = 1 << stage;
    const int halfBlock = blockSize >> 1;
    const double angle = -2.0 * 3.14159265358979323846 / blockSize;
    const float wRe = static_cast<float>(std::cos(angle));
    const float wIm = static_cast<float>(std::sin(angle));

    for (int start = 0; start < kFFTSize; start += blockSize) {
      float twiddleRe = 1.0f;
      float twiddleIm = 0.0f;
      for (int index = 0; index < halfBlock; ++index) {
        const size_t u = static_cast<size_t>(start + index);
        const size_t v = static_cast<size_t>(start + index + halfBlock);

        const float tempRe = twiddleRe * re[v] - twiddleIm * im[v];
        const float tempIm = twiddleRe * im[v] + twiddleIm * re[v];

        re[v] = re[u] - tempRe;
        im[v] = im[u] - tempIm;
        re[u] = re[u] + tempRe;
        im[u] = im[u] + tempIm;

        const float nextRe = twiddleRe * wRe - twiddleIm * wIm;
        const float nextIm = twiddleRe * wIm + twiddleIm * wRe;
        twiddleRe = nextRe;
        twiddleIm = nextIm;
      }
    }
  }

  for (int bin = 0; bin < kFFTBin; ++bin) {
    outReal[bin] = re[static_cast<size_t>(bin)];
    outImag[bin] = im[static_cast<size_t>(bin)];
  }
}

void TensionProcessor::inverseFFT(const float *inReal, const float *inImag,
                                  float *outFrame) const {
  const int log2N = static_cast<int>(
      std::round(std::log2(static_cast<double>(kFFTSize))));

  std::vector<float> re(static_cast<size_t>(kFFTSize), 0.0f);
  std::vector<float> im(static_cast<size_t>(kFFTSize), 0.0f);

  for (int bin = 0; bin < kFFTBin; ++bin) {
    re[static_cast<size_t>(bin)] = inReal[bin];
    im[static_cast<size_t>(bin)] = -inImag[bin];
  }

  for (int bin = 1; bin < kFFTBin - 1; ++bin) {
    re[static_cast<size_t>(kFFTSize - bin)] = inReal[bin];
    im[static_cast<size_t>(kFFTSize - bin)] = inImag[bin];
  }

  std::vector<float> rePermuted(static_cast<size_t>(kFFTSize), 0.0f);
  std::vector<float> imPermuted(static_cast<size_t>(kFFTSize), 0.0f);

  for (int index = 0; index < kFFTSize; ++index) {
    const int reversed = bitReverse(index, log2N);
    rePermuted[static_cast<size_t>(reversed)] = re[static_cast<size_t>(index)];
    imPermuted[static_cast<size_t>(reversed)] = im[static_cast<size_t>(index)];
  }

  for (int stage = 1; stage <= log2N; ++stage) {
    const int blockSize = 1 << stage;
    const int halfBlock = blockSize >> 1;
    const double angle = -2.0 * 3.14159265358979323846 / blockSize;
    const float wRe = static_cast<float>(std::cos(angle));
    const float wIm = static_cast<float>(std::sin(angle));

    for (int start = 0; start < kFFTSize; start += blockSize) {
      float twiddleRe = 1.0f;
      float twiddleIm = 0.0f;
      for (int index = 0; index < halfBlock; ++index) {
        const size_t u = static_cast<size_t>(start + index);
        const size_t v = static_cast<size_t>(start + index + halfBlock);

        const float tempRe = twiddleRe * rePermuted[v] - twiddleIm * imPermuted[v];
        const float tempIm = twiddleRe * imPermuted[v] + twiddleIm * rePermuted[v];

        rePermuted[v] = rePermuted[u] - tempRe;
        imPermuted[v] = imPermuted[u] - tempIm;
        rePermuted[u] = rePermuted[u] + tempRe;
        imPermuted[u] = imPermuted[u] + tempIm;

        const float nextRe = twiddleRe * wRe - twiddleIm * wIm;
        const float nextIm = twiddleRe * wIm + twiddleIm * wRe;
        twiddleRe = nextRe;
        twiddleIm = nextIm;
      }
    }
  }

  const float invN = 1.0f / static_cast<float>(kFFTSize);
  for (int index = 0; index < kFFTSize; ++index)
    outFrame[index] = rePermuted[static_cast<size_t>(index)] * invN;
}