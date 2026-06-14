#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../UI/StyledComponents.h"
#include "../Utils/Constants.h"
#include "../Utils/Localization.h"
#include "../Utils/UndoManager.h"

#include <array>
#include <map>

class HNSepLaneComponent : public juce::Component {
public:
  enum class LaneType { Voicing, Breath, Tension, Shfc };

  struct CurveEdit {
    LaneType lane;
    int frameIndex = -1;
    float oldValue = 0.0f;
    float newValue = 0.0f;
  };

  HNSepLaneComponent();
  ~HNSepLaneComponent() override = default;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override;
  void visibilityChanged() override;

  void setProject(Project *proj);
  Project *getProject() const { return project; }

  void setUndoManager(PitchUndoManager *manager) { undoManager = manager; }
  void setPixelsPerSecond(float pps);
  float getPixelsPerSecond() const { return pixelsPerSecond; }
  void setScrollX(double x);
  double getScrollX() const { return scrollX; }
  void setViewTransform(float pps, double x);
  void setPianoKeysWidth(int width);
  void setShfcEnabled(bool enabled);
  void setMouseWheelPassthroughTarget(juce::Component *target) {
    mouseWheelPassthroughTarget = target;
  }
  LaneType getSelectedLane() const { return selectedLane; }
  void setSelectedLane(LaneType lane);

  std::function<void()> onParamEdited;
  std::function<void()> onParamEditFinished;

private:
  struct LaneInfo {
    LaneType type;
    juce::String label;
    juce::Colour colour;
    float minValue;
    float maxValue;
    float defaultValue;
  };

  juce::Rectangle<int> getLaneBounds(int laneIndex) const;
  int getLaneIndexAt(juce::Point<float> position) const;
  int getLaneIndexForType(LaneType lane) const;
  int getSelectedLaneIndex() const;
  int xToFrame(float x) const;
  float frameToX(int frame) const;
  float valueToY(float value, const LaneInfo &lane,
                 const juce::Rectangle<int> &bounds) const;
  float yToValue(float y, const LaneInfo &lane,
                 const juce::Rectangle<int> &bounds) const;

  std::vector<float> *curveForLane(AudioData &audioData, LaneType lane);
  const std::vector<float> *curveForLane(const AudioData &audioData,
                                         LaneType lane) const;

  void drawLane(juce::Graphics &g, int laneIndex) const;
  void drawNoteOverlay(juce::Graphics &g,
                       const juce::Rectangle<int> &bounds) const;
  void drawEnergyOverlay(juce::Graphics &g, const juce::Rectangle<int> &bounds,
                          LaneType lane) const;
  void drawCurve(juce::Graphics &g, const juce::Rectangle<int> &bounds,
                  const LaneInfo &lane, const std::vector<float> &curve) const;
  void updateControlBounds();
  void refreshShfcAvailability();
  bool isLaneAvailable(LaneType lane) const;
  bool isFrameInAudibleNote(int frame) const;
  float getEnergyMaxDb(LaneType lane) const;
  bool isEnergyOverlayVisible(LaneType lane) const;

  void applyGesturePoint(float localX, float localY);
  void applyValueAtFrame(int frameIndex, float value);
  void commitPendingEdits();
  void markDirtyRange(int startFrame, int endFrame) const;

  Project *project = nullptr;
  PitchUndoManager *undoManager = nullptr;
  juce::Component *mouseWheelPassthroughTarget = nullptr;

  std::array<LaneInfo, 4> lanes;
  LaneType selectedLane = LaneType::Voicing;
  float pixelsPerSecond = DEFAULT_PIXELS_PER_SECOND;
  double scrollX = 0.0;
  int pianoKeysWidth = 60;
  float voicingEnergyMaxDb = -3.0f;
  float breathEnergyMaxDb = -12.0f;

  StyledComboBox parameterDropdown;
  juce::ComboBox voicingEnergyDropdown;
  juce::ComboBox breathEnergyDropdown;
  StyledToggleButton voicingEnergyVisibilityToggle;
  StyledToggleButton breathEnergyVisibilityToggle;

  bool isDrawing = false;
  bool isResetting = false;
  bool isGesturePending = false;
  bool pendingGestureResetting = false;
  bool shfcEnabled = false;
  int activeLaneIndex = -1;
  int lastDrawFrame = -1;
  float lastDrawValue = 0.0f;
  juce::Point<float> pendingGestureStart;
  std::vector<CurveEdit> pendingEdits;
  std::map<int, size_t> pendingEditIndexByFrame;

  static constexpr int lanePadding = 8;
  static constexpr int toolbarHeight = 36;
  static constexpr int parameterDropdownWidth = 132;
  static constexpr int labelWidth = 64;
  static constexpr int energyDropdownWidth = 76;
  static constexpr int energyToggleWidth = 72;
  static constexpr int energyControlHeight = 20;
  static constexpr float energyMinDb = -90.0f;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HNSepLaneComponent)
};
