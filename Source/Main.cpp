// Main.cpp - Cross-platform entry point (macOS uses native menu inside
// MainComponent)

#include "JuceHeader.h"
#include "UI/MainComponent.h"
#include "UI/StyledComponents.h"
#include "Audio/EditorController.h"
#include "Audio/HNSepModel.h"
#include "Audio/FCPEPitchDetector.h"
#include "Utils/AppLogger.h"
#include "Utils/Constants.h"
#include "Utils/Localization.h"
#include "Utils/MelSpectrogram.h"
#include "Utils/OnnxRuntimeLoader.h"
#include "Utils/PlatformPaths.h"
#include "Utils/PlatformUtils.h"
#include "Utils/UI/WindowSizing.h"
#include "Utils/UI/TimecodeFont.h"

#include <atomic>
#include <future>

#if JUCE_WINDOWS
#include <dwmapi.h>
#include <dxgi1_4.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#endif

#if JUCE_WINDOWS
static void logGpuMemory(const juce::String &label) {
  IDXGIFactory4 *factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                   reinterpret_cast<void **>(&factory)))) {
    IDXGIAdapter3 *adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapters(0, reinterpret_cast<IDXGIAdapter **>(&adapter)))) {
      DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
      if (SUCCEEDED(adapter->QueryVideoMemoryInfo(
              0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        LOG("GPUVRAM [" + label + "]: usage=" +
            juce::String(static_cast<int>(info.CurrentUsage / (1024 * 1024))) +
            " MB, budget=" +
            juce::String(static_cast<int>(info.Budget / (1024 * 1024))) +
            " MB");
      }
      adapter->Release();
    }
    factory->Release();
  }
}
#endif

#include <chrono>
#include <cmath>

#if JUCE_WINDOWS
namespace {
juce::String getRendererName(const juce::StringArray &renderers, int index) {
  if (index >= 0 && index < renderers.size())
    return renderers[index];

  return "unknown";
}

void requestDirect2DRenderer(juce::Component &window,
                             const juce::String &windowName) {
  auto *peer = window.getPeer();
  if (peer == nullptr) {
    LOG(windowName + ": no native peer available for renderer selection");
    return;
  }

  const auto renderers = peer->getAvailableRenderingEngines();
  LOG(windowName + ": available renderers: " +
      renderers.joinIntoString(", "));

  int direct2DIndex = -1;
  for (int i = 0; i < renderers.size(); ++i) {
    if (renderers[i].equalsIgnoreCase("Direct2D")) {
      direct2DIndex = i;
      break;
    }
  }

  if (direct2DIndex < 0) {
    LOG(windowName + ": Direct2D renderer unavailable; keeping current renderer");
    return;
  }

  const auto beforeIndex = peer->getCurrentRenderingEngine();
  const auto beforeName = getRendererName(renderers, beforeIndex);
  LOG(windowName + ": renderer before request: " + beforeName + " (#" +
      juce::String(beforeIndex) + ")");

  if (beforeIndex != direct2DIndex)
    peer->setCurrentRenderingEngine(direct2DIndex);

  const auto afterIndex = peer->getCurrentRenderingEngine();
  const auto afterName = getRendererName(renderers, afterIndex);

  LOG(windowName + ": active renderer: " + afterName + " (#" +
      juce::String(afterIndex) + ")" +
      (afterIndex == direct2DIndex ? "" : " - Direct2D request failed"));
}
} // namespace
#endif

class SplashComponent : public juce::Component, private juce::Timer {
public:
  SplashComponent() { startTimerHz(30); }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient background(
        juce::Colour(APP_COLOR_BACKGROUND).brighter(0.12f),
        bounds.getTopLeft(),
        juce::Colour(APP_COLOR_BACKGROUND).darker(0.12f),
        bounds.getBottomRight(),
        false);
    g.setGradientFill(background);
    g.fillAll();

    const auto titleFont = AppFont::getBoldFont(34.0f);
    const auto subtitleFont = AppFont::getFont(15.0f);

    g.setColour(juce::Colours::white);
    g.setFont(titleFont);
    g.drawText("SVCFusion Studio", getLocalBounds().reduced(24, 20),
               juce::Justification::centredTop);

    g.setColour(juce::Colour(APP_COLOR_PRIMARY));
    g.setFont(subtitleFont);
    g.drawText(TR("progress.loading"), 0, 150, getWidth(), 24,
               juce::Justification::centredTop);

    const float dotRadius = 5.0f;
    const float dotSpacing = 14.0f;
    const float baseY = 190.0f;
    const float startX = (getWidth() - dotSpacing * 2.0f) * 0.5f;

    for (int i = 0; i < 3; ++i) {
      const int phase = (tick / 6 + i) % 3;
      const float alpha = (phase == 0 ? 1.0f : (phase == 1 ? 0.6f : 0.35f));
      g.setColour(juce::Colour(APP_COLOR_PRIMARY).withAlpha(alpha));
      g.fillEllipse(startX + dotSpacing * static_cast<float>(i),
                    baseY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
    }
  }

private:
  void timerCallback() override {
    ++tick;
    repaint();
  }

  int tick = 0;
};

class SplashWindow : public juce::DocumentWindow {
public:
  SplashWindow()
      : juce::DocumentWindow("",
                             juce::Colour(APP_COLOR_BACKGROUND),
                             juce::DocumentWindow::closeButton,
                             false) {
    setUsingNativeTitleBar(false);
    setTitleBarButtonsRequired(0, false);
    setResizable(false, false);
    setAlwaysOnTop(true);
    setOpaque(true);

    auto *content = new SplashComponent();
    setContentOwned(content, true);
    setSize(420, 230);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
  }

  void closeButtonPressed() override {}
};

class SVCFusionStudioApplication : public juce::JUCEApplication {
public:
  SVCFusionStudioApplication() {}

  const juce::String getApplicationName() override { return "SVCFusion Studio"; }

  const juce::String getApplicationVersion() override { return "1.0.0"; }

  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String &commandLine) override {
    AppLogger::init();
    LOG("========== APP STARTING ==========");
    OnnxRuntimeLoader::ensureLoaded();

    if (commandLine.contains("--benchmark-hnsep")) {
      runHNSepBenchmark();
      quit();
      return;
    }

    if (commandLine.contains("--test-inference")) {
      runTestInference(commandLine);
      quit();
      return;
    }

    LOG("Initializing fonts...");
    AppFont::initialize();
    TimecodeFont::initialize();
    LOG("Loading localization...");
    Localization::loadFromSettings();
    LOG("Localization loaded, showing splash...");
#if JUCE_STANDALONE_APPLICATION
    splashWindow = std::make_unique<SplashWindow>();
#endif
    juce::MessageManager::callAsync([this]() {
      LOG("Creating MainWindow...");
      mainWindow = std::make_unique<MainWindow>(getApplicationName());
      splashWindow = nullptr;
      LOG("MainWindow created and visible");
    });
  }

  void shutdown() override {
    mainWindow = nullptr;
    TimecodeFont::shutdown();
    AppFont::shutdown(); // Release font resources before JUCE shuts down
  }

  void systemRequestedQuit() override { quit(); }

  void anotherInstanceStarted(const juce::String &commandLine) override {
    juce::ignoreUnused(commandLine);
  }

  class MainWindow : public juce::DocumentWindow {
  public:
    MainWindow(juce::String name)
        : DocumentWindow(name, juce::Colour(APP_COLOR_BACKGROUND),
                         DocumentWindow::allButtons,
                         false) // Don't add to desktop yet
    {
      LOG("MainWindow: constructor start");

      // Ensure window is opaque - this must be set before any
      // transparency-related operations
      setOpaque(true);

      LOG("MainWindow: creating MainComponent...");
      // Set content first, ensuring it's also opaque
      auto *content = new MainComponent();
      LOG("MainWindow: MainComponent created");
      content->setOpaque(true);
      setContentOwned(content, true);

      // Now set native title bar after content is set
      setUsingNativeTitleBar(true);

      setResizable(true, true);

      // Ensure window is still opaque before adding to desktop
      // (some operations might affect opacity state)
      setOpaque(true);

      LOG("MainWindow: adding to desktop...");
      // Now add to desktop after all properties are set
      addToDesktop();

#if JUCE_WINDOWS
      requestDirect2DRenderer(*this, "MainWindow");
#endif

      auto *display = WindowSizing::getDisplayForComponent(this);
      auto constraints = WindowSizing::Constraints();
      auto desiredSize = content->getSavedWindowSize();
      if (desiredSize.x <= 0 || desiredSize.y <= 0)
        desiredSize = {WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight};

      if (display != nullptr) {
        auto initialBounds = WindowSizing::getInitialBounds(
            desiredSize.x, desiredSize.y, *display, constraints);
        auto maxBounds = WindowSizing::getMaxBounds(*display);
        setBounds(initialBounds);
        setResizeLimits(constraints.minWidth, constraints.minHeight,
                        maxBounds.getWidth(), maxBounds.getHeight());
      } else {
        setSize(desiredSize.x, desiredSize.y);
        centreWithSize(getWidth(), getHeight());
      }

      LOG("MainWindow: initial size " + juce::String(getWidth()) + "x" +
          juce::String(getHeight()));
      setVisible(true);
      LOG("MainWindow: setVisible(true) done");

#if JUCE_WINDOWS
      // Enable dark mode for title bar
      if (auto *peer = getPeer()) {
        if (auto hwnd = (HWND)peer->getNativeHandle()) {
          // Enable immersive dark mode for title bar
          constexpr DWORD darkMode = 1;
          DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                                &darkMode, sizeof(darkMode));

          // Enable rounded corners on Windows 11+
          DWORD preference = 2; // DWMWCP_ROUND
          DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                                &preference, sizeof(preference));
        }
      }
#elif JUCE_MAC
      // Enable dark mode for macOS window
      if (auto *peer = getPeer())
        PlatformUtils::setDarkAppearance(peer->getNativeHandle());
#endif
    }

    void closeButtonPressed() override {
      JUCEApplication::getInstance()->systemRequestedQuit();
    }

  private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
  };

private:
  std::unique_ptr<MainWindow> mainWindow;
#if JUCE_STANDALONE_APPLICATION
  std::unique_ptr<SplashWindow> splashWindow;
#endif

  void runTestInference(const juce::String &commandLine) {
    LOG("===== TEST INFERENCE START =====");

    auto args = juce::StringArray::fromTokens(commandLine, true);
    juce::String wavPath;
    juce::String voicebankPath;
    juce::String deviceName = "DirectML";
    juce::String pitchName = "RMVPE";
    int testDeviceId = 0;

    for (int i = 0; i < args.size(); ++i) {
      if (args[i] == "--test-inference" && i + 1 < args.size())
        wavPath = args[++i];
      else if (args[i] == "--voicebank" && i + 1 < args.size())
        voicebankPath = args[++i];
      else if (args[i] == "--device" && i + 1 < args.size())
        deviceName = args[++i];
      else if (args[i] == "--pitch" && i + 1 < args.size())
        pitchName = args[++i];
      else if (args[i] == "--device-id" && i + 1 < args.size())
        testDeviceId = args[++i].getIntValue();
    }

    if (wavPath.isEmpty()) {
      LOG("TEST: usage --test-inference <wav> [--voicebank <dir>] [--device DirectML|CPU] [--device-id N] [--pitch RMVPE|FCPE]");
      return;
    }

    LOG("TEST: wav=" + wavPath + " voicebank=" + voicebankPath +
        " device=" + deviceName + " id=" + juce::String(testDeviceId) +
        " pitch=" + pitchName);

    auto ec = std::make_unique<EditorController>(false);
    ec->setPitchDetectorType(pitchName.equalsIgnoreCase("FCPE")
                                 ? PitchDetectorType::FCPE
                                 : PitchDetectorType::RMVPE);
    ec->setDeviceConfig(deviceName, testDeviceId);

    LOG("TEST: reloading inference models (RMVPE/HNSep/GAME)...");
    logGpuMemory("before model load");
    auto tModelStart = std::chrono::steady_clock::now();
    ec->reloadInferenceModels(false);
    auto tModelEnd = std::chrono::steady_clock::now();
    LOG("TEST: models loaded in " +
        juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(
                         tModelEnd - tModelStart)
                         .count()) +
        " ms");
    logGpuMemory("after RMVPE load");

    LOG("TEST: loading audio...");
    juce::File wavFile(wavPath);
    if (!wavFile.existsAsFile()) {
      LOG("TEST: wav file not found: " + wavPath);
      return;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(wavFile));
    if (!reader) {
      LOG("TEST: failed to create reader");
      return;
    }

    const int numSamples = static_cast<int>(reader->lengthInSamples);
    const int srcSampleRate = static_cast<int>(reader->sampleRate);
    LOG("TEST: audio file: " + juce::String(reader->numChannels) + "ch, " +
        juce::String(numSamples) + " samples, " +
        juce::String(srcSampleRate) + " Hz");

    juce::AudioBuffer<float> buffer;
    if (reader->numChannels == 1) {
      buffer.setSize(1, numSamples);
      reader->read(&buffer, 0, numSamples, 0, true, false);
    } else {
      juce::AudioBuffer<float> stereoBuffer(
          static_cast<int>(reader->numChannels), numSamples);
      reader->read(&stereoBuffer, 0, numSamples, 0, true, true);
      buffer.setSize(1, numSamples);
      for (int ch = 0; ch < stereoBuffer.getNumChannels(); ++ch) {
        const float *src = stereoBuffer.getReadPointer(ch);
        float *dst = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
          dst[i] += src[i] / stereoBuffer.getNumChannels();
      }
    }

    if (srcSampleRate != SAMPLE_RATE) {
      LOG("TEST: resampling from " + juce::String(srcSampleRate) + " to " +
          juce::String(SAMPLE_RATE));
      const double ratio =
          static_cast<double>(srcSampleRate) / SAMPLE_RATE;
      const int newNumSamples = static_cast<int>(numSamples / ratio);
      juce::AudioBuffer<float> resampled(1, newNumSamples);
      const float *src = buffer.getReadPointer(0);
      float *dst = resampled.getWritePointer(0);
      for (int i = 0; i < newNumSamples; ++i) {
        const double srcPos = i * ratio;
        const int srcIndex = static_cast<int>(srcPos);
        const double frac = srcPos - srcIndex;
        if (srcIndex + 1 < numSamples)
          dst[i] = static_cast<float>(src[srcIndex] * (1.0 - frac) +
                                      src[srcIndex + 1] * frac);
        else
          dst[i] = src[srcIndex];
      }
      buffer = std::move(resampled);
    }

    LOG("TEST: audio loaded: " + juce::String(buffer.getNumSamples()) +
        " samples (" + juce::String(static_cast<double>(buffer.getNumSamples()) / SAMPLE_RATE, 1) + "s)");

    auto project = std::make_unique<Project>();
    auto &audioData = project->getAudioData();
    audioData.waveform = std::move(buffer);
    audioData.sampleRate = SAMPLE_RATE;
    audioData.originalWaveform.makeCopyOf(audioData.waveform);

    LOG("TEST: computing mel spectrogram...");
    {
      const float *samples = audioData.waveform.getReadPointer(0);
      const int n = audioData.waveform.getNumSamples();
      MelSpectrogram melComputer(SAMPLE_RATE, N_FFT, HOP_SIZE, NUM_MELS, FMIN,
                                 FMAX);
      audioData.melSpectrogram = melComputer.compute(samples, n);
    }
    LOG("TEST: mel frames=" + juce::String(audioData.melSpectrogram.size()));

    ec->setProject(std::move(project));
    auto *proj = ec->getProject();

    LOG("TEST: analyzing audio (HNSep + pitch + GAME)...");
    logGpuMemory("before analyze");
    auto tAnalyzeStart = std::chrono::steady_clock::now();
    std::atomic<bool> analyzeDone(false);
    ec->analyzeAudio(
        *proj,
        [](double progress, const juce::String &msg) {
          LOG("TEST: analyze progress=" + juce::String(progress, 2) + " " + msg);
        },
        [&analyzeDone]() {
          analyzeDone.store(true);
          LOG("TEST: analyze complete callback fired");
        });
    auto tAnalyzeEnd = std::chrono::steady_clock::now();
    LOG("TEST: analysis took " +
        juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(
                         tAnalyzeEnd - tAnalyzeStart)
                         .count()) +
        " ms");
    logGpuMemory("after analyze (HNSep+RMVPE+Vocoder resident)");

    if (!analyzeDone.load()) {
      LOG("TEST: analyze did not complete (model error?)");
    }

    auto &ad = proj->getAudioData();
    LOG("TEST: analysis result -- f0=" + juce::String(ad.f0.size()) +
        " frames, voiced=" +
        juce::String(std::count(ad.voicedMask.begin(), ad.voicedMask.end(),
                                true)) +
        " harmonic=" + juce::String(ad.harmonicWaveform.getNumSamples()) +
        " noise=" + juce::String(ad.noiseWaveform.getNumSamples()));

    if (voicebankPath.isNotEmpty()) {
      juce::File vbDir(voicebankPath);
      if (!vbDir.isDirectory()) {
        LOG("TEST: voicebank dir not found: " + voicebankPath);
      } else {
        LOG("TEST: loading voicebank: " + voicebankPath);
        auto tVbStart = std::chrono::steady_clock::now();
        bool vbOk = ec->loadSVCModelFromDirectory(vbDir);
        auto tVbEnd = std::chrono::steady_clock::now();
        LOG("TEST: voicebank load " + juce::String(vbOk ? "OK" : "FAILED") +
            " in " +
            juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(
                             tVbEnd - tVbStart)
                             .count()) +
            " ms");

        if (vbOk) {
          LOG("TEST: running full SVC conversion...");
          auto tSvcStart = std::chrono::steady_clock::now();

          std::atomic<bool> svcDone(false);
          std::atomic<bool> svcSuccess(false);

          ec->runFullSVCConversionAsync(
              [](const juce::String &msg) {
                LOG("TEST: svc progress: " + msg);
              },
              [&svcDone, &svcSuccess](bool success) {
                LOG("TEST: svc complete callback: success=" +
                    juce::String(success ? "true" : "false"));
                svcSuccess.store(success);
                svcDone.store(true);
              });

          auto *mm = juce::MessageManager::getInstance();
          auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(600);
          while (!svcDone.load() &&
                 std::chrono::steady_clock::now() < deadline) {
            mm->runDispatchLoopUntil(50);
          }

          auto tSvcEnd = std::chrono::steady_clock::now();
          if (!svcDone.load()) {
            LOG("TEST: SVC conversion TIMED OUT after 600s");
          } else {
            LOG("TEST: SVC conversion " +
                juce::String(svcSuccess.load() ? "SUCCESS" : "FAILED") +
                " in " +
                juce::String(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 tSvcEnd - tSvcStart)
                                 .count()) +
                " ms");
            logGpuMemory("after SVC conversion");
          }
        }
      }
    }

    auto &finalAudio = proj->getAudioData().waveform;
    juce::File outFile = wavFile.getSiblingFile(
        wavFile.getFileNameWithoutExtension() + "_output.wav");
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(
        new juce::FileOutputStream(outFile), SAMPLE_RATE,
        finalAudio.getNumChannels(), 16, {}, 0));
    if (writer) {
      writer->writeFromAudioSampleBuffer(finalAudio, 0,
                                         finalAudio.getNumSamples());
      LOG("TEST: output saved to " + outFile.getFullPathName());
    } else {
      LOG("TEST: failed to create output writer");
    }

    LOG("===== TEST INFERENCE END =====");
  }

  void runHNSepBenchmark() {
    LOG("===== HNSEP BENCHMARK START =====");

    const auto hnsepDir = PlatformPaths::getModelSubDir("hnsep", "hnsep_VR.onnx");
    const auto dmlFile = hnsepDir.getChildFile("hnsep_VR_convstft.onnx");
    const auto cpuFile = hnsepDir.getChildFile("hnsep_VR.onnx");

    if (!dmlFile.existsAsFile()) {
      LOG("BENCHMARK: DML model not found: " + dmlFile.getFullPathName());
      return;
    }

    const int sampleRate = 44100;
    const int numSamples = sampleRate * 30;
    std::vector<float> audio(static_cast<size_t>(numSamples), 0.0f);
    for (int i = 0; i < numSamples; ++i)
      audio[static_cast<size_t>(i)] =
          0.1f * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 *
                          static_cast<double>(i) / static_cast<double>(sampleRate));

    HNSepModel model;
    const auto provider = GPUProvider::DirectML;
    const int devId = 0;

    LOG("BENCHMARK: loading model...");
    if (!model.loadModel(dmlFile, provider, devId)) {
      LOG("BENCHMARK: model load failed, trying CPU fallback: " +
          cpuFile.getFullPathName());
      if (!model.loadModel(cpuFile, GPUProvider::CPU, 0)) {
        LOG("BENCHMARK: CPU fallback also failed");
        return;
      }
    }
    LOG("BENCHMARK: model loaded, externalStft=" +
        juce::String(model.isLoaded() ? "yes" : "no"));

    std::vector<float> harmonic, noise;
    constexpr int warmup = 1;
    constexpr int runs = 3;
    double warmupPreMs = 0.0;

    for (int i = 0; i < warmup + runs; ++i) {
      const auto isWarmup = i < warmup;
      const auto start = std::chrono::high_resolution_clock::now();
      const bool ok = model.separate(audio.data(), numSamples, harmonic, noise);
      const auto end = std::chrono::high_resolution_clock::now();
      const double totalMs =
          std::chrono::duration<double, std::milli>(end - start).count();

      if (!ok) {
        LOG("BENCHMARK run " + juce::String(i) + ": FAILED");
        continue;
      }

      const auto &t = model.lastChunkTiming;
      if (isWarmup)
        warmupPreMs = t.preMs;
      LOG("BENCHMARK run " + juce::String(i) +
          (isWarmup ? " (warmup)" : "") +
          ": total=" + juce::String(totalMs, 1) + " ms" +
          ", stft=" + juce::String(t.stftMs, 1) +
          ", pre=" + juce::String(t.preMs, 1) +
          ", core=" + juce::String(t.coreMs, 1) +
          ", post=" + juce::String(t.postMs, 1) +
          ", chunkTotal=" + juce::String(t.totalMs, 1));
    }

    model.unload();

    // ── Test: does DML shader cache survive unload/reload? ──
    LOG("BENCHMARK: === SHADER CACHE TEST ===");
    LOG("BENCHMARK: reloading model after unload (same shape)...");

    auto tReloadStart = std::chrono::high_resolution_clock::now();
    if (!model.loadModel(dmlFile, provider, devId)) {
      LOG("BENCHMARK: reload failed");
    } else {
      auto tReloadEnd = std::chrono::high_resolution_clock::now();
      LOG("BENCHMARK: reload took " +
          juce::String(std::chrono::duration<double, std::milli>(
                           tReloadEnd - tReloadStart)
                           .count(), 1) + " ms");

      const auto start = std::chrono::high_resolution_clock::now();
      std::vector<float> h2, n2;
      const bool ok2 = model.separate(audio.data(), numSamples, h2, n2);
      const auto end = std::chrono::high_resolution_clock::now();
      const double totalMs2 =
          std::chrono::duration<double, std::milli>(end - start).count();

      if (ok2) {
        const auto &t2 = model.lastChunkTiming;
        LOG("BENCHMARK reload+infer: total=" + juce::String(totalMs2, 1) +
            " ms, pre=" + juce::String(t2.preMs, 1) +
            ", core=" + juce::String(t2.coreMs, 1) +
            " (warmup was pre=" + juce::String(warmupPreMs, 1) + ")");
        if (t2.preMs < 1000.0) {
          LOG("BENCHMARK: *** DML SHADER CACHE SURVIVES UNLOAD *** -- pre-warm at startup is viable");
        } else {
          LOG("BENCHMARK: *** DML SHADER CACHE DOES NOT SURVIVE UNLOAD *** -- must keep session resident");
        }
      }
      model.unload();
    }

    LOG("===== HNSEP BENCHMARK END =====");
  }
};

START_JUCE_APPLICATION(SVCFusionStudioApplication)
