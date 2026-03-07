#pragma once

#include "../JuceHeader.h"

namespace SHA256Utils {

juce::String fileSHA256(const juce::File &file);

} // namespace SHA256Utils

