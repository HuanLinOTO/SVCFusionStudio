#include "GAMEDetector.h"
#include "../Utils/AppLogger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

GAMEDetector::GAMEDetector() = default;
GAMEDetector::~GAMEDetector() = default;

bool GAMEDetector::loadConfig(const juce::File& configPath) {
  if (!configPath.existsAsFile())
    return false;

  auto jsonText = configPath.loadFileAsString();
  auto parsed = juce::JSON::parse(jsonText);
  if (parsed.isVoid())
    return false;

  if (auto* obj = parsed.getDynamicObject()) {
    modelSampleRate = static_cast<int>(obj->getProperty("samplerate"));
    timestep = static_cast<float>(
        static_cast<double>(obj->getProperty("timestep")));
    supportsLoop = static_cast<bool>(obj->getProperty("loop"));
    embeddingDim = static_cast<int>(obj->getProperty("embedding_dim"));
  }

  LOG("GAME config: sr=" + juce::String(modelSampleRate) +
      " timestep=" + juce::String(timestep) +
      " loop=" + juce::String(supportsLoop ? "true" : "false") +
      " dim=" + juce::String(embeddingDim));
  return true;
}

std::vector<float> GAMEDetector::resampleToModelRate(const float* audio,
                                                     int numSamples,
                                                     int srcRate) {
  if (srcRate == modelSampleRate)
    return std::vector<float>(audio, audio + numSamples);

  double ratio = static_cast<double>(modelSampleRate) / srcRate;
  int outSamples = static_cast<int>(numSamples * ratio);
  std::vector<float> resampled(outSamples);

  for (int i = 0; i < outSamples; ++i) {
    double srcPos = i / ratio;
    int srcIdx = static_cast<int>(srcPos);
    double frac = srcPos - srcIdx;

    if (srcIdx + 1 < numSamples)
      resampled[i] = static_cast<float>(audio[srcIdx] * (1.0 - frac) +
                                        audio[srcIdx + 1] * frac);
    else if (srcIdx < numSamples)
      resampled[i] = audio[srcIdx];
  }
  return resampled;
}

#ifdef HAVE_ONNXRUNTIME

bool GAMEDetector::ModelSession::load(Ort::Env& env, const juce::File& path,
                                      const Ort::SessionOptions& options) {
  try {
#ifdef _WIN32
    std::wstring modelPathW = path.getFullPathName().toWideCharPointer();
    session = std::make_unique<Ort::Session>(env, modelPathW.c_str(), options);
#else
    std::string modelPathStr = path.getFullPathName().toStdString();
    session = std::make_unique<Ort::Session>(env, modelPathStr.c_str(), options);
#endif

    Ort::AllocatorWithDefaultOptions allocator;

    inputNameStrings.clear();
    outputNameStrings.clear();
    inputNames.clear();
    outputNames.clear();

    size_t numInputs = session->GetInputCount();
    size_t numOutputs = session->GetOutputCount();

    for (size_t i = 0; i < numInputs; ++i) {
      auto namePtr = session->GetInputNameAllocated(i, allocator);
      inputNameStrings.push_back(namePtr.get());
    }
    for (size_t i = 0; i < numOutputs; ++i) {
      auto namePtr = session->GetOutputNameAllocated(i, allocator);
      outputNameStrings.push_back(namePtr.get());
    }

    for (const auto& name : inputNameStrings)
      inputNames.push_back(name.c_str());
    for (const auto& name : outputNameStrings)
      outputNames.push_back(name.c_str());

    return true;
  } catch (const Ort::Exception& e) {
    LOG("ONNX load failed for " + path.getFileName() + ": " +
        juce::String(e.what()));
    session.reset();
    return false;
  }
}

int GAMEDetector::ModelSession::findInput(const std::string& name) const {
  for (size_t i = 0; i < inputNameStrings.size(); ++i) {
    if (inputNameStrings[i] == name)
      return static_cast<int>(i);
  }
  return -1;
}

int GAMEDetector::ModelSession::findOutput(const std::string& name) const {
  for (size_t i = 0; i < outputNameStrings.size(); ++i) {
    if (outputNameStrings[i] == name)
      return static_cast<int>(i);
  }
  return -1;
}

Ort::SessionOptions GAMEDetector::createSessionOptions(GPUProvider provider,
                                                       int deviceId) {
  Ort::SessionOptions options;

  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  options.EnableCpuMemArena();

  if (provider == GPUProvider::CPU) {
    const int numThreads =
        std::max(1u, std::thread::hardware_concurrency()) / 2;
    options.SetIntraOpNumThreads(std::max(numThreads, 2));
    options.EnableMemPattern();
  } else {
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
  }

#if defined(_WIN32) && defined(USE_DIRECTML)
  if (provider == GPUProvider::DirectML) {
    try {
      const OrtApi& ortApi = Ort::GetApi();
      const OrtDmlApi* ortDmlApi = nullptr;
      Ort::ThrowOnError(ortApi.GetExecutionProviderApi(
          "DML", ORT_API_VERSION,
          reinterpret_cast<const void**>(&ortDmlApi)));
      options.DisableMemPattern();
      options.SetExecutionMode(ORT_SEQUENTIAL);
      Ort::ThrowOnError(
          ortDmlApi->SessionOptionsAppendExecutionProvider_DML(options,
                                                               deviceId));
    } catch (const Ort::Exception&) {
    }
  } else
#endif
#ifdef USE_CUDA
      if (provider == GPUProvider::CUDA) {
    try {
      OrtCUDAProviderOptions cudaOptions{};
      cudaOptions.device_id = deviceId;
      cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
      cudaOptions.arena_extend_strategy = 1;
      options.AppendExecutionProvider_CUDA(cudaOptions);
    } catch (const Ort::Exception&) {
    }
  } else
#endif
      if (provider == GPUProvider::CoreML) {
    try {
      options.AppendExecutionProvider("CoreML", {{"MLComputeUnits", "ALL"}});
    } catch (const Ort::Exception&) {
    }
  }

  return options;
}

#endif

bool GAMEDetector::loadModels(const juce::File& gameDir, GPUProvider provider,
                              int deviceId) {
#ifdef HAVE_ONNXRUNTIME
  if (!gameDir.isDirectory()) {
    LOG("GAME model directory not found: " + gameDir.getFullPathName());
    return false;
  }

  if (!loadConfig(gameDir.getChildFile("config.json"))) {
    LOG("Failed to load GAME config.json");
    return false;
  }

  try {
    encoder.session.reset();
    segmenter.session.reset();
    estimator.session.reset();
    bd2dur.session.reset();

    onnxEnv =
        std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "GAMEDetector");
    auto heavyOpts = createSessionOptions(provider, deviceId);
    auto lightOpts = createSessionOptions(provider, deviceId);
    lightOpts.SetIntraOpNumThreads(1);

    struct ModelEntry {
      const char* filename;
      ModelSession* session;
      Ort::SessionOptions* options;
    };

    ModelEntry models[] = {{"encoder.onnx", &encoder, &heavyOpts},
                           {"segmenter.onnx", &segmenter, &heavyOpts},
                           {"estimator.onnx", &estimator, &lightOpts},
                           {"bd2dur.onnx", &bd2dur, &lightOpts}};

    for (auto& entry : models) {
      auto path = gameDir.getChildFile(entry.filename);
      if (!path.existsAsFile()) {
        LOG("GAME model not found: " + path.getFullPathName());
        loaded = false;
        return false;
      }
      if (!entry.session->load(*onnxEnv, path, *entry.options)) {
        loaded = false;
        return false;
      }
      LOG("Loaded GAME " + juce::String(entry.filename) + " (inputs=" +
          juce::String(static_cast<int>(entry.session->inputNames.size())) +
          " outputs=" +
          juce::String(static_cast<int>(entry.session->outputNames.size())) +
          ")");
    }

    loaded = true;
    return true;
  } catch (const Ort::Exception& e) {
    LOG("GAME model load error: " + juce::String(e.what()));
    loaded = false;
    return false;
  }
#else
  juce::ignoreUnused(gameDir, provider, deviceId);
  return false;
#endif
}

std::vector<GAMEDetector::NoteEvent> GAMEDetector::detectNotes(
    const float* audio, int numSamples, int sampleRate) {
  return detectNotesWithProgress(audio, numSamples, sampleRate, nullptr);
}

std::vector<GAMEDetector::NoteEvent> GAMEDetector::detectNotesWithProgress(
    const float* audio, int numSamples, int sampleRate,
    std::function<void(double)> progressCallback) {
#ifdef HAVE_ONNXRUNTIME
  if (!loaded)
    return {};

  if (progressCallback)
    progressCallback(0.02);

  std::vector<float> waveform =
      resampleToModelRate(audio, numSamples, sampleRate);

  if (progressCallback)
    progressCallback(0.05);

  auto chunks = findSilenceChunks(waveform);
  lastChunkRanges = chunks;

  LOG("GAME: split audio into " + juce::String(static_cast<int>(chunks.size())) +
      " chunks (total " + juce::String(static_cast<int>(waveform.size())) +
      " samples)");

  std::vector<NoteEvent> allNotes;
  const double progressPerChunk =
      chunks.empty() ? 0.90 : 0.90 / static_cast<double>(chunks.size());

  for (size_t chunkIndex = 0; chunkIndex < chunks.size(); ++chunkIndex) {
    const auto& chunk = chunks[chunkIndex];
    std::vector<float> chunkWaveform(waveform.begin() + chunk.startSample,
                                     waveform.begin() + chunk.endSample);

    double base = 0.05 + static_cast<double>(chunkIndex) * progressPerChunk;
    auto chunkNotes = processChunk(chunkWaveform, chunk.startSample,
                                   progressCallback, base, progressPerChunk);
    allNotes.insert(allNotes.end(), chunkNotes.begin(), chunkNotes.end());
  }

  if (progressCallback)
    progressCallback(1.0);

  LOG("GAME detection complete: " +
      juce::String(static_cast<int>(allNotes.size())) + " notes");
  return allNotes;
#else
  juce::ignoreUnused(audio, numSamples, sampleRate, progressCallback);
  return {};
#endif
}

std::vector<GAMEDetector::ChunkRange> GAMEDetector::findSilenceChunks(
    const std::vector<float>& waveform) const {
  const int totalSamples = static_cast<int>(waveform.size());
  if (totalSamples == 0)
    return {};

  constexpr float thresholdDb = -40.0f;
  constexpr int minLengthMs = 1000;
  constexpr int minIntervalMs = 200;
  constexpr int maxSilKeptMs = 100;

  const float threshold = std::pow(10.0f, thresholdDb / 20.0f);
  const int hop = samplesPerEncoderFrame();
  const int numHops = (totalSamples + hop - 1) / hop;
  const int minLenHops = minLengthMs * modelSampleRate / (1000 * hop);
  const int minIntHops = minIntervalMs * modelSampleRate / (1000 * hop);
  const int maxSilHops = maxSilKeptMs * modelSampleRate / (1000 * hop);

  std::vector<float> rms(numHops, 0.0f);
  for (int hopIndex = 0; hopIndex < numHops; ++hopIndex) {
    int start = hopIndex * hop;
    int end = std::min(start + hop, totalSamples);
    double sum = 0.0;
    for (int sample = start; sample < end; ++sample)
      sum += static_cast<double>(waveform[sample]) * waveform[sample];
    rms[hopIndex] = static_cast<float>(std::sqrt(sum / (end - start)));
  }

  struct Interval {
    int start;
    int end;
  };
  std::vector<Interval> silences;
  {
    int silenceStart = -1;
    for (int hopIndex = 0; hopIndex <= numHops; ++hopIndex) {
      bool silent = (hopIndex < numHops) && (rms[hopIndex] < threshold);
      if (silent && silenceStart < 0)
        silenceStart = hopIndex;
      if (!silent && silenceStart >= 0) {
        if (hopIndex - silenceStart >= minIntHops)
          silences.push_back({silenceStart, hopIndex});
        silenceStart = -1;
      }
    }
  }

  struct VoicedSegment {
    int startHop;
    int endHop;
  };
  std::vector<VoicedSegment> voiced;
  {
    int previousEnd = 0;
    for (const auto& silence : silences) {
      if (silence.start > previousEnd)
        voiced.push_back({previousEnd, silence.start});
      previousEnd = silence.end;
    }
    if (previousEnd < numHops)
      voiced.push_back({previousEnd, numHops});
  }

  if (voiced.empty())
    return {{0, totalSamples}};

  for (size_t segmentIndex = 0; segmentIndex + 1 < voiced.size();) {
    if (voiced[segmentIndex].endHop - voiced[segmentIndex].startHop <
        minLenHops) {
      voiced[segmentIndex].endHop = voiced[segmentIndex + 1].endHop;
      voiced.erase(voiced.begin() + static_cast<ptrdiff_t>(segmentIndex) + 1);
    } else {
      ++segmentIndex;
    }
  }
  if (voiced.size() > 1 &&
      voiced.back().endHop - voiced.back().startHop < minLenHops) {
    voiced[voiced.size() - 2].endHop = voiced.back().endHop;
    voiced.pop_back();
  }

  std::vector<ChunkRange> padded;
  for (const auto& segment : voiced) {
    int startSample = std::max(0, segment.startHop - maxSilHops) * hop;
    int endSample = std::min(totalSamples, (segment.endHop + maxSilHops) * hop);
    padded.push_back({startSample, endSample});
  }

  const int maxChunk = maxChunkSamples();
  std::vector<ChunkRange> result;
  for (const auto& chunk : padded) {
    if (chunk.endSample - chunk.startSample <= maxChunk) {
      result.push_back(chunk);
      continue;
    }

    int currentStart = chunk.startSample;
    while (currentStart < chunk.endSample) {
      int remaining = chunk.endSample - currentStart;
      if (remaining <= maxChunk) {
        result.push_back({currentStart, chunk.endSample});
        break;
      }

      int searchStart = (currentStart + maxChunk / 2) / hop;
      int searchEnd = std::min((currentStart + maxChunk) / hop, numHops);
      int bestHop = -1;
      float bestRms = std::numeric_limits<float>::max();
      for (int hopIndex = searchStart; hopIndex < searchEnd; ++hopIndex) {
        if (rms[hopIndex] < bestRms) {
          bestRms = rms[hopIndex];
          bestHop = hopIndex;
        }
      }

      int splitSample =
          (bestHop >= 0) ? bestHop * hop + hop / 2 : currentStart + maxChunk;
      splitSample = std::min(splitSample, chunk.endSample);
      result.push_back({currentStart, splitSample});
      currentStart = splitSample;
    }
  }

  return result;
}

std::vector<GAMEDetector::NoteEvent> GAMEDetector::processChunk(
    const std::vector<float>& chunkWaveform, int chunkStartSample,
    std::function<void(double)> progressCallback, double progressBase,
    double progressSpan) {
#ifdef HAVE_ONNXRUNTIME
  const int64_t sampleCount = static_cast<int64_t>(chunkWaveform.size());
  float duration =
      static_cast<float>(sampleCount) / static_cast<float>(modelSampleRate);

  static const auto memInfo =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  static const Ort::RunOptions runOptions{nullptr};

  const int chunkFrameOffset = chunkStartSample / HOP_SIZE;

  int64_t waveformShape[] = {1, sampleCount};
  int64_t durationShape[] = {1};
  auto* waveformPtr = const_cast<float*>(chunkWaveform.data());

  std::vector<Ort::Value> encoderInputs;
  encoderInputs.reserve(encoder.inputNames.size());

  for (size_t i = 0; i < encoder.inputNameStrings.size(); ++i) {
    const auto& name = encoder.inputNameStrings[i];
    if (name == "waveform") {
      encoderInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, waveformPtr, chunkWaveform.size(), waveformShape, 2));
    } else if (name == "duration") {
      encoderInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, &duration, 1, durationShape, 1));
    } else {
      LOG("GAME encoder: unexpected input '" + juce::String(name) + "'");
      return {};
    }
  }

  auto encoderStart = std::chrono::high_resolution_clock::now();
  auto encoderOutputs = encoder.session->Run(
      runOptions, encoder.inputNames.data(), encoderInputs.data(),
      encoderInputs.size(), encoder.outputNames.data(),
      encoder.outputNames.size());
  auto encoderEnd = std::chrono::high_resolution_clock::now();
  LOG("GAME encoder: " +
      juce::String(
          std::chrono::duration<double, std::milli>(encoderEnd - encoderStart)
              .count(),
          1) +
      " ms (L=" + juce::String(sampleCount) + ")");

  if (progressCallback)
    progressCallback(progressBase + progressSpan * 0.15);

  int xSegIndex = encoder.findOutput("x_seg");
  int xEstIndex = encoder.findOutput("x_est");
  int maskTIndex = encoder.findOutput("maskT");
  if (xSegIndex < 0 || xEstIndex < 0 || maskTIndex < 0) {
    LOG("GAME encoder outputs not found (x_seg/x_est/maskT)");
    return {};
  }

  auto maskShape =
      encoderOutputs[maskTIndex].GetTensorTypeAndShapeInfo().GetShape();
  const int64_t frameCount =
      maskShape.size() >= 2 ? maskShape[1] : maskShape[0];

  auto featureShape =
      encoderOutputs[xSegIndex].GetTensorTypeAndShapeInfo().GetShape();
  const int64_t channelCount =
      featureShape.size() >= 3 ? featureShape[2] : embeddingDim;

  const size_t featureSize =
      static_cast<size_t>(frameCount * channelCount);
  std::vector<float> xSegData(featureSize);
  std::vector<float> xEstData(featureSize);
  auto maskSize = static_cast<size_t>(frameCount);

  std::memcpy(xSegData.data(), encoderOutputs[xSegIndex].GetTensorData<float>(),
              featureSize * sizeof(float));
  std::memcpy(xEstData.data(), encoderOutputs[xEstIndex].GetTensorData<float>(),
              featureSize * sizeof(float));

  auto maskTData = std::make_unique<uint8_t[]>(maskSize);
  std::memcpy(maskTData.get(), encoderOutputs[maskTIndex].GetTensorData<bool>(),
              maskSize);
  encoderOutputs.clear();

  auto knownBoundaries = std::make_unique<uint8_t[]>(maskSize);
  auto previousBoundaries = std::make_unique<uint8_t[]>(maskSize);
  std::memset(knownBoundaries.get(), 0, maskSize);
  std::memset(previousBoundaries.get(), 0, maskSize);

  int64_t featureTensorShape[] = {1, frameCount, channelCount};
  int64_t frameMaskShape[] = {1, frameCount};
  int64_t langShape[] = {1};
  int64_t language = 0;
  int64_t radiusValue = static_cast<int64_t>(segRadius);
  float tValue = 0.0f;
  int64_t tShape[] = {1};

  const int totalSteps = supportsLoop ? numD3PMSteps : 1;
  const int boundariesIndex = [&]() {
    int index = segmenter.findOutput("boundaries");
    return index >= 0 ? index : 0;
  }();

  std::vector<Ort::Value> segmenterInputs;
  segmenterInputs.reserve(segmenter.inputNames.size());
  for (size_t i = 0; i < segmenter.inputNameStrings.size(); ++i) {
    const auto& name = segmenter.inputNameStrings[i];
    if (name == "x_seg") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, xSegData.data(), featureSize, featureTensorShape, 3));
    } else if (name == "maskT") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, maskTData.get(), maskSize, frameMaskShape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "known_boundaries") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, knownBoundaries.get(), maskSize, frameMaskShape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "prev_boundaries") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, previousBoundaries.get(), maskSize, frameMaskShape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "threshold") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, &segThreshold, 1, nullptr, 0));
    } else if (name == "radius") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor<int64_t>(
          memInfo, &radiusValue, 1, nullptr, 0));
    } else if (name == "t") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, &tValue, 1, tShape, 1));
    } else if (name == "language") {
      segmenterInputs.emplace_back(Ort::Value::CreateTensor<int64_t>(
          memInfo, &language, 1, langShape, 1));
    } else {
      LOG("GAME segmenter: unexpected input '" + juce::String(name) + "'");
      return {};
    }
  }

  auto segmenterStart = std::chrono::high_resolution_clock::now();
  for (int step = 0; step < totalSteps; ++step) {
    tValue = supportsLoop ? static_cast<float>(step) /
                                static_cast<float>(totalSteps)
                          : 0.0f;
    auto segmenterOutputs = segmenter.session->Run(
        runOptions, segmenter.inputNames.data(), segmenterInputs.data(),
        segmenterInputs.size(), segmenter.outputNames.data(),
        segmenter.outputNames.size());
    std::memcpy(previousBoundaries.get(),
                segmenterOutputs[boundariesIndex].GetTensorData<bool>(),
                maskSize);

    if (progressCallback) {
      progressCallback(progressBase +
                       progressSpan *
                           (0.15 + 0.50 * static_cast<double>(step + 1) /
                                       totalSteps));
    }
  }
  auto segmenterEnd = std::chrono::high_resolution_clock::now();
  LOG("GAME segmenter (" + juce::String(totalSteps) + " steps): " +
      juce::String(
          std::chrono::duration<double, std::milli>(segmenterEnd -
                                                    segmenterStart)
              .count(),
          1) +
      " ms");

  std::vector<Ort::Value> bd2durInputs;
  bd2durInputs.reserve(bd2dur.inputNames.size());
  for (size_t i = 0; i < bd2dur.inputNameStrings.size(); ++i) {
    const auto& name = bd2dur.inputNameStrings[i];
    if (name == "boundaries") {
      bd2durInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, previousBoundaries.get(), maskSize, frameMaskShape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "maskT") {
      bd2durInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, maskTData.get(), maskSize, frameMaskShape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "duration") {
      bd2durInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, &duration, 1, durationShape, 1));
    } else {
      LOG("GAME bd2dur: unexpected input '" + juce::String(name) + "'");
      return {};
    }
  }

  auto bdStart = std::chrono::high_resolution_clock::now();
  auto bdOutputs = bd2dur.session->Run(
      runOptions, bd2dur.inputNames.data(), bd2durInputs.data(),
      bd2durInputs.size(), bd2dur.outputNames.data(), bd2dur.outputNames.size());
  auto bdEnd = std::chrono::high_resolution_clock::now();
  LOG("GAME bd2dur: " +
      juce::String(std::chrono::duration<double, std::milli>(bdEnd - bdStart)
                       .count(),
                   1) +
      " ms");

  if (progressCallback)
    progressCallback(progressBase + progressSpan * 0.70);

  int durationIndex = bd2dur.findOutput("durations");
  if (durationIndex < 0)
    durationIndex = 0;

  auto durationOutputShape =
      bdOutputs[durationIndex].GetTensorTypeAndShapeInfo().GetShape();
  const int64_t noteSlots = durationOutputShape.size() >= 2
                                ? durationOutputShape[1]
                                : durationOutputShape[0];
  const float* durations = bdOutputs[durationIndex].GetTensorData<float>();
  int bdMaskIndex = bd2dur.findOutput("maskN");

  const bool* bdMaskData =
      bdMaskIndex >= 0 ? bdOutputs[bdMaskIndex].GetTensorData<bool>() : nullptr;

  int64_t maskNShape[] = {1, noteSlots};
  auto maskNBuffer = std::make_unique<uint8_t[]>(static_cast<size_t>(noteSlots));
  if (bdMaskData)
    std::memcpy(maskNBuffer.get(), bdMaskData, static_cast<size_t>(noteSlots));
  else
    std::memset(maskNBuffer.get(), 1, static_cast<size_t>(noteSlots));

  std::vector<Ort::Value> estimatorInputs;
  estimatorInputs.reserve(estimator.inputNames.size());
  for (size_t i = 0; i < estimator.inputNameStrings.size(); ++i) {
    const auto& name = estimator.inputNameStrings[i];
    if (name == "x_est") {
      estimatorInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, xEstData.data(), featureSize, featureTensorShape, 3));
    } else if (name == "maskT") {
      estimatorInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, maskTData.get(), maskSize, frameMaskShape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "boundaries") {
      estimatorInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, previousBoundaries.get(), maskSize, frameMaskShape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "maskN") {
      estimatorInputs.emplace_back(Ort::Value::CreateTensor(
          memInfo, maskNBuffer.get(), static_cast<size_t>(noteSlots), maskNShape,
          2, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL));
    } else if (name == "threshold") {
      estimatorInputs.emplace_back(Ort::Value::CreateTensor<float>(
          memInfo, &estThreshold, 1, nullptr, 0));
    } else {
      LOG("GAME estimator: unexpected input '" + juce::String(name) + "'");
      return {};
    }
  }

  auto estimatorStart = std::chrono::high_resolution_clock::now();
  auto estimatorOutputs = estimator.session->Run(
      runOptions, estimator.inputNames.data(), estimatorInputs.data(),
      estimatorInputs.size(), estimator.outputNames.data(),
      estimator.outputNames.size());
  auto estimatorEnd = std::chrono::high_resolution_clock::now();
  LOG("GAME estimator: " +
      juce::String(
          std::chrono::duration<double, std::milli>(estimatorEnd -
                                                    estimatorStart)
              .count(),
          1) +
      " ms");

  if (progressCallback)
    progressCallback(progressBase + progressSpan * 0.85);

  int presenceIndex = estimator.findOutput("presence");
  int scoresIndex = estimator.findOutput("scores");
  int estimatorMaskIndex = estimator.findOutput("maskN");
  if (scoresIndex < 0) {
    LOG("GAME estimator: 'scores' output not found");
    return {};
  }

  auto scoreShape =
      estimatorOutputs[scoresIndex].GetTensorTypeAndShapeInfo().GetShape();
  const int64_t estimatedNotes =
      scoreShape.size() >= 2 ? scoreShape[1] : scoreShape[0];
  const int64_t noteCount = std::min(noteSlots, estimatedNotes);

  const float* scores = estimatorOutputs[scoresIndex].GetTensorData<float>();
  const bool* presence = presenceIndex >= 0
                             ? estimatorOutputs[presenceIndex].GetTensorData<bool>()
                             : nullptr;
  const bool* finalMask =
      estimatorMaskIndex >= 0
          ? estimatorOutputs[estimatorMaskIndex].GetTensorData<bool>()
          : bdMaskData;

  std::vector<NoteEvent> notes;
  notes.reserve(static_cast<size_t>(noteCount));

  const double secondsPerFrame =
      static_cast<double>(HOP_SIZE) / static_cast<double>(SAMPLE_RATE);

  double cumulativeTime = 0.0;
  for (int64_t noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
    if (finalMask && !finalMask[noteIndex])
      break;

    double noteStartTime = cumulativeTime;
    cumulativeTime += static_cast<double>(durations[noteIndex]);
    double noteEndTime = cumulativeTime;

    int startFrame = chunkFrameOffset +
                     static_cast<int>(std::round(noteStartTime / secondsPerFrame));
    int endFrame = chunkFrameOffset +
                   static_cast<int>(std::round(noteEndTime / secondsPerFrame));
    if (endFrame <= startFrame)
      endFrame = startFrame + 1;

    bool voiced = presence ? presence[noteIndex] : true;
    NoteEvent noteEvent;
    noteEvent.startFrame = startFrame;
    noteEvent.endFrame = endFrame;
    noteEvent.midiNote = scores[noteIndex];
    noteEvent.isRest = !voiced;
    notes.push_back(noteEvent);
  }

  if (progressCallback)
    progressCallback(progressBase + progressSpan);

  return notes;
#else
  juce::ignoreUnused(chunkWaveform, chunkStartSample, progressCallback,
                     progressBase, progressSpan);
  return {};
#endif
}