/**
 * SVCInferenceEngine.cpp -- Full SVC voice conversion pipeline implementation.
 *
 * Supports:
 *   - DDSP-SVC 6.0 / 6.1 / 6.3  (model_type_index 0, 3, 4)
 *   - Reflow-VAE-SVC             (model_type_index 1)
 *   - So-VITS-SVC                (model_type_index 2)  [TODO]
 */

#include "SVCInferenceEngine.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#include <random>

#ifdef HAVE_ONNXRUNTIME
#ifdef USE_DIRECTML
#include <dml_provider_factory.h>
#endif
#endif

// =======================================================================
// Construction / Destruction
// =======================================================================

SVCInferenceEngine::SVCInferenceEngine()
{
#ifdef HAVE_ONNXRUNTIME
    try
    {
        onnxEnv = std::make_shared<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "SVCInference");
        allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();
    }
    catch (const Ort::Exception& e)
    {
        DBG("SVCInferenceEngine: Failed to init ONNX env: " + juce::String(e.what()));
    }
#endif
}

SVCInferenceEngine::~SVCInferenceEngine()
{
#ifdef HAVE_ONNXRUNTIME
    contentVecSession.reset();
    onnxEnv.reset();
#endif
}

// =======================================================================
// ContentVec Loading
// =======================================================================

bool SVCInferenceEngine::loadContentVec(const juce::File& onnxPath,
                                        const juce::String& device,
                                        int deviceId)
{
#ifdef HAVE_ONNXRUNTIME
    if (!onnxEnv)
        return false;

    if (!onnxPath.existsAsFile())
    {
        LOG("SVCInferenceEngine: ContentVec ONNX not found: " + onnxPath.getFullPathName());
        return false;
    }

    try
    {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(4);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef USE_DIRECTML
        if (device == "DirectML")
        {
            try {
                const OrtApi& ortApi = Ort::GetApi();
                const OrtDmlApi* ortDmlApi = nullptr;
                Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
                    "DML", ORT_API_VERSION,
                    reinterpret_cast<const void**>(&ortDmlApi)));
                opts.DisableMemPattern();
                opts.SetExecutionMode(ORT_SEQUENTIAL);
                Ort::ThrowOnError(
                    ortDmlApi->SessionOptionsAppendExecutionProvider_DML(
                        opts, deviceId));
                LOG("SVCInferenceEngine: ContentVec DirectML EP added (device " +
                    juce::String(deviceId) + ")");
            } catch (const Ort::Exception& e) {
                LOG("SVCInferenceEngine: ContentVec DirectML failed: " +
                    juce::String(e.what()) + " -- falling back to CPU");
            }
        }
        else
#endif
        if (device == "CUDA")
        {
            try {
                OrtCUDAProviderOptions cudaOpts{};
                cudaOpts.device_id = deviceId;
                opts.AppendExecutionProvider_CUDA(cudaOpts);
                LOG("SVCInferenceEngine: ContentVec CUDA EP added");
            } catch (const Ort::Exception& e) {
                LOG("SVCInferenceEngine: ContentVec CUDA failed: " +
                    juce::String(e.what()) + " -- falling back to CPU");
            }
        }

#ifdef _WIN32
        std::wstring wpath(onnxPath.getFullPathName().toWideCharPointer());
        contentVecSession = std::make_unique<Ort::Session>(*onnxEnv, wpath.c_str(), opts);
#else
        contentVecSession = std::make_unique<Ort::Session>(
            *onnxEnv, onnxPath.getFullPathName().toRawUTF8(), opts);
#endif

        contentVecLoaded = true;
        LOG("SVCInferenceEngine: ContentVec loaded on " + device + " (" +
            juce::String(onnxPath.getSize() / 1024 / 1024) + " MB)");
        return true;
    }
    catch (const Ort::Exception& e)
    {
        LOG("SVCInferenceEngine: ContentVec load failed: " + juce::String(e.what()));
        return false;
    }
#else
    juce::ignoreUnused(onnxPath, device, deviceId);
    return false;
#endif
}

bool SVCInferenceEngine::isContentVecLoaded() const
{
    return contentVecLoaded;
}

// =======================================================================
// ContentVec Feature Extraction
// =======================================================================

std::vector<std::vector<float>> SVCInferenceEngine::extractContentVec(
    const float* audio16k, int numSamples16k)
{
#ifdef HAVE_ONNXRUNTIME
    if (!contentVecSession)
        return {};

    try
    {
        auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // Input: source [1, audio_length]
        std::vector<int64_t> inputShape = {1, static_cast<int64_t>(numSamples16k)};
        std::vector<float> inputData(audio16k, audio16k + numSamples16k);

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo, inputData.data(), inputData.size(),
            inputShape.data(), inputShape.size()));

        const char* inputNames[] = {"source"};
        const char* outputNames[] = {"features"};

        auto outputs = contentVecSession->Run(
            Ort::RunOptions{nullptr},
            inputNames, inputs.data(), 1,
            outputNames, 1);

        // Output: features [1, T_units, 768]
        auto& outTensor = outputs[0];
        auto outShape = outTensor.GetTensorTypeAndShapeInfo().GetShape();
        int T = static_cast<int>(outShape[1]);
        int dim = static_cast<int>(outShape[2]); // 768
        const float* outData = outTensor.GetTensorData<float>();

        std::vector<std::vector<float>> features(T, std::vector<float>(dim));
        for (int t = 0; t < T; ++t)
            std::copy(outData + t * dim, outData + (t + 1) * dim, features[t].begin());

        LOG("SVCInferenceEngine: ContentVec extracted [" +
            juce::String(T) + ", " + juce::String(dim) + "]");
        return features;
    }
    catch (const Ort::Exception& e)
    {
        LOG("SVCInferenceEngine: ContentVec inference failed: " + juce::String(e.what()));
        return {};
    }
#else
    juce::ignoreUnused(audio16k, numSamples16k);
    return {};
#endif
}

// =======================================================================
// Audio Resampling (simple linear interpolation to 16 kHz)
// =======================================================================

std::vector<float> SVCInferenceEngine::resampleTo16k(
    const float* audio, int numSamples, int srcSampleRate)
{
    if (srcSampleRate == 16000)
        return std::vector<float>(audio, audio + numSamples);

    double ratio = 16000.0 / srcSampleRate;
    int outLen = static_cast<int>(std::ceil(numSamples * ratio));
    std::vector<float> out(outLen);

    for (int i = 0; i < outLen; ++i)
    {
        double srcIdx = i / ratio;
        int idx0 = static_cast<int>(srcIdx);
        int idx1 = std::min(idx0 + 1, numSamples - 1);
        float frac = static_cast<float>(srcIdx - idx0);
        out[i] = audio[idx0] * (1.f - frac) + audio[idx1] * frac;
    }

    return out;
}

// =======================================================================
// Volume Envelope Extraction (std-based, matches Python)
// =======================================================================

std::vector<float> SVCInferenceEngine::extractVolume(
    const float* audio, int numSamples, int hopSize, int winSize)
{
    // Reflect-pad the audio: left = winSize/2, right = winSize/2
    int padLeft = winSize / 2;
    int padRight = winSize / 2;
    int paddedLen = padLeft + numSamples + padRight;
    std::vector<float> padded(paddedLen);

    // Reflect padding (left)
    for (int i = 0; i < padLeft; ++i)
    {
        int srcIdx = padLeft - i;
        if (srcIdx >= numSamples) srcIdx = numSamples - 1;
        padded[i] = audio[srcIdx];
    }
    // Copy original
    std::copy(audio, audio + numSamples, padded.begin() + padLeft);
    // Reflect padding (right)
    for (int i = 0; i < padRight; ++i)
    {
        int srcIdx = numSamples - 2 - i;
        if (srcIdx < 0) srcIdx = 0;
        padded[padLeft + numSamples + i] = audio[srcIdx];
    }

    // Compute std for each frame
    int numFrames = (numSamples + hopSize - 1) / hopSize;
    std::vector<float> volume(numFrames, 0.f);

    for (int f = 0; f < numFrames; ++f)
    {
        int center = padLeft + f * hopSize;
        int start = center - winSize / 2;
        int end = start + winSize;
        if (end > paddedLen) end = paddedLen;
        if (start < 0) start = 0;
        int n = end - start;
        if (n <= 0) continue;

        // std = sqrt(max(mean(x^2) - mean(x)^2, 0))
        float sumX = 0.f, sumX2 = 0.f;
        for (int i = start; i < end; ++i)
        {
            sumX += padded[i];
            sumX2 += padded[i] * padded[i];
        }
        float meanX = sumX / n;
        float meanX2 = sumX2 / n;
        float var = meanX2 - meanX * meanX;
        volume[f] = std::sqrt(std::max(var, 0.f));
    }

    return volume;
}

// =======================================================================
// Align ContentVec units to model frame rate
// =======================================================================

std::vector<std::vector<float>> SVCInferenceEngine::alignUnits(
    const std::vector<std::vector<float>>& units,
    int targetFrames, int hopSize, int sampleRate,
    int encoderHopSize, int encoderSampleRate)
{
    if (units.empty() || targetFrames <= 0)
        return {};

    int unitDim = static_cast<int>(units[0].size()); // 768
    int numUnits = static_cast<int>(units.size());

    // ratio = (hopSize / sampleRate) / (encoderHopSize / encoderSampleRate)
    double ratio = (static_cast<double>(hopSize) / sampleRate) /
                   (static_cast<double>(encoderHopSize) / encoderSampleRate);

    std::vector<std::vector<float>> aligned(targetFrames, std::vector<float>(unitDim, 0.f));

    for (int i = 0; i < targetFrames; ++i)
    {
        int srcIdx = static_cast<int>(std::round(ratio * i));
        srcIdx = std::clamp(srcIdx, 0, numUnits - 1);
        aligned[i] = units[srcIdx];
    }

    return aligned;
}

// =======================================================================
// DDSP Encoder (model_type_index = 0, 3, 4)
// =======================================================================

bool SVCInferenceEngine::runDDSPEncoder(
    SVCModelSession& model,
    const std::vector<std::vector<float>>& units,
    const std::vector<float>& f0,
    const std::vector<float>& volume,
    int numFrames,
    const InferenceParams& params,
    std::vector<float>& outX,
    std::vector<float>& outCond)
{
#ifdef HAVE_ONNXRUNTIME
    auto* session = model.getEncoderSession();
    if (!session) { LOG("DDSP encoder: no session"); return false; }

    auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto& cfg = model.getConfig();
    int unitDim = 768;
    int hopSize = cfg.blockSize;
    int nSpk = cfg.nSpk;
    int64_t T = numFrames;
    bool multiSpk = (nSpk > 1);

    // --- Flatten units [1, T, 768] ---
    std::vector<float> hubertData(T * unitDim);
    for (int t = 0; t < T; ++t)
        std::copy(units[t].begin(), units[t].end(), hubertData.begin() + t * unitDim);

    // --- mel2ph [1, T] int64 = arange(T) : [0, 1, 2, ..., T-1]  (0-based) ---
    std::vector<int64_t> mel2phData(T);
    std::iota(mel2phData.begin(), mel2phData.end(), 0);

    // --- f0 [1, T] float32 ---
    std::vector<float> f0Data(f0.begin(), f0.begin() + std::min((int)f0.size(), (int)T));
    f0Data.resize(T, 0.f);

    // Apply key shift
    if (params.keyShift != 0)
    {
        float shiftFactor = std::pow(2.f, params.keyShift / 12.f);
        for (auto& v : f0Data)
            if (v > 0.f) v *= shiftFactor;
    }

    // --- volume [1, T] float32 ---
    std::vector<float> volData(volume.begin(), volume.begin() + std::min((int)volume.size(), (int)T));
    volData.resize(T, 0.f);

    // --- randn [1, T * hopSize] float32 ---
    int randLen = static_cast<int>(T) * hopSize;
    std::vector<float> randnData(randLen);
    {
        std::mt19937 gen(42);
        std::normal_distribution<float> dist(0.f, 1.f);
        for (auto& v : randnData) v = dist(gen);
    }

    // --- Build tensors ---
    std::vector<int64_t> hubertShape  = {1, T, unitDim};
    std::vector<int64_t> mel2phShape  = {1, T};
    std::vector<int64_t> f0Shape      = {1, T};
    std::vector<int64_t> volShape     = {1, T};
    std::vector<int64_t> randnShape   = {1, static_cast<int64_t>(randLen)};

    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, hubertData.data(), hubertData.size(), hubertShape.data(), hubertShape.size()));
    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        memInfo, mel2phData.data(), mel2phData.size(), mel2phShape.data(), mel2phShape.size()));
    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, f0Data.data(), f0Data.size(), f0Shape.data(), f0Shape.size()));
    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, volData.data(), volData.size(), volShape.data(), volShape.size()));

    // spk_mix only for multi-speaker models: [1, T, n_spk] (per-frame)
    std::vector<float> spkMixData;
    std::vector<int64_t> spkShape;
    if (multiSpk)
    {
        spkMixData.resize(T * nSpk, 0.f);
        int sid = (params.speakerId >= 0 && params.speakerId < nSpk) ? params.speakerId : 0;
        for (int t = 0; t < T; ++t)
            spkMixData[t * nSpk + sid] = 1.f;
        spkShape = {1, T, static_cast<int64_t>(nSpk)};
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo, spkMixData.data(), spkMixData.size(), spkShape.data(), spkShape.size()));
    }

    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, randnData.data(), randnData.size(), randnShape.data(), randnShape.size()));

    // Input names differ: multi-speaker includes "spk_mix"
    std::vector<const char*> inputNamesVec;
    if (multiSpk)
        inputNamesVec = {"hubert", "mel2ph", "f0", "volume", "spk_mix", "randn"};
    else
        inputNamesVec = {"hubert", "mel2ph", "f0", "volume", "randn"};

    const char* outputNames[] = {"x", "cond"};
    int numInputs = static_cast<int>(inputNamesVec.size());

    LOG("SVCInferenceEngine: DDSP encoder -- T=" + juce::String(T) +
        " nSpk=" + juce::String(nSpk) + " numInputs=" + juce::String(numInputs));

    try
    {
        auto outputs = session->Run(
            Ort::RunOptions{nullptr},
            inputNamesVec.data(), inputs.data(), numInputs,
            outputNames, 2);

        // x: [1, 1, 128, T]
        auto& xTensor = outputs[0];
        auto xShape = xTensor.GetTensorTypeAndShapeInfo().GetShape();
        int melBins = static_cast<int>(xShape[2]);
        int xT = static_cast<int>(xShape[3]);
        const float* xData = xTensor.GetTensorData<float>();
        outX.assign(xData, xData + melBins * xT);

        // cond: [1, 128, T]
        auto& condTensor = outputs[1];
        auto condShape = condTensor.GetTensorTypeAndShapeInfo().GetShape();
        int condDim = static_cast<int>(condShape[1]);
        int condT = static_cast<int>(condShape[2]);
        const float* condData = condTensor.GetTensorData<float>();
        outCond.assign(condData, condData + condDim * condT);

        LOG("SVCInferenceEngine: DDSP encoder OK -- x[1,1," +
            juce::String(melBins) + "," + juce::String(xT) +
            "] cond[1," + juce::String(condDim) + "," + juce::String(condT) + "]");
        return true;
    }
    catch (const Ort::Exception& e)
    {
        LOG("SVCInferenceEngine: DDSP encoder failed: " + juce::String(e.what()));
        return false;
    }
#else
    return false;
#endif
}

// =======================================================================
// Reflow-VAE Encoder (model_type_index = 1)
// =======================================================================

bool SVCInferenceEngine::runReflowEncoder(
    SVCModelSession& model,
    const std::vector<std::vector<float>>& units,
    const std::vector<float>& f0,
    const std::vector<float>& volume,
    int numFrames,
    const InferenceParams& params,
    std::vector<float>& outX,
    std::vector<float>& outCond)
{
#ifdef HAVE_ONNXRUNTIME
    auto* session = model.getEncoderSession();
    if (!session) { LOG("Reflow encoder: no session"); return false; }

    auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto& cfg = model.getConfig();
    int unitDim = 768;
    int64_t T = numFrames;

    // --- units [1, T, 768] ---
    std::vector<float> unitsData(T * unitDim);
    for (int t = 0; t < T; ++t)
        std::copy(units[t].begin(), units[t].end(), unitsData.begin() + t * unitDim);

    // --- f0 [1, T, 1] (3D for Reflow-VAE!) ---
    std::vector<float> f0Data(T);
    for (int t = 0; t < T; ++t)
        f0Data[t] = (t < (int)f0.size()) ? f0[t] : 0.f;

    // Apply key shift
    if (params.keyShift != 0)
    {
        float shiftFactor = std::pow(2.f, params.keyShift / 12.f);
        for (auto& v : f0Data)
            if (v > 0.f) v *= shiftFactor;
    }

    // --- volume [1, T, 1] (3D for Reflow-VAE!) ---
    std::vector<float> volData(T);
    for (int t = 0; t < T; ++t)
        volData[t] = (t < (int)volume.size()) ? volume[t] : 0.f;

    // --- vae_noise [1, T, 128] -- VAE reparametrization noise ---
    int outDims = 128;
    std::vector<float> vaeNoiseData(T * outDims);
    {
        std::mt19937 gen(42);
        std::normal_distribution<float> dist(0.f, 1.f);
        for (auto& v : vaeNoiseData) v = dist(gen);
    }

    // --- Build tensors ---
    std::vector<int64_t> unitsShape    = {1, T, unitDim};
    std::vector<int64_t> f0Shape       = {1, T, 1};
    std::vector<int64_t> volShape      = {1, T, 1};
    std::vector<int64_t> vaeNoiseShape = {1, T, outDims};

    std::vector<Ort::Value> inputs;
    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, unitsData.data(), unitsData.size(), unitsShape.data(), unitsShape.size()));
    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, f0Data.data(), f0Data.size(), f0Shape.data(), f0Shape.size()));
    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, volData.data(), volData.size(), volShape.data(), volShape.size()));
    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, vaeNoiseData.data(), vaeNoiseData.size(), vaeNoiseShape.data(), vaeNoiseShape.size()));

    // Reflow-VAE single-speaker: 4 inputs (units, f0, volume, vae_noise)
    // Output names: x_start and cond (not "x" and "cond")
    const char* inputNames[] = {"units", "f0", "volume", "vae_noise"};
    const char* outputNames[] = {"x_start", "cond"};

    LOG("SVCInferenceEngine: Reflow encoder -- T=" + juce::String(T) +
        " nSpk=" + juce::String(cfg.nSpk));

    try
    {
        auto outputs = session->Run(
            Ort::RunOptions{nullptr},
            inputNames, inputs.data(), 4,
            outputNames, 2);

        // x_start: [1, 1, 128, T]
        auto& xTensor = outputs[0];
        auto xShape = xTensor.GetTensorTypeAndShapeInfo().GetShape();
        int melBins = static_cast<int>(xShape[2]);
        int xT = static_cast<int>(xShape[3]);
        const float* xData = xTensor.GetTensorData<float>();
        outX.assign(xData, xData + melBins * xT);

        // cond: [1, n_hidden, T]  (n_hidden may be 256 for Reflow-VAE)
        auto& condTensor = outputs[1];
        auto condShape = condTensor.GetTensorTypeAndShapeInfo().GetShape();
        int condDim = static_cast<int>(condShape[1]);
        int condT = static_cast<int>(condShape[2]);
        const float* condData = condTensor.GetTensorData<float>();
        outCond.assign(condData, condData + condDim * condT);

        LOG("SVCInferenceEngine: Reflow encoder OK -- x[1,1," +
            juce::String(melBins) + "," + juce::String(xT) +
            "] cond[1," + juce::String(condDim) + "," + juce::String(condT) + "]");
        return true;
    }
    catch (const Ort::Exception& e)
    {
        LOG("SVCInferenceEngine: Reflow encoder failed: " + juce::String(e.what()));
        return false;
    }
#else
    return false;
#endif
}

// =======================================================================
// Reflow ODE Sampling (Euler method)
// =======================================================================

bool SVCInferenceEngine::runReflowSampling(
    SVCModelSession& model,
    std::vector<float>& x,
    const std::vector<float>& cond,
    int melBins, int numFrames,
    int steps, float tStart,
    bool tIsInt64)
{
#ifdef HAVE_ONNXRUNTIME
    auto* session = model.getVelocitySession();
    if (!session) return false;

    auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    float dt = (1.f - tStart) / steps;
    float t = tStart;

    // Rectified Flow: x goes from noise (t=0) to target mel (t=1).
    // The encoder outputs norm_spec(mel) as 'x' and raw mel as 'cond'.
    //
    // When tStart == 0:  start from pure Gaussian noise (encoder x is discarded)
    // When tStart > 0:   mix encoder x with noise:  x = tStart * x + (1-tStart) * noise
    //                    ("shallow diffusion" / partial flow)
    {
        std::mt19937 gen(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::normal_distribution<float> dist(0.f, 1.f);

        if (tStart <= 0.f)
        {
            // Pure noise start -- discard encoder x entirely
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = dist(gen);
        }
        else
        {
            // Shallow flow: blend encoder output with noise
            for (size_t i = 0; i < x.size(); ++i)
            {
                float noise = dist(gen);
                x[i] = tStart * x[i] + (1.f - tStart) * noise;
            }
        }
    }

    // x shape: [1, 1, melBins, numFrames]
    std::vector<int64_t> xShape = {1, 1, static_cast<int64_t>(melBins),
                                    static_cast<int64_t>(numFrames)};
    // cond shape: [1, condDim, numFrames]
    int condDim = static_cast<int>(cond.size()) / numFrames;
    std::vector<int64_t> condShape = {1, static_cast<int64_t>(condDim),
                                       static_cast<int64_t>(numFrames)};
    std::vector<int64_t> tShape = {1};

    const char* inputNames[] = {"x", "t", "cond"};
    const char* outputNames[] = {"o"};

    DBG("SVCInferenceEngine: ODE sampling -- " + juce::String(steps) +
        " steps, dt=" + juce::String(dt, 4) + ", tStart=" + juce::String(tStart, 4) +
        " tIsInt64=" + juce::String(tIsInt64 ? 1 : 0));

    for (int step = 0; step < steps; ++step)
    {
        float tVal = t * 1000.f;

        // Allocate t data in the same scope as session->Run to avoid dangling pointers
        std::vector<int64_t> tDataInt = {static_cast<int64_t>(tVal)};
        std::vector<float> tDataFloat = {tVal};

        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo, x.data(), x.size(), xShape.data(), xShape.size()));

        if (tIsInt64)
        {
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memInfo, tDataInt.data(), tDataInt.size(), tShape.data(), tShape.size()));
        }
        else
        {
            inputs.push_back(Ort::Value::CreateTensor<float>(
                memInfo, tDataFloat.data(), tDataFloat.size(), tShape.data(), tShape.size()));
        }

        // Need a mutable copy of cond for the tensor
        std::vector<float> condCopy(cond);
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo, condCopy.data(), condCopy.size(), condShape.data(), condShape.size()));

        try
        {
            auto outputs = session->Run(
                Ort::RunOptions{nullptr},
                inputNames, inputs.data(), 3,
                outputNames, 1);

            // velocity output: same shape as x
            const float* vel = outputs[0].GetTensorData<float>();
            for (size_t i = 0; i < x.size(); ++i)
                x[i] += vel[i] * dt;
        }
        catch (const Ort::Exception& e)
        {
            LOG("SVCInferenceEngine: velocity step " + juce::String(step) +
                " failed: " + juce::String(e.what()));
            return false;
        }

        t += dt;
    }

    LOG("SVCInferenceEngine: ODE sampling complete");
    return true;
#else
    return false;
#endif
}

// =======================================================================
// Mel Denormalization
// =======================================================================

void SVCInferenceEngine::denormalizeMel(
    std::vector<float>& mel, int melBins, int numFrames,
    float specMin, float specMax)
{
    // mel is in [-1, 1] -> denormalize to [specMin, specMax]
    // mel_out = (mel + 1) / 2 * (specMax - specMin) + specMin
    // Output is in ln(mel) domain — the same domain the encoder produces and
    // the PC-NSF-HiFiGAN vocoder expects.  No log-base conversion needed.
    float range = specMax - specMin;
    for (auto& v : mel)
        v = (v + 1.f) / 2.f * range + specMin;
}

// =======================================================================
// Main Inference Entry Point
// =======================================================================

std::vector<std::vector<float>> SVCInferenceEngine::infer(
    SVCModelSession& model,
    const float* originalAudio,
    int numSamples,
    int sampleRate,
    const std::vector<float>& f0,
    InferenceParams params)
{
#ifdef HAVE_ONNXRUNTIME
    std::lock_guard<std::mutex> lock(inferenceMutex);

    if (!model.isLoaded())
    {
        LOG("SVCInferenceEngine: Model not loaded");
        return {};
    }

    if (!contentVecLoaded)
    {
        LOG("SVCInferenceEngine: ContentVec not loaded");
        return {};
    }

    auto& cfg = model.getConfig();
    int modelType = cfg.modelTypeIndex;

    // SoVITS (type=2) should use inferSoVITS(), not infer()
    if (modelType == 2)
    {
        LOG("SVCInferenceEngine: infer() called with SoVITS model (type=2) — use inferSoVITS() instead");
        return {};
    }

    int hopSize = cfg.blockSize;
    int modelSR = cfg.sampleRate;
    int melBins = 128;

    // Number of output frames
    int numFrames = static_cast<int>(f0.size());
    if (numFrames <= 0)
    {
        LOG("SVCInferenceEngine: No F0 frames provided");
        return {};
    }

    LOG("SVCInferenceEngine: infer() -- type=" + juce::String(modelType) +
        " frames=" + juce::String(numFrames) +
        " sr=" + juce::String(sampleRate) + "->" + juce::String(modelSR) +
        " hop=" + juce::String(hopSize) +
        " samples=" + juce::String(numSamples));

    auto tTotal = std::chrono::steady_clock::now();
    auto tStep  = tTotal;

    // --- Step 1: Resample to 16 kHz for ContentVec ---
    auto audio16k = resampleTo16k(originalAudio, numSamples, sampleRate);
    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] Resample 16kHz: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms (" + juce::String(audio16k.size()) + " samples)");
      tStep = now; }

    // --- Step 2: Extract ContentVec features ---
    auto hubertRaw = extractContentVec(audio16k.data(), static_cast<int>(audio16k.size()));
    if (hubertRaw.empty())
    {
        LOG("SVCInferenceEngine: ContentVec extraction failed");
        return {};
    }
    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] ContentVec: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
      tStep = now; }

    // --- Step 3: Align units to model frame rate ---
    auto units = alignUnits(hubertRaw, numFrames, hopSize, modelSR);
    LOG("SVCInferenceEngine: Aligned units [" + juce::String(units.size()) + ", 768]");

    // --- Step 4: Extract volume envelope ---
    auto volume = extractVolume(originalAudio, numSamples, hopSize);
    // Ensure volume has correct number of frames
    volume.resize(numFrames, 0.f);
    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] Volume extraction: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
      tStep = now; }

    // --- Step 5: Run encoder ---
    std::vector<float> x, cond;

    if (modelType == 1) // Reflow-VAE-SVC
    {
        if (!runReflowEncoder(model, units, f0, volume, numFrames, params, x, cond))
            return {};
    }
    else // DDSP-SVC 6.0/6.1/6.3
    {
        if (!runDDSPEncoder(model, units, f0, volume, numFrames, params, x, cond))
            return {};
    }
    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] Encoder: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
      tStep = now; }

    // --- Step 6: Reflow ODE sampling ---
    bool tIsInt64 = model.isVelocityTInt64();
    if (!runReflowSampling(model, x, cond, melBins, numFrames,
                           params.inferSteps, params.tStart, tIsInt64))
        return {};
    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] ODE sampling (" + juce::String(params.inferSteps) + " steps): " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
      tStep = now; }

    // --- Step 7: Denormalize mel ---
    denormalizeMel(x, melBins, numFrames);

    // --- Step 8: Reshape from [melBins * T] to [T][melBins] ---
    // x is [1, 1, 128, T] flattened -> we read as [128][T] then transpose to [T][128]
    std::vector<std::vector<float>> mel(numFrames, std::vector<float>(melBins));
    for (int t = 0; t < numFrames; ++t)
        for (int m = 0; m < melBins; ++m)
            mel[t][m] = x[m * numFrames + t];

    // --- Apply silence masking ---
    float silenceThresh = std::pow(10.f, params.silenceThresholdDb / 20.f);
    for (int t = 0; t < numFrames; ++t)
    {
        if (t < (int)volume.size() && volume[t] < silenceThresh)
        {
            // Set mel to very low values (silence) in ln domain
            std::fill(mel[t].begin(), mel[t].end(), -18.f);
        }
    }

    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] Total infer(): " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tTotal).count()) + " ms");
    }

    LOG("SVCInferenceEngine: Inference complete -- mel[" +
        juce::String(numFrames) + "][" + juce::String(melBins) + "]");
    return mel;
#else
    juce::ignoreUnused(model, originalAudio, numSamples, sampleRate, f0, params);
    return {};
#endif
}

// =======================================================================
// So-VITS-SVC Inference (outputs audio directly)
// =======================================================================

std::vector<float> SVCInferenceEngine::inferSoVITS(
    SVCModelSession& model,
    const float* originalAudio,
    int numSamples,
    int sampleRate,
    const std::vector<float>& f0,
    InferenceParams params)
{
#ifdef HAVE_ONNXRUNTIME
    std::lock_guard<std::mutex> lock(inferenceMutex);

    if (!model.isLoaded() || !contentVecLoaded)
        return {};

    auto& cfg = model.getConfig();
    int hopSize = cfg.blockSize;
    int modelSR = cfg.sampleRate;
    int numFrames = static_cast<int>(f0.size());
    if (numFrames <= 0) return {};

    LOG("SVCInferenceEngine: inferSoVITS() -- frames=" + juce::String(numFrames) +
        " sr=" + juce::String(sampleRate) + " hop=" + juce::String(hopSize) +
        " samples=" + juce::String(numSamples));

    auto tTotal = std::chrono::steady_clock::now();
    auto tStep  = tTotal;

    // --- ContentVec features ---
    auto audio16k = resampleTo16k(originalAudio, numSamples, sampleRate);
    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] SoVITS Resample 16kHz: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
      tStep = now; }

    auto hubertRaw = extractContentVec(audio16k.data(), static_cast<int>(audio16k.size()));
    if (hubertRaw.empty()) return {};
    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] SoVITS ContentVec: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
      tStep = now; }

    auto units = alignUnits(hubertRaw, numFrames, hopSize, modelSR);

    // --- Extract volume envelope for vol input ---
    auto volume = extractVolume(originalAudio, numSamples, hopSize);
    volume.resize(numFrames, 0.f);

    // --- Prepare inputs for sovits.onnx ---
    auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    int64_t T = numFrames;
    int unitDim = 768;

    // c: [1, T, 768]
    std::vector<float> cData(T * unitDim);
    for (int t = 0; t < T; ++t)
        std::copy(units[t].begin(), units[t].end(), cData.begin() + t * unitDim);

    // f0: [1, T]
    std::vector<float> f0Data(f0.begin(), f0.begin() + std::min((int)f0.size(), (int)T));
    f0Data.resize(T, 0.f);
    if (params.keyShift != 0)
    {
        float shiftFactor = std::pow(2.f, params.keyShift / 12.f);
        for (auto& v : f0Data)
            if (v > 0.f) v *= shiftFactor;
    }

    // mel2ph: [1, T] int64 = arange(T) + 1
    std::vector<int64_t> mel2phData(T);
    std::iota(mel2phData.begin(), mel2phData.end(), 1);

    // uv: [1, T] float32 -- voiced = 1, unvoiced = 0
    std::vector<float> uvData(T);
    for (int t = 0; t < T; ++t)
        uvData[t] = (f0Data[t] > 0.f) ? 1.f : 0.f;

    // noise: [1, 192, T] * 0.35
    int interChannels = 192;
    std::vector<float> noiseData(interChannels * T);
    {
        std::mt19937 gen(42);
        std::normal_distribution<float> dist(0.f, 0.35f);
        for (auto& v : noiseData) v = dist(gen);
    }

    // sid: [1] int64 -- speaker ID
    std::vector<int64_t> sidData = {static_cast<int64_t>(params.speakerId)};

    // vol: [1, T] float32 -- volume envelope
    std::vector<float> volData(volume.begin(), volume.begin() + std::min((int)volume.size(), (int)T));
    volData.resize(T, 0.f);

    // Build tensors
    std::vector<int64_t> cShape      = {1, T, unitDim};
    std::vector<int64_t> f0Shape     = {1, T};
    std::vector<int64_t> mel2phShape = {1, T};
    std::vector<int64_t> uvShape     = {1, T};
    std::vector<int64_t> noiseShape  = {1, static_cast<int64_t>(interChannels), T};
    std::vector<int64_t> sidShape    = {1};
    std::vector<int64_t> volShape    = {1, T};

    auto* session = model.getSovitsSession();
    if (!session) { LOG("SVCInferenceEngine: SoVITS session null"); return {}; }

    // Detect which inputs the ONNX model actually expects
    // Some SoVITS models have 7 inputs (c, f0, mel2ph, uv, noise, sid, vol)
    // while others (vol_embedding=false) have only 6 (no vol).
    size_t numModelInputs = session->GetInputCount();
    bool hasVolInput = false;
    {
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < numModelInputs; ++i) {
            auto namePtr = session->GetInputNameAllocated(i, allocator);
            if (std::string(namePtr.get()) == "vol") {
                hasVolInput = true;
                break;
            }
        }
    }
    LOG("SVCInferenceEngine: SoVITS model has " + juce::String((int)numModelInputs)
        + " inputs, hasVol=" + juce::String(hasVolInput ? "true" : "false"));

    // Build input tensors dynamically based on model requirements
    std::vector<Ort::Value> inputs;
    std::vector<const char*> inputNameVec;

    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, cData.data(), cData.size(), cShape.data(), cShape.size()));
    inputNameVec.push_back("c");

    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, f0Data.data(), f0Data.size(), f0Shape.data(), f0Shape.size()));
    inputNameVec.push_back("f0");

    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        memInfo, mel2phData.data(), mel2phData.size(), mel2phShape.data(), mel2phShape.size()));
    inputNameVec.push_back("mel2ph");

    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, uvData.data(), uvData.size(), uvShape.data(), uvShape.size()));
    inputNameVec.push_back("uv");

    inputs.push_back(Ort::Value::CreateTensor<float>(
        memInfo, noiseData.data(), noiseData.size(), noiseShape.data(), noiseShape.size()));
    inputNameVec.push_back("noise");

    inputs.push_back(Ort::Value::CreateTensor<int64_t>(
        memInfo, sidData.data(), sidData.size(), sidShape.data(), sidShape.size()));
    inputNameVec.push_back("sid");

    if (hasVolInput) {
        inputs.push_back(Ort::Value::CreateTensor<float>(
            memInfo, volData.data(), volData.size(), volShape.data(), volShape.size()));
        inputNameVec.push_back("vol");
    }

    const char* outputNames[] = {"audio"};
    size_t numInputs = inputs.size();

    { auto now = std::chrono::steady_clock::now();
      LOG("  [Timer] SoVITS prepare tensors: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
      tStep = now; }

    try
    {
        auto outputs = session->Run(
            Ort::RunOptions{nullptr},
            inputNameVec.data(), inputs.data(), numInputs,
            outputNames, 1);

        auto& outTensor = outputs[0];
        auto outShape = outTensor.GetTensorTypeAndShapeInfo().GetShape();
        int audioLen = 1;
        for (auto d : outShape) audioLen *= static_cast<int>(d);
        const float* outData = outTensor.GetTensorData<float>();

        { auto now = std::chrono::steady_clock::now();
          LOG("  [Timer] SoVITS ONNX Run: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tStep).count()) + " ms");
          LOG("  [Timer] SoVITS Total: " + juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(now - tTotal).count()) + " ms"); }

        LOG("SVCInferenceEngine: SoVITS OK -- audio[" + juce::String(audioLen) + "]");
        return std::vector<float>(outData, outData + audioLen);
    }
    catch (const Ort::Exception& e)
    {
        LOG("SVCInferenceEngine: SoVITS failed: " + juce::String(e.what()));
        return {};
    }
#else
    juce::ignoreUnused(model, originalAudio, numSamples, sampleRate, f0, params);
    return {};
#endif
}
