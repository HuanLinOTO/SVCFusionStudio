#include "HNSepModel.h"
#include "../Utils/AppLogger.h"

#include <algorithm>
#include <chrono>
#include <thread>

HNSepModel::HNSepModel() = default;

HNSepModel::~HNSepModel() = default;

bool HNSepModel::loadModel(const juce::File &modelPath,
                           GPUProvider provider, int deviceId) {
#ifdef HAVE_ONNXRUNTIME
  try {
    GPUProvider effectiveProvider = provider;
#if defined(_WIN32) && defined(USE_DIRECTML)
    if (provider == GPUProvider::DirectML) {
      // This model is currently unstable on DirectML and can saturate system
      // resources during separation. Keep the rest of the stack on DML, but
      // run HNSep on CPU until a bounded DML path exists.
      LOG("HNSep: DirectML requested, forcing CPU fallback for stability");
      effectiveProvider = GPUProvider::CPU;
    }
#endif

    onnxEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                         "HNSepModel");

    Ort::SessionOptions sessionOptions;
    sessionOptions.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOptions.EnableCpuMemArena();

    if (effectiveProvider == GPUProvider::CPU) {
      const int numThreads =
          std::max(1u, std::thread::hardware_concurrency()) / 2;
      sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
      sessionOptions.EnableMemPattern();
    } else {
      sessionOptions.SetIntraOpNumThreads(1);
      sessionOptions.SetInterOpNumThreads(1);
    }

#if defined(_WIN32) && defined(USE_DIRECTML)
    if (effectiveProvider == GPUProvider::DirectML) {
      try {
        const OrtApi &ortApi = Ort::GetApi();
        const OrtDmlApi *ortDmlApi = nullptr;
        Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
            "DML", ORT_API_VERSION,
            reinterpret_cast<const void **>(&ortDmlApi)));

        sessionOptions.DisableMemPattern();
        sessionOptions.SetExecutionMode(ORT_SEQUENTIAL);

        Ort::ThrowOnError(ortDmlApi->SessionOptionsAppendExecutionProvider_DML(
            sessionOptions, deviceId));
      } catch (const Ort::Exception &e) {
        LOG("HNSep: DirectML provider failed, falling back to CPU: " +
            juce::String(e.what()));
      }
    } else
#endif
#ifdef USE_CUDA
        if (effectiveProvider == GPUProvider::CUDA) {
      try {
        OrtCUDAProviderOptions cudaOptions;
        cudaOptions.device_id = deviceId;
        cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
        cudaOptions.arena_extend_strategy = 1;
        sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
      } catch (const Ort::Exception &e) {
        LOG("HNSep: CUDA provider failed, falling back to CPU: " +
            juce::String(e.what()));
      }
    } else
#endif
        if (effectiveProvider == GPUProvider::CoreML) {
      try {
        sessionOptions.AppendExecutionProvider("CoreML",
                                               {{"MLComputeUnits", "ALL"}});
      } catch (const Ort::Exception &e) {
        LOG("HNSep: CoreML provider failed, falling back to CPU: " +
            juce::String(e.what()));
      }
    } else if (effectiveProvider != GPUProvider::CPU) {
      LOG("HNSep: Unsupported provider, using CPU");
    }

#ifdef _WIN32
    std::wstring modelPathW = modelPath.getFullPathName().toWideCharPointer();
    onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathW.c_str(),
                                                 sessionOptions);
#else
    std::string modelPathStr = modelPath.getFullPathName().toStdString();
    onnxSession = std::make_unique<Ort::Session>(*onnxEnv, modelPathStr.c_str(),
                                                 sessionOptions);
#endif

    allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();

    const size_t numInputs = onnxSession->GetInputCount();
    const size_t numOutputs = onnxSession->GetOutputCount();

    inputNameStrings.clear();
    outputNameStrings.clear();
    inputNames.clear();
    outputNames.clear();

    for (size_t index = 0; index < numInputs; ++index) {
      auto namePtr = onnxSession->GetInputNameAllocated(index, *allocator);
      inputNameStrings.push_back(namePtr.get());
    }
    for (size_t index = 0; index < numOutputs; ++index) {
      auto namePtr = onnxSession->GetOutputNameAllocated(index, *allocator);
      outputNameStrings.push_back(namePtr.get());
    }

    for (const auto &name : inputNameStrings)
      inputNames.push_back(name.c_str());
    for (const auto &name : outputNameStrings)
      outputNames.push_back(name.c_str());

    loaded = true;
    LOG("HNSep model loaded successfully (" + juce::String(numInputs) +
        " inputs, " + juce::String(numOutputs) + " outputs)");
    return true;
  } catch (const Ort::Exception &e) {
    LOG("HNSep model load failed (Ort): " + juce::String(e.what()));
  } catch (const std::exception &e) {
    LOG("HNSep model load failed: " + juce::String(e.what()));
  }

  loaded = false;
  return false;
#else
  juce::ignoreUnused(modelPath, provider, deviceId);
  LOG("HNSep: ONNX Runtime not available (HAVE_ONNXRUNTIME not defined)");
  return false;
#endif
}

bool HNSepModel::separateChunk(const float *audio, int numSamples,
                               std::vector<float> &harmonic,
                               std::vector<float> &noise) {
#ifdef HAVE_ONNXRUNTIME
  if (!onnxSession || numSamples <= 0)
    return false;

  waveformShapeScratch[1] = static_cast<int64_t>(numSamples);

  static const auto memoryInfo = Ort::MemoryInfo::CreateCpu(
      OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
  static const Ort::RunOptions runOptions{nullptr};

  std::vector<Ort::Value> inputTensors;
  inputTensors.emplace_back(Ort::Value::CreateTensor<float>(
      memoryInfo, const_cast<float *>(audio), static_cast<size_t>(numSamples),
      waveformShapeScratch.data(), waveformShapeScratch.size()));

  auto startTime = std::chrono::high_resolution_clock::now();
  auto outputs = onnxSession->Run(runOptions, inputNames.data(),
                                  inputTensors.data(), inputTensors.size(),
                                  outputNames.data(), outputNames.size());
  auto endTime = std::chrono::high_resolution_clock::now();
  LOG("HNSep chunk inference: " +
      juce::String(
          std::chrono::duration<double, std::milli>(endTime - startTime)
              .count(),
          1) +
      " ms (" + juce::String(numSamples) + " samples)");

  if (outputs.size() < 2) {
    LOG("HNSep: unexpected output count: " + juce::String(outputs.size()));
    return false;
  }

  float *harmonicPtr = outputs[0].GetTensorMutableData<float>();
  float *noisePtr = outputs[1].GetTensorMutableData<float>();
  auto harmonicShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
  const int outSamples = static_cast<int>(harmonicShape.back());
  const int copyLen = std::min(numSamples, outSamples);

  harmonic.assign(harmonicPtr, harmonicPtr + copyLen);
  noise.assign(noisePtr, noisePtr + copyLen);
  harmonic.resize(numSamples, 0.0f);
  noise.resize(numSamples, 0.0f);
  return true;
#else
  juce::ignoreUnused(audio, numSamples, harmonic, noise);
  return false;
#endif
}

bool HNSepModel::separate(const float *audio, int numSamples,
                          std::vector<float> &harmonic,
                          std::vector<float> &noise) {
  return separateWithProgress(audio, numSamples, harmonic, noise, nullptr);
}

bool HNSepModel::separateWithProgress(
    const float *audio, int numSamples, std::vector<float> &harmonic,
    std::vector<float> &noise,
    std::function<void(double)> progressCallback) {
#ifdef HAVE_ONNXRUNTIME
  if (!loaded || numSamples <= 0)
    return false;

  if (progressCallback)
    progressCallback(0.0);

  if (numSamples <= MAX_CHUNK_SAMPLES) {
    const bool ok = separateChunk(audio, numSamples, harmonic, noise);
    if (progressCallback)
      progressCallback(1.0);
    return ok;
  }

  harmonic.resize(numSamples, 0.0f);
  noise.resize(numSamples, 0.0f);

  int position = 0;
  int chunkIndex = 0;
  const int stride = MAX_CHUNK_SAMPLES - OVERLAP_SAMPLES;

  while (position < numSamples) {
    const int chunkEnd = std::min(position + MAX_CHUNK_SAMPLES, numSamples);
    const int chunkSize = chunkEnd - position;

    std::vector<float> chunkHarmonic;
    std::vector<float> chunkNoise;
    if (!separateChunk(audio + position, chunkSize, chunkHarmonic, chunkNoise))
      return false;

    if (chunkIndex == 0) {
      std::copy(chunkHarmonic.begin(), chunkHarmonic.end(),
                harmonic.begin() + position);
      std::copy(chunkNoise.begin(), chunkNoise.end(), noise.begin() + position);
    } else {
      const int overlapSamples = std::min(OVERLAP_SAMPLES, chunkSize);
      for (int sampleIndex = 0; sampleIndex < overlapSamples; ++sampleIndex) {
        const float alpha = static_cast<float>(sampleIndex) /
                            static_cast<float>(overlapSamples);
        const int destIndex = position + sampleIndex;
        harmonic[destIndex] = harmonic[destIndex] * (1.0f - alpha) +
                              chunkHarmonic[sampleIndex] * alpha;
        noise[destIndex] = noise[destIndex] * (1.0f - alpha) +
                           chunkNoise[sampleIndex] * alpha;
      }

      if (overlapSamples < chunkSize) {
        std::copy(chunkHarmonic.begin() + overlapSamples, chunkHarmonic.end(),
                  harmonic.begin() + position + overlapSamples);
        std::copy(chunkNoise.begin() + overlapSamples, chunkNoise.end(),
                  noise.begin() + position + overlapSamples);
      }
    }

    position += stride;
    ++chunkIndex;

    if (progressCallback) {
      const double progress =
          static_cast<double>(std::min(position, numSamples)) /
          static_cast<double>(numSamples);
      progressCallback(progress);
    }
  }

  if (progressCallback)
    progressCallback(1.0);
  return true;
#else
  juce::ignoreUnused(audio, numSamples, harmonic, noise, progressCallback);
  return false;
#endif
}