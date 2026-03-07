#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/**
 * Audio segment produced by the slicer.
 * Represents a contiguous region of audio to be processed by SVC inference.
 */
struct AudioSegment {
    int startFrame;   ///< Frame offset in model-hop units (for F0 slicing)
    int startSample;  ///< Sample offset in the original audio waveform
    int numSamples;   ///< Number of audio samples in this segment
};

/**
 * RMS-based audio slicer for chunked SVC inference.
 *
 * Splits audio at silence boundaries so that each chunk can be processed
 * independently through the SVC pipeline (ContentVec -> encoder -> ODE ->
 * vocoder). This dramatically reduces peak memory usage and improves
 * throughput on GPU (DirectML / CUDA).
 *
 * Algorithm based on openvpi/audio-slicer, as used in SVCFusion Python.
 */
class AudioSlicer {
public:
    /**
     * Slice audio at silence boundaries.
     *
     * @param audio         Raw mono audio samples
     * @param numSamples    Total number of audio samples
     * @param sampleRate    Audio sample rate (Hz)
     * @param modelHopSize  Model hop size in samples (for frame alignment)
     * @param thresholdDb   RMS silence threshold in dB (default -40)
     * @param minLengthMs   Minimum segment length in ms (default 5000)
     * @param minIntervalMs Minimum silence length to trigger a cut (default 300)
     * @param hopSizeMs     RMS analysis hop in ms (default 20)
     * @param maxSilKeptMs  Max silence retained at segment edges (default 1000)
     * @return Ordered list of non-empty audio segments
     */
    static std::vector<AudioSegment> slice(
        const float* audio, int numSamples, int sampleRate, int modelHopSize,
        float thresholdDb   = -40.f,
        int   minLengthMs   = 5000,
        int   minIntervalMs = 300,
        int   hopSizeMs     = 20,
        int   maxSilKeptMs  = 1000);

    /**
     * Cross-fade two audio buffers.
     *
     * @param a   Accumulated result audio
     * @param b   New segment to append/overlap
     * @param idx Position in result where cross-fade begins.
     *            The overlap length is  a.size() - idx.
     * @return    Cross-faded result of length  idx + b.size()
     */
    static std::vector<float> crossFade(
        const std::vector<float>& a,
        const std::vector<float>& b,
        int idx);

private:
    /// Compute RMS energy per frame.
    static std::vector<float> computeRMS(
        const float* audio, int numSamples, int hopSize, int winSize);
};
