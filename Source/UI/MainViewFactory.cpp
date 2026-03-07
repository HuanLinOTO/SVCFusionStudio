#include "MainViewFactory.h"
#include "MainComponent.h"
#include "../Utils/AppLogger.h"
#include "../Utils/UI/TimecodeFont.h"
#include "../Utils/UI/WindowSizing.h"
#include "StyledComponents.h"

std::unique_ptr<IMainView> createMainView(bool enableAudioDevice) {
  return std::make_unique<MainComponent>(enableAudioDevice);
}

void initializeUiResources() {
  AppLogger::init();
  AppFont::initialize();
  TimecodeFont::initialize();
}

void shutdownUiResources() {
  TimecodeFont::shutdown();
  AppFont::shutdown();
}

juce::Point<int> getDefaultMainViewSize(juce::Component *component) {
  auto *display = WindowSizing::getDisplayForComponent(component);
  auto constraints = WindowSizing::Constraints();
  if (display != nullptr) {
    return WindowSizing::getClampedSize(WindowSizing::kDefaultWidth,
                                        WindowSizing::kDefaultHeight,
                                        *display, constraints);
  }
  return {WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight};
}
