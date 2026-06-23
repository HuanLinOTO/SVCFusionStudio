#include "MainComponent.h"
#include "../Audio/RealtimePitchProcessor.h"
#include "../Audio/IO/MidiExporter.h"
#include "../Models/ProjectSerializer.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Localization.h"
#include "../Utils/PlatformPaths.h"
#include "../Utils/MelSpectrogram.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/SHA256Utils.h"
#include "../Utils/UI/WindowSizing.h"
#include <atomic>
#include <climits>
#include <cmath>
#include <iostream>
#include <optional>
#include <thread>

namespace {
enum class ExportFormat { wav, flac, aiff, ogg };

static bool isMenuPopupActive() {
  if (auto *modal = juce::Component::getCurrentlyModalComponent())
    return modal->getName() == "menu";

  return false;
}

struct ExportSettings {
  ExportFormat format = ExportFormat::wav;
  int sampleRate = SAMPLE_RATE;
  int channels = 1;
  int bitsPerSample = 16;
  int bitrateKbps = 192; // Used for compressed formats where available
};

static juce::String getFormatDisplayName(ExportFormat format) {
  switch (format) {
  case ExportFormat::wav:
    return "WAV";
  case ExportFormat::flac:
    return "FLAC";
  case ExportFormat::aiff:
    return "AIFF";
  case ExportFormat::ogg:
    return "OGG";
  }
  return "WAV";
}

static juce::String getFormatExtension(ExportFormat format) {
  switch (format) {
  case ExportFormat::wav:
    return "wav";
  case ExportFormat::flac:
    return "flac";
  case ExportFormat::aiff:
    return "aiff";
  case ExportFormat::ogg:
    return "ogg";
  }
  return "wav";
}

static juce::String getFormatWildcard(ExportFormat format) {
  return "*." + getFormatExtension(format);
}

static juce::AudioBuffer<float> convertChannels(const juce::AudioBuffer<float> &input,
                                                int outChannels) {
  outChannels = juce::jlimit(1, 2, outChannels);
  const int inChannels = juce::jmax(1, input.getNumChannels());
  const int numSamples = input.getNumSamples();

  juce::AudioBuffer<float> output(outChannels, numSamples);
  output.clear();

  if (outChannels == 1) {
    float *dst = output.getWritePointer(0);
    for (int i = 0; i < numSamples; ++i) {
      float sum = 0.0f;
      for (int ch = 0; ch < inChannels; ++ch)
        sum += input.getSample(ch, i);
      dst[i] = sum / static_cast<float>(inChannels);
    }
    return output;
  }

  // Stereo output
  if (inChannels == 1) {
    output.copyFrom(0, 0, input, 0, 0, numSamples);
    output.copyFrom(1, 0, input, 0, 0, numSamples);
  } else {
    output.copyFrom(0, 0, input, 0, 0, numSamples);
    output.copyFrom(1, 0, input, 1, 0, numSamples);
  }
  return output;
}

static juce::AudioBuffer<float> resampleAudio(const juce::AudioBuffer<float> &input,
                                              int sourceRate, int targetRate) {
  if (sourceRate <= 0 || targetRate <= 0 || sourceRate == targetRate)
    return input;

  const int channels = juce::jmax(1, input.getNumChannels());
  const int inSamples = input.getNumSamples();
  const double ratio = static_cast<double>(sourceRate) / targetRate;
  const int outSamples = juce::jmax(1, static_cast<int>(std::llround(inSamples / ratio)));

  juce::AudioBuffer<float> output(channels, outSamples);
  output.clear();

  for (int ch = 0; ch < channels; ++ch) {
    juce::LagrangeInterpolator interpolator;
    interpolator.reset();
    interpolator.process(ratio, input.getReadPointer(ch), output.getWritePointer(ch),
                         outSamples);
  }
  return output;
}

static std::optional<int> parseFirstInt(const juce::String &s) {
  juce::String digits;
  bool started = false;
  for (auto c : s) {
    if (juce::CharacterFunctions::isDigit(c)) {
      digits += juce::String::charToString(c);
      started = true;
    } else if (started) {
      break;
    }
  }
  if (digits.isEmpty())
    return std::nullopt;
  return digits.getIntValue();
}

static int chooseQualityIndex(const juce::StringArray &options, int targetKbps) {
  if (options.isEmpty())
    return 0;

  int bestIdx = 0;
  int bestDist = INT_MAX;
  for (int i = 0; i < options.size(); ++i) {
    auto parsed = parseFirstInt(options[i]);
    if (!parsed.has_value())
      continue;
    const int dist = std::abs(parsed.value() - targetKbps);
    if (dist < bestDist) {
      bestDist = dist;
      bestIdx = i;
    }
  }
  return bestIdx;
}

static juce::AudioFormat *findFormatForExtension(juce::AudioFormatManager &manager,
                                                 const juce::String &extension) {
  for (int i = 0; i < manager.getNumKnownFormats(); ++i) {
    auto *fmt = manager.getKnownFormat(i);
    if (!fmt)
      continue;
    auto exts = fmt->getFileExtensions();
    for (const auto &ext : exts) {
      if (ext.equalsIgnoreCase(extension))
        return fmt;
    }
  }
  return nullptr;
}

class ExportSettingsContent final : public juce::Component, private juce::Button::Listener {
public:
  ExportSettingsContent(int inputSampleRate,
                        std::function<void(std::optional<ExportSettings>)> done)
      : onDone(std::move(done)) {
    addAndMakeVisible(title);
    title.setText("Export Settings", juce::dontSendNotification);
    title.setJustificationType(juce::Justification::centredLeft);
    title.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    title.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));

    setupCombo(formatBox, formatLabel, "Format", {"WAV", "FLAC", "AIFF", "OGG"}, 1);

    int srId = 3;
    if (inputSampleRate >= 47000)
      srId = 4;
    else if (inputSampleRate >= 43000)
      srId = 3;
    else if (inputSampleRate >= 30000)
      srId = 2;
    else
      srId = 1;
    setupCombo(sampleRateBox, sampleRateLabel, "Sample Rate",
               {"22050", "32000", "44100", "48000"}, srId);
    setupCombo(bitDepthBox, bitDepthLabel, "Bit Depth", {"16", "24", "32"}, 1);
    setupCombo(bitrateBox, bitrateLabel, "Bitrate (kbps)",
               {"64", "96", "128", "160", "192", "256", "320"}, 5);
    setupCombo(channelsBox, channelsLabel, "Channels", {"Mono", "Stereo"}, 1);

    addAndMakeVisible(cancelButton);
    addAndMakeVisible(exportButton);
    cancelButton.setButtonText("Cancel");
    exportButton.setButtonText("Export");
    cancelButton.addListener(this);
    exportButton.addListener(this);
  }

  ~ExportSettingsContent() override {
    cancelButton.removeListener(this);
    exportButton.removeListener(this);
  }

  void paint(juce::Graphics &g) override {
    g.fillAll(APP_COLOR_SURFACE);
  }

  void resized() override {
    auto area = getLocalBounds().reduced(12);
    title.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);

    const int rowH = 28;
    layoutRow(area.removeFromTop(rowH), formatLabel, formatBox);
    area.removeFromTop(6);
    layoutRow(area.removeFromTop(rowH), sampleRateLabel, sampleRateBox);
    area.removeFromTop(6);
    layoutRow(area.removeFromTop(rowH), bitDepthLabel, bitDepthBox);
    area.removeFromTop(6);
    layoutRow(area.removeFromTop(rowH), bitrateLabel, bitrateBox);
    area.removeFromTop(6);
    layoutRow(area.removeFromTop(rowH), channelsLabel, channelsBox);

    area.removeFromTop(12);
    auto btnRow = area.removeFromTop(30);
    auto right = btnRow.removeFromRight(190);
    cancelButton.setBounds(right.removeFromLeft(90));
    right.removeFromLeft(10);
    exportButton.setBounds(right.removeFromLeft(90));
  }

private:
  void setupCombo(juce::ComboBox &box, juce::Label &label, const juce::String &labelText,
                  const juce::StringArray &items, int selectedId) {
    addAndMakeVisible(label);
    addAndMakeVisible(box);
    label.setText(labelText, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    box.addItemList(items, 1);
    box.setSelectedId(selectedId, juce::dontSendNotification);
  }

  static void layoutRow(juce::Rectangle<int> row, juce::Label &label, juce::ComboBox &box) {
    label.setBounds(row.removeFromLeft(120));
    box.setBounds(row);
  }

  ExportSettings getSettings() const {
    ExportSettings settings;
    switch (formatBox.getSelectedId()) {
    case 2:
      settings.format = ExportFormat::flac;
      break;
    case 3:
      settings.format = ExportFormat::aiff;
      break;
    case 4:
      settings.format = ExportFormat::ogg;
      break;
    default:
      settings.format = ExportFormat::wav;
      break;
    }
    settings.sampleRate = sampleRateBox.getText().getIntValue();
    settings.bitsPerSample = bitDepthBox.getText().getIntValue();
    settings.bitrateKbps = bitrateBox.getText().getIntValue();
    settings.channels = channelsBox.getSelectedId() == 2 ? 2 : 1;
    return settings;
  }

  void buttonClicked(juce::Button *button) override {
    auto closeWith = [this](std::optional<ExportSettings> result) {
      if (onDone)
        onDone(std::move(result));
      if (auto *dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState(0);
    };

    if (button == &exportButton)
      closeWith(getSettings());
    else if (button == &cancelButton)
      closeWith(std::nullopt);
  }

  std::function<void(std::optional<ExportSettings>)> onDone;
  juce::Label title;
  juce::Label formatLabel, sampleRateLabel, bitDepthLabel, bitrateLabel, channelsLabel;
  juce::ComboBox formatBox, sampleRateBox, bitDepthBox, bitrateBox, channelsBox;
  juce::TextButton cancelButton, exportButton;
};

static void showExportSettingsDialogAsync(
    juce::Component *parent, int inputSampleRate,
    std::function<void(std::optional<ExportSettings>)> onDone) {
  auto *content = new ExportSettingsContent(inputSampleRate, std::move(onDone));
  content->setSize(420, 270);

  juce::DialogWindow::LaunchOptions opts;
  opts.content.setOwned(content);
  opts.dialogTitle = "Export Settings";
  opts.componentToCentreAround = parent;
  opts.dialogBackgroundColour = APP_COLOR_SURFACE;
  opts.escapeKeyTriggersCloseButton = true;
  opts.useNativeTitleBar = false;
  opts.resizable = false;
  opts.useBottomRightCornerResizer = false;
  opts.launchAsync();
}
} // namespace

// ── Draggable splitter bar between track overview and editor ──

class MainComponent::SplitterBar : public juce::Component {
public:
    SplitterBar() {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(APP_COLOR_BORDER);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        dragStartY = e.y;
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (onResize)
            onResize(e.y - dragStartY);
    }

    std::function<void(int deltaY)> onResize;

private:
    int dragStartY = 0;
};

// ── Global right sidebar for parameter/voicebank panels ──

class MainComponent::SidebarComponent : public juce::Component {
public:
    SidebarComponent() {
        addAndMakeVisible(titleLabel);
        titleLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        titleLabel.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        titleLabel.setJustificationType(juce::Justification::centredLeft);
    }

    void setContent(juce::Component* content, const juce::String& title) {
        if (currentContent != content) {
            if (currentContent != nullptr)
                removeChildComponent(currentContent);
            currentContent = content;
            if (content != nullptr)
                addAndMakeVisible(content);
        }
        titleLabel.setText(title, juce::dontSendNotification);
        resized();
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(8);
        titleLabel.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(4);
        if (currentContent != nullptr)
            currentContent->setBounds(bounds);
    }

    void paint(juce::Graphics& g) override {
        g.setColour(APP_COLOR_SURFACE);
        g.fillAll();
        g.setColour(APP_COLOR_BORDER);
        g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));
    }

private:
    juce::Label titleLabel;
    juce::Component* currentContent = nullptr;
};

MainComponent::MainComponent(bool enableAudioDevice)
    : enableAudioDeviceFlag(enableAudioDevice), pianoRollView(pianoRoll) {
  LOG("MainComponent: constructor start");
  setSize(WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight);
  setOpaque(true); // Required for native title bar

  LOG("MainComponent: creating core components...");
  // Initialize components
  editorController = std::make_unique<EditorController>(enableAudioDeviceFlag);
  LOG("MainComponent: editorController created");
  undoManager = std::make_unique<PitchUndoManager>(100);
  LOG("MainComponent: undoManager created");
  commandManager = std::make_unique<juce::ApplicationCommandManager>();
  LOG("MainComponent: commandManager created");
  undoManager->onHistoryChanged = [this]() {
    if (commandManager)
      commandManager->commandStatusChanged();
  };

  // Initialize new modular components
  fileManager = std::make_unique<AudioFileManager>();
  LOG("MainComponent: fileManager created");
  menuHandler = std::make_unique<MenuHandler>();
  LOG("MainComponent: menuHandler created");
  settingsManager = std::make_unique<SettingsManager>();
  AppFont::setScale(settingsManager->getUIFontScale());
  LOG("MainComponent: settingsManager created");

  LOG("MainComponent: loading ONNX models...");
  editorController->setPitchDetectorType(
      settingsManager->getPitchDetectorType());
  editorController->setDeviceConfig(settingsManager->getDevice(),
                                    settingsManager->getGPUDeviceId());
  editorController->reloadInferenceModels(false);

  LOG("MainComponent: wiring up components...");
  menuHandler->setUndoManager(undoManager.get());
  menuHandler->setCommandManager(commandManager.get());
  menuHandler->setCommandInfoProvider(
      [this](juce::CommandID commandID, juce::ApplicationCommandInfo &info) {
        this->getCommandInfo(commandID, info);
        return info.shortName.isNotEmpty();
      });
  menuHandler->setPluginMode(isPluginMode());
  recentFiles = settingsManager->getRecentFiles();
  refreshRecentFilesMenu();
  menuHandler->setOnRecentFileSelected(
      [this](const juce::File &file) { openRecentFile(file); });
  settingsManager->setVocoder(editorController->getVocoder());

  // Load vocoder settings
  settingsManager->applySettings();

  LOG("MainComponent: initializing audio device...");
  // Initialize audio (standalone app only)
  if (auto *audioEngine = editorController->getAudioEngine())
    audioEngine->initializeAudio();
  LOG("MainComponent: audio initialized");

  LOG("MainComponent: setting up callbacks...");
  if (editorController) {
    editorController->setBackgroundStatusCallback(
        [this](const juce::String &message, bool active) {
          if (active) {
            toolbar.showProgress(message);
            toolbar.setProgress(-1.0f);
          } else if (!isLoadingAudio.load()) {
            toolbar.hideProgress();
          }
        });
  }

  // Initialize view state from settings
  pianoRoll.setShowDeltaPitch(settingsManager->getShowDeltaPitch());
  pianoRoll.setShowBasePitch(settingsManager->getShowBasePitch());
  pianoRoll.setShowSomeSegmentsDebug(
      settingsManager->getShowSomeSegmentsDebug());
  pianoRoll.setShowSomeValuesDebug(
      settingsManager->getShowSomeValuesDebug());
  pianoRoll.setShowUvInterpolationDebug(
      settingsManager->getShowUvInterpolationDebug());
  pianoRoll.setShowActualF0Debug(
      settingsManager->getShowActualF0Debug());
  pianoRoll.setShowFpsOverlay(settingsManager->getShowFpsOverlay());
  pianoRoll.setShowBackgroundWaveform(
      settingsManager->getShowBackgroundWaveform());
  pianoRollView.setShowSomeSegmentsDebug(
      settingsManager->getShowSomeSegmentsDebug());

  // Add child components - macOS uses native menu, others use in-app menu bar
#if JUCE_MAC
  if (!isPluginMode())
    juce::MenuBarModel::setMacMainMenu(menuHandler.get());
#else
  menuBar.setModel(menuHandler.get());
  addAndMakeVisible(menuBar);
#endif
  addAndMakeVisible(toolbar);
  addAndMakeVisible(trackList);
  splitterBar = std::make_unique<SplitterBar>();
  addAndMakeVisible(*splitterBar);
  addAndMakeVisible(workspace);
  sidebar = std::make_unique<SidebarComponent>();
  addAndMakeVisible(*sidebar);
  addChildComponent(modelDebugPanel);
  modelDebugPanel.setMultiLine(true);
  modelDebugPanel.setReadOnly(true);
  modelDebugPanel.setScrollbarsShown(true);
  modelDebugPanel.setCaretVisible(false);
  modelDebugPanel.setPopupMenuEnabled(true);
  modelDebugPanel.setInterceptsMouseClicks(false, false);
  modelDebugPanel.setColour(juce::TextEditor::backgroundColourId,
                            APP_COLOR_SURFACE.withAlpha(0.95f));
  modelDebugPanel.setColour(juce::TextEditor::textColourId,
                            APP_COLOR_TEXT_PRIMARY);
  modelDebugPanel.setColour(juce::TextEditor::outlineColourId,
                            APP_COLOR_BORDER);
  modelDebugPanel.setColour(juce::TextEditor::focusedOutlineColourId,
                            APP_COLOR_PRIMARY);
#if !JUCE_MAC
  menuBar.toFront(false);
#endif

  // Setup workspace with stacked piano roll + overview cards
  workspace.setMainContent(&pianoRollView);
  workspace.getMainCard().setBackgroundColour(juce::Colours::transparentBlack);
  workspace.getMainCard().setBorderColour(juce::Colours::transparentBlack);

  // Setup track list
  trackList.setEditorController(editorController.get());
  trackList.onTrackSelected = [this](int trackIndex) { onTrackSelected(trackIndex); };
  trackList.onTrackTypeChanged = [this](int trackIndex, TrackType newType) { onTrackTypeChanged(trackIndex, newType); };
  trackList.onTrackDeleted = [this](int trackIndex) { onTrackDeleted(trackIndex); };
  trackList.onTracksChanged = [this]() { rebuildAudioEngine(); };

  // Setup splitter bar resize
  splitterBar->onResize = [this](int deltaY) {
    int newHeight = trackListHeight + deltaY;
    int availableHeight = getHeight() - 24 - 52 - 4; // menu + toolbar + splitter
    newHeight = juce::jlimit(48, availableHeight - 100, newHeight);
    if (newHeight != trackListHeight) {
      trackListHeight = newHeight;
      resized();
    }
  };

  // Setup global sidebar with parameter panel (visible by default)
  sidebar->setContent(&parameterPanel, TR("panel.parameters"));


  // Configure toolbar for plugin mode
  if (isPluginMode())
    toolbar.setPluginMode(true);

  // Set undo manager for piano roll
  pianoRoll.setUndoManager(undoManager.get());
  parameterPanel.setUndoManager(undoManager.get());
  pianoRollView.setUndoManager(undoManager.get());

  // Setup toolbar callbacks
  toolbar.onPlay = [this]() { play(); };
  toolbar.onPause = [this]() { pause(); };
  toolbar.onStop = [this]() { stop(); };
  toolbar.onGoToStart = [this]() { seek(0.0); };
  toolbar.onGoToEnd = [this]() {
    if (auto *project = getProject())
      seek(project->getAudioData().getDuration());
  };
  toolbar.onZoomChanged = [this](float pps) { onZoomChanged(pps); };
  toolbar.onEditModeChanged = [this](EditMode mode) { setEditMode(mode); };
  toolbar.onHNSepRequested = [this]() {
    const bool visible = !pianoRollView.isHNSepVisible();
    pianoRollView.getHNSepLane().setViewTransform(pianoRoll.getPixelsPerSecond(),
                                                  pianoRoll.getScrollX());
    pianoRollView.setHNSepVisible(visible);
    toolbar.setHNSepVisible(visible);
  };
  toolbar.onLoopToggled = [this](bool enabled) {
    auto *project = getProject();
    if (!project)
      return;
    project->setLoopEnabled(enabled);
    const auto &loopRange = project->getLoopRange();
    toolbar.setLoopEnabled(loopRange.enabled);
    if (auto *audioEngine = editorController
                                ? editorController->getAudioEngine()
                                : nullptr) {
      if (loopRange.enabled)
        audioEngine->setLoopRange(loopRange.startSeconds, loopRange.endSeconds);
      audioEngine->setLoopEnabled(loopRange.enabled);
    }
    pianoRoll.repaint();
  };
  toolbar.onToggleParameters = [this](bool visible) {
    parametersVisible = visible;
    if (visible) {
      voicebankVisible = false;
      toolbar.setVoicebankVisible(false);
      sidebar->setContent(&parameterPanel, TR("panel.parameters"));
    }
    resized();
  };
  toolbar.onToggleVoicebank = [this](bool visible) {
    voicebankVisible = visible;
    if (visible) {
      parametersVisible = false;
      toolbar.setParametersVisible(false);
      sidebar->setContent(&voicebankPanel, TR("panel.voicebank"));
    }
    resized();
  };

  // Setup voicebank activation callback - loads SVC model when user activates a voicebank
  voicebankPanel.onVoicebankActivated = [this](const VoicebankPanel::VoicebankInfo& info) {
    if (!editorController) return;

    // Prevent voicebank activation while audio is loading
    if (isLoadingAudio.load()) {
      DBG("MainComponent: Cannot activate voicebank while audio is loading");
      return;
    }

    juce::File voicebankDir(info.path);
    if (!voicebankDir.isDirectory())
    {
      DBG("MainComponent: Voicebank directory not found: " + info.path);
      return;
    }

    // Load the SVC model on a background thread to avoid UI blocking
    auto* ec = editorController.get();
    toolbar.showProgress("Loading SVC model...");
    toolbar.setProgress(-1.0f);
    // Disable editing during SVC model loading + conversion
    pianoRoll.setEnabled(false);
    parameterPanel.setEnabled(false);

    juce::Component::SafePointer<MainComponent> safeThis(this);
    std::thread([ec, voicebankDir, safeThis]() {
      bool ok = ec->loadSVCModelFromDirectory(voicebankDir);
      juce::MessageManager::callAsync([ok, voicebankDir, safeThis, ec]() {
        if (!safeThis) return;
        if (ok)
        {
          DBG("MainComponent: SVC model loaded: " + voicebankDir.getFileName());
          safeThis->pianoRollView.getHNSepLane().setShfcEnabled(true);
          // If audio is already loaded, run full SVC conversion
          auto* proj = ec->getProject();
          if (proj && proj->getAudioData().waveform.getNumSamples() > 0
              && !proj->getAudioData().f0.empty())
          {
            safeThis->toolbar.showProgress("Converting audio with SVC...");
            safeThis->toolbar.setProgress(-1.0f);
            ec->runFullSVCConversionAsync(
                [safeThis](const juce::String& msg) {
                  if (safeThis) safeThis->toolbar.showProgress(msg);
                },
                [safeThis](bool success) {
                  if (!safeThis) return;
                  safeThis->toolbar.hideProgress();
                  safeThis->pianoRoll.setEnabled(true);
                  safeThis->parameterPanel.setEnabled(true);
                  if (success) {
                    safeThis->pianoRoll.invalidateWaveformCache();
                    safeThis->pianoRoll.repaint();
                    DBG("MainComponent: Full SVC conversion complete");
                  } else {
                    DBG("MainComponent: Full SVC conversion failed");
                  }
                });
          }
          else
          {
            safeThis->toolbar.hideProgress();
            safeThis->pianoRoll.setEnabled(true);
            safeThis->parameterPanel.setEnabled(true);
          }
        }
        else
        {
          safeThis->toolbar.hideProgress();
          safeThis->pianoRoll.setEnabled(true);
          safeThis->parameterPanel.setEnabled(true);
          safeThis->pianoRollView.getHNSepLane().setShfcEnabled(false);
          DBG("MainComponent: Failed to load SVC model: " + voicebankDir.getFileName());
        }
      });
    }).detach();
  };

  // Scan persistent voicebanks directory on startup
  voicebankPanel.scanVoicebanksDirectory();

  // Plugin mode callbacks
  toolbar.onReanalyze = [this]() {
    if (onReanalyzeRequested)
      onReanalyzeRequested();
  };
  // Removed onRender callback - Melodyne-style: edits automatically trigger
  // real-time processing

  // Setup piano roll callbacks
  pianoRoll.onSeek = [this](double time) { seek(time); };
  pianoRoll.onNoteSelected = [this](Note *note) { onNoteSelected(note); };
  pianoRoll.onPitchEdited = [this]() { onPitchEdited(); };
  pianoRoll.isSVCActive = [this]() -> bool {
    return editorController && editorController->isSVCModelActive();
  };
  pianoRollView.getHNSepLane().setShfcEnabled(
      editorController && editorController->isSVCModelActive());
  pianoRoll.onPitchEditFinished = [this]() {
    resynthesizeIncremental();
    // Melodyne-style: trigger real-time processor update in plugin mode
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };
  pianoRoll.onZoomChanged = [this](float pps) {
    onZoomChanged(pps);
    pianoRollView.getHNSepLane().setViewTransform(pianoRoll.getPixelsPerSecond(),
                                                  pianoRoll.getScrollX());
    pianoRollView.refreshOverview();
  };
  pianoRoll.onScrollChanged = [this](double x) {
    pianoRollView.getHNSepLane().setViewTransform(pianoRoll.getPixelsPerSecond(), x);
    pianoRollView.refreshOverview();
  };
  pianoRoll.onLoopRangeChanged = [this](const LoopRange &range) {
    toolbar.setLoopEnabled(range.enabled);
    if (auto *audioEngine = editorController
                                ? editorController->getAudioEngine()
                                : nullptr) {
      audioEngine->setLoopRange(range.startSeconds, range.endSeconds);
      audioEngine->setLoopEnabled(range.enabled);
    }
  };

  // Setup parameter panel callbacks
  parameterPanel.onParameterChanged = [this]() { onPitchEdited(); };
  parameterPanel.onParameterEditFinished = [this]() {
    resynthesizeIncremental();
    // Melodyne-style: trigger real-time processor update in plugin mode
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };
  parameterPanel.onGlobalPitchChanged = [this]() {
    pianoRoll.repaint(); // Update display
  };
  parameterPanel.onGlobalPitchEditFinished = [this]() {
    // When SVC is active, global pitch change affects the entire F0 sent to SVC,
    // so we must re-run full SVC conversion instead of incremental resynthesis.
    if (editorController && editorController->isSVCModelActive()) {
      toolbar.showProgress("Re-converting with SVC...");
      toolbar.setProgress(-1.0f);
      toolbar.setEnabled(false);
      juce::Component::SafePointer<MainComponent> safeThis(this);
      editorController->runFullSVCConversionAsync(
          [safeThis](const juce::String& msg) {
            if (safeThis) safeThis->toolbar.showProgress(msg);
          },
          [safeThis](bool success) {
            if (!safeThis) return;
            safeThis->toolbar.setEnabled(true);
            safeThis->toolbar.hideProgress();
            if (success) {
              safeThis->pianoRoll.invalidateWaveformCache();
              safeThis->pianoRoll.repaint();
              DBG("Global pitch -> full SVC conversion complete");
            } else {
              DBG("Global pitch -> full SVC conversion failed");
            }
            safeThis->notifyProjectDataChanged();
          });
    } else {
      // No SVC active — mark all notes dirty and do incremental resynthesis
      if (auto* proj = getProject()) {
        for (auto& note : proj->getNotes())
          note.markDirty();
      }
      resynthesizeIncremental();
      notifyProjectDataChanged();
    }
  };
  parameterPanel.onVolumeChanged = [this](float dB) {
    if (auto *audioEngine = editorController
                                ? editorController->getAudioEngine()
                                : nullptr)
      audioEngine->setVolumeDb(dB);
  };
  parameterPanel.onPitchOffsetModeChanged = [this](bool preMode) {
    DBG("Pitch offset mode changed: " + juce::String(preMode ? "pre-SVC" : "post-SVC"));
    // When SVC is active and the mode changes, re-run full SVC conversion
    // because the F0 sent to SVC is now different
    if (editorController && editorController->isSVCModelActive()) {
      toolbar.showProgress("Re-converting with SVC...");
      toolbar.setProgress(-1.0f);
      toolbar.setEnabled(false);
      juce::Component::SafePointer<MainComponent> safeThis(this);
      editorController->runFullSVCConversionAsync(
          [safeThis](const juce::String& msg) {
            if (safeThis) safeThis->toolbar.showProgress(msg);
          },
          [safeThis](bool success) {
            if (!safeThis) return;
            safeThis->toolbar.setEnabled(true);
            safeThis->toolbar.hideProgress();
            if (success) {
              safeThis->pianoRoll.invalidateWaveformCache();
              safeThis->pianoRoll.repaint();
              DBG("Pitch offset mode -> full SVC conversion complete");
            }
            safeThis->notifyProjectDataChanged();
          });
    }
  };
  parameterPanel.setProject(getProject());

  pianoRollView.getHNSepLane().onParamEdited = [this]() {
    pianoRollView.repaint();
    onPitchEdited();
  };
  pianoRollView.getHNSepLane().onParamEditFinished = [this]() {
    resynthesizeIncremental();
    notifyProjectDataChanged();
    if (isPluginMode() && onPitchEditFinished)
      onPitchEditFinished();
  };

  // Sync toolbar toggle with panel visibility
  toolbar.setParametersVisible(workspace.isPanelVisible("parameters"));
  toolbar.setVoicebankVisible(workspace.isPanelVisible("voicebank"));
  workspace.onPanelVisibilityChanged = [this](const juce::String& id, bool visible) {
    if (id == "parameters")
      toolbar.setParametersVisible(visible);
    else if (id == "voicebank")
      toolbar.setVoicebankVisible(visible);
  };

  // Setup audio engine callbacks
  if (auto *audioEngine = editorController->getAudioEngine()) {
    juce::Component::SafePointer<MainComponent> safeThis(this);

    audioEngine->setPositionCallback([safeThis](double position) {
      if (safeThis == nullptr)
        return;
      // Throttle cursor updates - store position and let timer handle it
      safeThis->pendingCursorTime.store(position);
      safeThis->hasPendingCursorUpdate.store(true);
    });

    audioEngine->setFinishCallback([safeThis]() {
      if (safeThis == nullptr)
        return;
      safeThis->isPlaying = false;
      safeThis->toolbar.setPlaying(false);
    });
  }

  // Set initial project (may be nullptr if no tracks loaded yet)
  if (auto* initialProject = editorController->getProject()) {
    pianoRoll.setProject(initialProject);
    pianoRollView.setProject(initialProject);
  }
  pianoRollView.setHNSepVisible(false);
  trackList.setVisible(true);

  // Register commands with the command manager
  commandManager->registerAllCommandsForTarget(this);
  commandManager->setFirstCommandTarget(this);

  // Connect MenuHandler to ApplicationCommandManager for automatic menu updates
  // This is required for macOS native menu bar to reflect command states
  menuHandler->setApplicationCommandManagerToWatch(commandManager.get());

  // Add command manager key mappings as a KeyListener
  // This enables automatic keyboard shortcut dispatch
  addKeyListener(commandManager->getKeyMappings());
  setWantsKeyboardFocus(true);

  // Load config
  if (enableAudioDeviceFlag)
    settingsManager->loadConfig();

  LOG("MainComponent: starting timer...");
  // Start timer for UI updates
  startTimerHz(30);
  LOG("MainComponent: constructor complete");
}

void MainComponent::reloadInferenceModels(bool async) {
  if (!settingsManager || !editorController)
    return;

  editorController->setDeviceConfig(settingsManager->getDevice(),
                                    settingsManager->getGPUDeviceId());
  editorController->reloadInferenceModels(async);
}

bool MainComponent::isInferenceBusy() const {
  if (isLoadingAudio.load())
    return true;
  if (editorController && editorController->isLoading())
    return true;
  if (editorController && editorController->isRendering())
    return true;
  if (editorController && editorController->isInferenceBusy())
    return true;
  return false;
}

MainComponent::~MainComponent() {
#if JUCE_MAC
  juce::MenuBarModel::setMacMainMenu(nullptr);
#else
  menuBar.setModel(nullptr);
#endif
  removeKeyListener(commandManager->getKeyMappings());
  stopTimer();


  if (auto *audioEngine = editorController->getAudioEngine()) {
    audioEngine->clearCallbacks();
    audioEngine->shutdownAudio();
  }

  if (enableAudioDeviceFlag)
    settingsManager->saveConfig();
}

juce::Point<int> MainComponent::getSavedWindowSize() const {
  if (settingsManager)
    return {settingsManager->getWindowWidth(),
            settingsManager->getWindowHeight()};
  return {WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight};
}

void MainComponent::paint(juce::Graphics &g) {
  g.fillAll(APP_COLOR_BACKGROUND);
}

void MainComponent::resized() {
  auto bounds = getLocalBounds();

#if !JUCE_MAC
  // The custom menu layer spans the window so its in-component popup is not
  // clipped. Its hitTest lets normal clicks pass through when the menu is shut.
  menuBar.setBounds(getLocalBounds());
  bounds.removeFromTop(24);
#endif

  // Toolbar
  toolbar.setBounds(bounds.removeFromTop(52));

  // Right sidebar (global, spans track list + editor)
  int sidebarWidth = (parametersVisible || voicebankVisible) ? 320 : 0;
  if (sidebarWidth > 0)
    sidebar->setBounds(bounds.removeFromRight(sidebarWidth));

  // Track list (top section)
  int availableHeight = bounds.getHeight();
  int trackH = juce::jlimit(48, juce::jmax(48, availableHeight - 100), trackListHeight);
  trackListHeight = trackH;
  trackList.setBounds(bounds.removeFromTop(trackH));

  // Splitter bar between track list and editor
  if (splitterBar)
    splitterBar->setBounds(bounds.removeFromTop(4));

  // Workspace takes remaining space (piano roll only)
  workspace.setBounds(bounds);

  if (modelDebugPanelVisible) {
    const auto pianoBounds = getLocalArea(&pianoRoll, pianoRoll.getLocalBounds());
    const int panelWidth = std::min(320, std::max(220, pianoBounds.getWidth() / 3));
    const int panelHeight = std::min(220, std::max(150, pianoBounds.getHeight() / 3));
    const int margin = 14;
    const int bottomControlClearance = 34;
    auto panelBounds = juce::Rectangle<int>(panelWidth, panelHeight)
                           .withRightX(pianoBounds.getRight() - margin)
                           .withBottomY(pianoBounds.getBottom() -
                                        bottomControlClearance);
    modelDebugPanel.setBounds(panelBounds);
    modelDebugPanel.toFront(false);
  }

  if (settingsOverlay)
    settingsOverlay->setBounds(getLocalBounds());

  if (enableAudioDeviceFlag && settingsManager)
    settingsManager->setWindowSize(getWidth(), getHeight());
}

void MainComponent::mouseDown(const juce::MouseEvent &e) {
  juce::ignoreUnused(e);
}

void MainComponent::mouseDrag(const juce::MouseEvent &e) {
  juce::ignoreUnused(e);
}

void MainComponent::mouseDoubleClick(const juce::MouseEvent &e) {
  juce::ignoreUnused(e);
}

void MainComponent::timerCallback() {
  if (isMenuPopupActive())
    return;

  // Handle throttled cursor updates (30Hz max)
  if (hasPendingCursorUpdate.load()) {
    double position = pendingCursorTime.load();
    hasPendingCursorUpdate.store(false);

    pianoRoll.setCursorTime(position);
    toolbar.setCurrentTime(position);

    // Update playhead in track overview
    if (editorController) {
      double dur = 0.0;
      int count = editorController->getTrackCount();
      for (int i = 0; i < count; ++i) {
        auto* t = editorController->getTrack(i);
        if (t && t->project)
          dur = std::max(dur, static_cast<double>(t->project->getAudioData().getDuration()));
      }
      trackList.setPlayheadPosition(position, dur);
    }

    // Follow playback: scroll to keep cursor visible
    if (isPlaying && toolbar.isFollowPlayback()) {
      float cursorX =
          static_cast<float>(position * pianoRoll.getPixelsPerSecond());
      float viewWidth = static_cast<float>(
          pianoRoll.getWidth() - 74); // minus piano keys and scrollbar
      float scrollX = static_cast<float>(pianoRoll.getScrollX());

      // If cursor is outside visible area, scroll to center it
      if (cursorX < scrollX || cursorX > scrollX + viewWidth) {
        double newScrollX =
            std::max(0.0, static_cast<double>(cursorX - viewWidth * 0.3f));
        pianoRoll.setScrollX(newScrollX);
      }
    }
  }

  if (isLoadingAudio.load()) {
    const auto progress = static_cast<float>(loadingProgress.load());
    toolbar.setProgress(progress);

    juce::String msg;
    {
      const juce::ScopedLock sl(loadingMessageLock);
      msg = loadingMessage;
    }

    if (msg.isNotEmpty() && msg != lastLoadingMessage) {
      toolbar.showProgress(msg);
      lastLoadingMessage = msg;
    }

    return;
  }

  if (lastLoadingMessage.isNotEmpty()) {
    toolbar.hideProgress();
    lastLoadingMessage.clear();
  }

  if (modelDebugPanelVisible && editorController) {
    auto text = editorController->getModelDebugStatusText();
    if (text != lastModelDebugText) {
      modelDebugPanel.setText(text, juce::dontSendNotification);
      lastModelDebugText = text;
    }
  }
}

void MainComponent::setModelDebugPanelVisible(bool visible) {
  modelDebugPanelVisible = visible;
  modelDebugPanel.setVisible(visible);
  if (visible && editorController) {
    lastModelDebugText = editorController->getModelDebugStatusText();
    modelDebugPanel.setText(lastModelDebugText, juce::dontSendNotification);
  }
  resized();
  if (commandManager)
    commandManager->commandStatusChanged();
}

bool MainComponent::keyPressed(const juce::KeyPress &key,
                               juce::Component * /*originatingComponent*/) {
  // All keyboard shortcuts are now handled by ApplicationCommandManager
  // This method is kept for potential future non-command keyboard handling
  juce::ignoreUnused(key);
  return false;
}

void MainComponent::clearProject() {
  if (!editorController) return;

  // Cancel any running SVC conversion
  if (editorController->isSVCConvertingNow()) {
    editorController->cancelSVCConversion();
  }

  // Stop playback
  stop();

  // Clear undo history
  if (undoManager) undoManager->clear();

  // Clear all tracks
  auto& tracks = editorController->getTracks();
  tracks.clear();
  editorController->setActiveTrack(-1);

  // Create a default empty project for the piano roll
  auto emptyProject = std::make_unique<Project>();
  pianoRoll.setProject(emptyProject.get());
  pianoRollView.setProject(emptyProject.get());
  parameterPanel.setProject(emptyProject.get());

  // Update UI
  toolbar.setTotalTime(0.0);
  toolbar.setCurrentTime(0.0);
  toolbar.hideProgress();

  // Clear audio engine
  if (auto* engine = editorController->getAudioEngine()) {
    engine->setTracks({});
    engine->loadWaveform(juce::AudioBuffer<float>(), 44100);
  }

  pianoRoll.invalidateWaveformCache();
  pianoRoll.repaint();
  refreshTrackList();
  resized();

  DBG("MainComponent: Project cleared (multi-track)");
}

void MainComponent::saveProject() {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]() {
      if (safeThis != nullptr)
        safeThis->saveProject();
    });
    return;
  }

  if (!editorController)
    return;

  auto& tracks = editorController->getTracks();
  if (tracks.empty())
    return;

  // Ensure audio SHA is computed for all tracks
  for (auto& track : tracks) {
    if (track && track->project) {
      auto audioFile = track->project->getFilePath();
      if (audioFile.existsAsFile())
        track->project->setAudioSha256(SHA256Utils::fileSHA256(audioFile));
    }
  }

  // Determine target file path
  auto target = juce::File{};
  if (auto* activeProject = getProject())
    target = activeProject->getProjectFilePath();

  auto doSave = [this](const juce::File& file) {
    if (!editorController)
      return false;

    auto& tracksRef = editorController->getTracks();
    const auto& loopRange = editorController->getSessionLoopRange();

    bool ok = ProjectSerializer::saveTracksToFile(tracksRef, file, loopRange);
    if (ok) {
      for (auto& track : tracksRef) {
        if (track && track->project)
          track->project->setProjectFilePath(file);
      }
    }
    return ok;
  };

  if (target == juce::File{}) {
    // Prevent re-triggering while dialog is open
    if (fileChooser != nullptr)
      return;

    // Default next to first track's audio if possible
    auto audio = juce::File{};
    for (auto& track : tracks) {
      if (track && track->project) {
        audio = track->project->getFilePath();
        if (audio.existsAsFile())
          break;
      }
    }
    if (audio.existsAsFile())
      target = audio.withFileExtension("htpx");
    else
      target =
          juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
              .getChildFile("Untitled.htpx");

#if JUCE_WINDOWS && JUCE_MODAL_LOOPS_PERMITTED
    juce::FileChooser chooser(TR("dialog.save_project"), target, "*.htpx",
                              true, false, this);
    if (!chooser.browseForFileToSave(true))
      return;

    auto file = chooser.getResult();
    if (file == juce::File{})
      return;

    if (file.getFileExtension().isEmpty())
      file = file.withFileExtension("htpx");

    toolbar.showProgress(TR("progress.saving"));
    toolbar.setProgress(-1.0f);

    doSave(file);

    toolbar.hideProgress();
    return;
#else
    fileChooser = std::make_unique<juce::FileChooser>(
        TR("dialog.save_project"), target, "*.htpx");

    auto chooserFlags = juce::FileBrowserComponent::saveMode |
                        juce::FileBrowserComponent::canSelectFiles |
                        juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    fileChooser->launchAsync(chooserFlags, [safeThis, doSave](
                                               const juce::FileChooser &fc) {
      if (safeThis == nullptr)
        return;

      auto file = fc.getResult();
      safeThis->fileChooser
          .reset();

      if (file == juce::File{})
        return;

      if (file.getFileExtension().isEmpty())
        file = file.withFileExtension("htpx");

      safeThis->toolbar.showProgress(TR("progress.saving"));
      safeThis->toolbar.setProgress(-1.0f);

      doSave(file);

      safeThis->toolbar.hideProgress();
    });

    return;
#endif
  }

  toolbar.showProgress(TR("progress.saving"));
  toolbar.setProgress(-1.0f);

  doSave(target);

  toolbar.hideProgress();
}

void MainComponent::openFile() {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]() {
      if (safeThis != nullptr)
        safeThis->openFile();
    });
    return;
  }

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.select_audio"), juce::File{},
      "*.htpx;*.wav;*.mp3;*.flac;*.aiff");

  auto chooserFlags = juce::FileBrowserComponent::openMode |
                      juce::FileBrowserComponent::canSelectFiles;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc) {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser.reset(); // Allow next dialog to open
        if (file.existsAsFile()) {
          safeThis->addRecentFile(file);
          if (file.hasFileExtension("htpx") || file.hasFileExtension(".htpx"))
            safeThis->openProjectFile(file);
          else
            safeThis->loadAudioFile(file);
        }
      });
}

void MainComponent::refreshRecentFilesMenu() {
  if (menuHandler)
    menuHandler->setRecentFiles(recentFiles);
  if (menuHandler)
    menuHandler->menuItemsChanged();
}

void MainComponent::addRecentFile(const juce::File &file) {
  if (!file.existsAsFile())
    return;

  const auto fullPath = file.getFullPathName();
  recentFiles.removeString(fullPath);
  recentFiles.insert(0, fullPath);

  constexpr int kMaxRecentFiles = 10;
  while (recentFiles.size() > kMaxRecentFiles)
    recentFiles.remove(recentFiles.size() - 1);

  if (settingsManager) {
    settingsManager->setLastFilePath(file);
    settingsManager->setRecentFiles(recentFiles);
    settingsManager->saveConfig();
  }
  refreshRecentFilesMenu();
}

void MainComponent::openRecentFile(const juce::File &file) {
  if (!file.existsAsFile()) {
    StyledMessageBox::show(this, "Recent file missing",
                           "File not found:\n" + file.getFullPathName(),
                           StyledMessageBox::WarningIcon);
    recentFiles.removeString(file.getFullPathName());
    if (settingsManager) {
      settingsManager->setRecentFiles(recentFiles);
      settingsManager->saveConfig();
    }
    refreshRecentFilesMenu();
    return;
  }

  if (file.hasFileExtension("htpx") || file.hasFileExtension(".htpx"))
    openProjectFile(file);
  else
    loadAudioFile(file);
}

void MainComponent::openProjectFile(const juce::File &file) {
  if (isLoadingAudio.load())
    return;
  addRecentFile(file);

  auto result = ProjectSerializer::loadTracksFromFile(file);
  auto loadedTracks = std::move(result.first);
  LoopRange sessionLoopRange = result.second;
  if (loadedTracks.empty()) {
    StyledMessageBox::show(this, "Open failed",
                           "Failed to load project:\n" + file.getFullPathName(),
                           StyledMessageBox::WarningIcon);
    return;
  }

  // Set project file path on all tracks
  for (auto& track : loadedTracks) {
    if (track && track->project)
      track->project->setProjectFilePath(file);
  }

  isLoadingAudio = true;
  loadingProgress = 0.0;
  {
    const juce::ScopedLock sl(loadingMessageLock);
    loadingMessage = TR("progress.loading_audio");
  }
  toolbar.showProgress(TR("progress.loading_audio"));
  toolbar.setProgress(0.0f);

  juce::Component::SafePointer<MainComponent> safeThis(this);

  std::thread([safeThis, tracks = std::move(loadedTracks),
               sessionLoopRange, file]() mutable {
    auto loadAudioForTrack = [](Track& track) -> bool {
      if (!track.project)
        return false;

      auto audioFile = track.project->getFilePath();
      if (!audioFile.existsAsFile())
        return false;

      juce::AudioFormatManager formatManager;
      formatManager.registerBasicFormats();
      std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(audioFile));
      if (!reader)
        return false;

      const int numSamples = static_cast<int>(reader->lengthInSamples);
      const int srcSampleRate = static_cast<int>(reader->sampleRate);

      juce::AudioBuffer<float> buffer(1, numSamples);
      if (reader->numChannels == 1) {
        reader->read(&buffer, 0, numSamples, 0, true, false);
      } else {
        juce::AudioBuffer<float> stereoBuffer(2, numSamples);
        reader->read(&stereoBuffer, 0, numSamples, 0, true, true);
        const float* left = stereoBuffer.getReadPointer(0);
        const float* right = stereoBuffer.getReadPointer(1);
        float* mono = buffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
          mono[i] = (left[i] + right[i]) * 0.5f;
      }

      if (srcSampleRate != SAMPLE_RATE) {
        const double ratio = static_cast<double>(srcSampleRate) / SAMPLE_RATE;
        const int newNumSamples = static_cast<int>(numSamples / ratio);
        juce::AudioBuffer<float> resampledBuffer(1, newNumSamples);
        const float* src = buffer.getReadPointer(0);
        float* dst = resampledBuffer.getWritePointer(0);
        for (int i = 0; i < newNumSamples; ++i) {
          const double srcPos = i * ratio;
          const int srcIndex = static_cast<int>(srcPos);
          const double frac = srcPos - srcIndex;
          if (srcIndex + 1 < numSamples)
            dst[i] = static_cast<float>(src[srcIndex] * (1.0 - frac) + src[srcIndex + 1] * frac);
          else
            dst[i] = src[srcIndex];
        }
        buffer = std::move(resampledBuffer);
      }

      const juce::String currentSha = SHA256Utils::fileSHA256(audioFile);
      track.project->setFilePath(audioFile);
      track.project->setAudioSha256(currentSha);

      auto& audioData = track.project->getAudioData();
      audioData.waveform = std::move(buffer);
      audioData.sampleRate = SAMPLE_RATE;
      audioData.originalWaveform.makeCopyOf(audioData.waveform);

      // Recompute mel if needed (pitch/note data from project file is kept)
      if (audioData.waveform.getNumSamples() > 0 &&
          audioData.melSpectrogram.empty() && !audioData.f0.empty()) {
        const float* samples = audioData.waveform.getReadPointer(0);
        const int nSamples = audioData.waveform.getNumSamples();
        MelSpectrogram melComputer(audioData.sampleRate, N_FFT, HOP_SIZE, NUM_MELS, FMIN, FMAX);
        audioData.melSpectrogram = melComputer.compute(samples, nSamples);
      }

      // Rebuild voiced mask if missing
      if (audioData.voicedMask.empty() && !audioData.f0.empty()) {
        audioData.voicedMask.resize(audioData.f0.size(), false);
        for (size_t i = 0; i < audioData.f0.size(); ++i)
          audioData.voicedMask[i] = audioData.f0[i] > 0.0f;
      }

      // Rebuild pitch curves if needed
      if (audioData.basePitch.empty() || audioData.deltaPitch.empty()) {
        if (!audioData.f0.empty())
          PitchCurveProcessor::rebuildCurvesFromSource(*track.project, audioData.f0);
        else if (!audioData.melSpectrogram.empty())
          PitchCurveProcessor::rebuildBaseFromNotes(*track.project);
      } else {
        PitchCurveProcessor::composeF0InPlace(*track.project, false);
      }

      return true;
    };

    // Load audio for all tracks
    int totalTracks = static_cast<int>(tracks.size());
    for (int i = 0; i < totalTracks; ++i) {
      if (tracks[static_cast<size_t>(i)]) {
        loadAudioForTrack(*tracks[static_cast<size_t>(i)]);
        double progress = 0.3 + 0.5 * (double)(i + 1) / totalTracks;
        juce::MessageManager::callAsync([safeThis, progress]() {
          if (safeThis) {
            safeThis->loadingProgress = progress;
            safeThis->toolbar.setProgress(static_cast<float>(progress));
          }
        });
      }
    }

    juce::MessageManager::callAsync(
        [safeThis, tracks = std::move(tracks), sessionLoopRange, file]() mutable {
          if (safeThis == nullptr || !safeThis->editorController) {
            return;
          }

          if (safeThis->undoManager)
            safeThis->undoManager->clear();

          // Clear existing tracks and set new ones
          auto& existingTracks = safeThis->editorController->getTracks();
          existingTracks.clear();

          int firstVocalIndex = -1;
          for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
            auto& track = tracks[static_cast<size_t>(i)];
            if (track && track->isVocal() && firstVocalIndex < 0)
              firstVocalIndex = i;
            existingTracks.push_back(std::move(track));
          }

          // Set active track to first vocal track (or first track if no vocal)
          int activeIdx = firstVocalIndex >= 0 ? firstVocalIndex : 0;
          safeThis->editorController->setActiveTrack(activeIdx);

          // Set session loop range
          safeThis->editorController->setSessionLoopRange(
              sessionLoopRange.startSeconds, sessionLoopRange.endSeconds);
          safeThis->editorController->setSessionLoopEnabled(sessionLoopRange.enabled);

          // Refresh audio engine
          safeThis->editorController->refreshAudioEngine();

          // Update UI
          auto* activeTrack = safeThis->editorController->getActiveTrack();
          auto* project = activeTrack ? activeTrack->getProject() : nullptr;

          if (project) {
            safeThis->pianoRoll.setProject(project);
            safeThis->pianoRollView.setProject(project);
            safeThis->parameterPanel.setProject(project);
            safeThis->toolbar.setTotalTime(project->getAudioData().getDuration());
            safeThis->toolbar.setLoopEnabled(sessionLoopRange.enabled);

            // Restore SVC state if needed
            auto& audioData = project->getAudioData();
            if (audioData.svcEnabled && safeThis->editorController) {
              juce::File voicebankPath(audioData.svcVoicebankPath);
              if (!voicebankPath.exists() && audioData.svcVoicebankName.isNotEmpty()) {
                voicebankPath = PlatformPaths::getVoicebanksDirectory()
                    .getChildFile(audioData.svcVoicebankName);
              }
              if (voicebankPath.isDirectory())
                safeThis->editorController->loadSVCModelFromDirectory(voicebankPath);
              else if (voicebankPath.existsAsFile())
                safeThis->editorController->loadSVCModel(voicebankPath);
            }
          }

          // Sync audio engine volume
          if (auto* engine = safeThis->editorController->getAudioEngine())
            if (project)
              engine->setVolumeDb(project->getVolume());

          // Load vocoder if needed
          if (auto* vocoder = safeThis->editorController->getVocoder();
              vocoder && !vocoder->isLoaded()) {
            auto modelPath = PlatformPaths::getModelFile("pc_nsf_hifigan.onnx");
            if (modelPath.existsAsFile())
              vocoder->loadModel(modelPath);
          }

          safeThis->refreshTrackList();
          safeThis->resized();
          safeThis->pianoRoll.invalidateWaveformCache();
          safeThis->pianoRoll.repaint();
          safeThis->isLoadingAudio = false;
          safeThis->toolbar.hideProgress();

          if (safeThis->isPluginMode())
            safeThis->notifyProjectDataChanged();
        });
  }).detach();
}

void MainComponent::loadAudioFile(const juce::File &file) {
  if (isLoadingAudio.load())
    return;
  // Cancel any running SVC conversion before loading new audio
  if (editorController && editorController->isSVCConvertingNow()) {
    editorController->cancelSVCConversion();
    pianoRoll.setEnabled(true);
    parameterPanel.setEnabled(true);
  }
  addRecentFile(file);

  isLoadingAudio = true;
  loadingProgress = 0.0;
  {
    const juce::ScopedLock sl(loadingMessageLock);
    loadingMessage = TR("progress.loading_audio");
  }
  toolbar.showProgress(TR("progress.loading_audio"));
  toolbar.setProgress(0.0f);

  juce::Component::SafePointer<MainComponent> safeThis(this);
  if (!editorController) {
    isLoadingAudio = false;
    return;
  }

  editorController->loadAudioFileAsync(
      file,
      [safeThis](double p, const juce::String &msg) {
        if (safeThis == nullptr)
          return;
        safeThis->loadingProgress = juce::jlimit(0.0, 1.0, p);
        const juce::ScopedLock sl(safeThis->loadingMessageLock);
        safeThis->loadingMessage = msg;
      },
      [safeThis](const juce::AudioBuffer<float> &original) {
        if (safeThis == nullptr)
          return;

        // Clear undo history before adding new track
        if (safeThis->undoManager)
          safeThis->undoManager->clear();

        // Refresh track list UI
        safeThis->refreshTrackList();

        // If this is the first track, update duration
        auto* ec = safeThis->editorController.get();
        if (ec && ec->getTrackCount() > 0) {
          auto* track = ec->getTrack(ec->getTrackCount() - 1);
          if (track && track->getProject()) {
            double dur = track->getProject()->getAudioData().getDuration();
            safeThis->toolbar.setTotalTime(dur);
          }
        }

        // Reset playing state
        if (safeThis->isPlaying) {
          safeThis->isPlaying = false;
          safeThis->toolbar.setPlaying(false);
        }

        // Audio engine is already updated by EditorController
        // Just sync volume display
        if (ec) {
          auto* activeTrack = ec->getActiveTrack();
          if (activeTrack && activeTrack->getProject()) {
            if (auto* engine = ec->getAudioEngine())
              engine->setVolumeDb(activeTrack->getProject()->getVolume());
          }
        }

        // Load vocoder if needed
        if (auto *vocoder = safeThis->editorController
                                ? safeThis->editorController->getVocoder()
                                : nullptr;
            vocoder && !vocoder->isLoaded()) {
          auto modelPath = PlatformPaths::getModelFile("pc_nsf_hifigan.onnx");
          if (modelPath.existsAsFile()) {
            if (!vocoder->loadModel(modelPath)) {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::AlertWindow::WarningIcon, "Inference failed",
                  "Failed to load vocoder model at:\n" +
                      modelPath.getFullPathName() +
                      "\n\nPlease check your model installation and try again.");
              return;
            }
          }
        }

        // If the new track is the first track and is an accompaniment track,
        // set it as active (for waveform display) but don't open piano roll editor
        if (ec && ec->getActiveTrackIndex() < 0 && ec->getTrackCount() > 0) {
          // Auto-set first track as active
          ec->setActiveTrack(ec->getTrackCount() - 1);
        }

        safeThis->repaint();
        safeThis->isLoadingAudio = false;
        safeThis->resized();

        if (safeThis->isPluginMode())
          safeThis->notifyProjectDataChanged();
      },
      [safeThis]() {
        if (safeThis == nullptr)
          return;
        safeThis->isLoadingAudio = false;
      });
}

void MainComponent::analyzeAudio() {
  auto *project = getProject();
  if (!project || !editorController)
    return;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->analyzeAudioAsync(
      [safeThis](Project &projectRef) {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.setProject(&projectRef);
        safeThis->pianoRollView.setProject(&projectRef);
        safeThis->pianoRoll.repaint();
      },
      [safeThis]() {
        if (safeThis == nullptr)
          return;
        safeThis->notifyProjectDataChanged();
      });
}

void MainComponent::analyzeAudio(
    Project &targetProject,
    const std::function<void(double, const juce::String &)> &onProgress,
    std::function<void()> onComplete) {
  if (editorController) {
    editorController->analyzeAudio(targetProject, onProgress, onComplete);
  }
}

// ── Multi-track management ──

void MainComponent::refreshTrackList() {
  trackList.refresh();
  resized();
}

void MainComponent::rebuildAudioEngine() {
  if (editorController)
    editorController->refreshAudioEngine(true);
  refreshTrackList();
}

void MainComponent::setActiveTrack(int trackIndex) {
  if (!editorController) return;

  editorController->setActiveTrack(trackIndex);
  auto* track = editorController->getTrack(trackIndex);
  if (!track) return;

  auto* project = track->getProject();
  if (!project) return;

  // Update piano roll and panels with the active track's project
  pianoRoll.setProject(project);
  pianoRollView.setProject(project);
  parameterPanel.setProject(project);

  // Update toolbar
  toolbar.setTotalTime(project->getAudioData().getDuration());
  toolbar.setLoopEnabled(project->getLoopRange().enabled);

  // Center pitch range if vocal track
  if (track->isVocal()) {
    pianoRoll.setEnabled(true);
    const auto& f0 = project->getAudioData().f0;
    if (!f0.empty()) {
      float minF0 = 10000.0f, maxF0 = 0.0f;
      for (float freq : f0) {
        if (freq > 50.0f) {
          minF0 = std::min(minF0, freq);
          maxF0 = std::max(maxF0, freq);
        }
      }
      if (maxF0 > minF0) {
        float minMidi = freqToMidi(minF0) - 2.0f;
        float maxMidi = freqToMidi(maxF0) + 2.0f;
        pianoRoll.centerOnPitchRange(minMidi, maxMidi);
      }
    }
  } else {
    // Accompaniment track: show waveform but disable editing
    pianoRoll.setEnabled(false);
  }

  // Update audio engine volume to active track's volume
  if (auto* engine = editorController->getAudioEngine())
    engine->setVolumeDb(project->getVolume());

  refreshTrackList();
  pianoRoll.invalidateWaveformCache();
  pianoRoll.repaint();
}

void MainComponent::onTrackSelected(int trackIndex) {
  if (!editorController) return;
  if (trackIndex < 0 || trackIndex >= editorController->getTrackCount()) return;
  setActiveTrack(trackIndex);
}

void MainComponent::onTrackTypeChanged(int trackIndex, TrackType newType) {
  if (!editorController) return;

  auto* track = editorController->getTrack(trackIndex);
  if (!track) return;

  if (newType == TrackType::Vocal && track->isAccompaniment()) {
    // Accompaniment -> Vocal: run analysis (no confirmation dialog needed,
    // user explicitly chose this from the dropdown)
    juce::Component::SafePointer<MainComponent> safeThis(this);
    toolbar.showProgress(TR("progress.analyzing_audio"));
    toolbar.setProgress(-1.0f);

    editorController->convertTrackToVocal(
        trackIndex,
        [safeThis](double p, const juce::String& msg) {
          if (safeThis == nullptr) return;
          safeThis->toolbar.setProgress(static_cast<float>(p));
          safeThis->toolbar.showProgress(msg);
        },
        [safeThis, trackIndex](int completedTrackIndex, bool success) {
          if (safeThis == nullptr) return;
          safeThis->toolbar.hideProgress();

          if (success) {
            safeThis->setActiveTrack(trackIndex);
            safeThis->refreshTrackList();
          } else {
            safeThis->refreshTrackList();
            StyledMessageBox::show(safeThis.getComponent(),
                TR("error.inference_failed"),
                "Failed to convert track to vocal. Check model availability.",
                StyledMessageBox::WarningIcon);
          }
        });
  } else if (newType == TrackType::Accompaniment && track->isVocal()) {
    // Vocal -> Accompaniment: just change type, keep analysis data
    track->type = TrackType::Accompaniment;
    refreshTrackList();

    if (editorController->getActiveTrackIndex() == trackIndex)
      pianoRoll.setEnabled(false);
  }
}

void MainComponent::onTrackDeleted(int trackIndex) {
  if (!editorController) return;
  if (trackIndex < 0 || trackIndex >= editorController->getTrackCount()) return;

  editorController->removeTrack(trackIndex);

  // Update active track
  int newActive = editorController->getActiveTrackIndex();
  if (newActive >= 0) {
    setActiveTrack(newActive);
  } else {
    pianoRoll.setProject(nullptr);
    pianoRollView.setProject(nullptr);
    parameterPanel.setProject(nullptr);
  }

  refreshTrackList();
  rebuildAudioEngine();
}

void MainComponent::exportFile() {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]() {
      if (safeThis != nullptr)
        safeThis->exportFile();
    });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  const int inputSampleRate = project->getAudioData().sampleRate;
  juce::Component::SafePointer<MainComponent> safeThis(this);
  showExportSettingsDialogAsync(
      this, inputSampleRate,
      [safeThis](std::optional<ExportSettings> exportSettings) {
        if (safeThis == nullptr || !exportSettings.has_value())
          return;

        auto performExport = [safeThis, exportSettings](const juce::File &targetFile) {
          if (safeThis == nullptr)
            return;

          auto *activeProject = safeThis->getProject();
          if (!activeProject)
      return;

          auto file = targetFile;
          const auto extension = getFormatExtension(exportSettings->format);
          if (file.getFileExtension().isEmpty())
            file = file.withFileExtension(extension);

          // Use the mixed waveform from AudioEngine (all tracks combined)
          juce::AudioBuffer<float> sourceBuffer;
          int sourceRate = 44100;
          if (auto* engine = safeThis->editorController ? safeThis->editorController->getAudioEngine() : nullptr) {
            sourceBuffer.makeCopyOf(engine->getCurrentWaveform());
            sourceRate = engine->getWaveformSampleRate();
          } else {
            sourceBuffer.makeCopyOf(activeProject->getAudioData().waveform);
            sourceRate = activeProject->getAudioData().sampleRate;
          }

          safeThis->toolbar.showProgress(TR("progress.exporting_audio"));
          safeThis->toolbar.setProgress(-1.0f);
          safeThis->toolbar.setEnabled(false);

          std::thread([safeThis, settings = *exportSettings, file = std::move(file),
                       sourceBuffer = std::move(sourceBuffer), sourceRate]() mutable {
            juce::String error;
            bool success = false;

            do {
              juce::AudioBuffer<float> exportBuffer =
                  convertChannels(sourceBuffer, settings.channels);
              exportBuffer = resampleAudio(exportBuffer, sourceRate, settings.sampleRate);

              if (file.existsAsFile() && !file.deleteFile()) {
                error = TR("dialog.failed_delete") + "\n" + file.getFullPathName();
                break;
              }

              auto fileStream = std::make_unique<juce::FileOutputStream>(file);
              if (!fileStream || !fileStream->openedOk()) {
                error = TR("dialog.failed_open") + "\n" + file.getFullPathName();
                break;
              }
              std::unique_ptr<juce::OutputStream> outputStream = std::move(fileStream);

              juce::AudioFormatManager formatManager;
              formatManager.registerBasicFormats();
              auto *format = findFormatForExtension(
                  formatManager, juce::String(".") + getFormatExtension(settings.format));
              if (!format) {
                error = "No exporter is available for format: " +
                        getFormatDisplayName(settings.format);
                break;
              }

              auto writerOptions = juce::AudioFormatWriterOptions{}
                                       .withSampleRate(settings.sampleRate)
                                       .withNumChannels(exportBuffer.getNumChannels())
                                       .withBitsPerSample(settings.bitsPerSample);
              if (format->isCompressed()) {
                writerOptions = writerOptions.withQualityOptionIndex(
                    chooseQualityIndex(format->getQualityOptions(),
                                       settings.bitrateKbps));
              }

              auto writer = format->createWriterFor(outputStream, writerOptions);
              if (!writer) {
                error = TR("dialog.failed_create_writer") + "\n" +
                        file.getFullPathName();
                break;
              }

              success = writer->writeFromAudioSampleBuffer(
                  exportBuffer, 0, exportBuffer.getNumSamples());
              writer->flush();
              writer.reset();
              if (!success) {
                error = TR("dialog.failed_write") + "\n" + file.getFullPathName();
                break;
              }
            } while (false);

            juce::MessageManager::callAsync([safeThis, success, error, file]() {
              if (safeThis == nullptr)
                return;
              safeThis->toolbar.setEnabled(true);
              safeThis->toolbar.hideProgress();
              if (success) {
                StyledMessageBox::show(
                    safeThis.getComponent(), TR("dialog.export_complete"),
                    TR("dialog.audio_exported") + "\n" + file.getFullPathName(),
                    StyledMessageBox::InfoIcon);
              } else {
                StyledMessageBox::show(
                    safeThis.getComponent(), TR("dialog.export_failed"), error,
                    StyledMessageBox::WarningIcon);
              }
            });
          }).detach();
        };

        if (safeThis->fileChooser != nullptr)
          return;

        safeThis->fileChooser = std::make_unique<juce::FileChooser>(
            TR("dialog.save_audio"), juce::File{},
            getFormatWildcard(exportSettings->format));

        auto chooserFlags = juce::FileBrowserComponent::saveMode |
                            juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting;

        safeThis->fileChooser->launchAsync(
            chooserFlags, [safeThis, performExport](const juce::FileChooser &fc) {
              if (safeThis == nullptr)
                return;
              const auto file = fc.getResult();
              safeThis->fileChooser.reset();
              if (file == juce::File{})
                return;
              performExport(file);
            });
      });
}

void MainComponent::exportMidiFile() {
  if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
    juce::Component::SafePointer<MainComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis]() {
      if (safeThis != nullptr)
        safeThis->exportMidiFile();
    });
    return;
  }

  auto *project = getProject();
  if (!project)
    return;

  // Prevent re-triggering while dialog is open
  if (fileChooser != nullptr)
    return;

  const auto &notes = project->getNotes();
  if (notes.empty()) {
    StyledMessageBox::show(this, TR("dialog.export_failed"),
                           TR("dialog.no_notes_to_export"),
                           StyledMessageBox::WarningIcon);
    return;
  }

  // Suggest filename based on project or audio file
  juce::File defaultFile;
  if (project->getFilePath().existsAsFile()) {
    defaultFile = project->getFilePath().withFileExtension("mid");
  } else if (project->getProjectFilePath().existsAsFile()) {
    defaultFile = project->getProjectFilePath().withFileExtension("mid");
  }

#if JUCE_WINDOWS && JUCE_MODAL_LOOPS_PERMITTED
  juce::FileChooser chooser(TR("dialog.export_midi"), defaultFile,
                            "*.mid;*.midi", true, false, this);
  if (!chooser.browseForFileToSave(true))
    return;

  auto file = chooser.getResult();
  if (file == juce::File{})
    return;

  // Ensure .mid extension
  if (file.getFileExtension().isEmpty())
    file = file.withFileExtension("mid");

  // Show progress
  toolbar.showProgress(TR("progress.exporting_midi"));
  toolbar.setProgress(0.3f);

  // Configure export options
  MidiExporter::ExportOptions options;
  options.tempo = 120.0f; // Default BPM
  options.ticksPerQuarterNote = 480;
  options.velocity = 100;
  options.quantizePitch = true; // Round to nearest semitone

  toolbar.setProgress(0.6f);

  // Export using adjusted pitch data (includes user edits)
  bool success = MidiExporter::exportToFile(project->getNotes(), file, options);

  toolbar.setProgress(1.0f);
  toolbar.hideProgress();

  if (success) {
    StyledMessageBox::show(
        this, TR("dialog.export_complete"),
        TR("dialog.midi_exported") + "\n" + file.getFullPathName(),
        StyledMessageBox::InfoIcon);
  } else {
    StyledMessageBox::show(
        this, TR("dialog.export_failed"),
        TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
        StyledMessageBox::WarningIcon);
  }
#else
  fileChooser = std::make_unique<juce::FileChooser>(
      TR("dialog.export_midi"), defaultFile, "*.mid;*.midi");

  auto chooserFlags = juce::FileBrowserComponent::saveMode |
                      juce::FileBrowserComponent::canSelectFiles |
                      juce::FileBrowserComponent::warnAboutOverwriting;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  fileChooser->launchAsync(
      chooserFlags, [safeThis](const juce::FileChooser &fc) {
        if (safeThis == nullptr)
          return;

        auto file = fc.getResult();
        safeThis->fileChooser
            .reset(); // Allow next dialog to open (must be after getResult)

        if (file == juce::File{})
          return;

        // Ensure .mid extension
        if (file.getFileExtension().isEmpty())
          file = file.withFileExtension("mid");

        // Show progress
        safeThis->toolbar.showProgress(TR("progress.exporting_midi"));
        safeThis->toolbar.setProgress(0.3f);

        // Configure export options
        MidiExporter::ExportOptions options;
        options.tempo = 120.0f; // Default BPM
        options.ticksPerQuarterNote = 480;
        options.velocity = 100;
        options.quantizePitch = true; // Round to nearest semitone

        safeThis->toolbar.setProgress(0.6f);

        // Export using adjusted pitch data (includes user edits)
        auto *project = safeThis ? safeThis->getProject() : nullptr;
        if (!project)
          return;
        bool success =
            MidiExporter::exportToFile(project->getNotes(), file, options);

        safeThis->toolbar.setProgress(1.0f);
        safeThis->toolbar.hideProgress();

        if (success) {
          StyledMessageBox::show(
              safeThis.getComponent(), TR("dialog.export_complete"),
              TR("dialog.midi_exported") + "\n" + file.getFullPathName(),
              StyledMessageBox::InfoIcon);
        } else {
          StyledMessageBox::show(
              safeThis.getComponent(), TR("dialog.export_failed"),
              TR("dialog.failed_write_midi") + "\n" + file.getFullPathName(),
              StyledMessageBox::WarningIcon);
        }
      });
#endif
}

void MainComponent::play() {
  auto *project = getProject();
  if (!project)
    return;

  // In plugin mode, playback is controlled by the host
  // We only update UI state, but don't actually start playback
  if (isPluginMode()) {
    if (onRequestHostPlayState)
      onRequestHostPlayState(true);
    // UI will be kept in sync by host callbacks; still update immediately for
    // responsiveness
    isPlaying = true;
    toolbar.setPlaying(true);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;

  const auto &loopRange = project->getLoopRange();
  if (loopRange.isValid()) {
    double position = audioEngine->getPosition();
    if (position < loopRange.startSeconds || position >= loopRange.endSeconds) {
      audioEngine->seek(loopRange.startSeconds);
      pianoRoll.setCursorTime(loopRange.startSeconds);
      toolbar.setCurrentTime(loopRange.startSeconds);
    }
  }

  LOG("MainComponent::play -- calling audioEngine->play()");
  if (audioEngine->play()) {
    isPlaying = true;
    toolbar.setPlaying(true);
    LOG("MainComponent::play -- OK, isPlaying=true");
  } else {
    LOG("MainComponent::play -- FAILED, engine returned false");
  }
}

void MainComponent::pause() {
  // In plugin mode, playback is controlled by the host
  if (isPluginMode()) {
    if (onRequestHostPlayState)
      onRequestHostPlayState(false);
    isPlaying = false;
    toolbar.setPlaying(false);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  LOG("MainComponent::pause -- stopping playback");
  isPlaying = false;
  toolbar.setPlaying(false);
  audioEngine->pause();
}

void MainComponent::stop() {
  // In plugin mode, playback is controlled by the host
  if (isPluginMode()) {
    if (onRequestHostStop)
      onRequestHostStop();
    isPlaying = false;
    toolbar.setPlaying(false);
    return;
  }

  // Standalone mode: use AudioEngine for playback
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  isPlaying = false;
  toolbar.setPlaying(false);
  audioEngine->stop();
  // Keep cursor at current position - user can press Home to go to start
}

void MainComponent::seek(double time) {
  // In plugin mode, we can only update the UI cursor position
  // The host controls the actual playback position (no seek API available)
  if (isPluginMode()) {
    // Update UI cursor position only - don't try to control host
    pianoRoll.setCursorTime(time);
    toolbar.setCurrentTime(time);
    // Note: We don't call onRequestHostSeek because hosts don't support seek
    // The cursor position shown is for visual reference only
    return;
  }

  // Standalone mode: use AudioEngine for seeking
  auto *audioEngine = editorController ? editorController->getAudioEngine() : nullptr;
  if (!audioEngine)
    return;
  audioEngine->seek(time);
  pianoRoll.setCursorTime(time);
  toolbar.setCurrentTime(time);

  // Scroll view to make cursor visible
  float cursorX = static_cast<float>(time * pianoRoll.getPixelsPerSecond());
  float viewWidth = static_cast<float>(pianoRoll.getWidth() -
                                       74); // minus piano keys and scrollbar
  float scrollX = static_cast<float>(pianoRoll.getScrollX());

  // If cursor is outside visible area, scroll to show it
  if (cursorX < scrollX || cursorX > scrollX + viewWidth) {
    // Center cursor in view, or scroll to start if time is 0
    double newScrollX;
    if (time < 0.001) {
      newScrollX = 0.0; // Go to start
    } else {
      newScrollX =
          std::max(0.0, static_cast<double>(cursorX - viewWidth * 0.3));
    }
    pianoRoll.setScrollX(newScrollX);
  }
}

void MainComponent::resynthesizeIncremental() {
  LOG("[STRETCH-DBG] resynthesizeIncremental() ENTER");

  auto *project = getProject();
  if (!project || !editorController) {
    LOG("[STRETCH-DBG] resynthesizeIncremental: BAIL — no project or controller");
    return;
  }
  LOG("[STRETCH-DBG] resynthesizeIncremental: hasDirtyNotes=" + juce::String((int)project->hasDirtyNotes())
      + " hasF0DirtyRange=" + juce::String((int)project->hasF0DirtyRange())
      + " svcActive=" + juce::String((int)editorController->isSVCModelActive()));

  toolbar.showProgress(TR("progress.synthesizing"));
  toolbar.setProgress(-1.0f);
  toolbar.setEnabled(false);

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->resynthesizeIncrementalAsync(
      *project,
      [safeThis](const juce::String &message) {
        if (safeThis == nullptr)
          return;
        safeThis->toolbar.showProgress(message);
      },
      [safeThis](bool success) {
        if (safeThis == nullptr)
          return;

        safeThis->toolbar.setEnabled(true);
        safeThis->toolbar.hideProgress();

        if (!success) {
          DBG("resynthesizeIncremental: Synthesis failed or was cancelled");
          return;
        }

        // Rebuild mixed waveform for multi-track playback
        if (safeThis->editorController)
          safeThis->editorController->refreshAudioEngine(true);

        safeThis->pianoRoll.invalidateWaveformCache();
        safeThis->pianoRoll.repaint();
        if (safeThis->isPluginMode())
          safeThis->notifyProjectDataChanged();
      },
      pendingIncrementalResynth,
      isPluginMode());
}

void MainComponent::onNoteSelected(Note *note) {
  parameterPanel.setSelectedNote(note);
}

void MainComponent::onPitchEdited() {
  pianoRoll.repaint();
  parameterPanel.updateFromNote();
}

void MainComponent::onZoomChanged(float pixelsPerSecond) {
  if (isSyncingZoom)
    return;

  isSyncingZoom = true;

  // Update all components with zoom centered on cursor
  pianoRoll.setPixelsPerSecond(pixelsPerSecond, true);
  toolbar.setZoom(pixelsPerSecond);
  pianoRollView.refreshOverview();

  isSyncingZoom = false;
}

void MainComponent::notifyProjectDataChanged() {
  if (onProjectDataChanged)
    onProjectDataChanged();
}

void MainComponent::undo() {
  // Cancel any in-progress drawing first
  pianoRoll.cancelDrawing();

  if (undoManager && undoManager->canUndo()) {
    undoManager->undo();
    parameterPanel.updateFromNote();
    pianoRoll.invalidateBasePitchCache(); // Refresh cache after note split etc.
    pianoRoll.repaint();
    pianoRollView.repaint();

    if (getProject()) {
      // Don't mark all notes as dirty - let undo action callbacks handle
      // the specific dirty range. This avoids synthesizing the entire project.
      // The undo action's callback will set the correct F0 dirty range.
      resynthesizeIncremental();
    }
    
    // Update command states (undo/redo availability changed)
    if (commandManager)
      commandManager->commandStatusChanged();
  }
}

void MainComponent::redo() {
  if (undoManager && undoManager->canRedo()) {
    undoManager->redo();
    parameterPanel.updateFromNote();
    pianoRoll.invalidateBasePitchCache(); // Refresh cache after note split etc.
    pianoRoll.repaint();
    pianoRollView.repaint();

    if (getProject()) {
      // Don't mark all notes as dirty - let redo action callbacks handle
      // the specific dirty range. This avoids synthesizing the entire project.
      // The redo action's callback will set the correct F0 dirty range.
      resynthesizeIncremental();
    }
    
    // Update command states (undo/redo availability changed)
    if (commandManager)
      commandManager->commandStatusChanged();
  }
}

void MainComponent::setEditMode(EditMode mode) {
  pianoRoll.setEditMode(mode);
  toolbar.setEditMode(mode);
  
  // Update command states (draw mode toggle state changed)
  if (commandManager)
    commandManager->commandStatusChanged();
}

void MainComponent::segmentIntoNotes() {
  auto *project = getProject();
  if (!project || !editorController)
    return;

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->segmentIntoNotesAsync(
      [safeThis](Project &projectRef) {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.setProject(&projectRef);
        safeThis->pianoRollView.setProject(&projectRef);
      },
      [safeThis]() {
        if (safeThis == nullptr)
          return;
        safeThis->pianoRoll.invalidateBasePitchCache();
        safeThis->pianoRoll.repaint();
      });
}

void MainComponent::segmentIntoNotes(Project &targetProject) {
  if (editorController) {
    editorController->segmentIntoNotes(targetProject, [this]() {
      pianoRoll.invalidateBasePitchCache();
      pianoRoll.repaint();
    });
  }
}

void MainComponent::showSettings() {
  if (!settingsOverlay) {
    // Pass AudioDeviceManager only in standalone mode
    juce::AudioDeviceManager *deviceMgr = nullptr;
    if (!isPluginMode() && editorController &&
        editorController->getAudioEngine())
      deviceMgr = &editorController->getAudioEngine()->getDeviceManager();

    settingsOverlay =
        std::make_unique<SettingsOverlay>(settingsManager.get(), deviceMgr);
    addAndMakeVisible(settingsOverlay.get());
    settingsOverlay->setVisible(false);
    settingsOverlay->onClose = [this]() {
      if (settingsOverlay)
        settingsOverlay->closeAnimated();
    };

    settingsOverlay->getSettingsComponent()->onSettingsChanged = [this]() {
      settingsManager->applySettings();
      reloadInferenceModels(true);
    };
    settingsOverlay->getSettingsComponent()->onUIFontScaleChanged =
        [this](float scale) {
          if (settingsManager) {
            settingsManager->setUIFontScale(scale);
            settingsManager->saveConfig();
          }
          AppFont::setScale(scale);
          sendLookAndFeelChange();
          resized();
          repaint();
        };
    settingsOverlay->getSettingsComponent()->canChangeDevice = [this]() {
      return !isInferenceBusy();
    };
    settingsOverlay->getSettingsComponent()->onPitchDetectorChanged =
        [this](PitchDetectorType type) {
          if (editorController) {
            editorController->setPitchDetectorType(type);
            reloadInferenceModels(false);
          }
        };
    settingsOverlay->getSettingsComponent()->onShowSomeSegmentsDebugChanged =
        [this](bool show) {
          if (settingsManager) {
            settingsManager->setShowSomeSegmentsDebug(show);
            settingsManager->saveConfig();
          }
          pianoRoll.setShowSomeSegmentsDebug(show);
          pianoRollView.setShowSomeSegmentsDebug(show);
          pianoRollView.refreshOverview();
          pianoRoll.repaint();
        };
    settingsOverlay->getSettingsComponent()->onShowSomeValuesDebugChanged =
        [this](bool show) {
          if (settingsManager) {
            settingsManager->setShowSomeValuesDebug(show);
            settingsManager->saveConfig();
          }
          pianoRoll.setShowSomeValuesDebug(show);
          pianoRoll.repaint();
        };
    settingsOverlay->getSettingsComponent()->onShowUvInterpolationDebugChanged =
        [this](bool show) {
          if (settingsManager) {
            settingsManager->setShowUvInterpolationDebug(show);
            settingsManager->saveConfig();
          }
          pianoRoll.setShowUvInterpolationDebug(show);
          pianoRoll.repaint();
        };
    settingsOverlay->getSettingsComponent()->onShowActualF0DebugChanged =
        [this](bool show) {
          if (settingsManager) {
            settingsManager->setShowActualF0Debug(show);
            settingsManager->saveConfig();
          }
          pianoRoll.setShowActualF0Debug(show);
          pianoRoll.repaint();
        };
    settingsOverlay->getSettingsComponent()->onShowFpsOverlayChanged =
        [this](bool show) {
          if (settingsManager) {
            settingsManager->setShowFpsOverlay(show);
            settingsManager->saveConfig();
          }
          pianoRoll.setShowFpsOverlay(show);
        };
    settingsOverlay->getSettingsComponent()->onShowBackgroundWaveformChanged =
        [this](bool show) {
          if (settingsManager) {
            settingsManager->setShowBackgroundWaveform(show);
            settingsManager->saveConfig();
          }
          pianoRoll.setShowBackgroundWaveform(show);
        };
  }

  settingsOverlay->setBounds(getLocalBounds());
  settingsOverlay->openAnimated();
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray &files) {
  for (const auto &file : files) {
    if (file.endsWithIgnoreCase(".htpx") || file.endsWithIgnoreCase(".wav") ||
        file.endsWithIgnoreCase(".mp3") ||
        file.endsWithIgnoreCase(".flac") || file.endsWithIgnoreCase(".aiff") ||
        file.endsWithIgnoreCase(".ogg") || file.endsWithIgnoreCase(".m4a"))
      return true;
  }
  return false;
}

void MainComponent::filesDropped(const juce::StringArray &files, int /*x*/,
                                 int /*y*/) {
  if (files.isEmpty())
    return;

  juce::File audioFile(files[0]);
  if (!audioFile.existsAsFile())
    return;

  if (audioFile.hasFileExtension("htpx") || audioFile.hasFileExtension(".htpx"))
    openProjectFile(audioFile);
  else
    loadAudioFile(audioFile);
}

void MainComponent::setHostAudio(const juce::AudioBuffer<float> &buffer,
                                 double sampleRate) {
  if (!isPluginMode())
    return;

  DBG("MainComponent::setHostAudio called - starting async analysis");

  if (!editorController)
    return;

  // Show analyzing progress
  toolbar.showProgress(TR("progress.analyzing"));

  juce::Component::SafePointer<MainComponent> safeThis(this);
  editorController->setHostAudioAsync(
      buffer, sampleRate,
      [safeThis](double /*p*/, const juce::String &msg) {
        if (safeThis == nullptr)
          return;
        juce::MessageManager::callAsync([safeThis, msg]() {
          if (safeThis != nullptr)
            safeThis->toolbar.showProgress(msg);
        });
      },
      [safeThis](const juce::AudioBuffer<float> &original) {
        if (safeThis == nullptr)
          return;

        if (safeThis->undoManager)
          safeThis->undoManager->clear();

        auto *project = safeThis->getProject();
        if (!project)
          return;

        safeThis->pianoRoll.setProject(project);
        safeThis->pianoRollView.setProject(project);
        safeThis->parameterPanel.setProject(project);
        safeThis->toolbar.setTotalTime(project->getAudioData().getDuration());

        const auto &f0 = project->getAudioData().f0;
        if (!f0.empty()) {
          float minF0 = 10000.0f, maxF0 = 0.0f;
          for (float freq : f0) {
            if (freq > 50.0f) {
              minF0 = std::min(minF0, freq);
              maxF0 = std::max(maxF0, freq);
            }
          }
          if (maxF0 > minF0) {
            float minMidi = freqToMidi(minF0) - 2.0f;
            float maxMidi = freqToMidi(maxF0) + 2.0f;
            safeThis->pianoRoll.centerOnPitchRange(minMidi, maxMidi);
          }
        }

        auto *vocoder = safeThis->editorController
                            ? safeThis->editorController->getVocoder()
                            : nullptr;
        if (vocoder && !vocoder->isLoaded()) {
          auto modelPath = PlatformPaths::getModelFile("pc_nsf_hifigan.onnx");
          if (modelPath.existsAsFile()) {
            if (!vocoder->loadModel(modelPath)) {
              juce::AlertWindow::showMessageBoxAsync(
                  juce::AlertWindow::WarningIcon, "Inference failed",
                  "Failed to load vocoder model at:\n" +
                      modelPath.getFullPathName() +
                      "\n\nPlease check your model installation and try again.");
              safeThis->toolbar.hideProgress();
              return;
            }
          } else {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Missing model file",
                "pc_nsf_hifigan.onnx was not found at:\n" +
                    modelPath.getFullPathName() +
                    "\n\nPlease install the required model files and try again.");
            safeThis->toolbar.hideProgress();
            return;
          }
        }

        safeThis->repaint();

        safeThis->notifyProjectDataChanged();

        safeThis->toolbar.hideProgress();
      });
}

void MainComponent::updatePlaybackPosition(double timeSeconds) {
  if (!isPluginMode())
    return;

  // Only follow host position if we have a valid project with audio
  auto *project = getProject();
  if (!project || project->getAudioData().waveform.getNumSamples() == 0)
    return;

  // Clamp position to audio duration (don't go beyond the end)
  double duration = project->getAudioData().getDuration();
  double clampedTime = std::min(timeSeconds, static_cast<double>(duration));

  // Update cursor position using the same mechanism as AudioEngine
  pendingCursorTime.store(clampedTime);
  hasPendingCursorUpdate.store(true);

  // In plugin mode, the host controls playback state
  // Set isPlaying to true when we receive position updates
  // This enables "follow playback" feature
  isPlaying = true;
}

bool MainComponent::hasAnalyzedProject() const {
  if (auto *project = getProject()) {
    auto &audioData = project->getAudioData();
    return audioData.waveform.getNumSamples() > 0 && !audioData.f0.empty();
  }
  return false;
}

void MainComponent::bindRealtimeProcessor(RealtimePitchProcessor &processor) {
  processor.setProject(getProject());
  processor.setVocoder(editorController ? editorController->getVocoder()
                                        : nullptr);
}

juce::String MainComponent::serializeProjectJson() const {
  if (auto *project = getProject()) {
    auto json = ProjectSerializer::toJson(*project);
    return juce::JSON::toString(json, false);
  }
  return {};
}

bool MainComponent::restoreProjectJson(const juce::String &jsonString) {
  if (jsonString.isEmpty())
    return false;
  if (auto *project = getProject()) {
    auto json = juce::JSON::parse(jsonString);
    if (json.isObject()) {
      ProjectSerializer::fromJson(*project, json);
      return true;
    }
  }
  return false;
}

void MainComponent::notifyHostStopped() {
  if (!isPluginMode())
    return;

  // In plugin mode, the host controls playback state
  isPlaying = false;
}

bool MainComponent::isARAModeActive() const {
  // Check if we're in plugin mode and have project data from ARA
  // ARA mode is indicated by having project data but no manual capture
  // In ARA mode, audio comes from ARA document controller, not from
  // processBlock
  if (!isPluginMode())
    return false;

  // If we have project data and it wasn't captured manually, it's likely from
  // ARA This is a heuristic - in a real implementation, we'd track this
  // explicitly
  if (auto *project = getProject();
      project && project->getAudioData().waveform.getNumSamples() > 0) {
    // Check if we're not currently capturing (which would indicate non-ARA
    // mode) Note: This requires access to PluginProcessor, which we don't have
    // here A better approach would be to set a flag when ARA audio is received
    return true; // Assume ARA if we have project data in plugin mode
  }

  return false;
}

void MainComponent::renderProcessedAudio() {
  auto *project = getProject();
  if (!isPluginMode() || !project ||
      project->getAudioData().originalWaveform.getNumSamples() == 0)
    return;

  // Show progress
  toolbar.showProgress(TR("progress.rendering"));
  auto *vocoder = editorController ? editorController->getVocoder() : nullptr;
  if (!vocoder) {
    toolbar.hideProgress();
    return;
  }

  const float globalOffset = project->getGlobalPitchOffset();
  juce::Component::SafePointer<MainComponent> safeThis(this);

  if (editorController) {
    editorController->renderProcessedAudioAsync(
        *project, globalOffset,
        [safeThis](bool ok) {
          if (safeThis == nullptr)
            return;
          safeThis->toolbar.hideProgress();
          if (ok)
            safeThis->notifyProjectDataChanged();
        });
  }
}

// ApplicationCommandTarget interface implementations
juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget() {
    return nullptr;
}

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& commands) {
    // Register all application commands that MainComponent handles
    const juce::CommandID commandArray[] = {
        // File commands
        CommandIDs::openFile,
        CommandIDs::saveProject,
        CommandIDs::exportAudio,
        CommandIDs::exportMidi,
        CommandIDs::clearProject,
        CommandIDs::quit,
        
        // Edit commands
        CommandIDs::undo,
        CommandIDs::redo,
        CommandIDs::selectAll,
        
        // View commands
        CommandIDs::showSettings,
        CommandIDs::showDeltaPitch,
        CommandIDs::showBasePitch,
        CommandIDs::toggleModelDebugPanel,
        CommandIDs::openLogFile,
        
        // Transport commands
        CommandIDs::playPause,
        CommandIDs::stop,
        CommandIDs::goToStart,
        CommandIDs::goToEnd,
        
        // Edit mode commands
        CommandIDs::toggleDrawMode,
        CommandIDs::exitDrawMode,

        // Toolbar edit mode commands
        CommandIDs::selectMode,
        CommandIDs::stretchMode,
        CommandIDs::drawMode,
        CommandIDs::splitMode,
        CommandIDs::hnsepMode,
        CommandIDs::followPlayback,
        CommandIDs::toggleLoop
    };
    
    commands.addArray(commandArray, sizeof(commandArray) / sizeof(commandArray[0]));
}

void MainComponent::getCommandInfo(juce::CommandID commandID,
                                   juce::ApplicationCommandInfo& result) {
    const auto primaryModifier =
#if JUCE_MAC
        juce::ModifierKeys::commandModifier;
#else
        juce::ModifierKeys::ctrlModifier;
#endif
    auto *project = getProject();
    switch (commandID) {
        // File commands
        case CommandIDs::openFile:
            result.setInfo(TR("command.open_audio"), TR("command.open_audio.desp"), "File", 0);
            result.addDefaultKeypress('o', primaryModifier);
            break;
            
        case CommandIDs::saveProject:
            result.setInfo(TR("command.save_project"), TR("command.save_project.desp"), "File", 0);
            result.addDefaultKeypress('s', primaryModifier);
            result.setActive(project != nullptr);
            break;
            
        case CommandIDs::exportAudio:
            result.setInfo(TR("command.export_audio"), TR("command.export_audio.desp"), "File", 0);
            result.addDefaultKeypress('e', primaryModifier);
            result.setActive(project != nullptr);
            break;
            
        case CommandIDs::exportMidi:
            result.setInfo(TR("command.export_midi"), TR("command.export_midi.desp"), "File", 0);
            result.setActive(project != nullptr);
            break;
            
        case CommandIDs::clearProject:
            result.setInfo(TR("command.clear_project"), TR("command.clear_project.desp"), "File", 0);
            result.setActive(project != nullptr && !isLoadingAudio.load());
            break;
            
        case CommandIDs::quit:
            result.setInfo(TR("command.quit"), TR("command.quit.desp"), "File", 0);
            result.addDefaultKeypress('q', primaryModifier);
            result.setActive(!isPluginMode());
            break;
            
        // Edit commands
        case CommandIDs::undo:
            result.setInfo(TR("command.undo"), TR("command.undo.desp"), "Edit", 0);
            result.addDefaultKeypress('z', primaryModifier);
            result.setActive(isPluginMode() || (undoManager != nullptr && undoManager->canUndo()));
            break;
            
        case CommandIDs::redo:
            result.setInfo(TR("command.redo"), TR("command.redo.desp"), "Edit", 0);
#if JUCE_MAC
            result.addDefaultKeypress('z', primaryModifier | juce::ModifierKeys::shiftModifier);
#else
            result.addDefaultKeypress('y', primaryModifier);
#endif
            result.setActive(isPluginMode() || (undoManager != nullptr && undoManager->canRedo()));
            break;
            
        case CommandIDs::selectAll:
            result.setInfo(TR("command.select_all"), TR("command.select_all.desp"), "Edit", 0);
            result.addDefaultKeypress('a', primaryModifier);
            result.setActive(project != nullptr);
            break;
            
        // View commands
        case CommandIDs::showSettings:
            result.setInfo(TR("command.settings"), TR("command.settings.desp"), "View", 0);
            result.addDefaultKeypress(',', primaryModifier);
            break;
            
        case CommandIDs::showDeltaPitch:
            result.setInfo(TR("command.show_delta_pitch"), TR("command.show_delta_pitch.desp"), "View", 0);
            result.addDefaultKeypress('d', primaryModifier | juce::ModifierKeys::shiftModifier);
            result.setTicked(settingsManager->getShowDeltaPitch());
            break;
            
        case CommandIDs::showBasePitch:
            result.setInfo(TR("command.show_base_pitch"), TR("command.show_base_pitch.desp"), "View", 0);
            result.addDefaultKeypress('b', primaryModifier | juce::ModifierKeys::shiftModifier);
            result.setTicked(settingsManager->getShowBasePitch());
            break;

        case CommandIDs::toggleModelDebugPanel:
            result.setInfo("Model Debug Panel", "Show loaded model and inference device status", "View", 0);
            result.setTicked(modelDebugPanelVisible);
            break;

        case CommandIDs::openLogFile:
            result.setInfo("Open Log File", "Open the current session log file", "View", 0);
            break;
            
        // Transport commands
        case CommandIDs::playPause:
            result.setInfo(TR("command.play_pause"), TR("command.play_pause.desp"), "Transport", 0);
            if (!isPluginMode())
                result.addDefaultKeypress(juce::KeyPress::spaceKey, juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;
            
        case CommandIDs::stop:
            result.setInfo(TR("command.stop"), TR("command.stop.desp"), "Transport", 0);
            result.addDefaultKeypress(juce::KeyPress::escapeKey, juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr && isPlaying);
            break;
            
        case CommandIDs::goToStart:
            result.setInfo(TR("command.go_to_start"), TR("command.go_to_start.desp"), "Transport", 0);
            result.addDefaultKeypress(juce::KeyPress::homeKey, juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;
            
        case CommandIDs::goToEnd:
            result.setInfo(TR("command.go_to_end"), TR("command.go_to_end.desp"), "Transport", 0);
            result.addDefaultKeypress(juce::KeyPress::endKey, juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;
            
        // Edit mode commands
        case CommandIDs::toggleDrawMode:
            result.setInfo(TR("command.toggle_draw"), TR("command.toggle_draw.desp"), "Edit Mode", 0);
            result.addDefaultKeypress('d', juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            result.setTicked(pianoRoll.getEditMode() == EditMode::Draw);
            break;
            
        case CommandIDs::exitDrawMode:
            result.setInfo(TR("command.exit_draw"), TR("command.exit_draw.desp"), "Edit Mode", 0);
            result.addDefaultKeypress(juce::KeyPress::escapeKey, juce::ModifierKeys::noModifiers);
            result.setActive(pianoRoll.getEditMode() == EditMode::Draw);
            break;

        // Toolbar edit mode commands (keys 1-6)
        case CommandIDs::selectMode:
            result.setInfo(TR("toolbar.select"), TR("toolbar.select"), "Edit Mode", 0);
            result.addDefaultKeypress('1', juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;

        case CommandIDs::stretchMode:
            result.setInfo(TR("toolbar.stretch"), TR("toolbar.stretch"), "Edit Mode", 0);
            result.addDefaultKeypress('2', juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;

        case CommandIDs::drawMode:
            result.setInfo(TR("toolbar.draw"), TR("toolbar.draw"), "Edit Mode", 0);
            result.addDefaultKeypress('3', juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;

        case CommandIDs::splitMode:
            result.setInfo(TR("toolbar.split"), TR("toolbar.split"), "Edit Mode", 0);
            result.addDefaultKeypress('4', juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;

        case CommandIDs::hnsepMode:
            result.setInfo("HNSep Parameters", "HNSep Parameters", "Edit Mode", 0);
            result.addDefaultKeypress('5', juce::ModifierKeys::noModifiers);
            result.setActive(project != nullptr);
            break;

        case CommandIDs::followPlayback:
            result.setInfo(TR("toolbar.follow"), TR("toolbar.follow"), "Edit Mode", 0);
            result.addDefaultKeypress('6', juce::ModifierKeys::noModifiers);
            break;

        case CommandIDs::toggleLoop:
            result.setInfo(TR("toolbar.loop"), TR("toolbar.loop"), "Edit Mode", 0);
            result.addDefaultKeypress('7', juce::ModifierKeys::noModifiers);
            break;
            
        default:
            break;
    }
}

bool MainComponent::perform(const ApplicationCommandTarget::InvocationInfo& info) {
    switch (info.commandID) {
        // File commands
        case CommandIDs::openFile:
            this->openFile();
            return true;
            
        case CommandIDs::saveProject:
            this->saveProject();
            return true;
            
        case CommandIDs::exportAudio:
            exportFile();
            return true;
            
        case CommandIDs::exportMidi:
            exportMidiFile();
            return true;
            
        case CommandIDs::clearProject:
            clearProject();
            return true;

        case CommandIDs::quit:
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
            return true;
            
        // Edit commands
        case CommandIDs::undo:
            if (isPluginMode()) {
                if (undoManager != nullptr && undoManager->canUndo())
                    this->undo();
                return true; // Consume in plugin mode to avoid host-level undo conflicts
            }
            this->undo();
            return true;
            
        case CommandIDs::redo:
            if (isPluginMode()) {
                if (undoManager != nullptr && undoManager->canRedo())
                    this->redo();
                return true; // Consume in plugin mode to avoid host-level redo conflicts
            }
            this->redo();
            return true;
            
        case CommandIDs::selectAll:
            if (auto *project = getProject()) {
                project->selectAllNotes();
                pianoRoll.repaint();
            }
            return true;
            
        // View commands
        case CommandIDs::showSettings:
            showSettings();
            return true;
            
        case CommandIDs::showDeltaPitch:
        {
            bool newState = !settingsManager->getShowDeltaPitch();
            pianoRoll.setShowDeltaPitch(newState);
            settingsManager->setShowDeltaPitch(newState);
            settingsManager->saveConfig();
            commandManager->commandStatusChanged();
            return true;
        }
        
        case CommandIDs::showBasePitch:
        {
            bool newState = !settingsManager->getShowBasePitch();
            pianoRoll.setShowBasePitch(newState);
            settingsManager->setShowBasePitch(newState);
            settingsManager->saveConfig();
            commandManager->commandStatusChanged();
            return true;
        }

        case CommandIDs::toggleModelDebugPanel:
            setModelDebugPanelVisible(!modelDebugPanelVisible);
            return true;

        case CommandIDs::openLogFile:
        {
            auto logFile = AppLogger::getLogFile();
            if (!logFile.existsAsFile())
                AppLogger::log("Opening log file");
            logFile.revealToUser();
            return true;
        }
        
        // Transport commands
        case CommandIDs::playPause:
            if (isPlaying)
                pause();
            else
                play();
            return true;
            
        case CommandIDs::stop:
            this->stop();
            return true;
            
        case CommandIDs::goToStart:
            seek(0.0);
            return true;
            
        case CommandIDs::goToEnd:
            if (auto *project = getProject()) {
                seek(project->getAudioData().getDuration());
            }
            return true;
            
        // Edit mode commands
        case CommandIDs::toggleDrawMode:
            if (pianoRoll.getEditMode() == EditMode::Draw)
                setEditMode(EditMode::Select);
            else
                setEditMode(EditMode::Draw);
            return true;
            
        case CommandIDs::exitDrawMode:
            if (pianoRoll.getEditMode() == EditMode::Draw) {
                setEditMode(EditMode::Select);
            }
            return true;

        // Toolbar edit mode commands (keys 1-5)
        case CommandIDs::selectMode:
            setEditMode(EditMode::Select);
            return true;

        case CommandIDs::stretchMode:
            setEditMode(EditMode::Stretch);
            return true;

        case CommandIDs::drawMode:
            setEditMode(EditMode::Draw);
            return true;

        case CommandIDs::splitMode:
            setEditMode(EditMode::Split);
            return true;

        case CommandIDs::hnsepMode:
            toolbar.buttonClicked(&toolbar.getHNSepButton());
            return true;

        case CommandIDs::followPlayback:
            toolbar.buttonClicked(&toolbar.getFollowButton());
            return true;

        case CommandIDs::toggleLoop:
            toolbar.buttonClicked(&toolbar.getLoopButton());
            return true;

        default:
            return false;
    }
}
