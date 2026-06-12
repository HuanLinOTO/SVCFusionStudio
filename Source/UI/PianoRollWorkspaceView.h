#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "HNSepLaneComponent.h"
#include "PianoRollComponent.h"
#include "PianoRoll/OverviewPanel.h"
#include "Workspace/RoundedCard.h"

#include <memory>

class PianoRollWorkspaceView : public juce::Component, private juce::Timer {
public:
  explicit PianoRollWorkspaceView(PianoRollComponent &pianoRoll);

  void paint(juce::Graphics &g) override;
  void resized() override;
  void timerCallback() override;

  void setProject(Project *project);
  void setUndoManager(PitchUndoManager *undoManager);
  void refreshOverview();
  void setShowSomeSegmentsDebug(bool show);
  void setHNSepVisible(bool show);
  bool isHNSepVisible() const { return hnsepVisible; }
  PianoRollComponent &getPianoRoll() { return pianoRoll; }
  HNSepLaneComponent &getHNSepLane() { return hnsepLane; }

private:
  void updateOverviewVisibility();

  PianoRollComponent &pianoRoll;
  OverviewPanel overviewPanel;
  HNSepLaneComponent hnsepLane;
  std::unique_ptr<juce::Component> playheadOverlay;

  RoundedCard pianoCard;
  RoundedCard hnsepCard;
  RoundedCard overviewCard;

  juce::TextButton overviewToggleButton{"[]"};
  bool overviewVisible = true;
  bool hnsepVisible = false;
  double lastOverlayCursorTime = -1.0;
  double lastOverlayScrollX = -1.0;
  float lastOverlayPixelsPerSecond = -1.0f;

  juce::Slider zoomXSlider;
  juce::Slider zoomYSlider;
  juce::Rectangle<float> zoomXBg;
  juce::Rectangle<float> zoomYBg;
  juce::Rectangle<float> toggleBg;

  static constexpr int overviewHeight = 78;
  static constexpr int hnsepHeight = 176;
  static constexpr int cardGap = 8;
  static constexpr int toggleSize = 24;
  static constexpr int toggleMargin = 8;
  static constexpr int zoomSliderWidth = 20;
  static constexpr int zoomSliderHeight = 96;
  static constexpr int zoomSliderLength = 120;
  static constexpr int zoomGap = 8;
  static constexpr int zoomBgPadding = 6;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollWorkspaceView)
};
