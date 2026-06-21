#include "HNSepModel.h"
#include "../Utils/AppLogger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#ifdef _WIN32
#include <dxgi1_2.h>
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
juce::StringArray getDxgiAdapterNamesForLogging() {
  juce::StringArray names;
  IDXGIFactory1 *factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void **>(&factory))) ||
      factory == nullptr) {
    return names;
  }

  for (UINT i = 0;; ++i) {
    IDXGIAdapter1 *adapter = nullptr;
    const auto hr = factory->EnumAdapters1(i, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(hr) || adapter == nullptr)
      continue;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
      names.add(juce::String(desc.Description));
    }
    adapter->Release();
  }

  factory->Release();
  return names;
}

bool isLikelyVirtualAdapter(const juce::String &name) {
  return name.containsIgnoreCase("virtual") || name.containsIgnoreCase("parsec") ||
         name.containsIgnoreCase("qdesk") || name.containsIgnoreCase("gameviewer") ||
         name.containsIgnoreCase("oray");
}
#endif
} // namespace

HNSepModel::HNSepModel() = default;

HNSepModel::~HNSepModel() = default;

int HNSepModel::getRuntimeMaxChunkSamples() const {
  return MAX_CHUNK_SAMPLES;
}

int HNSepModel::getRuntimeOverlapSamples(int maxChunkSamples) const {
  return std::min(OVERLAP_SAMPLES, std::max(0, maxChunkSamples / 3));
}

int HNSepModel::computeExternalStftFrameCount(int numSamples) const {
  const int coarseFrames = numSamples / STFT_HOP_SIZE + 1;
  return (coarseFrames / 32 + 1) * 32;
}

bool HNSepModel::computeExternalConvStft(const float *audio, int numSamples,
                                         std::vector<float> &stftConv) {
  if (audio == nullptr || numSamples <= 0)
    return false;

  const int numFrames = computeExternalStftFrameCount(numSamples);
  if (numFrames <= 0)
    return false;

  if (!stftFft)
    stftFft = std::make_unique<juce::dsp::FFT>(11);

  if (stftWindow.size() != static_cast<size_t>(STFT_FFT_SIZE)) {
    stftWindow.resize(STFT_FFT_SIZE);
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    for (int index = 0; index < STFT_FFT_SIZE; ++index) {
      stftWindow[static_cast<size_t>(index)] = static_cast<float>(
          0.5 * (1.0 - std::cos(twoPi * static_cast<double>(index) /
                                static_cast<double>(STFT_FFT_SIZE))));
    }
  }

  stftFrameScratch.assign(static_cast<size_t>(STFT_FFT_SIZE * 2), 0.0f);
  stftConv.assign(static_cast<size_t>(STFT_NUM_CHANNELS * numFrames), 0.0f);

  const int targetLengthBeforeCenterPad = (numFrames - 1) * STFT_HOP_SIZE;
  const int totalOuterPad = std::max(0, targetLengthBeforeCenterPad - numSamples);
  const int leftOuterPad = (totalOuterPad / 2 / STFT_HOP_SIZE) * STFT_HOP_SIZE;
  const int totalLeftPad = leftOuterPad + STFT_FFT_SIZE / 2;

  for (int frameIndex = 0; frameIndex < numFrames; ++frameIndex) {
    std::fill(stftFrameScratch.begin(), stftFrameScratch.end(), 0.0f);

    const int sourceStart = frameIndex * STFT_HOP_SIZE - totalLeftPad;
    for (int sampleIndex = 0; sampleIndex < STFT_FFT_SIZE; ++sampleIndex) {
      const int sourceIndex = sourceStart + sampleIndex;
      if (sourceIndex >= 0 && sourceIndex < numSamples) {
        stftFrameScratch[static_cast<size_t>(sampleIndex)] =
            audio[sourceIndex] * stftWindow[static_cast<size_t>(sampleIndex)];
      }
    }

    stftFft->performRealOnlyForwardTransform(stftFrameScratch.data());

    for (int bin = 0; bin < STFT_NUM_BINS; ++bin) {
      stftConv[static_cast<size_t>(bin * numFrames + frameIndex)] =
          stftFrameScratch[static_cast<size_t>(bin * 2)];
      stftConv[static_cast<size_t>((STFT_NUM_BINS + bin) * numFrames + frameIndex)] =
          stftFrameScratch[static_cast<size_t>(bin * 2 + 1)];
    }
  }

  return true;
}

bool HNSepModel::initializeSessionMetadata(
    Ort::Session &session, std::vector<std::string> &sessionInputNameStrings,
    std::vector<const char *> &sessionInputNames,
    std::vector<std::string> &sessionOutputNameStrings,
    std::vector<const char *> &sessionOutputNames) {
#ifdef HAVE_ONNXRUNTIME
  const size_t numInputs = session.GetInputCount();
  const size_t numOutputs = session.GetOutputCount();

  sessionInputNameStrings.clear();
  sessionOutputNameStrings.clear();
  sessionInputNames.clear();
  sessionOutputNames.clear();

  for (size_t index = 0; index < numInputs; ++index) {
    auto namePtr = session.GetInputNameAllocated(index, *allocator);
    sessionInputNameStrings.push_back(namePtr.get());
  }
  for (size_t index = 0; index < numOutputs; ++index) {
    auto namePtr = session.GetOutputNameAllocated(index, *allocator);
    sessionOutputNameStrings.push_back(namePtr.get());
  }

  for (const auto &name : sessionInputNameStrings)
    sessionInputNames.push_back(name.c_str());
  for (const auto &name : sessionOutputNameStrings)
    sessionOutputNames.push_back(name.c_str());

  return true;
#else
  juce::ignoreUnused(session, sessionInputNameStrings, sessionInputNames,
                     sessionOutputNameStrings, sessionOutputNames);
  return false;
#endif
}

bool HNSepModel::loadSingleSessionModel(const juce::File &modelPath,
                                        GPUProvider provider,
                                        int deviceId) {
#ifdef HAVE_ONNXRUNTIME
  try {
    GPUProvider effectiveProvider = provider;
    if (juce::SystemStats::getEnvironmentVariable("SVCFUSION_HNSEP_FORCE_CPU", "0") == "1") {
      LOG("HNSep: SVCFUSION_HNSEP_FORCE_CPU=1, forcing CPU provider");
      effectiveProvider = GPUProvider::CPU;
    }

    int effectiveDeviceId = deviceId;
    const auto hnsepDeviceOverride =
        juce::SystemStats::getEnvironmentVariable("SVCFUSION_HNSEP_DML_DEVICE", "");
    if (effectiveProvider == GPUProvider::DirectML && hnsepDeviceOverride.isNotEmpty()) {
      effectiveDeviceId = hnsepDeviceOverride.getIntValue();
      LOG("HNSep: SVCFUSION_HNSEP_DML_DEVICE override -> device " +
          juce::String(effectiveDeviceId));
    }

    onnxEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                          "HNSepModel");

    Ort::SessionOptions sessionOptions;
    sessionOptions.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);

    if (effectiveProvider == GPUProvider::CPU) {
      sessionOptions.EnableCpuMemArena();
      const int numThreads =
          std::max(1u, std::thread::hardware_concurrency()) / 2;
      sessionOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
      sessionOptions.EnableMemPattern();
    } else {
      sessionOptions.DisableCpuMemArena();
      sessionOptions.SetIntraOpNumThreads(1);
      sessionOptions.SetInterOpNumThreads(1);
    }

#if defined(_WIN32) && defined(USE_DIRECTML)
    if (effectiveProvider == GPUProvider::DirectML) {
      try {
#ifdef _WIN32
        const auto adapterNames = getDxgiAdapterNamesForLogging();
        if (adapterNames.isEmpty()) {
          LOG("HNSep: DirectML adapter enumeration returned no DXGI adapters");
        } else {
          juce::StringArray entries;
          for (int i = 0; i < adapterNames.size(); ++i)
            entries.add("[" + juce::String(i) + "] " + adapterNames[i]);
          LOG("HNSep: DirectML adapters: " + entries.joinIntoString(" | "));

          if (juce::isPositiveAndBelow(effectiveDeviceId, adapterNames.size())) {
            const auto selectedAdapter = adapterNames[effectiveDeviceId];
            LOG("HNSep: selected DirectML adapter [" + juce::String(effectiveDeviceId) +
                "] " + selectedAdapter);
            if (isLikelyVirtualAdapter(selectedAdapter)) {
              LOG("HNSep: WARNING selected DirectML adapter looks virtual; performance may be severely degraded");
            }
          } else {
            LOG("HNSep: selected DirectML adapter index " + juce::String(effectiveDeviceId) +
                " is out of range for enumerated adapters");
          }
        }
#endif

        const OrtApi &ortApi = Ort::GetApi();
        const OrtDmlApi *ortDmlApi = nullptr;
        Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
            "DML", ORT_API_VERSION,
            reinterpret_cast<const void **>(&ortDmlApi)));

        sessionOptions.DisableMemPattern();
        sessionOptions.SetExecutionMode(ORT_SEQUENTIAL);

        Ort::ThrowOnError(ortDmlApi->SessionOptionsAppendExecutionProvider_DML(
            sessionOptions, effectiveDeviceId));
        LOG("HNSep: DirectML execution provider added (device " +
            juce::String(effectiveDeviceId) + ")");
      } catch (const Ort::Exception &e) {
        LOG("HNSep: DirectML provider failed, falling back to CPU: " +
            juce::String(e.what()));
        effectiveProvider = GPUProvider::CPU;
      }
    } else
#endif
#ifdef USE_CUDA
        if (effectiveProvider == GPUProvider::CUDA) {
      try {
        OrtCUDAProviderOptions cudaOptions;
        cudaOptions.device_id = effectiveDeviceId;
        cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
        cudaOptions.arena_extend_strategy = 1;
        sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);
        LOG("HNSep: CUDA execution provider added (device " +
            juce::String(effectiveDeviceId) + ")");
      } catch (const Ort::Exception &e) {
        LOG("HNSep: CUDA provider failed, falling back to CPU: " +
            juce::String(e.what()));
        effectiveProvider = GPUProvider::CPU;
      }
    } else
#endif
        if (effectiveProvider == GPUProvider::CoreML) {
      try {
        sessionOptions.AppendExecutionProvider("CoreML",
                                               {{"MLComputeUnits", "ALL"}});
        LOG("HNSep: CoreML execution provider added");
      } catch (const Ort::Exception &e) {
        LOG("HNSep: CoreML provider failed, falling back to CPU: " +
            juce::String(e.what()));
        effectiveProvider = GPUProvider::CPU;
      }
    } else if (effectiveProvider != GPUProvider::CPU) {
      LOG("HNSep: Unsupported provider, using CPU");
      effectiveProvider = GPUProvider::CPU;
    }

    if (effectiveProvider == GPUProvider::CPU)
      LOG("HNSep: using CPU execution provider");

    activeProvider = effectiveProvider;
    splitPipelineEnabled = false;
    LOG("HNSep: creating session from " + modelPath.getFullPathName() +
        " (runtime chunk " + juce::String(getRuntimeMaxChunkSamples()) +
        " samples)");

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

    initializeSessionMetadata(*onnxSession, inputNameStrings, inputNames,
                              outputNameStrings, outputNames);

    const size_t numInputs = inputNames.size();
    const size_t numOutputs = outputNames.size();

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
  return false;
#endif
}

bool HNSepModel::loadSplitPipelineModels(const juce::File &modelPath,
                                         GPUProvider provider,
                                         int deviceId) {
#ifdef HAVE_ONNXRUNTIME
  try {
    const auto splitDir = modelPath.getSiblingFile("split");
    const auto preNoStftPath = splitDir.getChildFile("hnsep_pre_no_stft.onnx");
    const bool externalStftDisabled =
        juce::SystemStats::getEnvironmentVariable(
            "SVCFUSION_HNSEP_DISABLE_EXTERNAL_STFT", "0") == "1";
    const auto prePath = preNoStftPath.existsAsFile() && !externalStftDisabled
                             ? preNoStftPath
                             : splitDir.getChildFile("hnsep_pre.onnx");
    const auto corePath = splitDir.getChildFile("hnsep_core.onnx");
    const auto postPath = splitDir.getChildFile("hnsep_post.onnx");
    if (!prePath.existsAsFile() || !corePath.existsAsFile() ||
        !postPath.existsAsFile()) {
      LOG("HNSep split pipeline files missing, falling back to single session");
      return false;
    }

    preSession = std::make_unique<SplitSession>();
    coreSession = std::make_unique<SplitSession>();
    postSession = std::make_unique<SplitSession>();

    if (!allocator)
      allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();
    if (!onnxEnv)
      onnxEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                           "HNSepModel");

    Ort::SessionOptions cpuOptions;
    cpuOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    cpuOptions.EnableCpuMemArena();
    const int numThreads = std::max(1u, std::thread::hardware_concurrency()) / 2;
    cpuOptions.SetIntraOpNumThreads(std::max(numThreads, 2));
    cpuOptions.EnableMemPattern();

    Ort::SessionOptions gpuOptions;
    gpuOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    gpuOptions.DisableCpuMemArena();

    GPUProvider effectiveProvider = provider;
    if (juce::SystemStats::getEnvironmentVariable("SVCFUSION_HNSEP_FORCE_CPU", "0") == "1")
      effectiveProvider = GPUProvider::CPU;

    int effectiveDeviceId = deviceId;
    const auto hnsepDeviceOverride =
        juce::SystemStats::getEnvironmentVariable("SVCFUSION_HNSEP_DML_DEVICE", "");
    if (effectiveProvider == GPUProvider::DirectML && hnsepDeviceOverride.isNotEmpty())
      effectiveDeviceId = hnsepDeviceOverride.getIntValue();

#if defined(_WIN32) && defined(USE_DIRECTML)
    if (effectiveProvider == GPUProvider::DirectML) {
      const auto adapterNames = getDxgiAdapterNamesForLogging();
      if (!adapterNames.isEmpty()) {
        juce::StringArray entries;
        for (int i = 0; i < adapterNames.size(); ++i)
          entries.add("[" + juce::String(i) + "] " + adapterNames[i]);
        LOG("HNSep split: DirectML adapters: " + entries.joinIntoString(" | "));
      }

      const OrtApi &ortApi = Ort::GetApi();
      const OrtDmlApi *ortDmlApi = nullptr;
      Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
          "DML", ORT_API_VERSION, reinterpret_cast<const void **>(&ortDmlApi)));
      gpuOptions.DisableMemPattern();
      gpuOptions.SetExecutionMode(ORT_SEQUENTIAL);
      Ort::ThrowOnError(
          ortDmlApi->SessionOptionsAppendExecutionProvider_DML(gpuOptions,
                                                               effectiveDeviceId));
      LOG("HNSep split: DirectML core execution provider added (device " +
          juce::String(effectiveDeviceId) + ")");
    } else
#endif
    {
      effectiveProvider = GPUProvider::CPU;
      LOG("HNSep split: using CPU for core session");
    }

    activeProvider = effectiveProvider;
    splitPipelineEnabled = true;
    splitPreUsesExternalStft = prePath == preNoStftPath;
    if (externalStftDisabled)
      LOG("HNSep split: SVCFUSION_HNSEP_DISABLE_EXTERNAL_STFT=1, using original pre model");

#ifdef _WIN32
    const auto prePathW = prePath.getFullPathName().toWideCharPointer();
    const auto corePathW = corePath.getFullPathName().toWideCharPointer();
    const auto postPathW = postPath.getFullPathName().toWideCharPointer();
    if (splitPreUsesExternalStft && effectiveProvider != GPUProvider::CPU) {
      try {
        preSession->session =
            std::make_unique<Ort::Session>(*onnxEnv, prePathW, gpuOptions);
        LOG("HNSep split: external-STFT pre session using GPU provider");
      } catch (const Ort::Exception &e) {
        LOG("HNSep split: external-STFT pre GPU session failed, using CPU: " +
            juce::String(e.what()));
      }
    }
    if (!preSession->session)
      preSession->session =
          std::make_unique<Ort::Session>(*onnxEnv, prePathW, cpuOptions);
    coreSession->session = std::make_unique<Ort::Session>(*onnxEnv, corePathW,
                                                          effectiveProvider == GPUProvider::CPU
                                                              ? cpuOptions
                                                              : gpuOptions);
    postSession->session = std::make_unique<Ort::Session>(*onnxEnv, postPathW, cpuOptions);
#else
    const auto prePathStr = prePath.getFullPathName().toStdString();
    const auto corePathStr = corePath.getFullPathName().toStdString();
    const auto postPathStr = postPath.getFullPathName().toStdString();
    if (splitPreUsesExternalStft && effectiveProvider != GPUProvider::CPU) {
      try {
        preSession->session =
            std::make_unique<Ort::Session>(*onnxEnv, prePathStr.c_str(), gpuOptions);
        LOG("HNSep split: external-STFT pre session using GPU provider");
      } catch (const Ort::Exception &e) {
        LOG("HNSep split: external-STFT pre GPU session failed, using CPU: " +
            juce::String(e.what()));
      }
    }
    if (!preSession->session)
      preSession->session =
          std::make_unique<Ort::Session>(*onnxEnv, prePathStr.c_str(), cpuOptions);
    coreSession->session = std::make_unique<Ort::Session>(*onnxEnv, corePathStr.c_str(),
                                                          effectiveProvider == GPUProvider::CPU
                                                              ? cpuOptions
                                                              : gpuOptions);
    postSession->session = std::make_unique<Ort::Session>(*onnxEnv, postPathStr.c_str(), cpuOptions);
#endif

    initializeSessionMetadata(*preSession->session, preSession->inputNameStrings,
                              preSession->inputNames, preSession->outputNameStrings,
                              preSession->outputNames);
    initializeSessionMetadata(*coreSession->session, coreSession->inputNameStrings,
                              coreSession->inputNames, coreSession->outputNameStrings,
                              coreSession->outputNames);
    initializeSessionMetadata(*postSession->session, postSession->inputNameStrings,
                              postSession->inputNames, postSession->outputNameStrings,
                              postSession->outputNames);

    loaded = true;
    LOG("HNSep split pipeline loaded successfully (pre=" +
        juce::String(preSession->outputNames.size()) + " outputs, core=" +
        juce::String(coreSession->outputNames.size()) + " outputs, post=" +
        juce::String(postSession->outputNames.size()) + " outputs, externalStft=" +
        juce::String(splitPreUsesExternalStft ? "yes" : "no") + ")");
    return true;
  } catch (const Ort::Exception &e) {
    LOG("HNSep split pipeline load failed (Ort): " + juce::String(e.what()));
  } catch (const std::exception &e) {
    LOG("HNSep split pipeline load failed: " + juce::String(e.what()));
  }

  splitPipelineEnabled = false;
  preSession.reset();
  coreSession.reset();
  postSession.reset();
  return false;
#else
  juce::ignoreUnused(modelPath, provider, deviceId);
  return false;
#endif
}

bool HNSepModel::loadModel(const juce::File &modelPath,
                           GPUProvider provider, int deviceId) {
#ifdef HAVE_ONNXRUNTIME
  loaded = false;
  splitPipelineEnabled = false;
  preSession.reset();
  coreSession.reset();
  postSession.reset();
  onnxSession.reset();
  allocator.reset();
  inputNames.clear();
  outputNames.clear();
  inputNameStrings.clear();
  outputNameStrings.clear();

  if (provider == GPUProvider::DirectML && modelPath.getFileName() == "hnsep_VR_convstft.onnx") {
    if (loadSplitPipelineModels(modelPath, provider, deviceId))
      return true;
    LOG("HNSep: split pipeline unavailable, falling back to single-session model");
  }

  return loadSingleSessionModel(modelPath, provider, deviceId);
#else
  juce::ignoreUnused(modelPath, provider, deviceId);
  LOG("HNSep: ONNX Runtime not available (HAVE_ONNXRUNTIME not defined)");
  return false;
#endif
}

void HNSepModel::unload() {
#ifdef HAVE_ONNXRUNTIME
  splitPipelineEnabled = false;
  splitPreUsesExternalStft = false;
  preSession.reset();
  coreSession.reset();
  postSession.reset();
  onnxSession.reset();
  allocator.reset();
  onnxEnv.reset();
  inputNames.clear();
  outputNames.clear();
  inputNameStrings.clear();
  outputNameStrings.clear();
#endif
  loaded = false;
  LOG("HNSep model unloaded");
}

bool HNSepModel::separateChunk(const float *audio, int numSamples,
                               std::vector<float> &harmonic,
                               std::vector<float> &noise) {
#ifdef HAVE_ONNXRUNTIME
  if (numSamples <= 0)
    return false;

  waveformShapeScratch[1] = static_cast<int64_t>(numSamples);

  static const auto memoryInfo = Ort::MemoryInfo::CreateCpu(
      OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
  static const Ort::RunOptions runOptions{nullptr};

  std::vector<Ort::Value> inputTensors;
  inputTensors.emplace_back(Ort::Value::CreateTensor<float>(
      memoryInfo, const_cast<float *>(audio), static_cast<size_t>(numSamples),
      waveformShapeScratch.data(), waveformShapeScratch.size()));

  LOG("HNSep chunk start: " + juce::String(numSamples) + " samples");
  std::vector<Ort::Value> outputs;

  if (splitPipelineEnabled && preSession && coreSession && postSession) {
    const auto preStart = std::chrono::high_resolution_clock::now();
    std::vector<Ort::Value> preInputs;
    double externalStftMs = 0.0;
    if (splitPreUsesExternalStft) {
      const auto stftStart = std::chrono::high_resolution_clock::now();
      if (!computeExternalConvStft(audio, numSamples, stftConvScratch)) {
        LOG("HNSep: failed to compute external ConvSTFT input");
        return false;
      }
      const auto stftEnd = std::chrono::high_resolution_clock::now();
      externalStftMs =
          std::chrono::duration<double, std::milli>(stftEnd - stftStart)
              .count();

      const int stftFrames = computeExternalStftFrameCount(numSamples);
      stftConvShapeScratch[2] = static_cast<int64_t>(stftFrames);
      preInputs.reserve(preSession->inputNameStrings.size());

      for (const auto &preInputName : preSession->inputNameStrings) {
        if (preInputName == "waveform") {
          preInputs.emplace_back(Ort::Value::CreateTensor<float>(
              memoryInfo, const_cast<float *>(audio), static_cast<size_t>(numSamples),
              waveformShapeScratch.data(), waveformShapeScratch.size()));
        } else if (preInputName == "/HNSepConvSTFT/Conv_output_0") {
          preInputs.emplace_back(Ort::Value::CreateTensor<float>(
              memoryInfo, stftConvScratch.data(), stftConvScratch.size(),
              stftConvShapeScratch.data(), stftConvShapeScratch.size()));
        } else {
          LOG("HNSep: unexpected pre input for external STFT model: " +
              juce::String(preInputName));
          return false;
        }
      }
    }

    auto preOutputs = preSession->session->Run(
        runOptions, preSession->inputNames.data(),
        splitPreUsesExternalStft ? preInputs.data() : inputTensors.data(),
        splitPreUsesExternalStft ? preInputs.size() : inputTensors.size(),
        preSession->outputNames.data(), preSession->outputNames.size());
    const auto preEnd = std::chrono::high_resolution_clock::now();

    std::vector<Ort::Value> coreInputs;
    coreInputs.reserve(coreSession->inputNames.size());
    for (const auto &coreInputName : coreSession->inputNameStrings) {
      for (size_t i = 0; i < preSession->outputNameStrings.size(); ++i) {
        if (preSession->outputNameStrings[i] == coreInputName) {
          coreInputs.push_back(std::move(preOutputs[i]));
          break;
        }
      }
    }

    const auto coreStart = std::chrono::high_resolution_clock::now();
    auto coreOutputs = coreSession->session->Run(runOptions, coreSession->inputNames.data(),
                                                 coreInputs.data(), coreInputs.size(),
                                                 coreSession->outputNames.data(),
                                                 coreSession->outputNames.size());
    const auto coreEnd = std::chrono::high_resolution_clock::now();

    std::vector<Ort::Value> postInputs;
    postInputs.reserve(postSession->inputNames.size());
    for (const auto &postInputName : postSession->inputNameStrings) {
      if (postInputName == "waveform") {
        postInputs.emplace_back(Ort::Value::CreateTensor<float>(
            memoryInfo, const_cast<float *>(audio), static_cast<size_t>(numSamples),
            waveformShapeScratch.data(), waveformShapeScratch.size()));
        continue;
      }

      bool matched = false;
      for (size_t i = 0; i < coreSession->outputNameStrings.size(); ++i) {
        if (coreSession->outputNameStrings[i] == postInputName) {
          postInputs.push_back(std::move(coreOutputs[i]));
          matched = true;
          break;
        }
      }
      if (matched)
        continue;

      for (size_t i = 0; i < preSession->outputNameStrings.size(); ++i) {
        if (preSession->outputNameStrings[i] == postInputName) {
          postInputs.push_back(std::move(preOutputs[i]));
          matched = true;
          break;
        }
      }
    }

    const auto postStart = std::chrono::high_resolution_clock::now();
    outputs = postSession->session->Run(runOptions, postSession->inputNames.data(),
                                        postInputs.data(), postInputs.size(),
                                        postSession->outputNames.data(),
                                        postSession->outputNames.size());
    const auto postEnd = std::chrono::high_resolution_clock::now();

    const double preTotalMs =
        std::chrono::duration<double, std::milli>(preEnd - preStart).count();
    const double coreMs =
        std::chrono::duration<double, std::milli>(coreEnd - coreStart).count();
    const double postMs =
        std::chrono::duration<double, std::milli>(postEnd - postStart).count();

    lastChunkTiming.stftMs = externalStftMs;
    lastChunkTiming.preMs = preTotalMs - externalStftMs;
    lastChunkTiming.coreMs = coreMs;
    lastChunkTiming.postMs = postMs;
    lastChunkTiming.totalMs = preTotalMs + coreMs + postMs;
    lastChunkTiming.numSamples = numSamples;

    LOG("HNSep chunk inference: pre=" +
        juce::String(preTotalMs, 1) +
        " ms" +
        (splitPreUsesExternalStft
             ? ", stft=" + juce::String(externalStftMs, 1) + " ms"
             : juce::String()) +
        ", core=" +
        juce::String(coreMs, 1) +
        " ms, post=" +
        juce::String(postMs, 1) +
        " ms (" + juce::String(numSamples) + " samples)");
  } else {
    if (!onnxSession)
      return false;

    auto startTime = std::chrono::high_resolution_clock::now();
    outputs = onnxSession->Run(runOptions, inputNames.data(),
                               inputTensors.data(), inputTensors.size(),
                               outputNames.data(), outputNames.size());
    auto endTime = std::chrono::high_resolution_clock::now();
    const double totalMs =
        std::chrono::duration<double, std::milli>(endTime - startTime).count();
    lastChunkTiming = {0.0, totalMs, 0.0, 0.0, totalMs, numSamples};
    LOG("HNSep chunk inference: " + juce::String(totalMs, 1) +
        " ms (" + juce::String(numSamples) + " samples)");
  }

  if (outputs.size() < 2) {
    LOG("HNSep: unexpected output count: " + juce::String(outputs.size()));
    return false;
  }

  float *harmonicPtr = outputs[0].GetTensorMutableData<float>();
  float *noisePtr = outputs[1].GetTensorMutableData<float>();
  auto harmonicShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
  const int outSamples = static_cast<int>(harmonicShape.back());
  const int copyLen = std::min(numSamples, outSamples);
  LOG("HNSep chunk output: harmonic shape last=" + juce::String(outSamples) +
      ", copyLen=" + juce::String(copyLen));

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

  const int maxChunkSamples = getRuntimeMaxChunkSamples();
  const int overlapSamples = getRuntimeOverlapSamples(maxChunkSamples);
  const int stride = std::max(1, maxChunkSamples - overlapSamples);

  LOG("HNSep separate start: total=" + juce::String(numSamples) +
      " samples, chunk=" + juce::String(maxChunkSamples) +
      ", overlap=" + juce::String(overlapSamples) +
      ", stride=" + juce::String(stride));

  if (progressCallback)
    progressCallback(0.0);

  if (numSamples <= maxChunkSamples) {
    const bool ok = separateChunk(audio, numSamples, harmonic, noise);
    if (progressCallback)
      progressCallback(1.0);
    LOG("HNSep separate complete: single chunk ok=" + juce::String(ok ? "true" : "false"));
    return ok;
  }

  harmonic.resize(numSamples, 0.0f);
  noise.resize(numSamples, 0.0f);

  int position = 0;
  int chunkIndex = 0;

  while (position < numSamples) {
    const int chunkEnd = std::min(position + maxChunkSamples, numSamples);
    const int chunkSize = chunkEnd - position;
    LOG("HNSep separate chunk " + juce::String(chunkIndex) +
        ": position=" + juce::String(position) +
        ", size=" + juce::String(chunkSize));

    std::vector<float> chunkHarmonic;
    std::vector<float> chunkNoise;
    if (!separateChunk(audio + position, chunkSize, chunkHarmonic, chunkNoise)) {
      LOG("HNSep separate failed at chunk " + juce::String(chunkIndex));
      return false;
    }

    if (chunkIndex == 0) {
      std::copy(chunkHarmonic.begin(), chunkHarmonic.end(),
                harmonic.begin() + position);
      std::copy(chunkNoise.begin(), chunkNoise.end(), noise.begin() + position);
    } else {
      const int currentOverlapSamples = std::min(overlapSamples, chunkSize);
      for (int sampleIndex = 0; sampleIndex < currentOverlapSamples; ++sampleIndex) {
        const float alpha = static_cast<float>(sampleIndex) /
                            static_cast<float>(currentOverlapSamples);
        const int destIndex = position + sampleIndex;
        harmonic[destIndex] = harmonic[destIndex] * (1.0f - alpha) +
                              chunkHarmonic[sampleIndex] * alpha;
        noise[destIndex] = noise[destIndex] * (1.0f - alpha) +
                           chunkNoise[sampleIndex] * alpha;
      }

      if (currentOverlapSamples < chunkSize) {
        std::copy(chunkHarmonic.begin() + currentOverlapSamples, chunkHarmonic.end(),
                  harmonic.begin() + position + currentOverlapSamples);
        std::copy(chunkNoise.begin() + currentOverlapSamples, chunkNoise.end(),
                  noise.begin() + position + currentOverlapSamples);
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
  LOG("HNSep separate complete: chunks=" + juce::String(chunkIndex));
  return true;
#else
  juce::ignoreUnused(audio, numSamples, harmonic, noise, progressCallback);
  return false;
#endif
}
