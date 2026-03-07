#include "CenteredMelSpectrogram.h"
#include "Constants.h"
#include <cmath>
#include <algorithm>
#include <numeric>

CenteredMelSpectrogram::CenteredMelSpectrogram(int sampleRate, int nFft, int winSize,
                                               int numMels, float fMin, float fMax)
    : sampleRate(sampleRate), nFft(nFft), winSize(winSize),
      numMels(numMels), fMin(fMin), fMax(fMax),
      fft(static_cast<int>(std::log2(nFft)))
{
    createWindow();
    createMelFilterbank();
}

void CenteredMelSpectrogram::createWindow()
{
    // Create Hann window (periodic, matches librosa default)
    window.resize(winSize);
    for (int i = 0; i < winSize; ++i)
    {
        window[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / winSize));
    }
}

void CenteredMelSpectrogram::createMelFilterbank()
{
    // Slaney-style mel scale (matches librosa default with htk=False)
    const float f_min_mel = 0.0f;
    const float f_sp = 200.0f / 3.0f;  // ~66.67 Hz per mel below 1000 Hz
    const float min_log_hz = 1000.0f;
    const float min_log_mel = (min_log_hz - f_min_mel) / f_sp;  // = 15.0
    const float logstep = std::log(6.4f) / 27.0f;  // ~0.0687

    auto hzToMel = [=](float hz) -> float {
        if (hz < min_log_hz)
            return (hz - f_min_mel) / f_sp;
        else
            return min_log_mel + std::log(hz / min_log_hz) / logstep;
    };

    auto melToHz = [=](float mel) -> float {
        if (mel < min_log_mel)
            return f_min_mel + f_sp * mel;
        else
            return min_log_hz * std::exp(logstep * (mel - min_log_mel));
    };

    float melMin = hzToMel(fMin);
    float melMax = hzToMel(fMax);

    std::vector<float> melPoints(numMels + 2);
    for (int i = 0; i <= numMels + 1; ++i)
    {
        melPoints[i] = melMin + (melMax - melMin) * i / (numMels + 1);
    }

    std::vector<float> hzPoints(numMels + 2);
    for (int i = 0; i <= numMels + 1; ++i)
    {
        hzPoints[i] = melToHz(melPoints[i]);
    }

    int numBins = nFft / 2 + 1;

    melFilterbank.resize(numMels);
    for (int m = 0; m < numMels; ++m)
    {
        melFilterbank[m].resize(numBins, 0.0f);

        float fLow = hzPoints[m];
        float fCenter = hzPoints[m + 1];
        float fHigh = hzPoints[m + 2];

        // Slaney normalization
        float enorm = 2.0f / (fHigh - fLow);

        for (int k = 0; k < numBins; ++k)
        {
            float freq = static_cast<float>(k) * sampleRate / nFft;

            if (freq >= fLow && freq < fCenter)
            {
                melFilterbank[m][k] = enorm * (freq - fLow) / (fCenter - fLow);
            }
            else if (freq >= fCenter && freq <= fHigh)
            {
                melFilterbank[m][k] = enorm * (fHigh - freq) / (fHigh - fCenter);
            }
        }
    }
}

std::vector<float> CenteredMelSpectrogram::computeFrameAtCenter(
    const float* audio, int numSamples, double center)
{
    // Compute single STFT frame centered at given position
    // Uses reflect padding for boundary handling (matches librosa/torch)

    const int halfWin = winSize / 2;
    std::vector<float> frame(nFft * 2, 0.0f);  // Complex FFT buffer

    for (int j = 0; j < winSize; ++j)
    {
        // Calculate source index relative to center
        int srcIdx = static_cast<int>(std::round(center)) - halfWin + j;

        float sample = 0.0f;
        if (srcIdx < 0)
        {
            // Left boundary: reflect
            int reflectIdx = std::min(-srcIdx - 1, numSamples - 1);
            reflectIdx = std::max(0, reflectIdx);
            sample = audio[reflectIdx];
        }
        else if (srcIdx >= numSamples)
        {
            // Right boundary: reflect
            int reflectIdx = numSamples - 1 - (srcIdx - numSamples);
            reflectIdx = std::max(0, reflectIdx);
            sample = audio[reflectIdx];
        }
        else
        {
            sample = audio[srcIdx];
        }

        frame[j] = sample * window[j];
    }

    // Perform FFT
    fft.performRealOnlyForwardTransform(frame.data());

    // Compute magnitude spectrum
    int numBins = nFft / 2 + 1;
    std::vector<float> magnitude(numBins);
    for (int k = 0; k < numBins; ++k)
    {
        float real = frame[k * 2];
        float imag = frame[k * 2 + 1];
        magnitude[k] = std::sqrt(real * real + imag * imag + 1e-9f);
    }

    return magnitude;
}

std::vector<float> CenteredMelSpectrogram::applyMelFilterbank(const std::vector<float>& magnitude)
{
    std::vector<float> mel(numMels);
    int numBins = nFft / 2 + 1;

    for (int m = 0; m < numMels; ++m)
    {
        float sum = 0.0f;
        for (int k = 0; k < numBins; ++k)
        {
            sum += magnitude[k] * melFilterbank[m][k];
        }
        // Log scale (natural log for vocoder compatibility)
        // Use clamp value matching Python: 1e-9
        mel[m] = std::log(std::max(sum, 1e-9f));
    }

    return mel;
}

std::vector<std::vector<float>> CenteredMelSpectrogram::computeAtCenters(
    const float* audio, int numSamples, const std::vector<double>& centers)
{
    if (numSamples == 0 || centers.empty())
        return {};

    std::vector<std::vector<float>> result;
    result.reserve(centers.size());

    for (double center : centers)
    {
        auto magnitude = computeFrameAtCenter(audio, numSamples, center);
        auto mel = applyMelFilterbank(magnitude);
        result.push_back(std::move(mel));
    }

    return result;
}

void CenteredMelSpectrogram::computeTimeStretched(
    const float* globalAudio, int numSamples,
    int startFrame, int endFrame, int newLength,
    std::vector<std::vector<float>>& stretchedMel)
{
    // This function computes time-stretched mel spectrogram using centered STFT
    // Key insight from Python implementation:
    // - We compute STFT at non-uniform positions in the ORIGINAL waveform
    // - This avoids phase artifacts from waveform-domain time stretching

    if (numSamples == 0 || newLength <= 0 || startFrame >= endFrame)
    {
        stretchedMel.clear();
        return;
    }

    const int originalLength = endFrame - startFrame;
    if (originalLength <= 0)
    {
        stretchedMel.clear();
        return;
    }

    // Calculate stretch ratio
    const double stretchRatio = static_cast<double>(newLength) / originalLength;

    // HiFiGAN time offset (from Python: -pad_left + (win_size - 1) // 2 + 1)
    // This aligns mel frames with the vocoder's expectations
    const int padLeft = (winSize - HOP_SIZE) / 2;
    const int timeOffset = -padLeft + (winSize - 1) / 2 + 1;

    // Calculate center positions in original audio for each new frame
    std::vector<double> newCenters(newLength);

    for (int i = 0; i < newLength; ++i)
    {
        // Map new frame index to original frame position (fractional)
        double origFramePos = static_cast<double>(i) / stretchRatio;

        // Convert to sample position in original audio
        // Original frame 'f' corresponds to sample position: f * HOP_SIZE + timeOffset
        double origSamplePos = (startFrame + origFramePos) * HOP_SIZE + timeOffset;

        // Clamp to valid range
        origSamplePos = std::max(0.0, std::min(origSamplePos, static_cast<double>(numSamples - 1)));

        newCenters[i] = origSamplePos;
    }

    // Compute mel spectrogram at new center positions using the GLOBAL waveform
    stretchedMel = computeAtCenters(globalAudio, numSamples, newCenters);
}

std::vector<std::vector<float>> CenteredMelSpectrogram::computeWithSpeedCurve(
    const float* audio, int numSamples,
    int startSample, int endSample,
    const std::vector<float>& speeds, int hopSize)
{
    // This implements the full Python algorithm for non-uniform speed curves
    // speeds[i] is the playback speed at sample i (1.0 = normal, 0.5 = half speed)

    if (numSamples == 0 || speeds.empty() || startSample >= endSample)
        return {};

    const int regionLength = endSample - startSample;
    if (regionLength <= 0 || static_cast<int>(speeds.size()) < regionLength)
        return {};

    // Step 1: Calculate new time axis based on speed curve
    // dt_new = 1.0 / speeds
    // t_new = cumsum(dt_new)
    std::vector<double> tNew(regionLength);
    double cumTime = 0.0;
    for (int i = 0; i < regionLength; ++i)
    {
        float speed = std::max(0.01f, speeds[i]);  // Prevent division by zero
        cumTime += 1.0 / speed;
        tNew[i] = cumTime;
    }

    // Normalize to start from 0
    if (!tNew.empty())
    {
        double offset = tNew[0] - 1.0;
        for (auto& t : tNew)
            t -= offset;
    }

    // Step 2: Calculate number of output frames
    double totalNewTime = tNew.empty() ? 0.0 : tNew.back();
    int numOutputFrames = static_cast<int>(totalNewTime / hopSize) + 1;
    if (numOutputFrames <= 0)
        return {};

    // Step 3: HiFiGAN time offset
    const int padLeft = (winSize - hopSize) / 2;
    const int timeOffset = -padLeft + (winSize - 1) / 2 + 1;

    // Step 4: Build inverse mapping and compute centers
    // For each output frame, find the corresponding position in original audio
    std::vector<double> centers(numOutputFrames);

    for (int i = 0; i < numOutputFrames; ++i)
    {
        // New mel frame time (with offset)
        double newMelTime = i * hopSize + timeOffset;

        // Binary search to find position in tNew
        auto it = std::lower_bound(tNew.begin(), tNew.end(), newMelTime);
        int idx = static_cast<int>(std::distance(tNew.begin(), it));

        double origSample;
        if (idx == 0)
        {
            origSample = startSample;
        }
        else if (idx >= regionLength)
        {
            origSample = endSample - 1;
        }
        else
        {
            // Linear interpolation between tNew[idx-1] and tNew[idx]
            double t0 = tNew[idx - 1];
            double t1 = tNew[idx];
            double alpha = (newMelTime - t0) / (t1 - t0 + 1e-9);
            origSample = startSample + (idx - 1) + alpha;
        }

        centers[i] = origSample;
    }

    return computeAtCenters(audio, numSamples, centers);
}
