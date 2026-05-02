#include "WorkspaceComponent.h"

WorkspaceComponent::WorkspaceComponent()
{
    setOpaque(true);

    mainCard.setPadding(0);
    mainCard.setCornerRadius(10.0f);
    mainCard.setBorderColour(APP_COLOR_BORDER_SUBTLE.withAlpha(0.35f));
    addAndMakeVisible(mainCard);
    addAndMakeVisible(panelContainer);

    // Initially hide panel container (no panels visible)
    panelContainer.setVisible(false);
    panelContainer.setRevealProgress(0.0f);
}

void WorkspaceComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient bgGradient(
        APP_COLOR_BACKGROUND, bounds.getX(), bounds.getY(),
        APP_COLOR_SURFACE_ALT, bounds.getX(), bounds.getBottom(), false);
    g.setGradientFill(bgGradient);
    g.fillAll();
}

void WorkspaceComponent::resized()
{
    auto bounds = getLocalBounds();
    const int margin = 10;
    const int topMargin = 6; // Slight spacing from toolbar

    // Apply top margin first so sidebar aligns with content
    bounds.removeFromTop(topMargin);
    bounds.removeFromRight(margin); // Outer right padding

    // Apply left/bottom margins
    bounds.removeFromLeft(margin);
    bounds.removeFromBottom(margin);

    // No sidebar; main content starts after outer margin

    const auto clampedProgress = juce::jlimit(0.0f, 1.0f, panelAnimationProgress);
    const auto animatedPanelWidth = static_cast<int>(std::round(panelContainerWidth * clampedProgress));
    const auto animatedGap = static_cast<int>(std::round(margin * clampedProgress));

    if (animatedPanelWidth > 0)
    {
        // Panel on right, consistent margin between sidebar and panel
        auto panelBounds = bounds.removeFromRight(animatedPanelWidth);
        bounds.removeFromRight(animatedGap); // Gap between piano roll and panel
        panelContainer.setBounds(panelBounds);
    }
    else
    {
        panelContainer.setBounds({});
    }

    // Main content card
    mainCard.setBounds(bounds);
}

void WorkspaceComponent::setMainContent(juce::Component* content)
{
    mainContent = content;
    mainCard.setContentComponent(content);
}

void WorkspaceComponent::addPanel(const juce::String& id, const juce::String& title,
                                   juce::Component* content,
                                   bool initiallyVisible)
{
    // Set content size before adding to panel
    if (content != nullptr)
        content->setSize(panelContainerWidth - 40, 520);

    // Create draggable panel wrapper
    auto panel = std::make_unique<DraggablePanel>(id, title);
    panel->setContentComponent(content);

    // Add to panel container
    panelContainer.addPanel(std::move(panel));

    // Set initial visibility
    if (initiallyVisible)
    {
        panelContainer.showPanel(id, true);
        updatePanelContainerVisibility();

        if (onPanelVisibilityChanged)
            onPanelVisibilityChanged(id, true);
    }
}

void WorkspaceComponent::showPanel(const juce::String& id, bool show)
{
    panelContainer.showPanel(id, show);
    updatePanelContainerVisibility();

    if (onPanelVisibilityChanged)
        onPanelVisibilityChanged(id, show);
}

bool WorkspaceComponent::isPanelVisible(const juce::String& id) const
{
    return panelContainer.isPanelVisible(id);
}

void WorkspaceComponent::updatePanelContainerVisibility()
{
    bool hasPanels = false;
    for (const auto& id : panelContainer.getPanelOrder())
    {
        if (panelContainer.isPanelVisible(id))
        {
            hasPanels = true;
            break;
        }
    }

    if (hasPanels)
        panelContainer.setVisible(true);

    startPanelAnimation(hasPanels ? 1.0f : 0.0f);
    resized();
}

void WorkspaceComponent::timerCallback()
{
    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - panelAnimationStartTimeMs;
    const auto t = juce::jlimit(0.0, 1.0, elapsedMs / static_cast<double>(panelAnimationDurationMs));
    const auto eased = static_cast<float>(t * t * (3.0 - 2.0 * t));

    panelAnimationProgress = juce::jmap(eased, panelAnimationStartProgress, panelAnimationTargetProgress);
    panelContainer.setRevealProgress(panelAnimationProgress);
    resized();
    repaint();

    if (t >= 1.0)
    {
        stopTimer();
        panelAnimationProgress = panelAnimationTargetProgress;
        panelContainer.setRevealProgress(panelAnimationProgress);

        if (panelAnimationTargetProgress <= 0.0f)
            panelContainer.setVisible(false);

        resized();
        repaint();
    }
}

void WorkspaceComponent::startPanelAnimation(float nextTarget)
{
    if (std::abs(panelAnimationProgress - nextTarget) <= 0.001f &&
        std::abs(panelAnimationTargetProgress - nextTarget) <= 0.001f)
    {
        if (nextTarget <= 0.0f)
            panelContainer.setVisible(false);
        return;
    }

    if (nextTarget > 0.0f)
        panelContainer.setVisible(true);

    panelAnimationStartProgress = panelAnimationProgress;
    panelAnimationTargetProgress = nextTarget;
    panelAnimationStartTimeMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(60);
}
