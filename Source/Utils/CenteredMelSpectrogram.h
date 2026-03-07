#pragma once

#include "../JuceHeader.h"
#include <vector>

/**
 * Centered Mel Spectrogram computation.
 *
 * Unlike standard STFT which uses fixed hop_size intervals,
 * this implementation allows computing STFT at arbitrary center positions.
 * This is essential for high-quality time stretching without phase artifacts.
 *
 * Based on the centered_stft algorithm from:
 * https://github.com/openvpi/SingingVocoders
 *
 * Key concept:
 * - Standard STFT: frames at positions 0, hop, 2*hop, 3*hop...
 * - Centered STFT: frames at arbitrary positions specified by 'centers' array
 *
 * For time stretching:
 * 1. Given speed curve, compute new time axis: t_new = cumsum(1/speeds)
 * 2. Inverse map: new time -> original time
 * 3. Use centered STFT at non-uniform positions in original audio
 * 4. Result is time-stretched mel spectrogram without phase artifacts
 */
class CenteredMelSpectrogram
{
public:
    CenteredMelSpectrogram(int sampleRate = 44100, int nFft = 2048, int winSize = 2048,
                           int numMels = 128, float fMin = 40.0f, float fMax = 16000.0f);
    ~CenteredMelSpectrogram() = default;

    /**
     * Compute mel spectrogram at specified center positions.
     * @param audio Audio samples (GLOBAL waveform)
     * @param numSamples Number of samples
     * @param centers Center positions (in samples) for each frame
     * @return Mel spectrogram [T, numMels] in log scale
     */
    std::vector<std::vector<float>> computeAtCenters(const float* audio, int numSamples,
                                                      const std::vector<double>& centers);

    /**
     * Compute time-stretched mel spectrogram for a note region.
     * Uses the GLOBAL waveform and computes STFT at non-uniform positions.
     *
     * @param globalAudio Global audio samples (full waveform)
     * @param numSamples Total number of samples in global audio
     * @param startFrame Start frame of the note in original mel
     * @param endFrame End frame of the note in original mel
     * @param newLength Target length in frames after stretching
     * @param stretchedMel Output: stretched mel spectrogram [newLength, numMels]
     */
    void computeTimeStretched(const float* globalAudio, int numSamples,
                              int startFrame, int endFrame, int newLength,
                              std::vector<std::vector<float>>& stretchedMel);

    /**
     * Compute time-stretched mel spectrogram with non-uniform speed.
     * @param audio Audio samples
     * @param numSamples Number of samples
     * @param startSample Start sample in audio
     * @param endSample End sample in audio
     * @param speeds Speed values for each sample (1.0 = normal, 0.5 = half speed)
     * @param hopSize Hop size for output frames
     * @return Mel spectrogram [T_new, numMels] in log scale
     */
    std::vector<std::vector<float>> computeWithSpeedCurve(
        const float* audio, int numSamples,
        int startSample, int endSample,
        const std::vector<float>& speeds, int hopSize);

    int getNumMels() const { return numMels; }
    int getNFft() const { return nFft; }
    int getWinSize() const { return winSize; }

private:
    void createMelFilterbank();
    void createWindow();

    /**
     * Compute single STFT frame centered at given position.
     * Uses reflect padding for boundary handling.
     */
    std::vector<float> computeFrameAtCenter(const float* audio, int numSamples, double center);

    /**
     * Apply mel filterbank and log compression to magnitude spectrum.
     */
    std::vector<float> applyMelFilterbank(const std::vector<float>& magnitude);

    int sampleRate;
    int nFft;
    int winSize;
    int numMels;
    float fMin;
    float fMax;

    std::vector<float> window;  // Hann window
    std::vector<std::vector<float>> melFilterbank;

    juce::dsp::FFT fft;
};
