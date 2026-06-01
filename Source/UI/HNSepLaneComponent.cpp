#include "HNSepLaneComponent.h"

#include "../Utils/HNSepCurveProcessor.h"
#include "../Utils/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
float computeFrameRmsDb(const juce::AudioBuffer<float> &buffer, int frameIndex) {
  if (buffer.getNumSamples() <= 0)
    return -90.0f;

  const int startSample = std::max(0, frameIndex * HOP_SIZE);
  const int endSample = std::min(startSample + HOP_SIZE, buffer.getNumSamples());
  if (endSample <= startSample)
    return -90.0f;

  const float *samples = buffer.getReadPointer(0);
  double sumSquares = 0.0;
  for (int sampleIndex = startSample; sampleIndex < endSample; ++sampleIndex) {
    const double sample = samples[sampleIndex];
    sumSquares += sample * sample;
  }

  const float rms = static_cast<float>(
      std::sqrt(sumSquares / static_cast<double>(std::max(1, endSample - startSample))));
  return 20.0f * std::log10(std::max(rms, 1.0e-6f));
}

class HNSepCurveEditAction final : public UndoableAction {
public:
  HNSepCurveEditAction(Project *project, std::vector<HNSepLaneComponent::CurveEdit> edits)
      : project(project), edits(std::move(edits)) {}

  void undo() override { apply(/*useNewValues=*/false); }
  void redo() override { apply(/*useNewValues=*/true); }
  juce::String getName() const override { return "Edit HNSep Curve"; }

private:
  static std::vector<float> *curveForLane(AudioData &audioData,
                                          HNSepLaneComponent::LaneType lane) {
    switch (lane) {
    case HNSepLaneComponent::LaneType::Voicing:
      return &audioData.voicingCurve;
    case HNSepLaneComponent::LaneType::Breath:
      return &audioData.breathCurve;
    case HNSepLaneComponent::LaneType::Tension:
      return &audioData.tensionCurve;
    }
    return nullptr;
  }

  void apply(bool useNewValues) {
    if (!project || edits.empty())
      return;

    auto &audioData = project->getAudioData();
    int minFrame = std::numeric_limits<int>::max();
    int maxFrame = std::numeric_limits<int>::min();

    for (const auto &edit : edits) {
      auto *curve = curveForLane(audioData, edit.lane);
      if (!curve)
        continue;
      if (edit.frameIndex < 0 || edit.frameIndex >= static_cast<int>(curve->size()))
        continue;

      (*curve)[static_cast<size_t>(edit.frameIndex)] =
          useNewValues ? edit.newValue : edit.oldValue;
      minFrame = std::min(minFrame, edit.frameIndex);
      maxFrame = std::max(maxFrame, edit.frameIndex + 1);
    }

    if (minFrame <= maxFrame) {
      HNSepCurveProcessor::extractNoteCurvesFromMaster(*project);
      for (auto *note : project->getNotesInRange(minFrame, maxFrame))
        note->markDirty();
      project->setF0DirtyRange(minFrame, maxFrame);
      project->setModified(true);
    }
  }

  Project *project = nullptr;
  std::vector<HNSepLaneComponent::CurveEdit> edits;
};
} // namespace

HNSepLaneComponent::HNSepLaneComponent()
    : lanes{LaneInfo{LaneType::Voicing, TR("hnsep.lane.voicing"), juce::Colour(0xff6ed3cf),
                      0.0f, 200.0f, HNSepCurveProcessor::kDefaultVoicing},
            LaneInfo{LaneType::Breath, TR("hnsep.lane.breath"), juce::Colour(0xfff2b370),
                      0.0f, 200.0f, HNSepCurveProcessor::kDefaultBreath},
            LaneInfo{LaneType::Tension, TR("hnsep.lane.tension"), juce::Colour(0xfff06f5a),
                      -100.0f, 100.0f, HNSepCurveProcessor::kDefaultTension}} {
  setOpaque(true);
  setWantsKeyboardFocus(false);

  auto setupEnergyDropdown = [this](juce::ComboBox &dropdown, float &targetDb,
                                    int selectedId) {
    dropdown.addItem(TR("hnsep.energy.max.-60"), 1);
    dropdown.addItem(TR("hnsep.energy.max.-45"), 2);
    dropdown.addItem(TR("hnsep.energy.max.-30"), 3);
    dropdown.addItem(TR("hnsep.energy.max.-12"), 4);
    dropdown.addItem(TR("hnsep.energy.max.-3"), 5);
    dropdown.setSelectedId(selectedId, juce::dontSendNotification);
    dropdown.setColour(juce::ComboBox::backgroundColourId,
                       APP_COLOR_SURFACE_RAISED.withAlpha(0.9f));
    dropdown.setColour(juce::ComboBox::outlineColourId,
                       APP_COLOR_BORDER_SUBTLE.withAlpha(0.6f));
    dropdown.setColour(juce::ComboBox::textColourId, APP_COLOR_TEXT_PRIMARY);
    dropdown.onChange = [&dropdown, &targetDb, this]() {
      static constexpr float dbValues[] = {-60.0f, -45.0f, -30.0f, -12.0f, -3.0f};
      const int idx = dropdown.getSelectedId() - 1;
      if (idx >= 0 && idx < static_cast<int>(std::size(dbValues))) {
        targetDb = dbValues[idx];
        repaint();
      }
    };
    addAndMakeVisible(dropdown);
  };

  auto setupEnergyToggle = [this](StyledToggleButton &toggle) {
    toggle.setButtonText(TR("hnsep.energy.show"));
    toggle.setToggleState(true, juce::dontSendNotification);
    toggle.onClick = [this]() { repaint(); };
    addAndMakeVisible(toggle);
  };

  setupEnergyDropdown(voicingEnergyDropdown, voicingEnergyMaxDb, 5);
  setupEnergyDropdown(breathEnergyDropdown, breathEnergyMaxDb, 4);
  setupEnergyToggle(voicingEnergyVisibilityToggle);
  setupEnergyToggle(breathEnergyVisibilityToggle);
}

void HNSepLaneComponent::setProject(Project *proj) {
  project = proj;
  repaint();
}

void HNSepLaneComponent::setPixelsPerSecond(float pps) {
  if (std::abs(pixelsPerSecond - pps) < 0.01f)
    return;
  pixelsPerSecond = pps;
  repaint();
}

void HNSepLaneComponent::setScrollX(double x) {
  if (std::abs(scrollX - x) < 0.5)
    return;
  scrollX = x;
  repaint();
}

void HNSepLaneComponent::setPianoKeysWidth(int width) {
  if (pianoKeysWidth == width)
    return;
  pianoKeysWidth = width;
  repaint();
}

void HNSepLaneComponent::paint(juce::Graphics &g) {
  g.fillAll(APP_COLOR_BACKGROUND.brighter(0.04f));

  for (int laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
    drawLane(g, laneIndex);
  }
}

void HNSepLaneComponent::resized() { updateControlBounds(); }

void HNSepLaneComponent::visibilityChanged() { updateControlBounds(); }

void HNSepLaneComponent::mouseDown(const juce::MouseEvent &e) {
  if (!project)
    return;

  const int laneIndex = getLaneIndexAt(e.position);
  if (laneIndex < 0)
    return;
  if (!e.mods.isLeftButtonDown() && !e.mods.isRightButtonDown())
    return;

  activeLaneIndex = laneIndex;
  isDrawing = false;
  isResetting = false;
  isGesturePending = true;
  pendingGestureResetting = e.mods.isRightButtonDown();
  pendingEdits.clear();
  pendingEditIndexByFrame.clear();
  lastDrawFrame = -1;
  lastDrawValue = lanes[static_cast<size_t>(laneIndex)].defaultValue;
  pendingGestureStart = e.position;
}

void HNSepLaneComponent::mouseDrag(const juce::MouseEvent &e) {
  if (isGesturePending) {
    isGesturePending = false;
    isDrawing = !pendingGestureResetting;
    isResetting = pendingGestureResetting;
    applyGesturePoint(pendingGestureStart.x, pendingGestureStart.y);
  }

  if ((!isDrawing && !isResetting) || activeLaneIndex < 0)
    return;
  applyGesturePoint(e.position.x, e.position.y);
}

void HNSepLaneComponent::mouseUp(const juce::MouseEvent &) {
  if (isGesturePending) {
    isGesturePending = false;
    pendingGestureResetting = false;
    isDrawing = false;
    isResetting = false;
    activeLaneIndex = -1;
    lastDrawFrame = -1;
    lastDrawValue = 0.0f;
    pendingEdits.clear();
    pendingEditIndexByFrame.clear();
    return;
  }

  if (!isDrawing && !isResetting)
    return;

  isDrawing = false;
  isResetting = false;
  pendingGestureResetting = false;
  activeLaneIndex = -1;
  lastDrawFrame = -1;
  commitPendingEdits();
}

void HNSepLaneComponent::mouseWheelMove(const juce::MouseEvent &e,
                                        const juce::MouseWheelDetails &wheel) {
  if (mouseWheelPassthroughTarget) {
    mouseWheelPassthroughTarget->mouseWheelMove(
        e.getEventRelativeTo(mouseWheelPassthroughTarget), wheel);
    return;
  }
  juce::Component::mouseWheelMove(e, wheel);
}

juce::Rectangle<int> HNSepLaneComponent::getLaneBounds(int laneIndex) const {
  auto bounds = getLocalBounds();
  const int laneHeight = bounds.getHeight() / static_cast<int>(lanes.size());
  const int y = bounds.getY() + laneHeight * laneIndex;
  auto laneBounds = juce::Rectangle<int>(bounds.getX(), y, bounds.getWidth(),
                                         laneHeight);
  if (laneIndex == static_cast<int>(lanes.size()) - 1) {
    laneBounds.setHeight(bounds.getBottom() - laneBounds.getY());
  }
  return laneBounds;
}

int HNSepLaneComponent::getLaneIndexAt(juce::Point<float> position) const {
  if (position.x < static_cast<float>(pianoKeysWidth))
    return -1;
  for (int laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
    if (getLaneBounds(laneIndex).toFloat().contains(position))
      return laneIndex;
  }
  return -1;
}

int HNSepLaneComponent::xToFrame(float x) const {
  const float timeSeconds =
      (x - static_cast<float>(pianoKeysWidth) + static_cast<float>(scrollX)) /
      std::max(1.0f, pixelsPerSecond);
  return std::max(0, secondsToFrames(timeSeconds));
}

float HNSepLaneComponent::frameToX(int frame) const {
  return static_cast<float>(pianoKeysWidth) +
         framesToSeconds(frame) * pixelsPerSecond - static_cast<float>(scrollX);
}

float HNSepLaneComponent::valueToY(float value, const LaneInfo &lane,
                                   const juce::Rectangle<int> &bounds) const {
  const float normalized = juce::jlimit(
      0.0f, 1.0f,
      (value - lane.minValue) / std::max(1.0f, lane.maxValue - lane.minValue));
  return bounds.getBottom() - normalized * static_cast<float>(bounds.getHeight());
}

float HNSepLaneComponent::yToValue(float y, const LaneInfo &lane,
                                    const juce::Rectangle<int> &bounds) const {
  const float normalized = juce::jlimit(
      0.0f, 1.0f,
      (static_cast<float>(bounds.getBottom()) - y) /
          std::max(1.0f, static_cast<float>(bounds.getHeight())));
  return lane.minValue + normalized * (lane.maxValue - lane.minValue);
}

void HNSepLaneComponent::updateControlBounds() {
  const int margin = 6;
  for (int laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
    auto bounds = getLaneBounds(laneIndex).withTrimmedLeft(pianoKeysWidth);
    const auto lane = lanes[static_cast<size_t>(laneIndex)].type;
    juce::ComboBox *dropdown = nullptr;
    StyledToggleButton *toggle = nullptr;
    if (lane == LaneType::Voicing) {
      dropdown = &voicingEnergyDropdown;
      toggle = &voicingEnergyVisibilityToggle;
    } else if (lane == LaneType::Breath) {
      dropdown = &breathEnergyDropdown;
      toggle = &breathEnergyVisibilityToggle;
    }

    if (!dropdown || !toggle)
      continue;

    dropdown->setBounds(bounds.getX() + margin, bounds.getY() + margin,
                        energyDropdownWidth, energyControlHeight);
    toggle->setBounds(dropdown->getRight() + 4, bounds.getY() + margin,
                      energyToggleWidth, energyControlHeight);
  }
}

float HNSepLaneComponent::getEnergyMaxDb(LaneType lane) const {
  if (lane == LaneType::Voicing)
    return voicingEnergyMaxDb;
  if (lane == LaneType::Breath)
    return breathEnergyMaxDb;
  return -3.0f;
}

bool HNSepLaneComponent::isEnergyOverlayVisible(LaneType lane) const {
  if (lane == LaneType::Voicing)
    return voicingEnergyVisibilityToggle.getToggleState();
  if (lane == LaneType::Breath)
    return breathEnergyVisibilityToggle.getToggleState();
  return false;
}

std::vector<float> *HNSepLaneComponent::curveForLane(AudioData &audioData,
                                                     LaneType lane) {
  switch (lane) {
  case LaneType::Voicing:
    return &audioData.voicingCurve;
  case LaneType::Breath:
    return &audioData.breathCurve;
  case LaneType::Tension:
    return &audioData.tensionCurve;
  }
  return nullptr;
}

const std::vector<float> *
HNSepLaneComponent::curveForLane(const AudioData &audioData, LaneType lane) const {
  switch (lane) {
  case LaneType::Voicing:
    return &audioData.voicingCurve;
  case LaneType::Breath:
    return &audioData.breathCurve;
  case LaneType::Tension:
    return &audioData.tensionCurve;
  }
  return nullptr;
}

void HNSepLaneComponent::drawLane(juce::Graphics &g, int laneIndex) const {
  auto laneBounds = getLaneBounds(laneIndex);
  auto labelBounds = laneBounds.removeFromLeft(pianoKeysWidth);
  const auto contentBounds = laneBounds;
  const auto &lane = lanes[static_cast<size_t>(laneIndex)];

  g.setColour(APP_COLOR_SURFACE.withAlpha(0.92f));
  g.fillRect(getLaneBounds(laneIndex));
  g.setColour(APP_COLOR_SURFACE_RAISED.withAlpha(0.95f));
  g.fillRect(labelBounds);

  g.setColour(APP_COLOR_TEXT_PRIMARY);
  g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
  g.drawText(lane.label, labelBounds.getX() + 12, labelBounds.getY() + 8,
             labelWidth, labelBounds.getHeight() - 16,
             juce::Justification::topLeft);

  g.setColour(APP_COLOR_TEXT_MUTED);
  g.setFont(juce::Font(juce::FontOptions(11.0f)));
  g.drawText(juce::String(lane.minValue, 0), labelBounds.getX() + 12,
             labelBounds.getBottom() - 18, labelWidth, 14,
             juce::Justification::bottomLeft);
  g.drawText(juce::String(lane.maxValue, 0), labelBounds.getX() + 12,
             labelBounds.getY() + 8,
             labelWidth, 14, juce::Justification::topRight);

  g.setColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.55f));
  g.drawLine(static_cast<float>(contentBounds.getX()),
             static_cast<float>(contentBounds.getY()),
             static_cast<float>(contentBounds.getRight()),
             static_cast<float>(contentBounds.getY()));
  g.drawLine(static_cast<float>(contentBounds.getX()),
             static_cast<float>(contentBounds.getBottom()),
             static_cast<float>(contentBounds.getRight()),
             static_cast<float>(contentBounds.getBottom()));

  drawNoteOverlay(g, contentBounds);
  drawEnergyOverlay(g, contentBounds, lane.type);

  if (project) {
    const auto *curve = curveForLane(project->getAudioData(), lane.type);
    if (curve && !curve->empty())
      drawCurve(g, contentBounds.reduced(lanePadding, lanePadding), lane,
                *curve);
  }

  if (laneIndex > 0) {
    g.setColour(APP_COLOR_BORDER.withAlpha(0.6f));
    g.fillRect(0, getLaneBounds(laneIndex).getY(), getWidth(),
               separatorThickness);
  }
}

void HNSepLaneComponent::drawNoteOverlay(
    juce::Graphics &g, const juce::Rectangle<int> &bounds) const {
  if (!project)
    return;

  for (const auto &note : project->getNotes()) {
    if (note.isRest())
      continue;

    const float x1 = frameToX(note.getStartFrame());
    const float x2 = frameToX(note.getEndFrame());
    if (x2 <= static_cast<float>(bounds.getX()) ||
        x1 >= static_cast<float>(bounds.getRight()))
      continue;

    auto noteRect = juce::Rectangle<float>(
        std::max(x1, static_cast<float>(bounds.getX())),
        static_cast<float>(bounds.getY()),
        std::min(x2, static_cast<float>(bounds.getRight())) -
            std::max(x1, static_cast<float>(bounds.getX())),
        static_cast<float>(bounds.getHeight()));

    g.setColour(note.isSelected() ? APP_COLOR_PRIMARY.withAlpha(0.10f)
                                  : APP_COLOR_TEXT_MUTED.withAlpha(0.05f));
    g.fillRect(noteRect);
    g.setColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.15f));
    g.drawVerticalLine(static_cast<int>(noteRect.getX()), noteRect.getY(),
                       noteRect.getBottom());
  }
}

void HNSepLaneComponent::drawEnergyOverlay(juce::Graphics &g,
                                           const juce::Rectangle<int> &bounds,
                                           LaneType lane) const {
  if (!project)
    return;
  if (!isEnergyOverlayVisible(lane))
    return;

  const juce::AudioBuffer<float> *buffer = nullptr;
  juce::Colour colour = APP_COLOR_TEXT_MUTED.withAlpha(0.16f);
  if (lane == LaneType::Voicing) {
    buffer = &project->getAudioData().harmonicWaveform;
    colour = lanes[0].colour.withAlpha(0.18f);
  } else if (lane == LaneType::Breath) {
    buffer = &project->getAudioData().noiseWaveform;
    colour = lanes[1].colour.withAlpha(0.18f);
  }

  if (!buffer || buffer->getNumSamples() <= 0)
    return;

  juce::Path path;
  bool started = false;
  const int startFrame = std::max(0, xToFrame(static_cast<float>(bounds.getX())));
  const int endFrame = std::max(startFrame + 1, xToFrame(static_cast<float>(bounds.getRight())) + 1);
  const float pixelsPerFrame = framesToSeconds(1) * pixelsPerSecond;
  const int frameStep = juce::jmax(
      1, static_cast<int>(std::ceil(1.0f / juce::jmax(0.001f, pixelsPerFrame))));

  for (int frame = startFrame; frame < endFrame; frame += frameStep) {
    const float x = frameToX(frame);
    const float db = computeFrameRmsDb(*buffer, frame);
    const float maxDb = getEnergyMaxDb(lane);
    const float normalized = juce::jlimit(
        0.0f, 1.0f,
        (db - energyMinDb) / std::max(0.001f, maxDb - energyMinDb));
    const float y = bounds.getBottom() - normalized * bounds.getHeight();
    if (!started) {
      path.startNewSubPath(x, y);
      started = true;
    } else {
      path.lineTo(x, y);
    }
  }

  if (!started)
    return;

  g.setColour(colour);
  g.strokePath(path, juce::PathStrokeType(1.0f));
}

void HNSepLaneComponent::drawCurve(juce::Graphics &g,
                                   const juce::Rectangle<int> &bounds,
                                   const LaneInfo &lane,
                                   const std::vector<float> &curve) const {
  if (curve.empty())
    return;

  const int startFrame = std::max(0, xToFrame(static_cast<float>(bounds.getX())));
  const int endFrame = std::min(static_cast<int>(curve.size()),
                                std::max(startFrame + 1,
                                         xToFrame(static_cast<float>(bounds.getRight())) + 1));
  if (endFrame <= startFrame)
    return;
  const float pixelsPerFrame = framesToSeconds(1) * pixelsPerSecond;
  const int frameStep = juce::jmax(
      1, static_cast<int>(std::ceil(1.0f / juce::jmax(0.001f, pixelsPerFrame))));

  juce::Path path;
  path.startNewSubPath(frameToX(startFrame),
                       valueToY(curve[static_cast<size_t>(startFrame)], lane,
                                 bounds));
  for (int frame = startFrame + frameStep; frame < endFrame; frame += frameStep) {
    path.lineTo(frameToX(frame),
                valueToY(curve[static_cast<size_t>(frame)], lane, bounds));
  }

  if ((endFrame - 1 - startFrame) % frameStep != 0) {
    path.lineTo(frameToX(endFrame - 1),
                valueToY(curve[static_cast<size_t>(endFrame - 1)], lane, bounds));
  }

  g.setColour(lane.colour.withAlpha(0.16f));
  juce::Path fillPath(path);
  fillPath.lineTo(frameToX(endFrame - 1), static_cast<float>(bounds.getBottom()));
  fillPath.lineTo(frameToX(startFrame), static_cast<float>(bounds.getBottom()));
  fillPath.closeSubPath();
  g.fillPath(fillPath);

  g.setColour(lane.colour);
  g.strokePath(path, juce::PathStrokeType(2.0f));

  const float defaultY = valueToY(lane.defaultValue, lane, bounds);
  g.setColour(APP_COLOR_TEXT_MUTED.withAlpha(0.25f));
  g.drawHorizontalLine(static_cast<int>(defaultY), static_cast<float>(bounds.getX()),
                       static_cast<float>(bounds.getRight()));
}

void HNSepLaneComponent::applyGesturePoint(float localX, float localY) {
  if (!project || activeLaneIndex < 0)
    return;

  const auto &lane = lanes[static_cast<size_t>(activeLaneIndex)];
  const auto laneBounds =
      getLaneBounds(activeLaneIndex).withTrimmedLeft(pianoKeysWidth).reduced(
          lanePadding, lanePadding);
  const int frameIndex = xToFrame(localX);
  const float value = isResetting ? lane.defaultValue
                                  : yToValue(localY, lane, laneBounds);

  if (lastDrawFrame < 0) {
    applyValueAtFrame(frameIndex, value);
    lastDrawFrame = frameIndex;
    lastDrawValue = value;
    return;
  }

  const int startFrame = std::min(lastDrawFrame, frameIndex);
  const int endFrame = std::max(lastDrawFrame, frameIndex);
  for (int frame = startFrame; frame <= endFrame; ++frame) {
    const float alpha = endFrame == startFrame
                            ? 1.0f
                            : static_cast<float>(frame - startFrame) /
                                  static_cast<float>(endFrame - startFrame);
    const float interpolated = lastDrawValue + (value - lastDrawValue) * alpha;
    applyValueAtFrame(frame, interpolated);
  }

  lastDrawFrame = frameIndex;
  lastDrawValue = value;
}

void HNSepLaneComponent::applyValueAtFrame(int frameIndex, float value) {
  if (!project || activeLaneIndex < 0)
    return;

  auto &audioData = project->getAudioData();
  auto *curve = curveForLane(audioData, lanes[static_cast<size_t>(activeLaneIndex)].type);
  if (!curve || frameIndex < 0 || frameIndex >= static_cast<int>(curve->size()))
    return;
  if (!project->getNoteAtFrame(frameIndex))
    return;

  const auto &lane = lanes[static_cast<size_t>(activeLaneIndex)];
  const float clampedValue = juce::jlimit(lane.minValue, lane.maxValue, value);
  auto existing = pendingEditIndexByFrame.find(frameIndex);
  if (existing == pendingEditIndexByFrame.end()) {
    pendingEdits.push_back(CurveEdit{lane.type, frameIndex,
                                     (*curve)[static_cast<size_t>(frameIndex)],
                                     clampedValue});
    pendingEditIndexByFrame.emplace(frameIndex, pendingEdits.size() - 1);
  } else {
    pendingEdits[existing->second].newValue = clampedValue;
  }

  (*curve)[static_cast<size_t>(frameIndex)] = clampedValue;
  HNSepCurveProcessor::extractNoteCurvesFromMaster(*project);

  if (onParamEdited)
    onParamEdited();
  repaint();
}

void HNSepLaneComponent::commitPendingEdits() {
  if (!project || pendingEdits.empty()) {
    pendingEdits.clear();
    pendingEditIndexByFrame.clear();
    return;
  }

  int minFrame = std::numeric_limits<int>::max();
  int maxFrame = std::numeric_limits<int>::min();
  for (const auto &edit : pendingEdits) {
    minFrame = std::min(minFrame, edit.frameIndex);
    maxFrame = std::max(maxFrame, edit.frameIndex + 1);
  }

  if (undoManager) {
    undoManager->addAction(
        std::make_unique<HNSepCurveEditAction>(project, pendingEdits));
  }

  markDirtyRange(minFrame, maxFrame);
  project->setModified(true);

  pendingEdits.clear();
  pendingEditIndexByFrame.clear();

  if (onParamEditFinished)
    onParamEditFinished();
  repaint();
}

void HNSepLaneComponent::markDirtyRange(int startFrame, int endFrame) const {
  if (!project || startFrame < 0 || endFrame <= startFrame)
    return;

  for (auto *note : project->getNotesInRange(startFrame, endFrame))
    note->markDirty();
  project->setF0DirtyRange(startFrame, endFrame);
}
