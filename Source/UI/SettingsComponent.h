#pragma once

#include "../Audio/PitchDetectorType.h"
#include "../JuceHeader.h"
#include "../Utils/Constants.h"
#include "Main/SettingsManager.h"
#include "StyledComponents.h"
#include <functional>

enum class Language; // Forward declaration

/**
 * Settings dialog for application configuration.
 * Includes device selection for ONNX inference.
 */
class SettingsComponent : public juce::Component,
                          public juce::ComboBox::Listener,
                          public juce::ChangeListener,
                          public juce::Timer {
public:
  SettingsComponent(SettingsManager *settingsManager,
                    juce::AudioDeviceManager *audioDeviceManager = nullptr);
  ~SettingsComponent() override;

  void paint(juce::Graphics &g) override;
  void resized() override;

  // ComboBox::Listener
  void comboBoxChanged(juce::ComboBox *comboBox) override;

  // ChangeListener
  void changeListenerCallback(juce::ChangeBroadcaster *source) override;

  // Timer
  void timerCallback() override;

  // Get current settings
  juce::String getSelectedDevice() const { return currentDevice; }
  int getGPUDeviceId() const { return gpuDeviceId; }
  PitchDetectorType getPitchDetectorType() const { return pitchDetectorType; }

  // Plugin mode (disables audio device settings)
  bool isPluginMode() const { return pluginMode; }

  // Callbacks
  std::function<void()> onSettingsChanged;
  std::function<void()> onLanguageChanged;
  std::function<void(float)> onUIFontScaleChanged;
  std::function<void(PitchDetectorType)> onPitchDetectorChanged;
  std::function<void(bool)> onShowSomeSegmentsDebugChanged;
  std::function<void(bool)> onShowSomeValuesDebugChanged;
  std::function<void(bool)> onShowUvInterpolationDebugChanged;
  std::function<void(bool)> onShowActualF0DebugChanged;
  std::function<void(bool)> onShowFpsOverlayChanged;
  std::function<void(bool)> onShowBackgroundWaveformChanged;
  std::function<void(bool)> onShowRainbowWaveformChanged;
  std::function<void(int)> onColormapChanged;
  std::function<bool()> canChangeDevice;

  // Load/save settings
  void loadSettings();
  void saveSettings();

  // Get available execution providers
  static juce::StringArray getAvailableDevices();

private:
  class SettingsLookAndFeel : public DarkLookAndFeel {
  public:
    juce::Font getTextButtonFont(juce::TextButton &, int) override {
      return AppFont::getFont(15.0f);
    }

    juce::Font getLabelFont(juce::Label &) override {
      return AppFont::getFont(15.0f);
    }

    juce::Font getComboBoxFont(juce::ComboBox &) override {
      return AppFont::getFont(15.0f);
    }

    juce::Font getPopupMenuFont() override { return AppFont::getFont(15.0f); }
  };

  enum class SettingsTab { General, Audio };

  void updateDeviceList();
  void updateGPUDeviceList(const juce::String &deviceType);
  void updateAudioDeviceTypes();
  void updateAudioOutputDevices(bool force = false);
  void updateSampleRates();
  void updateBufferSizes();
  void applyAudioSettings();
  void setActiveTab(SettingsTab tab);
  void updateTabButtonStyles();
  void updateTabVisibility();
  bool shouldShowGpuDeviceList() const;
  bool isTabAvailable(SettingsTab tab) const;
  void layoutGeneralTab(juce::Rectangle<int> content);
  void layoutAudioTab(juce::Rectangle<int> content);
  void applyFontSizes();
  void applyTabAnimationState();
  void startTabTransition(SettingsTab fromTab, SettingsTab toTab);

  bool pluginMode = false;
  juce::AudioDeviceManager *deviceManager = nullptr;
  SettingsManager *settingsManager = nullptr;
  SettingsLookAndFeel settingsLookAndFeel;

  juce::Label titleLabel;
  juce::Label generalSectionLabel;
  juce::Label inferenceSectionLabel;
  juce::Label displaySectionLabel;
  juce::Label debugSectionLabel;

  juce::Label languageLabel;
  juce::Label languageDescriptionLabel;
  StyledComboBox languageComboBox;
  juce::Label uiFontSizeLabel;
  juce::Label uiFontSizeDescriptionLabel;
  StyledComboBox uiFontSizeComboBox;

  juce::Label deviceLabel;
  juce::Label deviceDescriptionLabel;
  StyledComboBox deviceComboBox;
  juce::Label gpuDeviceLabel;
  juce::Label gpuDeviceDescriptionLabel;
  StyledComboBox gpuDeviceComboBox;

  juce::Label pitchDetectorLabel;
  juce::Label pitchDetectorDescriptionLabel;
  StyledComboBox pitchDetectorComboBox;
  juce::Label someSegmentsDebugLabel;
  juce::Label someSegmentsDebugDescriptionLabel;
  juce::ToggleButton someSegmentsDebugToggle;
  juce::Label someValuesDebugLabel;
  juce::Label someValuesDebugDescriptionLabel;
  juce::ToggleButton someValuesDebugToggle;
  juce::Label uvInterpolationDebugLabel;
  juce::Label uvInterpolationDebugDescriptionLabel;
  juce::ToggleButton uvInterpolationDebugToggle;
  juce::Label actualF0DebugLabel;
  juce::Label actualF0DebugDescriptionLabel;
  juce::ToggleButton actualF0DebugToggle;
  juce::Label fpsOverlayLabel;
  juce::Label fpsOverlayDescriptionLabel;
  juce::ToggleButton fpsOverlayToggle;
  juce::Label backgroundWaveformLabel;
  juce::Label backgroundWaveformDescriptionLabel;
  juce::ToggleButton backgroundWaveformToggle;
  juce::Label rainbowWaveformLabel;
  juce::Label rainbowWaveformDescriptionLabel;
  juce::ToggleButton rainbowWaveformToggle;
  juce::Label colormapLabel;
  juce::Label colormapDescriptionLabel;
  juce::ComboBox colormapComboBox;

  juce::Label infoLabel;

  // Audio device settings (standalone mode only)
  juce::Label audioSectionLabel;
  juce::Label audioDeviceTypeLabel;
  StyledComboBox audioDeviceTypeComboBox;
  juce::Array<juce::AudioIODeviceType *> audioDeviceTypeOrder;
  juce::Label audioOutputLabel;
  StyledComboBox audioOutputComboBox;
  juce::Label sampleRateLabel;
  StyledComboBox sampleRateComboBox;
  juce::Label bufferSizeLabel;
  StyledComboBox bufferSizeComboBox;
  juce::Label outputChannelsLabel;
  StyledComboBox outputChannelsComboBox;

  juce::StringArray cachedOutputDevices;
  juce::String cachedOutputDeviceName;
  juce::String cachedDeviceTypeName;

  juce::String currentDevice = "CPU";
  bool followSystemAudioOutput = true;
  bool hasLoadedSettings = false;
  int gpuDeviceId = 0;
  juce::String lastConfirmedDevice = "CPU";
  int lastConfirmedGpuDeviceId = 0;
  PitchDetectorType pitchDetectorType = PitchDetectorType::RMVPE;
  PitchDetectorType lastConfirmedPitchDetectorType = PitchDetectorType::RMVPE;
  float uiFontScale = 1.0f;
  bool showSomeSegmentsDebug = false;
  bool showSomeValuesDebug = false;
  bool showUvInterpolationDebug = false;
  bool showActualF0Debug = false;
  bool showFpsOverlay = false;
  bool showBackgroundWaveform = false;
  bool showRainbowWaveform = false;
  int colormapIndex = 0;
  SettingsTab activeTab = SettingsTab::General;
  SettingsTab previousTab = SettingsTab::General;
  juce::TextButton generalTabButton;
  juce::TextButton audioTabButton;
  juce::Rectangle<int> cardBounds;
  juce::Rectangle<int> sidebarBounds;
  juce::Array<int> separatorYs;
  float cornerRadius = 10.0f;
  bool tabAnimationActive = false;
  float tabAnimationProgress = 1.0f;
  double tabAnimationStartTimeMs = 0.0;
  int tabAnimationDurationMs = 160;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsComponent)
};

/**
 * Settings overlay panel (in-window modal).
 */
class SettingsOverlay : public juce::Component, private juce::Timer {
public:
  SettingsOverlay(SettingsManager *settingsManager,
                  juce::AudioDeviceManager *audioDeviceManager = nullptr);
  ~SettingsOverlay() override;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent &e) override;
  bool keyPressed(const juce::KeyPress &key) override;

  void openAnimated();
  void closeAnimated();

  SettingsComponent *getSettingsComponent() { return settingsComponent.get(); }

  std::function<void()> onClose;

private:
  void timerCallback() override;
  void startAnimation(float nextTarget);
  void updateAnimatedState();
  void captureBackgroundSnapshot();
  void invalidateShadowCache();
  void drawCachedShadow(juce::Graphics &g);
  juce::Rectangle<float> getAnimatedContentBounds() const;
  juce::AffineTransform getAnimatedTransform() const;
  void closeIfPossible();

  std::unique_ptr<SettingsComponent> settingsComponent;
  juce::TextButton closeButton{"X"};
  juce::Rectangle<int> contentBounds;
  juce::Image backgroundSnapshot;
  juce::Image shadowCache;
  juce::Rectangle<int> shadowCacheBounds;
  float animationProgress = 0.0f;
  float animationStartProgress = 0.0f;
  float animationTargetProgress = 0.0f;
  double animationStartTimeMs = 0.0;
  int animationDurationMs = 170;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsOverlay)
};

/**
 * Settings dialog window.
 */
class SettingsDialog : public juce::DialogWindow {
public:
  SettingsDialog(SettingsManager *settingsManager,
                 juce::AudioDeviceManager *audioDeviceManager = nullptr);
  ~SettingsDialog() override = default;

  void closeButtonPressed() override;
  void paint(juce::Graphics &g) override;

  SettingsComponent *getSettingsComponent() { return settingsComponent.get(); }

private:
  std::unique_ptr<SettingsComponent> settingsComponent;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsDialog)
};
