#include "PianoRollWorkspaceView.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Constants.h"
#include <algorithm>
#include <cmath>

namespace {
bool isMenuPopupActive() {
  if (auto *modal = juce::Component::getCurrentlyModalComponent())
    return modal->getName() == "menu";

  return false;
}

class PianoRollPlayheadOverlay final : public juce::Component {
public:
  explicit PianoRollPlayheadOverlay(PianoRollComponent &piano) : pianoRoll(piano) {
    setInterceptsMouseClicks(false, false);
    setOpaque(false);
  }

  void paint(juce::Graphics &g) override {
    constexpr int scrollBarSize = 8;
    constexpr float cornerRadius = 10.0f;

    juce::Path clipPath;
    clipPath.addRoundedRectangle(getLocalBounds().toFloat(), cornerRadius);
    g.reduceClipRegion(clipPath);

    const int pianoKeysWidth = pianoRoll.getPianoKeysWidth();
    const float x = static_cast<float>(pianoKeysWidth) +
                    static_cast<float>(pianoRoll.getCursorTime() *
                                       pianoRoll.getPixelsPerSecond()) -
                    static_cast<float>(pianoRoll.getScrollX());
    const float cursorBottom = static_cast<float>(getHeight() - scrollBarSize);

    if (x < static_cast<float>(pianoKeysWidth) ||
        x >= static_cast<float>(getWidth() - scrollBarSize))
      return;

    g.setColour(APP_COLOR_PRIMARY);
    g.fillRect(x - 0.5f, 0.0f, 1.0f, cursorBottom);
  }

private:
  PianoRollComponent &pianoRoll;
};
}

PianoRollWorkspaceView::PianoRollWorkspaceView(PianoRollComponent &piano)
    : pianoRoll(piano)
{
  pianoCard.setPadding(0);
  pianoCard.setCornerRadius(10.0f);
  pianoCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
  pianoCard.setContentComponent(&pianoRoll);

  overviewCard.setPadding(0);
  overviewCard.setCornerRadius(10.0f);
  overviewCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
  overviewCard.setContentComponent(&overviewPanel);
  overviewPanel.setDrawBackground(false);

  hnsepCard.setPadding(0);
  hnsepCard.setCornerRadius(10.0f);
  hnsepCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
  hnsepCard.setContentComponent(&hnsepLane);
  hnsepLane.setPianoKeysWidth(pianoRoll.getPianoKeysWidth());
  hnsepLane.setMouseWheelPassthroughTarget(&pianoRoll);

  overviewPanel.getViewState = [this]()
  {
    OverviewPanel::ViewState state;
    auto *project = pianoRoll.getProject();
    state.totalTime = project ? project->getAudioData().getDuration() : 0.0;
    state.scrollX = pianoRoll.getScrollX();
    state.pixelsPerSecond = pianoRoll.getPixelsPerSecond();
    state.visibleWidth = pianoRoll.getVisibleContentWidth();
    return state;
  };
  overviewPanel.onScrollXChanged = [this](double x)
  {
    pianoRoll.setScrollX(x);
    if (pianoRoll.onScrollChanged)
      pianoRoll.onScrollChanged(x);
  };
  overviewPanel.onZoomChanged = [this](float pps)
  {
    pianoRoll.setPixelsPerSecond(pps, false);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };

  zoomXSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  zoomXSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomXSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 0.1);
  zoomXSlider.setValue(pianoRoll.getPixelsPerSecond(),
                       juce::dontSendNotification);
  zoomXSlider.setColour(juce::Slider::trackColourId,
                        APP_COLOR_SURFACE_RAISED);
  zoomXSlider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);
  zoomXSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSecond(static_cast<float>(zoomXSlider.getValue()),
                                 false);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };
  pianoRoll.onCursorMoved = [this]()
  {
    if (playheadOverlay != nullptr && hnsepVisible)
      playheadOverlay->repaint();
  };

  zoomYSlider.setSliderStyle(juce::Slider::LinearVertical);
  zoomYSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomYSlider.setRange(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, 0.1);
  zoomYSlider.setValue(pianoRoll.getPixelsPerSemitone(),
                       juce::dontSendNotification);
  zoomYSlider.setColour(juce::Slider::trackColourId,
                        APP_COLOR_SURFACE_RAISED);
  zoomYSlider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);
  zoomYSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSemitone(static_cast<float>(zoomYSlider.getValue()),
                                   static_cast<float>(pianoRoll.getVisibleContentHeight()) *
                                       0.5f);
  };

  overviewToggleButton.setClickingTogglesState(true);
  overviewToggleButton.setToggleState(overviewVisible,
                                      juce::dontSendNotification);
  overviewToggleButton.setColour(juce::TextButton::buttonColourId,
                                 APP_COLOR_SURFACE.withAlpha(0.9f));
  overviewToggleButton.setColour(juce::TextButton::buttonOnColourId,
                                 APP_COLOR_PRIMARY.withAlpha(0.9f));
  overviewToggleButton.setColour(juce::TextButton::textColourOffId,
                                 APP_COLOR_TEXT_PRIMARY);
  overviewToggleButton.setColour(juce::TextButton::textColourOnId,
                                 juce::Colours::white);
  overviewToggleButton.onClick = [this]()
  {
    overviewVisible = overviewToggleButton.getToggleState();
    updateOverviewVisibility();
    resized();
  };

  addAndMakeVisible(pianoCard);
  addChildComponent(hnsepCard);
  playheadOverlay = std::make_unique<PianoRollPlayheadOverlay>(pianoRoll);
  addChildComponent(*playheadOverlay);
  playheadOverlay->setVisible(hnsepVisible);
  addAndMakeVisible(overviewCard);
  addAndMakeVisible(overviewToggleButton);
  addAndMakeVisible(zoomXSlider);
  addAndMakeVisible(zoomYSlider);

  updateOverviewVisibility();
  startTimerHz(10);
}

void PianoRollWorkspaceView::paint(juce::Graphics &g)
{
  const auto bg = APP_COLOR_SURFACE.withAlpha(0.85f);
  const auto border = APP_COLOR_BORDER_SUBTLE.withAlpha(0.7f);

  g.setColour(bg);
  g.fillRoundedRectangle(zoomXBg, 6.0f);
  g.fillRoundedRectangle(zoomYBg, 6.0f);
  g.fillRoundedRectangle(toggleBg, 6.0f);

  g.setColour(border);
  g.drawRoundedRectangle(zoomXBg, 6.0f, 1.0f);
  g.drawRoundedRectangle(zoomYBg, 6.0f, 1.0f);
  g.drawRoundedRectangle(toggleBg, 6.0f, 1.0f);

  if (hnsepVisible && !hnsepResizeBounds.isEmpty()) {
    const float handleWidth = std::min(
        72.0f,
        std::max(24.0f,
                 static_cast<float>(hnsepResizeBounds.getWidth()) - 24.0f));
    const float handleX = static_cast<float>(hnsepResizeBounds.getCentreX()) -
                          handleWidth * 0.5f;
    const float handleY = static_cast<float>(hnsepResizeBounds.getCentreY()) - 1.0f;
    g.setColour(APP_COLOR_BORDER.withAlpha(isResizingHNSep ? 0.9f : 0.55f));
    g.fillRoundedRectangle(handleX, handleY, handleWidth, 2.0f, 1.0f);
  }
}

void PianoRollWorkspaceView::resized()
{
  auto bounds = getLocalBounds();

  if (overviewVisible)
  {
    auto overviewBounds = bounds.removeFromBottom(overviewHeight);
    bounds.removeFromBottom(cardGap);
    overviewCard.setBounds(overviewBounds);
  }
  else
  {
    overviewCard.setBounds({});
  }

  if (hnsepVisible)
  {
    hnsepHeight = juce::jlimit(hnsepMinHeight, getMaxHNSepHeight(), hnsepHeight);
    auto hnsepBounds = bounds.removeFromBottom(hnsepHeight);
    hnsepResizeBounds = bounds.removeFromBottom(cardGap);
    hnsepCard.setBounds(hnsepBounds);
  }
  else
  {
    hnsepResizeBounds = {};
    hnsepCard.setBounds({});
  }

  pianoCard.setBounds(bounds);

  if (playheadOverlay != nullptr)
    playheadOverlay->setBounds(pianoCard.getBounds());

  auto overlay = pianoCard.getBounds();
  const int sliderBottom = overlay.getBottom() - toggleMargin;
  const int sliderRight = overlay.getRight() - toggleMargin;
  const int zoomXHeight = 20;
  const int zoomXTop = sliderBottom - zoomXHeight;
  const int zoomYBottom = zoomXTop - zoomGap;
  const int zoomCornerGap = 6;

  auto zoomXRect = juce::Rectangle<int>(
      sliderRight - zoomSliderLength - toggleSize - zoomCornerGap, zoomXTop,
      zoomSliderLength, zoomXHeight);
  auto zoomYRect = juce::Rectangle<int>(
      sliderRight - zoomSliderWidth, zoomYBottom - zoomSliderHeight,
      zoomSliderWidth, zoomSliderHeight);

  zoomXSlider.setBounds(zoomXRect);
  zoomYSlider.setBounds(zoomYRect);

  overviewToggleButton.setBounds(
      zoomXRect.getRight() + zoomCornerGap,
      zoomYRect.getBottom() - toggleSize + 22, toggleSize, toggleSize);

  zoomXBg = zoomXRect.toFloat().expanded(static_cast<float>(zoomBgPadding),
                                         static_cast<float>(zoomBgPadding));
  zoomYBg = zoomYRect.toFloat().expanded(static_cast<float>(zoomBgPadding),
                                         static_cast<float>(zoomBgPadding));
  toggleBg = overviewToggleButton.getBounds().toFloat().expanded(
      static_cast<float>(zoomBgPadding), static_cast<float>(zoomBgPadding));
}

void PianoRollWorkspaceView::mouseDown(const juce::MouseEvent &e)
{
  if (!hnsepVisible || !hnsepResizeBounds.contains(e.getPosition()))
    return;

  isResizingHNSep = true;
  resizeStartY = e.y;
  resizeStartHeight = hnsepHeight;
  setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
  repaint(hnsepResizeBounds);
}

void PianoRollWorkspaceView::mouseDrag(const juce::MouseEvent &e)
{
  if (!isResizingHNSep)
    return;

  const int nextHeight = juce::jlimit(hnsepMinHeight, getMaxHNSepHeight(),
                                      resizeStartHeight + resizeStartY - e.y);
  if (hnsepHeight == nextHeight)
    return;

  hnsepHeight = nextHeight;
  resized();
  repaint();
}

void PianoRollWorkspaceView::mouseUp(const juce::MouseEvent &)
{
  if (!isResizingHNSep)
    return;

  isResizingHNSep = false;
  setMouseCursor(juce::MouseCursor::NormalCursor);
  repaint(hnsepResizeBounds);
}

void PianoRollWorkspaceView::mouseMove(const juce::MouseEvent &e)
{
  setMouseCursor(hnsepVisible && hnsepResizeBounds.contains(e.getPosition())
                     ? juce::MouseCursor::UpDownResizeCursor
                     : juce::MouseCursor::NormalCursor);
}

void PianoRollWorkspaceView::mouseExit(const juce::MouseEvent &)
{
  if (!isResizingHNSep)
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void PianoRollWorkspaceView::setProject(Project *project)
{
  overviewPanel.setProject(project);
  hnsepLane.setProject(project);
}

void PianoRollWorkspaceView::setUndoManager(PitchUndoManager *undoManager)
{
  hnsepLane.setUndoManager(undoManager);
}

void PianoRollWorkspaceView::refreshOverview()
{
  if (overviewVisible)
    overviewPanel.repaint();
}

void PianoRollWorkspaceView::setShowSomeSegmentsDebug(bool show)
{
  overviewPanel.setShowSomeSegmentsDebug(show);
}

void PianoRollWorkspaceView::setHNSepVisible(bool show)
{
  if (hnsepVisible == show)
    return;

  hnsepVisible = show;
  lastOverlayCursorTime = -1.0;
  lastOverlayScrollX = -1.0;
  lastOverlayPixelsPerSecond = -1.0f;
  hnsepCard.setVisible(show);
  hnsepLane.setVisible(show);
  if (playheadOverlay != nullptr)
    playheadOverlay->setVisible(show);
  resized();
  repaint();
}

void PianoRollWorkspaceView::updateOverviewVisibility()
{
  overviewCard.setVisible(overviewVisible);
  overviewPanel.setVisible(overviewVisible);
}

int PianoRollWorkspaceView::getMaxHNSepHeight() const
{
  const int overviewReserve = overviewVisible ? overviewHeight + cardGap : 0;
  const int available = getHeight() - overviewReserve - cardGap - pianoMinHeight;
  return juce::jlimit(hnsepMinHeight, hnsepMaxHeight,
                      std::max(hnsepMinHeight, available));
}

void PianoRollWorkspaceView::timerCallback()
{
  if (isMenuPopupActive())
    return;

  const float pps = pianoRoll.getPixelsPerSecond();
  if (std::abs(zoomXSlider.getValue() - pps) > 0.05)
    zoomXSlider.setValue(pps, juce::dontSendNotification);
  hnsepLane.setViewTransform(pps, pianoRoll.getScrollX());
  if (playheadOverlay != nullptr && hnsepVisible) {
    const double cursorTime = pianoRoll.getCursorTime();
    const double scrollX = pianoRoll.getScrollX();
    if (std::abs(lastOverlayCursorTime - cursorTime) > 0.0001 ||
        std::abs(lastOverlayScrollX - scrollX) > 0.5 ||
        std::abs(lastOverlayPixelsPerSecond - pps) > 0.05f) {
      lastOverlayCursorTime = cursorTime;
      lastOverlayScrollX = scrollX;
      lastOverlayPixelsPerSecond = pps;
      playheadOverlay->repaint();
    }
  }

  const float ppsY = pianoRoll.getPixelsPerSemitone();
  if (std::abs(zoomYSlider.getValue() - ppsY) > 0.05)
    zoomYSlider.setValue(ppsY, juce::dontSendNotification);
}
