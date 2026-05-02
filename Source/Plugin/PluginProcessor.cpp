#include "PluginProcessor.h"
#include "../UI/IMainView.h"
#include "../Utils/Localization.h"
#include "../Utils/OnnxRuntimeLoader.h"
#include "PluginEditor.h"

SVCFusionStudioAudioProcessor::SVCFusionStudioAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true))
#endif
{
  OnnxRuntimeLoader::ensureLoaded();
}

SVCFusionStudioAudioProcessor::~SVCFusionStudioAudioProcessor() = default;

const juce::String SVCFusionStudioAudioProcessor::getName() const {
  return JucePlugin_Name;
}

bool SVCFusionStudioAudioProcessor::acceptsMidi() const {
#if JucePlugin_WantsMidiInput
  return true;
#else
  return false;
#endif
}

bool SVCFusionStudioAudioProcessor::producesMidi() const {
#if JucePlugin_ProducesMidiOutput
  return true;
#else
  return false;
#endif
}

bool SVCFusionStudioAudioProcessor::isMidiEffect() const {
#if JucePlugin_IsMidiEffect
  return true;
#else
  return false;
#endif
}

void SVCFusionStudioAudioProcessor::prepareToPlay(double sampleRate,
                                            int samplesPerBlock) {
  hostSampleRate = sampleRate;
  realtimeProcessor.prepareToPlay(sampleRate, samplesPerBlock);

#if JucePlugin_Enable_ARA
  prepareToPlayForARA(sampleRate, samplesPerBlock,
                      getMainBusNumOutputChannels(), getProcessingPrecision());
#endif

  // Non-ARA capture controller
  captureController->prepare(sampleRate, getMainBusNumOutputChannels(),
                             MAX_CAPTURE_SECONDS);
  lastCaptureUiState = captureController->getState();
}

void SVCFusionStudioAudioProcessor::releaseResources() {
#if JucePlugin_Enable_ARA
  releaseResourcesForARA();
#endif
}

#if !JucePlugin_PreferredChannelConfigurations
bool SVCFusionStudioAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;
  auto out = layouts.getMainOutputChannelSet();
  return out == juce::AudioChannelSet::mono() ||
         out == juce::AudioChannelSet::stereo();
}
#endif

bool SVCFusionStudioAudioProcessor::isARAModeActive() const {
#if JucePlugin_Enable_ARA
  if (auto *editor = getActiveEditor()) {
    if (auto *araEditor =
            dynamic_cast<juce::AudioProcessorEditorARAExtension *>(editor)) {
      if (auto *editorView = araEditor->getARAEditorView()) {
        return editorView->getDocumentController() != nullptr;
      }
    }
  }
#endif
  return false;
}

HostCompatibility::HostInfo SVCFusionStudioAudioProcessor::getHostInfo() const {
  return HostCompatibility::detectHost(
      const_cast<SVCFusionStudioAudioProcessor *>(this));
}

juce::String SVCFusionStudioAudioProcessor::getHostStatusMessage() const {
  auto hostInfo = getHostInfo();
  bool araActive = isARAModeActive();

  if (hostInfo.type != HostCompatibility::HostType::Unknown) {
    if (araActive)
      return hostInfo.name + " - ARA Mode";
    if (hostInfo.supportsARA)
      return hostInfo.name + " - Non-ARA (ARA Available)";
    return hostInfo.name + " - Non-ARA Mode";
  }
  return araActive ? "ARA Mode" : "Non-ARA Mode";
}

void SVCFusionStudioAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                           juce::MidiBuffer &midiMessages) {
  juce::ignoreUnused(midiMessages);
  juce::ScopedNoDenormals noDenormals;

  // Process transport control requests and update sync state
  transportController.processBlock(getPlayHead(), hostSampleRate);

#if JucePlugin_Enable_ARA
  // ARA mode: let ARA renderer handle audio
  if (processBlockForARA(buffer, isRealtime(), getPlayHead()))
    return;
#endif

  // Non-ARA mode
  juce::AudioPlayHead::PositionInfo posInfo;
  if (auto *playHead = getPlayHead()) {
    if (auto info = playHead->getPosition())
      posInfo = *info;
  }

  processNonARAMode(buffer, posInfo,
                    isRealtime() == juce::AudioProcessor::Realtime::yes);
}

void SVCFusionStudioAudioProcessor::processNonARAMode(
    juce::AudioBuffer<float> &buffer,
    const juce::AudioPlayHead::PositionInfo &posInfo, bool isRealtime) {
  const int numSamples = buffer.getNumSamples();
  const int numChannels = buffer.getNumChannels();
  const bool hostIsPlaying = posInfo.getIsPlaying();

  // Check if we have analyzed project ready for real-time processing
  bool hasProject =
      mainComponent && mainComponent->hasAnalyzedProject();

  // Update UI cursor position from host playback position (only when we have
  // analyzed audio)
  if (isRealtime && mainComponent) {
    if (hostIsPlaying && hasProject) {
      // Only sync cursor after capture is complete and analyzed
      double timeInSeconds = 0.0;
      if (auto samples = posInfo.getTimeInSamples())
        timeInSeconds = static_cast<double>(*samples) / hostSampleRate;
      else if (auto time = posInfo.getTimeInSeconds())
        timeInSeconds = *time;

      auto state = hostUiSyncState;
      state->latestSeconds.store(timeInSeconds);

      // Never touch UI on the audio thread: coalesce to a single async update
      if (!state->posPending.exchange(true)) {
        juce::Component::SafePointer<juce::Component> safeMain(
            mainComponent->getComponent());
        juce::MessageManager::callAsync([safeMain, state]() {
          state->posPending.store(false);
          if (auto *view =
                  dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->updatePlaybackPosition(state->latestSeconds.load());
        });
      }
    } else if (!hostIsPlaying && hasProject) {
      auto state = hostUiSyncState;
      if (!state->stoppedPending.exchange(true)) {
        juce::Component::SafePointer<juce::Component> safeMain(
            mainComponent->getComponent());
        juce::MessageManager::callAsync([safeMain, state]() {
          state->stoppedPending.store(false);
          if (auto *view =
                  dynamic_cast<IMainView *>(safeMain.getComponent()))
            view->notifyHostStopped();
        });
      }
    }
  }

  if (!hostIsPlaying) {
    // Still let the capture state machine observe transport stop so it can
    // finalize and dispatch analysis, but never output audio when stopped.
    captureController->processBlock(buffer, false);

    if (captureController->shouldFinalize()) {
      NonAraCaptureController::FinalizeResult result;
      if (captureController->finalizeCapture(hostSampleRate, result) &&
          mainComponent) {
        juce::Component::SafePointer<juce::Component> safeMain(
            mainComponent->getComponent());
        auto controller = captureController;
        juce::MessageManager::callAsync([safeMain, controller,
                                         samples = result.numSamples,
                                         sr = result.sampleRate]() mutable {
          auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
          if (!view)
            return;
          if (!controller)
            return;
          auto trimmed = controller->copyCapturedAudio(samples);
          controller->onAnalysisDispatched();
          view->setStatusMessage(TR("progress.analyzing"));
          view->setHostAudio(trimmed, sr);
        });
      }
    }

    buffer.clear();
    return;
  }

  if (hasProject && realtimeProcessor.isReady()) {
    // Real-time pitch correction mode
    juce::AudioBuffer<float> outputBuffer(numChannels, numSamples);
    if (realtimeProcessor.processBlock(buffer, outputBuffer, &posInfo)) {
      for (int ch = 0; ch < numChannels; ++ch)
        buffer.copyFrom(ch, 0, outputBuffer, ch, 0, numSamples);
    }
    return;
  }

  // Capture mode
  captureController->processBlock(buffer, hostIsPlaying);

  // UI: transition into recording
  auto currentState = captureController->getState();
  if (currentState != lastCaptureUiState) {
    if (currentState == NonAraCaptureController::State::Capturing &&
        mainComponent) {
      juce::Component::SafePointer<juce::Component> safeMain(
          mainComponent->getComponent());
      juce::MessageManager::callAsync([safeMain]() {
        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->setStatusMessage(TR("progress.recording"));
      });
    }
    lastCaptureUiState = currentState;
  }

  if (captureController->shouldFinalize()) {
    NonAraCaptureController::FinalizeResult result;
    if (captureController->finalizeCapture(hostSampleRate, result) &&
        mainComponent) {
      juce::Component::SafePointer<juce::Component> safeMain(
          mainComponent->getComponent());
      auto controller = captureController;
      juce::MessageManager::callAsync([safeMain, controller,
                                       samples = result.numSamples,
                                       sr = result.sampleRate]() mutable {
        auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
        if (!view)
          return;
        if (!controller)
          return;
        auto trimmed = controller->copyCapturedAudio(samples);
        controller->onAnalysisDispatched();
        view->setStatusMessage(TR("progress.analyzing"));
        view->setHostAudio(trimmed, sr);
      });
    }
  }

  // Passthrough during capture
}

void SVCFusionStudioAudioProcessor::startCapture() {
  captureController->resetToWaiting();
}

void SVCFusionStudioAudioProcessor::stopCapture() { captureController->stop(); }

void SVCFusionStudioAudioProcessor::setMainComponent(IMainView *mc) {
  mainComponent = mc;
  if (mc) {
    mc->bindRealtimeProcessor(realtimeProcessor);
    if (pendingStateJson.isNotEmpty() &&
        mc->restoreProjectJson(pendingStateJson)) {
      pendingStateJson.clear();
    }
  } else {
    realtimeProcessor.setProject(nullptr);
    realtimeProcessor.setVocoder(nullptr);
  }
}

juce::AudioProcessorEditor *SVCFusionStudioAudioProcessor::createEditor() {
  return new SVCFusionStudioAudioProcessorEditor(*this);
}

void SVCFusionStudioAudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
  juce::String jsonString;
  if (mainComponent) {
    jsonString = mainComponent->serializeProjectJson();
  } else if (pendingStateJson.isNotEmpty()) {
    jsonString = pendingStateJson;
  }
  if (jsonString.isNotEmpty())
    destData.append(jsonString.toRawUTF8(), jsonString.getNumBytesAsUTF8());
}

void SVCFusionStudioAudioProcessor::setStateInformation(const void *data,
                                                  int sizeInBytes) {
  juce::String jsonString(
      juce::CharPointer_UTF8(static_cast<const char *>(data)),
      static_cast<size_t>(sizeInBytes));

  if (mainComponent && mainComponent->restoreProjectJson(jsonString)) {
    return;
  }

  pendingStateJson = jsonString;
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new SVCFusionStudioAudioProcessor();
}

#if JucePlugin_Enable_ARA
#include "ARADocumentController.h"

const ARA::ARAFactory *JUCE_CALLTYPE createARAFactory() {
  return juce::ARADocumentControllerSpecialisation::createARAFactory<
      SVCFusionStudioDocumentController>();
}
#endif
