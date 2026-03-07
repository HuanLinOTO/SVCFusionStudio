#pragma once

#include "../../JuceHeader.h"

/**
 * DPI Scale Manager - Handles multi-platform, multi-monitor DPI scaling.
 *
 * Design principles:
 * - Decoupled from UI components (pure utility)
 * - Thread-safe for UI thread access
 * - Supports per-monitor DPI awareness
 * - Provides both static utilities and component-aware scaling
 *
 * Usage:
 *   // Get scale factor for a component
 *   float scale = DPIScaleManager::getScaleForComponent(this);
 *
 *   // Scale a logical pixel value
 *   int scaledWidth = DPIScaleManager::scale(60, this);
 *
 *   // Scale font size
 *   float fontSize = DPIScaleManager::scaleFont(14.0f, this);
 */
class DPIScaleManager
{
public:
    // ========== Core Scale Factor Retrieval ==========

    /**
     * Get the DPI scale factor for a specific component.
     * Returns 1.0 if component is null or not on screen.
     */
    static float getScaleForComponent(const juce::Component* component);

    /**
     * Get the DPI scale factor for the primary display.
     */
    static float getPrimaryDisplayScale();

    /**
     * Get the DPI scale factor for a specific display.
     * Returns user override or 1.0 (JUCE handles DPI internally).
     */
    static float getScaleForDisplay(const juce::Displays::Display* display);

    /**
     * Get the physical display scale (physical pixels / logical pixels).
     * This is the actual Retina/HiDPI factor, useful for rendering high-res images.
     * Note: This is NOT for UI scaling - JUCE handles that internally.
     */
    static float getPhysicalDisplayScale(const juce::Displays::Display* display);

    /**
     * Get the display containing the given component.
     * Returns primary display if component is null or not on screen.
     */
    static const juce::Displays::Display* getDisplayForComponent(const juce::Component* component);

    // ========== Scaling Utilities ==========

    /**
     * Scale an integer pixel value by the component's DPI factor.
     */
    static int scale(int logicalPixels, const juce::Component* component);

    /**
     * Scale a float pixel value by the component's DPI factor.
     */
    static float scale(float logicalPixels, const juce::Component* component);

    /**
     * Scale a font size by the component's DPI factor.
     * Uses a slightly different scaling curve for better readability.
     */
    static float scaleFont(float baseFontSize, const juce::Component* component);

    /**
     * Scale a rectangle by the component's DPI factor.
     */
    static juce::Rectangle<int> scale(const juce::Rectangle<int>& rect, const juce::Component* component);

    /**
     * Scale a point by the component's DPI factor.
     */
    static juce::Point<int> scale(const juce::Point<int>& point, const juce::Component* component);

    // ========== Inverse Scaling (Physical to Logical) ==========

    /**
     * Convert physical pixels back to logical pixels.
     */
    static int unscale(int physicalPixels, const juce::Component* component);

    /**
     * Convert physical pixels back to logical pixels (float version).
     */
    static float unscale(float physicalPixels, const juce::Component* component);

    // ========== Configuration ==========

    /**
     * Set a global scale override (useful for user preferences).
     * Set to 0.0 to disable override and use system DPI.
     * Valid range: 0.5 to 3.0 (or 0.0 to disable)
     */
    static void setGlobalScaleOverride(float scale);

    /**
     * Get the current global scale override.
     * Returns 0.0 if no override is set.
     */
    static float getGlobalScaleOverride();

    /**
     * Check if a global scale override is active.
     */
    static bool hasGlobalScaleOverride();

    // ========== Platform-Specific Helpers ==========

    /**
     * Check if the current platform supports per-monitor DPI.
     */
    static bool supportsPerMonitorDPI();

    /**
     * Get the effective scale factor combining system DPI and user override.
     */
    static float getEffectiveScale(const juce::Component* component);

private:
    DPIScaleManager() = delete;

    static std::atomic<float> globalScaleOverride;
};

/**
 * RAII helper for temporarily applying a scale factor to drawing operations.
 * Useful for custom paint() implementations.
 */
class ScopedDPIScale
{
public:
    ScopedDPIScale(juce::Graphics& g, const juce::Component* component);
    ~ScopedDPIScale();

    float getScale() const { return scale; }

private:
    juce::Graphics& graphics;
    float scale;
    bool applied;
};

/**
 * Macro for defining DPI-aware constants.
 * Usage: DPI_SCALED(60, component) returns 60 * scaleFactor
 */
#define DPI_SCALED(value, component) DPIScaleManager::scale(value, component)
#define DPI_SCALED_FONT(size, component) DPIScaleManager::scaleFont(size, component)
