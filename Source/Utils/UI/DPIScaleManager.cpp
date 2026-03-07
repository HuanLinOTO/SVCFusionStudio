#include "DPIScaleManager.h"

// Static member initialization
std::atomic<float> DPIScaleManager::globalScaleOverride{0.0f};

// ========== Core Scale Factor Retrieval ==========

float DPIScaleManager::getScaleForComponent(const juce::Component* component)
{
    juce::ignoreUnused(component);
    // JUCE already handles DPI scaling internally.
    // Display::scale represents physical/logical pixel ratio, not UI scaling.
    // We only return user-configured scale override, or 1.0 (no scaling).
    float override = globalScaleOverride.load(std::memory_order_relaxed);
    if (override > 0.0f)
        return override;
    return 1.0f;
}

float DPIScaleManager::getPrimaryDisplayScale()
{
    // Return user override or 1.0
    float override = globalScaleOverride.load(std::memory_order_relaxed);
    if (override > 0.0f)
        return override;
    return 1.0f;
}

float DPIScaleManager::getScaleForDisplay(const juce::Displays::Display* display)
{
    juce::ignoreUnused(display);
    // Return user override or 1.0
    float override = globalScaleOverride.load(std::memory_order_relaxed);
    if (override > 0.0f)
        return override;
    return 1.0f;
}

float DPIScaleManager::getPhysicalDisplayScale(const juce::Displays::Display* display)
{
    if (display == nullptr)
        return 1.0f;
    // This returns the actual physical/logical pixel ratio
    // Useful for rendering high-resolution images
    return static_cast<float>(display->scale);
}

const juce::Displays::Display* DPIScaleManager::getDisplayForComponent(const juce::Component* component)
{
    if (component == nullptr)
        return juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();

    auto bounds = component->getScreenBounds();
    if (bounds.isEmpty())
        return juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();

    return juce::Desktop::getInstance().getDisplays().getDisplayForRect(bounds);
}

// ========== Scaling Utilities ==========

int DPIScaleManager::scale(int logicalPixels, const juce::Component* component)
{
    float scaleFactor = getEffectiveScale(component);
    return juce::roundToInt(static_cast<float>(logicalPixels) * scaleFactor);
}

float DPIScaleManager::scale(float logicalPixels, const juce::Component* component)
{
    float scaleFactor = getEffectiveScale(component);
    return logicalPixels * scaleFactor;
}

float DPIScaleManager::scaleFont(float baseFontSize, const juce::Component* component)
{
    float scaleFactor = getEffectiveScale(component);
    if (scaleFactor <= 1.0f)
        return baseFontSize;

    // For fonts, we use a slightly dampened scaling curve
    // This prevents fonts from becoming too large on high-DPI displays
    // while still maintaining readability
    float fontScale = 1.0f + (scaleFactor - 1.0f) * 0.7f;
    return baseFontSize * fontScale;
}

juce::Rectangle<int> DPIScaleManager::scale(const juce::Rectangle<int>& rect, const juce::Component* component)
{
    float scaleFactor = getEffectiveScale(component);
    return juce::Rectangle<int>(
        juce::roundToInt(rect.getX() * scaleFactor),
        juce::roundToInt(rect.getY() * scaleFactor),
        juce::roundToInt(rect.getWidth() * scaleFactor),
        juce::roundToInt(rect.getHeight() * scaleFactor)
    );
}

juce::Point<int> DPIScaleManager::scale(const juce::Point<int>& point, const juce::Component* component)
{
    float scaleFactor = getEffectiveScale(component);
    return juce::Point<int>(
        juce::roundToInt(point.x * scaleFactor),
        juce::roundToInt(point.y * scaleFactor)
    );
}

// ========== Inverse Scaling ==========

int DPIScaleManager::unscale(int physicalPixels, const juce::Component* component)
{
    float scaleFactor = getEffectiveScale(component);
    if (scaleFactor <= 0.0f)
        return physicalPixels;
    return juce::roundToInt(static_cast<float>(physicalPixels) / scaleFactor);
}

float DPIScaleManager::unscale(float physicalPixels, const juce::Component* component)
{
    float scaleFactor = getEffectiveScale(component);
    if (scaleFactor <= 0.0f)
        return physicalPixels;
    return physicalPixels / scaleFactor;
}

// ========== Configuration ==========

void DPIScaleManager::setGlobalScaleOverride(float scale)
{
    if (scale == 0.0f || (scale >= 0.5f && scale <= 3.0f))
        globalScaleOverride.store(scale, std::memory_order_relaxed);
}

float DPIScaleManager::getGlobalScaleOverride()
{
    return globalScaleOverride.load(std::memory_order_relaxed);
}

bool DPIScaleManager::hasGlobalScaleOverride()
{
    return globalScaleOverride.load(std::memory_order_relaxed) > 0.0f;
}

// ========== Platform-Specific Helpers ==========

bool DPIScaleManager::supportsPerMonitorDPI()
{
#if JUCE_WINDOWS
    return true;
#elif JUCE_MAC
    return true;
#elif JUCE_LINUX
    return true;
#else
    return false;
#endif
}

float DPIScaleManager::getEffectiveScale(const juce::Component* component)
{
    juce::ignoreUnused(component);
    float override = globalScaleOverride.load(std::memory_order_relaxed);
    if (override > 0.0f)
        return override;
    // Default: no scaling (JUCE handles DPI internally)
    return 1.0f;
}

// ========== ScopedDPIScale Implementation ==========

ScopedDPIScale::ScopedDPIScale(juce::Graphics& g, const juce::Component* component)
    : graphics(g)
    , scale(DPIScaleManager::getEffectiveScale(component))
    , applied(false)
{
    // Only apply transform if scale is significantly different from 1.0
    if (std::abs(scale - 1.0f) > 0.001f)
    {
        graphics.addTransform(juce::AffineTransform::scale(scale));
        applied = true;
    }
}

ScopedDPIScale::~ScopedDPIScale()
{
    // Transform will be restored when Graphics context scope ends
}
