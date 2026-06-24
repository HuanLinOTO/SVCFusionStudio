#include "SettingsComponent.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Localization.h"
#include <cmath>

#ifdef HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

#ifdef _WIN32
#include <dxgi1_2.h>
#include <windows.h>
#endif

namespace {
constexpr int kFollowSystemOutputId = 1;

float fontScaleForSelectedId(int selectedId) {
  switch (selectedId) {
  case 2:
    return 1.10f;
  case 3:
    return 1.20f;
  default:
    return 1.0f;
  }
}

int selectedIdForFontScale(float scale) {
  if (scale >= 1.15f)
    return 3;
  if (scale >= 1.05f)
    return 2;
  return 1;
}

#ifdef _WIN32
juce::StringArray getDxgiAdapterNames() {
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
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
        names.add(juce::String(desc.Description));
      }
    }
    adapter->Release();
  }

  factory->Release();
  return names;
}
#endif
} // namespace

//==============================================================================
// SettingsComponent
//==============================================================================

SettingsComponent::SettingsComponent(
    SettingsManager *settingsMgr, juce::AudioDeviceManager *audioDeviceManager)
    : deviceManager(audioDeviceManager),
      pluginMode(audioDeviceManager == nullptr), settingsManager(settingsMgr) {
  // Allow transparent corners for rounded window styling
  setOpaque(false);

  auto configureRowLabel = [](juce::Label &label) {
    label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    label.setFont(AppFont::getFont(14.5f));
    label.setJustificationType(juce::Justification::centredLeft);
  };

  auto configureDescriptionLabel = [](juce::Label &label) {
    label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    label.setFont(AppFont::getFont(13.0f));
    label.setJustificationType(juce::Justification::centredLeft);
    label.setMinimumHorizontalScale(0.72f);
  };

  auto configureSectionLabel = [](juce::Label &label) {
    label.setFont(AppFont::getBoldFont(15.5f));
    label.setColour(juce::Label::textColourId, APP_COLOR_PRIMARY);
    label.setJustificationType(juce::Justification::centredLeft);
  };

  // Title
  titleLabel.setText(TR("settings.title"), juce::dontSendNotification);
  titleLabel.setFont(AppFont::getBoldFont(20.0f));
  titleLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
  addAndMakeVisible(titleLabel);

  auto configureTabButton = [this](juce::TextButton &button) {
    button.setClickingTogglesState(false);
    button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    button.setLookAndFeel(&settingsLookAndFeel);
  };

  // Tabs
  generalTabButton.setButtonText(TR("settings.general"));
  configureTabButton(generalTabButton);
  generalTabButton.onClick = [this]() { setActiveTab(SettingsTab::General); };
  addAndMakeVisible(generalTabButton);

  audioTabButton.setButtonText(TR("settings.audio"));
  configureTabButton(audioTabButton);
  audioTabButton.onClick = [this]() { setActiveTab(SettingsTab::Audio); };
  addAndMakeVisible(audioTabButton);

  // General tab section labels
  generalSectionLabel.setText(TR("settings.section.interface"),
                              juce::dontSendNotification);
  configureSectionLabel(generalSectionLabel);
  addAndMakeVisible(generalSectionLabel);

  inferenceSectionLabel.setText(TR("settings.section.inference"),
                                juce::dontSendNotification);
  configureSectionLabel(inferenceSectionLabel);
  addAndMakeVisible(inferenceSectionLabel);

  displaySectionLabel.setText(TR("settings.section.display"),
                              juce::dontSendNotification);
  configureSectionLabel(displaySectionLabel);
  addAndMakeVisible(displaySectionLabel);

  debugSectionLabel.setText(TR("settings.section.debug"),
                            juce::dontSendNotification);
  configureSectionLabel(debugSectionLabel);
  addAndMakeVisible(debugSectionLabel);

  // Language selection
  languageLabel.setText(TR("settings.language"), juce::dontSendNotification);
  configureRowLabel(languageLabel);
  addAndMakeVisible(languageLabel);
  languageDescriptionLabel.setText(TR("settings.language_desc"),
                                   juce::dontSendNotification);
  configureDescriptionLabel(languageDescriptionLabel);
  addAndMakeVisible(languageDescriptionLabel);

  // Add "Auto" option first
  languageComboBox.addItem(TR("lang.auto"), 1);
  // Add available languages dynamically
  const auto &langs = Localization::getInstance().getAvailableLanguages();
  for (int i = 0; i < static_cast<int>(langs.size()); ++i)
    languageComboBox.addItem(langs[i].nativeName, i + 2); // IDs start at 2
  languageComboBox.addListener(this);
  languageComboBox.setLookAndFeel(&settingsLookAndFeel);
  addAndMakeVisible(languageComboBox);

  uiFontSizeLabel.setText(TR("settings.ui_font_size"),
                          juce::dontSendNotification);
  configureRowLabel(uiFontSizeLabel);
  addAndMakeVisible(uiFontSizeLabel);
  uiFontSizeDescriptionLabel.setText(TR("settings.ui_font_size_desc"),
                                     juce::dontSendNotification);
  configureDescriptionLabel(uiFontSizeDescriptionLabel);
  addAndMakeVisible(uiFontSizeDescriptionLabel);

  uiFontSizeComboBox.addItem(TR("settings.ui_font_size.default"), 1);
  uiFontSizeComboBox.addItem(TR("settings.ui_font_size.large"), 2);
  uiFontSizeComboBox.addItem(TR("settings.ui_font_size.extra_large"), 3);
  uiFontSizeComboBox.setSelectedId(1, juce::dontSendNotification);
  uiFontSizeComboBox.addListener(this);
  uiFontSizeComboBox.setLookAndFeel(&settingsLookAndFeel);
  addAndMakeVisible(uiFontSizeComboBox);

  // Device selection
  deviceLabel.setText(TR("settings.device"), juce::dontSendNotification);
  configureRowLabel(deviceLabel);
  addAndMakeVisible(deviceLabel);
  deviceDescriptionLabel.setText(TR("settings.device_desc"),
                                 juce::dontSendNotification);
  configureDescriptionLabel(deviceDescriptionLabel);
  addAndMakeVisible(deviceDescriptionLabel);

  deviceComboBox.addListener(this);
  deviceComboBox.setLookAndFeel(&settingsLookAndFeel);
  addAndMakeVisible(deviceComboBox);

  // GPU device ID selection
  gpuDeviceLabel.setText(TR("settings.gpu_device"), juce::dontSendNotification);
  configureRowLabel(gpuDeviceLabel);
  addAndMakeVisible(gpuDeviceLabel);
  gpuDeviceDescriptionLabel.setText(TR("settings.gpu_device_desc"),
                                    juce::dontSendNotification);
  configureDescriptionLabel(gpuDeviceDescriptionLabel);
  addAndMakeVisible(gpuDeviceDescriptionLabel);

  // GPU device list will be populated dynamically based on available devices
  gpuDeviceComboBox.addListener(this);
  gpuDeviceComboBox.setLookAndFeel(&settingsLookAndFeel);
  addAndMakeVisible(gpuDeviceComboBox);
  gpuDeviceLabel.setVisible(false);
  gpuDeviceComboBox.setVisible(false);

  // Pitch detector selection
  pitchDetectorLabel.setText(TR("settings.pitch_detector"),
                             juce::dontSendNotification);
  configureRowLabel(pitchDetectorLabel);
  addAndMakeVisible(pitchDetectorLabel);
  pitchDetectorDescriptionLabel.setText(TR("settings.pitch_detector_desc"),
                                        juce::dontSendNotification);
  configureDescriptionLabel(pitchDetectorDescriptionLabel);
  addAndMakeVisible(pitchDetectorDescriptionLabel);

  pitchDetectorComboBox.addItem("RMVPE", 1);
  pitchDetectorComboBox.addItem("FCPE", 2);
  pitchDetectorComboBox.setSelectedId(
      1, juce::dontSendNotification); // Default to RMVPE
  pitchDetectorComboBox.addListener(this);
  pitchDetectorComboBox.setLookAndFeel(&settingsLookAndFeel);
  addAndMakeVisible(pitchDetectorComboBox);

  someSegmentsDebugLabel.setText(TR("settings.debug.some_segments"),
                                 juce::dontSendNotification);
  configureRowLabel(someSegmentsDebugLabel);
  addAndMakeVisible(someSegmentsDebugLabel);
  someSegmentsDebugDescriptionLabel.setText(
      TR("settings.debug.some_segments_desc"), juce::dontSendNotification);
  configureDescriptionLabel(someSegmentsDebugDescriptionLabel);
  addAndMakeVisible(someSegmentsDebugDescriptionLabel);

  someSegmentsDebugToggle.setButtonText("");
  someSegmentsDebugToggle.setClickingTogglesState(true);
  someSegmentsDebugToggle.onClick = [this]() {
    showSomeSegmentsDebug = someSegmentsDebugToggle.getToggleState();
    if (settingsManager) {
      settingsManager->setShowSomeSegmentsDebug(showSomeSegmentsDebug);
      settingsManager->saveConfig();
    }
    if (onShowSomeSegmentsDebugChanged)
      onShowSomeSegmentsDebugChanged(showSomeSegmentsDebug);
  };
  addAndMakeVisible(someSegmentsDebugToggle);

  someValuesDebugLabel.setText(TR("settings.debug.some_values"),
                               juce::dontSendNotification);
  configureRowLabel(someValuesDebugLabel);
  addAndMakeVisible(someValuesDebugLabel);
  someValuesDebugDescriptionLabel.setText(TR("settings.debug.some_values_desc"),
                                          juce::dontSendNotification);
  configureDescriptionLabel(someValuesDebugDescriptionLabel);
  addAndMakeVisible(someValuesDebugDescriptionLabel);

  someValuesDebugToggle.setButtonText("");
  someValuesDebugToggle.setClickingTogglesState(true);
  someValuesDebugToggle.onClick = [this]() {
    showSomeValuesDebug = someValuesDebugToggle.getToggleState();
    if (settingsManager) {
      settingsManager->setShowSomeValuesDebug(showSomeValuesDebug);
      settingsManager->saveConfig();
    }
    if (onShowSomeValuesDebugChanged)
      onShowSomeValuesDebugChanged(showSomeValuesDebug);
  };
  addAndMakeVisible(someValuesDebugToggle);

  uvInterpolationDebugLabel.setText(TR("settings.debug.uv_interpolation"),
                                    juce::dontSendNotification);
  configureRowLabel(uvInterpolationDebugLabel);
  addAndMakeVisible(uvInterpolationDebugLabel);
  uvInterpolationDebugDescriptionLabel.setText(
      TR("settings.debug.uv_interpolation_desc"), juce::dontSendNotification);
  configureDescriptionLabel(uvInterpolationDebugDescriptionLabel);
  addAndMakeVisible(uvInterpolationDebugDescriptionLabel);

  uvInterpolationDebugToggle.setButtonText("");
  uvInterpolationDebugToggle.setClickingTogglesState(true);
  uvInterpolationDebugToggle.onClick = [this]() {
    showUvInterpolationDebug = uvInterpolationDebugToggle.getToggleState();
    if (settingsManager) {
      settingsManager->setShowUvInterpolationDebug(showUvInterpolationDebug);
      settingsManager->saveConfig();
    }
    if (onShowUvInterpolationDebugChanged)
      onShowUvInterpolationDebugChanged(showUvInterpolationDebug);
  };
  addAndMakeVisible(uvInterpolationDebugToggle);

  actualF0DebugLabel.setText(TR("settings.debug.actual_f0"),
                             juce::dontSendNotification);
  configureRowLabel(actualF0DebugLabel);
  addAndMakeVisible(actualF0DebugLabel);
  actualF0DebugDescriptionLabel.setText(TR("settings.debug.actual_f0_desc"),
                                        juce::dontSendNotification);
  configureDescriptionLabel(actualF0DebugDescriptionLabel);
  addAndMakeVisible(actualF0DebugDescriptionLabel);

  actualF0DebugToggle.setButtonText("");
  actualF0DebugToggle.setClickingTogglesState(true);
  actualF0DebugToggle.onClick = [this]() {
    showActualF0Debug = actualF0DebugToggle.getToggleState();
    if (settingsManager) {
      settingsManager->setShowActualF0Debug(showActualF0Debug);
      settingsManager->saveConfig();
    }
    if (onShowActualF0DebugChanged)
      onShowActualF0DebugChanged(showActualF0Debug);
  };
  addAndMakeVisible(actualF0DebugToggle);

  fpsOverlayLabel.setText(TR("settings.fps_overlay"),
                          juce::dontSendNotification);
  configureRowLabel(fpsOverlayLabel);
  addAndMakeVisible(fpsOverlayLabel);
  fpsOverlayDescriptionLabel.setText(TR("settings.fps_overlay_desc"),
                                     juce::dontSendNotification);
  configureDescriptionLabel(fpsOverlayDescriptionLabel);
  addAndMakeVisible(fpsOverlayDescriptionLabel);

  fpsOverlayToggle.setButtonText("");
  fpsOverlayToggle.setClickingTogglesState(true);
  fpsOverlayToggle.onClick = [this]() {
    showFpsOverlay = fpsOverlayToggle.getToggleState();
    if (settingsManager) {
      settingsManager->setShowFpsOverlay(showFpsOverlay);
      settingsManager->saveConfig();
    }
    if (onShowFpsOverlayChanged)
      onShowFpsOverlayChanged(showFpsOverlay);
  };
  addAndMakeVisible(fpsOverlayToggle);

  backgroundWaveformLabel.setText(TR("settings.background_waveform"),
                                  juce::dontSendNotification);
  configureRowLabel(backgroundWaveformLabel);
  addAndMakeVisible(backgroundWaveformLabel);
  backgroundWaveformDescriptionLabel.setText(
      TR("settings.background_waveform_desc"), juce::dontSendNotification);
  configureDescriptionLabel(backgroundWaveformDescriptionLabel);
  addAndMakeVisible(backgroundWaveformDescriptionLabel);

  backgroundWaveformToggle.setButtonText("");
  backgroundWaveformToggle.setClickingTogglesState(true);
  backgroundWaveformToggle.onClick = [this]() {
    showBackgroundWaveform = backgroundWaveformToggle.getToggleState();
    if (settingsManager) {
    settingsManager->setShowBackgroundWaveform(showBackgroundWaveform);
    settingsManager->setShowRainbowWaveform(showRainbowWaveform);
      settingsManager->saveConfig();
    }
    if (onShowBackgroundWaveformChanged)
      onShowBackgroundWaveformChanged(showBackgroundWaveform);
  };
  addAndMakeVisible(backgroundWaveformToggle);

  rainbowWaveformLabel.setText(TR("settings.rainbow_waveform"),
                                juce::dontSendNotification);
  configureRowLabel(rainbowWaveformLabel);
  addAndMakeVisible(rainbowWaveformLabel);
  rainbowWaveformDescriptionLabel.setText(
      TR("settings.rainbow_waveform_desc"), juce::dontSendNotification);
  configureDescriptionLabel(rainbowWaveformDescriptionLabel);
  addAndMakeVisible(rainbowWaveformDescriptionLabel);

  rainbowWaveformToggle.setButtonText("");
  rainbowWaveformToggle.setClickingTogglesState(true);
  rainbowWaveformToggle.onClick = [this]() {
    showRainbowWaveform = rainbowWaveformToggle.getToggleState();
    if (settingsManager) {
      settingsManager->setShowRainbowWaveform(showRainbowWaveform);
      settingsManager->saveConfig();
    }
    if (onShowRainbowWaveformChanged)
      onShowRainbowWaveformChanged(showRainbowWaveform);
  };
  addAndMakeVisible(rainbowWaveformToggle);

  // Info label
  infoLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
  infoLabel.setFont(AppFont::getFont(13.0f));
  infoLabel.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(infoLabel);

  // Audio device settings (standalone mode only)
  if (!pluginMode && deviceManager != nullptr) {
    deviceManager->addChangeListener(this);

    audioSectionLabel.setText(TR("settings.audio"), juce::dontSendNotification);
    audioSectionLabel.setFont(AppFont::getBoldFont(15.0f));
    audioSectionLabel.setColour(juce::Label::textColourId,
                                APP_COLOR_TEXT_MUTED);
    addAndMakeVisible(audioSectionLabel);

    // Audio device type (driver)
    audioDeviceTypeLabel.setText(TR("settings.audio_driver"),
                                 juce::dontSendNotification);
    configureRowLabel(audioDeviceTypeLabel);
    addAndMakeVisible(audioDeviceTypeLabel);
    audioDeviceTypeComboBox.addListener(this);
    audioDeviceTypeComboBox.setLookAndFeel(&settingsLookAndFeel);
    addAndMakeVisible(audioDeviceTypeComboBox);

    // Output device
    audioOutputLabel.setText(TR("settings.audio_output"),
                             juce::dontSendNotification);
    configureRowLabel(audioOutputLabel);
    addAndMakeVisible(audioOutputLabel);
    audioOutputComboBox.addListener(this);
    audioOutputComboBox.setLookAndFeel(&settingsLookAndFeel);
    addAndMakeVisible(audioOutputComboBox);

    // Sample rate
    sampleRateLabel.setText(TR("settings.sample_rate"),
                            juce::dontSendNotification);
    configureRowLabel(sampleRateLabel);
    addAndMakeVisible(sampleRateLabel);
    sampleRateComboBox.addListener(this);
    sampleRateComboBox.setLookAndFeel(&settingsLookAndFeel);
    addAndMakeVisible(sampleRateComboBox);

    // Buffer size
    bufferSizeLabel.setText(TR("settings.buffer_size"),
                            juce::dontSendNotification);
    configureRowLabel(bufferSizeLabel);
    addAndMakeVisible(bufferSizeLabel);
    bufferSizeComboBox.addListener(this);
    bufferSizeComboBox.setLookAndFeel(&settingsLookAndFeel);
    addAndMakeVisible(bufferSizeComboBox);

    // Output channels
    outputChannelsLabel.setText(TR("settings.output_channels"),
                                juce::dontSendNotification);
    configureRowLabel(outputChannelsLabel);
    addAndMakeVisible(outputChannelsLabel);
    outputChannelsComboBox.addItem(TR("settings.mono"), 1);
    outputChannelsComboBox.addItem(TR("settings.stereo"), 2);
    outputChannelsComboBox.setSelectedId(2, juce::dontSendNotification);
    outputChannelsComboBox.addListener(this);
    outputChannelsComboBox.setLookAndFeel(&settingsLookAndFeel);
    addAndMakeVisible(outputChannelsComboBox);

    updateAudioDeviceTypes();
  }

  // Load saved settings
  loadSettings();
  updateDeviceList();

  updateTabButtonStyles();
  updateTabVisibility();

  // Set size based on mode
  if (pluginMode)
    setSize(880, 760);
  else
    setSize(900, 760);
}

SettingsComponent::~SettingsComponent() {
  stopTimer();
  if (!pluginMode && deviceManager != nullptr)
    deviceManager->removeChangeListener(this);
  generalTabButton.setLookAndFeel(nullptr);
  audioTabButton.setLookAndFeel(nullptr);
  languageComboBox.setLookAndFeel(nullptr);
  uiFontSizeComboBox.setLookAndFeel(nullptr);
  deviceComboBox.setLookAndFeel(nullptr);
  gpuDeviceComboBox.setLookAndFeel(nullptr);
  pitchDetectorComboBox.setLookAndFeel(nullptr);
  audioDeviceTypeComboBox.setLookAndFeel(nullptr);
  audioOutputComboBox.setLookAndFeel(nullptr);
  sampleRateComboBox.setLookAndFeel(nullptr);
  bufferSizeComboBox.setLookAndFeel(nullptr);
  outputChannelsComboBox.setLookAndFeel(nullptr);
  someSegmentsDebugToggle.setLookAndFeel(nullptr);
  someValuesDebugToggle.setLookAndFeel(nullptr);
  uvInterpolationDebugToggle.setLookAndFeel(nullptr);
  actualF0DebugToggle.setLookAndFeel(nullptr);
  fpsOverlayToggle.setLookAndFeel(nullptr);
  backgroundWaveformToggle.setLookAndFeel(nullptr);
  rainbowWaveformToggle.setLookAndFeel(nullptr);
}

void SettingsComponent::changeListenerCallback(
    juce::ChangeBroadcaster *source) {
  if (source == deviceManager)
    updateAudioOutputDevices(true);
}

void SettingsComponent::timerCallback() {
  if (!tabAnimationActive)
    return;

  const auto elapsedMs =
      juce::Time::getMillisecondCounterHiRes() - tabAnimationStartTimeMs;
  const auto t = juce::jlimit(
      0.0, 1.0, elapsedMs / static_cast<double>(tabAnimationDurationMs));
  tabAnimationProgress = static_cast<float>(t * t * (3.0 - 2.0 * t));
  applyTabAnimationState();
  repaint();

  if (t >= 1.0) {
    tabAnimationActive = false;
    tabAnimationProgress = 1.0f;
    previousTab = activeTab;
    applyTabAnimationState();
    repaint();
    stopTimer();
  }
}

void SettingsComponent::paint(juce::Graphics &g) {
  juce::Path rounded;
  rounded.addRoundedRectangle(getLocalBounds().toFloat(), cornerRadius);
  g.reduceClipRegion(rounded);

  auto bounds = getLocalBounds().toFloat();
  juce::ColourGradient bgGradient(
      APP_COLOR_SURFACE_ALT, bounds.getX(), bounds.getY(),
      APP_COLOR_BACKGROUND, bounds.getX(), bounds.getBottom(), false);
  g.setGradientFill(bgGradient);
  g.fillRoundedRectangle(bounds, cornerRadius);

  if (!sidebarBounds.isEmpty()) {
    g.setColour(APP_COLOR_SURFACE);
    g.fillRect(sidebarBounds);
    g.setColour(APP_COLOR_BORDER_SUBTLE);
    g.drawLine((float)sidebarBounds.getRight(), (float)sidebarBounds.getY(),
               (float)sidebarBounds.getRight(),
               (float)sidebarBounds.getBottom(), 1.0f);
  }

  if (!cardBounds.isEmpty()) {
    auto card = cardBounds.toFloat();
    juce::ColourGradient cardGradient(
        APP_COLOR_SURFACE_RAISED, card.getX(), card.getY(),
        APP_COLOR_SURFACE, card.getX(), card.getBottom(), false);
    g.setGradientFill(cardGradient);
    g.fillRoundedRectangle(card, 8.0f);

    g.setColour(APP_COLOR_BORDER);
    g.drawRoundedRectangle(card.reduced(0.5f), 8.0f, 1.0f);

    g.setColour(APP_COLOR_BORDER_SUBTLE);
    for (int i = 0; i < separatorYs.size(); ++i) {
      int y = separatorYs[i];
      g.drawLine((float)cardBounds.getX() + 14.0f, (float)y,
                 (float)cardBounds.getRight() - 14.0f, (float)y, 1.0f);
    }
  }
}

void SettingsComponent::resized() {
  auto bounds = getLocalBounds().reduced(16);
  separatorYs.clear();

  const int sidebarWidth = 140;
  sidebarBounds = bounds.removeFromLeft(sidebarWidth);

  auto tabArea = sidebarBounds.reduced(10, 10);
  const int tabHeight = 32;
  generalTabButton.setBounds(tabArea.removeFromTop(tabHeight));
  tabArea.removeFromTop(6);
  audioTabButton.setBounds(tabArea.removeFromTop(tabHeight));

  bounds.removeFromLeft(10);

  auto titleArea = bounds.removeFromTop(34);
  titleLabel.setBounds(titleArea);
  bounds.removeFromTop(6);

  cardBounds = bounds;
  auto content = cardBounds.reduced(16, 12);

  layoutGeneralTab(content);

  if (isTabAvailable(SettingsTab::Audio))
    layoutAudioTab(content);

  applyTabAnimationState();
}

void SettingsComponent::layoutGeneralTab(juce::Rectangle<int> content) {
  const int sectionHeight = 22;
  const int sectionGap = 8;
  const int rowHeight = 40;
  const int rowGap = 4;
  const int controlWidth = 200;

  auto layoutSection = [&](juce::Label &sectionLabel) {
    sectionLabel.setBounds(content.removeFromTop(sectionHeight));
    if (activeTab == SettingsTab::General)
      separatorYs.add(sectionLabel.getBottom() + 3);
    content.removeFromTop(sectionGap);
  };

  auto layoutRow = [&](juce::Label &label, juce::Label &description,
                       juce::Component &control) {
    auto row = content.removeFromTop(rowHeight);
    auto controlArea = row.removeFromRight(controlWidth).reduced(0, 6);
    row.removeFromRight(18);
    auto labelArea = row.removeFromTop(20);
    label.setBounds(labelArea);
    description.setBounds(row);
    control.setBounds(controlArea);
    content.removeFromTop(rowGap);
  };

  auto hideGpuRow = [&]() {
    gpuDeviceLabel.setBounds({});
    gpuDeviceDescriptionLabel.setBounds({});
    gpuDeviceComboBox.setBounds({});
  };

  layoutSection(generalSectionLabel);
  layoutRow(languageLabel, languageDescriptionLabel, languageComboBox);
  layoutRow(uiFontSizeLabel, uiFontSizeDescriptionLabel, uiFontSizeComboBox);

  content.removeFromTop(8);
  layoutSection(inferenceSectionLabel);
  layoutRow(deviceLabel, deviceDescriptionLabel, deviceComboBox);
  if (shouldShowGpuDeviceList())
    layoutRow(gpuDeviceLabel, gpuDeviceDescriptionLabel, gpuDeviceComboBox);
  else
    hideGpuRow();
  layoutRow(pitchDetectorLabel, pitchDetectorDescriptionLabel,
            pitchDetectorComboBox);

  infoLabel.setBounds(content.removeFromTop(34));
  content.removeFromTop(8);

  layoutSection(displaySectionLabel);
  layoutRow(backgroundWaveformLabel, backgroundWaveformDescriptionLabel,
            backgroundWaveformToggle);
  layoutRow(rainbowWaveformLabel, rainbowWaveformDescriptionLabel,
            rainbowWaveformToggle);
  layoutRow(fpsOverlayLabel, fpsOverlayDescriptionLabel, fpsOverlayToggle);

  content.removeFromTop(8);
  layoutSection(debugSectionLabel);
  layoutRow(someSegmentsDebugLabel, someSegmentsDebugDescriptionLabel,
            someSegmentsDebugToggle);
  layoutRow(someValuesDebugLabel, someValuesDebugDescriptionLabel,
            someValuesDebugToggle);
  layoutRow(uvInterpolationDebugLabel, uvInterpolationDebugDescriptionLabel,
            uvInterpolationDebugToggle);
  layoutRow(actualF0DebugLabel, actualF0DebugDescriptionLabel,
            actualF0DebugToggle);
}

void SettingsComponent::layoutAudioTab(juce::Rectangle<int> content) {
  const int rowHeight = 32;
  const int rowGap = 8;
  const int labelWidth = 150;
  const int controlWidth = 190;

  auto layoutRow = [&](juce::Label &label, juce::Component &control) {
    auto row = content.removeFromTop(rowHeight);
    auto labelArea = row.removeFromLeft(labelWidth);
    auto controlArea = row.removeFromRight(controlWidth);
    label.setBounds(labelArea);
    control.setBounds(controlArea.reduced(0, 2));
    content.removeFromTop(rowGap);
  };

  audioSectionLabel.setBounds(content.removeFromTop(20));
  if (activeTab == SettingsTab::Audio)
    separatorYs.add(audioSectionLabel.getBottom() + 3);
  content.removeFromTop(10);

  layoutRow(audioDeviceTypeLabel, audioDeviceTypeComboBox);
  layoutRow(audioOutputLabel, audioOutputComboBox);
  layoutRow(sampleRateLabel, sampleRateComboBox);
  layoutRow(bufferSizeLabel, bufferSizeComboBox);
  layoutRow(outputChannelsLabel, outputChannelsComboBox);
}

void SettingsComponent::applyFontSizes() {
  titleLabel.setFont(AppFont::getBoldFont(20.0f));

  for (auto *label : {&generalSectionLabel, &inferenceSectionLabel,
                      &displaySectionLabel, &debugSectionLabel,
                      &audioSectionLabel}) {
    label->setFont(AppFont::getBoldFont(15.5f));
  }

  for (auto *label : {&languageLabel, &uiFontSizeLabel, &deviceLabel,
                      &gpuDeviceLabel, &pitchDetectorLabel,
                      &backgroundWaveformLabel, &rainbowWaveformLabel,
                      &fpsOverlayLabel,
                      &someSegmentsDebugLabel, &someValuesDebugLabel,
                      &uvInterpolationDebugLabel, &actualF0DebugLabel,
                      &audioDeviceTypeLabel, &audioOutputLabel,
                      &sampleRateLabel, &bufferSizeLabel,
                      &outputChannelsLabel}) {
    label->setFont(AppFont::getFont(14.5f));
  }

  for (auto *label : {&languageDescriptionLabel, &uiFontSizeDescriptionLabel,
                      &deviceDescriptionLabel, &gpuDeviceDescriptionLabel,
                      &pitchDetectorDescriptionLabel,
                      &backgroundWaveformDescriptionLabel,
                      &rainbowWaveformDescriptionLabel,
                      &fpsOverlayDescriptionLabel,
                      &someSegmentsDebugDescriptionLabel,
                      &someValuesDebugDescriptionLabel,
                      &uvInterpolationDebugDescriptionLabel,
                      &actualF0DebugDescriptionLabel}) {
    label->setFont(AppFont::getFont(13.0f));
  }

  infoLabel.setFont(AppFont::getFont(13.0f));
}

void SettingsComponent::comboBoxChanged(juce::ComboBox *comboBox) {
  if (comboBox == &languageComboBox) {
    int selectedId = languageComboBox.getSelectedId();
    if (selectedId == 1) {
      // Auto - detect system language
      Localization::detectSystemLanguage();
    } else if (selectedId >= 2) {
      // Get language code from index
      const auto &langs = Localization::getInstance().getAvailableLanguages();
      int langIndex = selectedId - 2;
      if (langIndex < static_cast<int>(langs.size()))
        Localization::getInstance().setLanguage(langs[langIndex].code);
    }
    saveSettings();

    if (onLanguageChanged)
      onLanguageChanged();
  } else if (comboBox == &uiFontSizeComboBox) {
    uiFontScale = fontScaleForSelectedId(uiFontSizeComboBox.getSelectedId());
    AppFont::setScale(uiFontScale);
    applyFontSizes();
    resized();
    repaint();

    if (settingsManager) {
      settingsManager->setUIFontScale(uiFontScale);
      settingsManager->saveConfig();
    }

    if (onUIFontScaleChanged)
      onUIFontScaleChanged(uiFontScale);
  } else if (comboBox == &deviceComboBox) {
    if (canChangeDevice && !canChangeDevice()) {
      for (int i = 0; i < deviceComboBox.getNumItems(); ++i) {
        if (deviceComboBox.getItemText(i) == lastConfirmedDevice) {
          deviceComboBox.setSelectedItemIndex(i,
                                              juce::dontSendNotification);
          break;
        }
      }
      currentDevice = lastConfirmedDevice;
      updateGPUDeviceList(currentDevice);
      gpuDeviceComboBox.setSelectedId(lastConfirmedGpuDeviceId + 1,
                                      juce::dontSendNotification);
      infoLabel.setText(TR("settings.inference_in_progress"),
                        juce::dontSendNotification);
      updateTabVisibility();
      resized();
      return;
    }

    currentDevice = deviceComboBox.getText();

    // Show/hide GPU device selector based on device type
    bool showGpuDeviceList = shouldShowGpuDeviceList();
    if (showGpuDeviceList) {
      // Update GPU device list for the selected device type
      updateGPUDeviceList(currentDevice);
    }
    updateTabVisibility();
    resized();

    saveSettings();

    // Update info label
    if (currentDevice == "CPU") {
      infoLabel.setText(TR("settings.cpu_desc"), juce::dontSendNotification);
    } else if (currentDevice == "CUDA") {
      infoLabel.setText(TR("settings.cuda_desc"), juce::dontSendNotification);
    } else if (currentDevice == "DirectML") {
      infoLabel.setText(TR("settings.directml_desc"),
                        juce::dontSendNotification);
    } else if (currentDevice == "CoreML") {
      infoLabel.setText(TR("settings.coreml_desc"), juce::dontSendNotification);
    }

    if (onSettingsChanged)
      onSettingsChanged();

    lastConfirmedDevice = currentDevice;
    lastConfirmedGpuDeviceId = gpuDeviceId;
  } else if (comboBox == &gpuDeviceComboBox) {
    if (canChangeDevice && !canChangeDevice()) {
      gpuDeviceComboBox.setSelectedId(lastConfirmedGpuDeviceId + 1,
                                      juce::dontSendNotification);
      infoLabel.setText(TR("settings.inference_in_progress"),
                        juce::dontSendNotification);
      return;
    }
    gpuDeviceId = gpuDeviceComboBox.getSelectedId() - 1;
    saveSettings();
    if (onSettingsChanged)
      onSettingsChanged();
    lastConfirmedGpuDeviceId = gpuDeviceId;
  } else if (comboBox == &pitchDetectorComboBox) {
    if (canChangeDevice && !canChangeDevice()) {
      pitchDetectorComboBox.setSelectedId(
          lastConfirmedPitchDetectorType == PitchDetectorType::RMVPE ? 1 : 2,
          juce::dontSendNotification);
      infoLabel.setText(TR("settings.inference_in_progress"),
                        juce::dontSendNotification);
      juce::AlertWindow::showMessageBoxAsync(
          juce::AlertWindow::InfoIcon, TR("settings.title"),
          TR("settings.inference_in_progress"));
      return;
    }

    int selectedId = pitchDetectorComboBox.getSelectedId();
    if (selectedId == 1)
      pitchDetectorType = PitchDetectorType::RMVPE;
    else if (selectedId == 2)
      pitchDetectorType = PitchDetectorType::FCPE;

    saveSettings();

    if (onPitchDetectorChanged)
      onPitchDetectorChanged(pitchDetectorType);
    lastConfirmedPitchDetectorType = pitchDetectorType;
  } else if (comboBox == &audioDeviceTypeComboBox) {
    int idx = audioDeviceTypeComboBox.getSelectedId() - 1;
    if (idx >= 0 && idx < audioDeviceTypeOrder.size()) {
      const auto targetType = audioDeviceTypeOrder.getReference(idx)->getTypeName();
      const auto *currentType = deviceManager->getCurrentDeviceTypeObject();
      if (currentType == nullptr || currentType->getTypeName() != targetType)
        deviceManager->setCurrentAudioDeviceType(targetType, true);
      updateAudioOutputDevices(false);
    }
  } else if (comboBox == &audioOutputComboBox) {
    followSystemAudioOutput =
        (audioOutputComboBox.getSelectedId() == kFollowSystemOutputId);
    if (settingsManager) {
      settingsManager->setFollowSystemAudioOutput(followSystemAudioOutput);
      settingsManager->saveConfig();
    }
    applyAudioSettings();
    updateSampleRates();
    updateBufferSizes();
  } else if (comboBox == &sampleRateComboBox ||
             comboBox == &bufferSizeComboBox ||
             comboBox == &outputChannelsComboBox) {
    applyAudioSettings();
  }
}

bool SettingsComponent::shouldShowGpuDeviceList() const {
  return currentDevice == "CUDA" || currentDevice == "DirectML";
}

bool SettingsComponent::isTabAvailable(SettingsTab tab) const {
  if (tab == SettingsTab::Audio)
    return !pluginMode && deviceManager != nullptr;

  return true;
}

void SettingsComponent::setActiveTab(SettingsTab tab) {
  if (!isTabAvailable(tab))
    tab = SettingsTab::General;

  if (activeTab == tab)
    return;

  const auto fromTab = activeTab;
  activeTab = tab;

  if (activeTab == SettingsTab::Audio && isTabAvailable(SettingsTab::Audio))
    updateAudioOutputDevices(true);

  updateTabButtonStyles();
  startTabTransition(fromTab, activeTab);
  updateTabVisibility();
  resized();
  repaint();
}

void SettingsComponent::updateTabButtonStyles() {
  auto applyStyle = [&](juce::TextButton &button, bool isActive) {
    if (isActive) {
      button.setColour(juce::TextButton::buttonColourId,
                       APP_COLOR_PRIMARY);
      button.setColour(juce::TextButton::textColourOffId,
                       juce::Colours::white);
    } else {
      button.setColour(juce::TextButton::buttonColourId,
                       APP_COLOR_SURFACE);
      button.setColour(juce::TextButton::textColourOffId,
                       APP_COLOR_TEXT_MUTED);
    }
  };

  applyStyle(generalTabButton, activeTab == SettingsTab::General);
  applyStyle(audioTabButton, activeTab == SettingsTab::Audio);
}

void SettingsComponent::updateTabVisibility() {
  if (!isTabAvailable(SettingsTab::Audio) && activeTab == SettingsTab::Audio) {
    activeTab = SettingsTab::General;
    previousTab = SettingsTab::General;
    tabAnimationActive = false;
    tabAnimationProgress = 1.0f;
  }

  audioTabButton.setVisible(isTabAvailable(SettingsTab::Audio));
  applyTabAnimationState();
}

void SettingsComponent::applyTabAnimationState() {
  const auto progressFor = [this](SettingsTab tab) {
    if (!isTabAvailable(tab))
      return 0.0f;

    if (!tabAnimationActive)
      return activeTab == tab ? 1.0f : 0.0f;

    if (tab == activeTab)
      return tabAnimationProgress;

    if (tab == previousTab)
      return 1.0f - tabAnimationProgress;

    return 0.0f;
  };

  const auto offsetFor = [this](SettingsTab tab) {
    if (!tabAnimationActive)
      return 0.0f;

    constexpr float kOffsetPx = 22.0f;
    if (tab == activeTab)
      return (1.0f - tabAnimationProgress) * kOffsetPx;

    if (tab == previousTab)
      return -tabAnimationProgress * kOffsetPx;

    return 0.0f;
  };

  const auto applyState = [](std::initializer_list<juce::Component *> components,
                             float alpha, float offsetX, bool enabled) {
    const bool visible = alpha > 0.001f;
    const auto transform = juce::AffineTransform::translation(offsetX, 0.0f);

    for (auto *component : components) {
      if (component == nullptr)
        continue;

      component->setVisible(visible);
      component->setAlpha(alpha);
      component->setTransform(transform);
      component->setEnabled(enabled);
    }
  };

  const auto generalAlpha = progressFor(SettingsTab::General);
  const auto audioAlpha = progressFor(SettingsTab::Audio);
  const auto generalEnabled = !tabAnimationActive && activeTab == SettingsTab::General;
  const auto audioEnabled = !tabAnimationActive && activeTab == SettingsTab::Audio;
  const auto showGpu = shouldShowGpuDeviceList();

  applyState({&generalSectionLabel, &languageLabel, &languageDescriptionLabel,
              &languageComboBox, &uiFontSizeLabel,
              &uiFontSizeDescriptionLabel, &uiFontSizeComboBox,
              &inferenceSectionLabel, &deviceLabel,
              &deviceDescriptionLabel, &deviceComboBox, &gpuDeviceLabel,
              &gpuDeviceDescriptionLabel, &gpuDeviceComboBox,
              &pitchDetectorLabel, &pitchDetectorDescriptionLabel,
              &pitchDetectorComboBox, &displaySectionLabel,
              &backgroundWaveformLabel, &backgroundWaveformDescriptionLabel,
              &backgroundWaveformToggle, &rainbowWaveformLabel,
              &rainbowWaveformDescriptionLabel, &rainbowWaveformToggle,
              &fpsOverlayLabel,
              &fpsOverlayDescriptionLabel, &fpsOverlayToggle, &debugSectionLabel,
              &someSegmentsDebugLabel, &someSegmentsDebugDescriptionLabel,
              &someSegmentsDebugToggle, &someValuesDebugLabel,
              &someValuesDebugDescriptionLabel, &someValuesDebugToggle,
              &uvInterpolationDebugLabel, &uvInterpolationDebugDescriptionLabel,
              &uvInterpolationDebugToggle, &actualF0DebugLabel,
              &actualF0DebugDescriptionLabel, &actualF0DebugToggle,
              &infoLabel},
             generalAlpha, offsetFor(SettingsTab::General), generalEnabled);

  if (!showGpu) {
    gpuDeviceLabel.setVisible(false);
    gpuDeviceDescriptionLabel.setVisible(false);
    gpuDeviceComboBox.setVisible(false);
  }

  applyState({&audioSectionLabel, &audioDeviceTypeLabel,
              &audioDeviceTypeComboBox, &audioOutputLabel,
              &audioOutputComboBox, &sampleRateLabel, &sampleRateComboBox,
              &bufferSizeLabel, &bufferSizeComboBox, &outputChannelsLabel,
              &outputChannelsComboBox},
             audioAlpha, offsetFor(SettingsTab::Audio), audioEnabled);
}

void SettingsComponent::startTabTransition(SettingsTab fromTab, SettingsTab toTab) {
  if (fromTab == toTab || !isTabAvailable(fromTab) || !isTabAvailable(toTab)) {
    tabAnimationActive = false;
    tabAnimationProgress = 1.0f;
    previousTab = toTab;
    stopTimer();
    return;
  }

  previousTab = fromTab;
  tabAnimationActive = true;
  tabAnimationProgress = 0.0f;
  tabAnimationStartTimeMs = juce::Time::getMillisecondCounterHiRes();
  startTimerHz(60);
}

void SettingsComponent::updateDeviceList() {
  deviceComboBox.clear();

  auto devices = getAvailableDevices();
  int selectedIndex = 0;

  // Auto-select based on compile-time flags (first run only)
  if (!hasLoadedSettings && currentDevice == "CPU") {
#ifdef USE_DIRECTML
    // If DirectML is compiled in, default to DirectML
    for (int i = 0; i < devices.size(); ++i) {
      if (devices[i] == "DirectML") {
        selectedIndex = i;
        currentDevice = devices[i];
        DBG("Auto-selecting DirectML (compiled in)");
        break;
      }
    }
#elif defined(USE_CUDA)
    // If CUDA is compiled in, default to CUDA
    for (int i = 0; i < devices.size(); ++i) {
      if (devices[i] == "CUDA") {
        selectedIndex = i;
        currentDevice = devices[i];
        DBG("Auto-selecting CUDA (compiled in)");
        break;
      }
    }
#else
    // No GPU compiled in, stay on CPU
    DBG("No GPU provider compiled in, using CPU");
#endif
  }

  for (int i = 0; i < devices.size(); ++i) {
    deviceComboBox.addItem(devices[i], i + 1);
    if (devices[i] == currentDevice)
      selectedIndex = i;
  }

  deviceComboBox.setSelectedItemIndex(selectedIndex,
                                      juce::dontSendNotification);

  // Update info for initially selected device
  comboBoxChanged(&deviceComboBox);
}

void SettingsComponent::updateGPUDeviceList(const juce::String &deviceType) {
  gpuDeviceComboBox.clear();

  if (deviceType == "CPU") {
    // No GPU devices for CPU
    return;
  }

#ifdef HAVE_ONNXRUNTIME
  if (deviceType == "CUDA") {
#ifdef USE_CUDA
    int deviceCount = 0;
    bool devicesDetected = false;
    juce::StringArray cudaDeviceNames;

    // Try to load CUDA runtime library to get actual device count and names
#ifdef _WIN32
    const char *cudaDllNames[] = {
        "cudart64_12.dll", // CUDA 12.x
        "cudart64_11.dll", // CUDA 11.x
        "cudart64_10.dll", // CUDA 10.x
        "cudart64.dll"     // Generic
    };

    HMODULE cudaLib = nullptr;
    for (const char *dllName : cudaDllNames) {
      cudaLib = LoadLibraryA(dllName);
      if (cudaLib) {
        DBG("Loaded CUDA runtime: " + juce::String(dllName));
        break;
      }
    }

    if (cudaLib) {
      typedef int (*cudaGetDeviceCountFunc)(int *);
      typedef int (*cudaGetDevicePropertiesFunc)(void *, int);

      auto cudaGetDeviceCount =
          (cudaGetDeviceCountFunc)GetProcAddress(cudaLib, "cudaGetDeviceCount");

      if (cudaGetDeviceCount) {
        int result = cudaGetDeviceCount(&deviceCount);
        if (result == 0 && deviceCount > 0) {
          DBG("CUDA device count: " + juce::String(deviceCount));

          // Try to get device properties for names
          auto cudaGetDeviceProperties =
              (cudaGetDevicePropertiesFunc)GetProcAddress(
                  cudaLib, "cudaGetDeviceProperties");

          for (int deviceId = 0; deviceId < deviceCount; ++deviceId) {
            juce::String deviceName = "GPU " + juce::String(deviceId);

            // Try to get device name from properties
            if (cudaGetDeviceProperties) {
              // Allocate full cudaDeviceProp structure (it's large, ~1KB)
              // We can't use the actual struct without CUDA headers, so
              // allocate enough space
              char propBuffer[2048]; // Large enough for cudaDeviceProp
              memset(propBuffer, 0, sizeof(propBuffer));

              if (cudaGetDeviceProperties(propBuffer, deviceId) == 0) {
                // Device name is at the start of the structure
                char *name = propBuffer;
                if (name[0] != '\0') {
                  deviceName = juce::String(name);
                  DBG("CUDA device " + juce::String(deviceId) + ": " +
                      deviceName);
                }
              }
            }

            cudaDeviceNames.add(deviceName + " (CUDA)");
          }
          devicesDetected = true;
        } else {
          DBG("cudaGetDeviceCount failed or returned 0 devices");
        }
      }
      FreeLibrary(cudaLib);
    } else {
      DBG("Failed to load CUDA runtime library");
    }
#endif

    if (devicesDetected && cudaDeviceNames.size() > 0) {
      for (int i = 0; i < cudaDeviceNames.size(); ++i)
        gpuDeviceComboBox.addItem(cudaDeviceNames[i], i + 1);
    } else {
#ifdef _WIN32
      auto dxgiNames = getDxgiAdapterNames();
      if (dxgiNames.size() > 0) {
        for (int i = 0; i < dxgiNames.size(); ++i)
          gpuDeviceComboBox.addItem(dxgiNames[i] + " (DXGI)", i + 1);
      }
#endif
    }

    // If no devices detected, add default
    if (gpuDeviceComboBox.getNumItems() == 0) {
      gpuDeviceComboBox.addItem("GPU 0 (CUDA)", 1);
      DBG("No CUDA devices detected, using default GPU 0");
    }
#else
    // CUDA not compiled in, but provider is available
    // This shouldn't happen, but add default option
    gpuDeviceComboBox.addItem("GPU 0 (CUDA)", 1);
    DBG("CUDA provider available but USE_CUDA not defined");
#endif
  } else if (deviceType == "DirectML") {
#ifdef USE_DIRECTML
    bool addedFromDxgi = false;
#ifdef _WIN32
    auto dxgiNames = getDxgiAdapterNames();
    if (dxgiNames.size() > 0) {
      for (int i = 0; i < dxgiNames.size(); ++i)
        gpuDeviceComboBox.addItem(dxgiNames[i] + " (DirectML)", i + 1);
      addedFromDxgi = true;
    }
#endif
    if (!addedFromDxgi) {
      // DirectML fallback: provide a small default list
      for (int deviceId = 0; deviceId < 4; ++deviceId) {
        gpuDeviceComboBox.addItem(
            "GPU " + juce::String(deviceId) + " (DirectML)", deviceId + 1);
      }
    }
#else
    // DirectML not compiled in
    gpuDeviceComboBox.addItem("GPU 0 (DirectML)", 1);
    DBG("DirectML provider available but USE_DIRECTML not defined");
#endif
  } else {
    // Other GPU providers (CoreML, TensorRT) - use default device
    gpuDeviceComboBox.addItem(TR("settings.default_gpu"), 1);
  }

  // Set default selection
  if (gpuDeviceComboBox.getNumItems() > 0) {
    // Try to restore saved selection, or use first device
    int savedId = gpuDeviceId + 1;
    if (savedId > 0 && savedId <= gpuDeviceComboBox.getNumItems())
      gpuDeviceComboBox.setSelectedId(savedId, juce::dontSendNotification);
    else
      gpuDeviceComboBox.setSelectedId(1, juce::dontSendNotification);
  }
#endif
}

juce::StringArray SettingsComponent::getAvailableDevices() {
  juce::StringArray devices;

  // CPU is always available
  devices.add("CPU");

#ifdef HAVE_ONNXRUNTIME
  try {
    // Get providers that are compiled into the ONNX Runtime library
    auto availableProviders = Ort::GetAvailableProviders();

    // Check which providers are available
    bool hasCuda = false, hasDml = false, hasCoreML = false,
         hasTensorRT = false;

    DBG("=== ONNX Runtime Provider Detection ===");
    DBG("Total providers found: " + juce::String(availableProviders.size()));
    DBG("Available ONNX Runtime providers:");
    for (const auto &provider : availableProviders) {
      juce::String providerStr(provider);
      DBG("  - " + providerStr);

      if (providerStr == "CUDAExecutionProvider")
        hasCuda = true;
      else if (providerStr == "DmlExecutionProvider")
        hasDml = true;
      else if (providerStr == "CoreMLExecutionProvider")
        hasCoreML = true;
      else if (providerStr == "TensorrtExecutionProvider")
        hasTensorRT = true;
    }

    // Add available GPU providers based on compile-time flags
    // DML and CUDA are mutually exclusive
#ifdef USE_DIRECTML
    if (hasDml) {
      devices.add("DirectML");
      DBG("DirectML provider: ENABLED");
    }
#elif defined(USE_CUDA)
    if (hasCuda) {
      devices.add("CUDA");
      DBG("CUDA provider: ENABLED");
    }
#else
    // No GPU compiled in, but show available providers for information
    if (hasCuda) {
      devices.add("CUDA");
      DBG("CUDA provider: AVAILABLE (not compiled in)");
    }
    if (hasDml) {
      devices.add("DirectML");
      DBG("DirectML provider: AVAILABLE (not compiled in)");
    }
#endif
    if (hasCoreML) {
      devices.add("CoreML");
      DBG("CoreML provider: ENABLED");
    }
    if (hasTensorRT) {
      devices.add("TensorRT");
      DBG("TensorRT provider: ENABLED");
    }

    // If no GPU providers found, show info about how to enable them
    if (!hasCuda && !hasDml && !hasCoreML && !hasTensorRT) {
      DBG("WARNING: No GPU execution providers available in this ONNX Runtime "
          "build.");
      DBG("This appears to be a CPU-only build of ONNX Runtime.");
      DBG("To enable GPU acceleration:");
      DBG("  - Windows DirectML: Use onnxruntime-directml package");
      DBG("  - NVIDIA CUDA: Use onnxruntime-gpu package (requires CUDA "
          "toolkit)");
    }
  } catch (const std::exception &e) {
    DBG("ERROR: Failed to get ONNX Runtime providers: " +
        juce::String(e.what()));
    DBG("Falling back to CPU-only mode.");
  }
#else
  DBG("WARNING: HAVE_ONNXRUNTIME not defined - only CPU available");
  DBG("To enable GPU support, ensure ONNX Runtime is properly configured in "
      "CMakeLists.txt");
#endif

  DBG("Final device list: " + devices.joinIntoString(", "));
  return devices;
}

void SettingsComponent::loadSettings() {
  const auto &langs = Localization::getInstance().getAvailableLanguages();

  if (settingsManager)
    settingsManager->loadConfig();

  if (settingsManager) {
    currentDevice = settingsManager->getDevice();
    gpuDeviceId = settingsManager->getGPUDeviceId();
    pitchDetectorType = settingsManager->getPitchDetectorType();
    uiFontScale = settingsManager->getUIFontScale();
    AppFont::setScale(uiFontScale);
    followSystemAudioOutput = settingsManager->getFollowSystemAudioOutput();
    showSomeSegmentsDebug = settingsManager->getShowSomeSegmentsDebug();
    showSomeValuesDebug = settingsManager->getShowSomeValuesDebug();
    showUvInterpolationDebug = settingsManager->getShowUvInterpolationDebug();
    showActualF0Debug = settingsManager->getShowActualF0Debug();
    showFpsOverlay = settingsManager->getShowFpsOverlay();
    showBackgroundWaveform = settingsManager->getShowBackgroundWaveform();
    showRainbowWaveform = settingsManager->getShowRainbowWaveform();

    auto langCode = settingsManager->getLanguage();
    if (langCode == "auto") {
      Localization::detectSystemLanguage();
      languageComboBox.setSelectedId(1, juce::dontSendNotification);
    } else {
      Localization::getInstance().setLanguage(langCode);
      for (int i = 0; i < static_cast<int>(langs.size()); ++i) {
        if (langs[i].code == langCode) {
          languageComboBox.setSelectedId(i + 2, juce::dontSendNotification);
          break;
        }
      }
    }
  }

  uiFontSizeComboBox.setSelectedId(selectedIdForFontScale(uiFontScale),
                                   juce::dontSendNotification);
  applyFontSizes();

  // Update the ComboBox selection to match loaded settings
  for (int i = 0; i < deviceComboBox.getNumItems(); ++i) {
    if (deviceComboBox.getItemText(i) == currentDevice) {
      deviceComboBox.setSelectedItemIndex(i, juce::dontSendNotification);
      break;
    }
  }

  // Update GPU device ID and visibility
  bool showGpuDeviceList =
      (currentDevice == "CUDA" || currentDevice == "DirectML");
  if (showGpuDeviceList) {
    // Update GPU device list for the loaded device type
    updateGPUDeviceList(currentDevice);
  }
  gpuDeviceComboBox.setSelectedId(gpuDeviceId + 1, juce::dontSendNotification);
  gpuDeviceLabel.setVisible(showGpuDeviceList);
  gpuDeviceComboBox.setVisible(showGpuDeviceList);

  // Update pitch detector combo box
  if (pitchDetectorType == PitchDetectorType::RMVPE)
    pitchDetectorComboBox.setSelectedId(1, juce::dontSendNotification);
  else if (pitchDetectorType == PitchDetectorType::FCPE)
    pitchDetectorComboBox.setSelectedId(2, juce::dontSendNotification);
  lastConfirmedPitchDetectorType = pitchDetectorType;

  someSegmentsDebugToggle.setToggleState(showSomeSegmentsDebug,
                                         juce::dontSendNotification);
  someValuesDebugToggle.setToggleState(showSomeValuesDebug,
                                       juce::dontSendNotification);
  uvInterpolationDebugToggle.setToggleState(showUvInterpolationDebug,
                                            juce::dontSendNotification);
  actualF0DebugToggle.setToggleState(showActualF0Debug,
                                     juce::dontSendNotification);
  fpsOverlayToggle.setToggleState(showFpsOverlay, juce::dontSendNotification);
  backgroundWaveformToggle.setToggleState(showBackgroundWaveform,
                                          juce::dontSendNotification);
  rainbowWaveformToggle.setToggleState(showRainbowWaveform,
                                       juce::dontSendNotification);

  hasLoadedSettings = true;
  lastConfirmedDevice = currentDevice;
  lastConfirmedGpuDeviceId = gpuDeviceId;

  if (!pluginMode && deviceManager != nullptr)
    updateAudioOutputDevices(true);
}

void SettingsComponent::saveSettings() {
  // Don't save if combo box not initialized yet
  if (languageComboBox.getSelectedId() == 0)
    return;

  // Save language code
  int langId = languageComboBox.getSelectedId();
  juce::String langCode = "auto";
  if (langId >= 2) {
    const auto &langs = Localization::getInstance().getAvailableLanguages();
    int langIndex = langId - 2;
    if (langIndex < static_cast<int>(langs.size()))
      langCode = langs[langIndex].code;
  }

  if (settingsManager) {
    settingsManager->setDevice(currentDevice);
    settingsManager->setGPUDeviceId(gpuDeviceId);
    settingsManager->setPitchDetectorType(pitchDetectorType);
    settingsManager->setLanguage(langCode);
    settingsManager->setUIFontScale(uiFontScale);
    settingsManager->setShowSomeSegmentsDebug(showSomeSegmentsDebug);
    settingsManager->setShowSomeValuesDebug(showSomeValuesDebug);
    settingsManager->setShowUvInterpolationDebug(showUvInterpolationDebug);
    settingsManager->setShowActualF0Debug(showActualF0Debug);
    settingsManager->setShowFpsOverlay(showFpsOverlay);
    settingsManager->setShowBackgroundWaveform(showBackgroundWaveform);
    settingsManager->setFollowSystemAudioOutput(followSystemAudioOutput);
    settingsManager->saveConfig();
  }
}

void SettingsComponent::updateAudioDeviceTypes() {
  if (deviceManager == nullptr)
    return;

  audioDeviceTypeComboBox.clear();
  audioDeviceTypeOrder.clear();

  auto &types = deviceManager->getAvailableDeviceTypes();
  juce::AudioIODeviceType *asioType = nullptr;
  for (int i = 0; i < types.size(); ++i) {
    if (types[i]->getTypeName() == "ASIO") {
      asioType = types[i];
    } else {
      audioDeviceTypeOrder.add(types[i]);
    }
  }

  if (asioType != nullptr)
    audioDeviceTypeOrder.insert(0, asioType);

  for (int i = 0; i < audioDeviceTypeOrder.size(); ++i)
    audioDeviceTypeComboBox.addItem(
        audioDeviceTypeOrder[i]->getTypeName(), i + 1);

  if (auto *currentType = deviceManager->getCurrentDeviceTypeObject()) {
    for (int i = 0; i < audioDeviceTypeOrder.size(); ++i) {
      if (audioDeviceTypeOrder[i] == currentType) {
        audioDeviceTypeComboBox.setSelectedId(i + 1,
                                              juce::dontSendNotification);
        break;
      }
    }
  }
  updateAudioOutputDevices(true);
}

void SettingsComponent::updateAudioOutputDevices(bool force) {
  if (deviceManager == nullptr)
    return;

  if (auto *currentType = deviceManager->getCurrentDeviceTypeObject()) {
    juce::StringArray devices;
    if (force) {
      devices = currentType->getDeviceNames(false); // false = output devices
      cachedOutputDevices = devices;
      cachedDeviceTypeName = currentType->getTypeName();
    } else {
      if (cachedDeviceTypeName != currentType->getTypeName())
        return;
      devices = cachedOutputDevices;
    }

    juce::String currentName;
    if (auto *audioDevice = deviceManager->getCurrentAudioDevice())
      currentName = audioDevice->getName();

    if (!force && currentName == cachedOutputDeviceName) {
      return;
    }

    cachedOutputDeviceName = currentName;

    audioOutputComboBox.clear();
    audioOutputComboBox.addItem("System Default", kFollowSystemOutputId);
    for (int i = 0; i < devices.size(); ++i)
      audioOutputComboBox.addItem(devices[i], i + 2);

    if (followSystemAudioOutput) {
      audioOutputComboBox.setSelectedId(kFollowSystemOutputId,
                                        juce::dontSendNotification);
    } else {
      for (int i = 0; i < devices.size(); ++i) {
        if (devices[i] == currentName) {
          audioOutputComboBox.setSelectedId(i + 2,
                                            juce::dontSendNotification);
          break;
        }
      }
      if (audioOutputComboBox.getSelectedId() == 0)
        audioOutputComboBox.setSelectedId(kFollowSystemOutputId,
                                          juce::dontSendNotification);
    }
  }
  updateSampleRates();
  updateBufferSizes();
}

void SettingsComponent::updateSampleRates() {
  if (deviceManager == nullptr)
    return;

  sampleRateComboBox.clear();
  if (auto *device = deviceManager->getCurrentAudioDevice()) {
    auto rates = device->getAvailableSampleRates();
    double currentRate = device->getCurrentSampleRate();
    for (int i = 0; i < rates.size(); ++i) {
      sampleRateComboBox.addItem(
          juce::String(static_cast<int>(rates[i])) + " Hz", i + 1);
      if (std::abs(rates[i] - currentRate) < 1.0)
        sampleRateComboBox.setSelectedId(i + 1, juce::dontSendNotification);
    }
  } else {
    auto setup = deviceManager->getAudioDeviceSetup();
    if (setup.sampleRate > 0.0) {
      sampleRateComboBox.addItem(
          juce::String(static_cast<int>(setup.sampleRate)) + " Hz", 1);
      sampleRateComboBox.setSelectedId(1, juce::dontSendNotification);
    }
  }
}

void SettingsComponent::updateBufferSizes() {
  if (deviceManager == nullptr)
    return;

  bufferSizeComboBox.clear();
  if (auto *device = deviceManager->getCurrentAudioDevice()) {
    auto sizes = device->getAvailableBufferSizes();
    int currentSize = device->getCurrentBufferSizeSamples();
    for (int i = 0; i < sizes.size(); ++i) {
      bufferSizeComboBox.addItem(juce::String(sizes[i]) + " samples", i + 1);
      if (sizes[i] == currentSize)
        bufferSizeComboBox.setSelectedId(i + 1, juce::dontSendNotification);
    }
  } else {
    auto setup = deviceManager->getAudioDeviceSetup();
    if (setup.bufferSize > 0) {
      bufferSizeComboBox.addItem(juce::String(setup.bufferSize) + " samples", 1);
      bufferSizeComboBox.setSelectedId(1, juce::dontSendNotification);
    }
  }
}

void SettingsComponent::applyAudioSettings() {
  if (deviceManager == nullptr)
    return;

  auto setup = deviceManager->getAudioDeviceSetup();
  const auto originalSetup = setup;
  const bool followSystemOutput =
      (audioOutputComboBox.getSelectedId() == kFollowSystemOutputId) ||
      followSystemAudioOutput;
  followSystemAudioOutput = followSystemOutput;

  // Get selected output device
  if (followSystemOutput) {
    setup.outputDeviceName.clear();
  } else if (audioOutputComboBox.getSelectedId() > kFollowSystemOutputId) {
    setup.outputDeviceName = audioOutputComboBox.getText();
  }

  // Get selected sample rate
  if (followSystemOutput) {
    // Let JUCE choose supported defaults for the current system default device.
    setup.sampleRate = 0.0;
    setup.bufferSize = 0;
  } else if (auto *device = deviceManager->getCurrentAudioDevice()) {
    auto rates = device->getAvailableSampleRates();
    int rateIdx = sampleRateComboBox.getSelectedId() - 1;
    if (rateIdx >= 0 && rateIdx < rates.size())
      setup.sampleRate = rates[rateIdx];

    auto sizes = device->getAvailableBufferSizes();
    int sizeIdx = bufferSizeComboBox.getSelectedId() - 1;
    if (sizeIdx >= 0 && sizeIdx < sizes.size())
      setup.bufferSize = sizes[sizeIdx];
  }

  // Output channels
  int channels = outputChannelsComboBox.getSelectedId();
  setup.outputChannels.setRange(0, channels, true);

  if (setup.outputDeviceName == originalSetup.outputDeviceName &&
      std::abs(setup.sampleRate - originalSetup.sampleRate) < 1.0 &&
      setup.bufferSize == originalSetup.bufferSize &&
      setup.outputChannels == originalSetup.outputChannels) {
    return;
  }

  deviceManager->setAudioDeviceSetup(setup, true);
}

//==============================================================================
// SettingsOverlay
//==============================================================================

SettingsOverlay::SettingsOverlay(SettingsManager *settingsManager,
                                 juce::AudioDeviceManager *audioDeviceManager) {
  setOpaque(false);
  setInterceptsMouseClicks(true, true);
  setWantsKeyboardFocus(true);

  settingsComponent =
      std::make_unique<SettingsComponent>(settingsManager, audioDeviceManager);
  addAndMakeVisible(settingsComponent.get());

  closeButton.setColour(juce::TextButton::buttonColourId,
                        APP_COLOR_SURFACE);
  closeButton.setColour(juce::TextButton::textColourOffId,
                        APP_COLOR_TEXT_PRIMARY);
  closeButton.setColour(juce::TextButton::buttonOnColourId,
                        APP_COLOR_SURFACE_RAISED);
  closeButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
  closeButton.setLookAndFeel(&DarkLookAndFeel::getInstance());
  closeButton.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  closeButton.onClick = [this]() { closeIfPossible(); };
  addAndMakeVisible(closeButton);
}

SettingsOverlay::~SettingsOverlay() {
  stopTimer();
  closeButton.setLookAndFeel(nullptr);
}

void SettingsOverlay::openAnimated() {
  settingsComponent->setVisible(true);
  closeButton.setVisible(true);

  if (!isVisible())
    setVisible(true);

  toFront(true);
  grabKeyboardFocus();
  startAnimation(1.0f);
}

void SettingsOverlay::closeAnimated() {
  if (!isVisible() && animationProgress <= 0.0f)
    return;

  startAnimation(0.0f);
}

void SettingsOverlay::paint(juce::Graphics &g) {
  g.fillAll(APP_COLOR_OVERLAY_DIM.withMultipliedAlpha(animationProgress));

  const auto animatedBounds = getAnimatedContentBounds().getSmallestIntegerContainer();
  if (!animatedBounds.isEmpty()) {
    juce::DropShadow shadow(APP_COLOR_OVERLAY_SHADOW.withMultipliedAlpha(animationProgress),
                            18, {0, 10});
    shadow.drawForRectangle(g, animatedBounds);
  }
}

void SettingsOverlay::resized() {
  auto bounds = getLocalBounds();

  if (settingsComponent != nullptr) {
    const int preferredWidth = settingsComponent->getWidth();
    const int preferredHeight = settingsComponent->getHeight();
    const int maxWidth = juce::jmax(420, bounds.getWidth() - 80);
    const int maxHeight = juce::jmax(320, bounds.getHeight() - 80);
    const int contentWidth = juce::jmin(preferredWidth, maxWidth);
    const int contentHeight = juce::jmin(preferredHeight, maxHeight);
    contentBounds = juce::Rectangle<int>(0, 0, contentWidth, contentHeight)
                        .withCentre(bounds.getCentre());
    settingsComponent->setBounds(contentBounds);

    const int buttonSize = 24;
    closeButton.setBounds(contentBounds.getRight() - buttonSize - 10,
                          contentBounds.getY() + 8, buttonSize, buttonSize);

    updateAnimatedState();
  }
}

void SettingsOverlay::mouseDown(const juce::MouseEvent &e) {
  if (!getAnimatedContentBounds().getSmallestIntegerContainer().contains(
          e.getPosition()))
    closeIfPossible();
}

bool SettingsOverlay::keyPressed(const juce::KeyPress &key) {
  if (key == juce::KeyPress::escapeKey) {
    closeIfPossible();
    return true;
  }
  return false;
}

void SettingsOverlay::closeIfPossible() {
  if (onClose) {
    onClose();
    return;
  }

  closeAnimated();
}

void SettingsOverlay::timerCallback() {
  const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - animationStartTimeMs;
  const auto t = juce::jlimit(0.0, 1.0, elapsedMs / static_cast<double>(animationDurationMs));
  const auto eased = static_cast<float>(t * t * (3.0 - 2.0 * t));

  animationProgress = juce::jmap(eased, animationStartProgress, animationTargetProgress);
  updateAnimatedState();
  repaint();

  if (t >= 1.0) {
    stopTimer();
    animationProgress = animationTargetProgress;
    updateAnimatedState();

    if (animationTargetProgress <= 0.0f) {
      setVisible(false);
    } else {
      repaint();
    }
  }
}

void SettingsOverlay::startAnimation(float nextTarget) {
  if (std::abs(animationProgress - nextTarget) <= 0.001f &&
      std::abs(animationTargetProgress - nextTarget) <= 0.001f) {
    if (nextTarget <= 0.0f)
      setVisible(false);
    else
      updateAnimatedState();
    return;
  }

  if (nextTarget > 0.0f && !isVisible())
    setVisible(true);

  settingsComponent->setVisible(true);
  closeButton.setVisible(true);

  animationStartProgress = animationProgress;
  animationTargetProgress = nextTarget;
  animationStartTimeMs = juce::Time::getMillisecondCounterHiRes();
  startTimerHz(60);
  updateAnimatedState();
  repaint();
}

void SettingsOverlay::updateAnimatedState() {
  if (settingsComponent == nullptr || contentBounds.isEmpty())
    return;

  const auto transform = getAnimatedTransform();
  settingsComponent->setVisible(true);
  settingsComponent->setTransform(transform);
  settingsComponent->setAlpha(animationProgress);
  settingsComponent->setEnabled(animationProgress >= 0.999f);

  closeButton.setVisible(true);
  closeButton.setTransform(transform);
  closeButton.setAlpha(animationProgress);
  closeButton.setEnabled(animationProgress >= 0.999f);
}

juce::Rectangle<float> SettingsOverlay::getAnimatedContentBounds() const {
  auto animatedBounds = contentBounds.toFloat();
  const auto scale = juce::jmap(animationProgress, 0.0f, 1.0f, 0.97f, 1.0f);
  const auto translateY = juce::jmap(animationProgress, 0.0f, 1.0f, 14.0f, 0.0f);
  const auto reducedWidth = animatedBounds.getWidth() * (1.0f - scale);
  const auto reducedHeight = animatedBounds.getHeight() * (1.0f - scale);

  animatedBounds = animatedBounds.reduced(reducedWidth * 0.5f, reducedHeight * 0.5f);
  animatedBounds.translate(0.0f, translateY);
  return animatedBounds;
}

juce::AffineTransform SettingsOverlay::getAnimatedTransform() const {
  if (contentBounds.isEmpty())
    return {};

  const auto scale = juce::jmap(animationProgress, 0.0f, 1.0f, 0.97f, 1.0f);
  const auto translateY = juce::jmap(animationProgress, 0.0f, 1.0f, 14.0f, 0.0f);
  const auto centreX = static_cast<float>(contentBounds.getCentreX());
  const auto centreY = static_cast<float>(contentBounds.getCentreY());

  return juce::AffineTransform::translation(-centreX, -centreY)
      .scaled(scale, scale)
      .translated(centreX, centreY + translateY);
}

//==============================================================================
// SettingsDialog
//==============================================================================

SettingsDialog::SettingsDialog(SettingsManager *settingsManager,
                               juce::AudioDeviceManager *audioDeviceManager)
    : DialogWindow("Settings", APP_COLOR_BACKGROUND, true) {
  // Set opaque before any other operations - this must be done first
  setOpaque(true);

  // Create and configure content component
  settingsComponent =
      std::make_unique<SettingsComponent>(settingsManager, audioDeviceManager);

  // Ensure content component is also opaque before setting it
  if (settingsComponent)
    settingsComponent->setOpaque(true);

  // Set content before native title bar
  setContentOwned(settingsComponent.get(), false);

  // Now set native title bar after content is set and opaque
  setUsingNativeTitleBar(true);

  // Set window properties
  setResizable(false, false);

  int dialogWidth = 460;
  int dialogHeight = audioDeviceManager != nullptr ? 600 : 320;
  if (settingsComponent != nullptr) {
    dialogWidth = settingsComponent->getWidth();
    dialogHeight = settingsComponent->getHeight();
  }
  centreWithSize(dialogWidth, dialogHeight);
}

void SettingsDialog::closeButtonPressed() { setVisible(false); }

void SettingsDialog::paint(juce::Graphics &g) {
  g.fillAll(APP_COLOR_BACKGROUND);
}
