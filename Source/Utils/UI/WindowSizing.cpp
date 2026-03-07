#include "WindowSizing.h"

namespace WindowSizing {

namespace {
juce::Rectangle<int> clampSizeToArea(const juce::Rectangle<int> &area,
                                    int desiredWidth, int desiredHeight,
                                    const Constraints &constraints) {
  auto maxWidth = juce::jmin(area.getWidth(),
                             static_cast<int>(area.getWidth() *
                                              constraints.initialMaxFraction));
  auto maxHeight = juce::jmin(area.getHeight(),
                              static_cast<int>(area.getHeight() *
                                               constraints.initialMaxFraction));

  if (maxWidth <= 0 || maxHeight <= 0)
    return area;

  int width = maxWidth;
  int height = maxHeight;

  if (maxWidth >= constraints.minWidth)
    width = juce::jlimit(constraints.minWidth, maxWidth, desiredWidth);

  if (maxHeight >= constraints.minHeight)
    height = juce::jlimit(constraints.minHeight, maxHeight, desiredHeight);

  return area.withSizeKeepingCentre(width, height);
}

juce::Rectangle<int> getSafeArea(const juce::Displays::Display &display,
                                 const Constraints &constraints) {
  auto area = display.userArea;
  return area.reduced(constraints.initialMargin);
}
} // namespace

const juce::Displays::Display *getPrimaryDisplay() {
  return juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
}

const juce::Displays::Display *getDisplayForComponent(
    const juce::Component *component) {
  if (component != nullptr) {
    auto bounds = component->getScreenBounds();
    if (!bounds.isEmpty())
      return juce::Desktop::getInstance().getDisplays().getDisplayForRect(bounds);
  }
  return getPrimaryDisplay();
}

juce::Rectangle<int> getInitialBounds(int desiredWidth, int desiredHeight,
                                      const juce::Displays::Display &display,
                                      const Constraints &constraints) {
  auto safeArea = getSafeArea(display, constraints);
  if (safeArea.isEmpty())
    safeArea = display.userArea;

  return clampSizeToArea(safeArea, desiredWidth, desiredHeight, constraints);
}

juce::Point<int> getClampedSize(int desiredWidth, int desiredHeight,
                                const juce::Displays::Display &display,
                                const Constraints &constraints) {
  auto bounds = getInitialBounds(desiredWidth, desiredHeight, display, constraints);
  return {bounds.getWidth(), bounds.getHeight()};
}

juce::Rectangle<int> getMaxBounds(const juce::Displays::Display &display) {
  return display.userArea;
}

} // namespace WindowSizing
