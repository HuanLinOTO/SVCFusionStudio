#pragma once

#include "../JuceHeader.h"
#include "IMainView.h"
#include <memory>

std::unique_ptr<IMainView> createMainView(bool enableAudioDevice);
void initializeUiResources();
void shutdownUiResources();
juce::Point<int> getDefaultMainViewSize(juce::Component *component);
