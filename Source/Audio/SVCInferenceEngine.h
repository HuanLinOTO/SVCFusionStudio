#pragma once

#include "../JuceHeader.h"
#include "SVCModelSession.h"
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

/**
 * SVC Inference Engine -- the core voice conversion pipeline.
 *
 * Pipeline:
 *   1. Resample audio to 16 kHz
 *   2. Run ContentVec ONNX -> hubert [1, T_units, 768]
 *   3. Align hubert features to model frame rate
 *   4. Extract volume envelope (std-based)
 *   5. Run encoder.onnx -> (x_init, cond)
 *   6. Run Reflow ODE sampling (velocity.onnx, Euler method)
 *   7. Denormalize mel  ->  output mel [T, 128]
 *
 * Model types supported:
 *   0,3,4 = DDSP-SVC 6.0/6.1/6.3
 *   1     = Reflow-VAE-SVC
 *   2     = So-VITS-SVC  (outputs audio directly, no separate vocoder)
 */

/** Parameters for SVC inference. Defined outside SVCInferenceEngine to allow
 *  use as default function arguments on both GCC and Clang (CWG1255). */
struct SVCInferenceParams
{
    int keyShift = 0;                // semitones shift
    int speakerId = 0;               // for multi-speaker models
    int inferSteps = 20;             // Reflow ODE steps
    float tStart = 0.0f;             // Reflow start time (0 = pure noise)
    float silenceThresholdDb = -60.f;
};

class SVCInferenceEngine
{
public:
    SVCInferenceEngine();
    ~SVCInferenceEngine();

    /** Alias so existing SVCInferenceEngine::InferenceParams usages keep working. */
    using InferenceParams = SVCInferenceParams;

    /**
     * Load the ContentVec768L12 ONNX model (shared across all SVC models).
     * Must be called once before any inference.
     * @param onnxPath  Path to contentvec768l12.onnx
     * @param device    Execution device ("CPU", "DirectML", "CUDA")
     * @param deviceId  GPU device ID (0 for default)
     * @return true on success
     */
    bool loadContentVec(const juce::File& onnxPath,
                        const juce::String& device = "CPU",
                        int deviceId = 0);
    void unloadContentVec();
    bool isContentVecLoaded() const;

    /**
     * Run full SVC inference pipeline for DDSP/Reflow models.
     *
     * @param model         Loaded SVC model session (encoder + velocity)
     * @param originalAudio Raw waveform [numSamples] at sampleRate
     * @param numSamples    Number of audio samples
     * @param sampleRate    Sample rate of the audio
     * @param f0            F0 array [numFrames] aligned to model hop rate
     * @param params        Inference parameters
     * @return              SVC mel spectrogram [T][128] for vocoder, or empty on failure
     */
    std::vector<std::vector<float>> infer(
        SVCModelSession& model,
        const float* originalAudio,
        int numSamples,
        int sampleRate,
        const std::vector<float>& f0,
        InferenceParams params = SVCInferenceParams{});

    /**
     * Run So-VITS-SVC inference (outputs audio directly).
     */
    std::vector<float> inferSoVITS(
        SVCModelSession& model,
        const float* originalAudio,
        int numSamples,
        int sampleRate,
        const std::vector<float>& f0,
        InferenceParams params = SVCInferenceParams{});

    /**
     * Run RVC inference (outputs audio directly).
     */
    std::vector<float> inferRVC(
        SVCModelSession& model,
        const float* originalAudio,
        int numSamples,
        int sampleRate,
        const std::vector<float>& f0,
        InferenceParams params = SVCInferenceParams{});

    /**
     * Extract volume envelope from waveform (std-based, matching Python).
     * @param audio     Input waveform
     * @param numSamples Number of samples
     * @param hopSize   Hop size in samples (model block_size)
     * @param winSize   Window size (default 2048)
     */
    static std::vector<float> extractVolume(
        const float* audio, int numSamples, int hopSize, int winSize = 2048);

private:
    // --- ContentVec feature extraction ---
    std::vector<std::vector<float>> extractContentVec(
        const float* audio16k, int numSamples16k);

    // --- Resample helper ---
    static std::vector<float> resampleTo16k(
        const float* audio, int numSamples, int srcSampleRate);

    // --- Align units to target frame count ---
    static std::vector<std::vector<float>> alignUnits(
        const std::vector<std::vector<float>>& units,
        int targetFrames, int hopSize, int sampleRate,
        int encoderHopSize = 320, int encoderSampleRate = 16000);

    // --- DDSP encoder ---
    bool runDDSPEncoder(
        SVCModelSession& model,
        const std::vector<std::vector<float>>& units,
        const std::vector<float>& f0,
        const std::vector<float>& volume,
        int numFrames,
        const InferenceParams& params,
        std::vector<float>& outX,     // [1, 1, 128, T]
        std::vector<float>& outCond); // [1, 128, T]

    // --- Reflow-VAE encoder ---
    bool runReflowEncoder(
        SVCModelSession& model,
        const std::vector<std::vector<float>>& units,
        const std::vector<float>& f0,
        const std::vector<float>& volume,
        int numFrames,
        const InferenceParams& params,
        std::vector<float>& outX,
        std::vector<float>& outCond);

    // --- Reflow ODE sampling (Euler method) ---
    bool runReflowSampling(
        SVCModelSession& model,
        std::vector<float>& x,        // [1, 1, 128, T] -- modified in-place
        const std::vector<float>& cond,
        int melBins, int numFrames,
        int steps, float tStart,
        bool tIsInt64);

    // --- Mel denormalization ---
    static void denormalizeMel(
        std::vector<float>& mel, int melBins, int numFrames,
        float specMin = -12.f, float specMax = 2.f);

    // --- ContentVec ONNX session ---
#ifdef HAVE_ONNXRUNTIME
    std::shared_ptr<Ort::Env> onnxEnv;
    std::unique_ptr<Ort::Session> contentVecSession;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;
    mutable std::mutex inferenceMutex;
#endif
    bool contentVecLoaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SVCInferenceEngine)
};
