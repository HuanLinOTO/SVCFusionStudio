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

    // Panel container (if any panels are visible)
    bool hasPanels = false;
    for (const auto& id : panelContainer.getPanelOrder())
    {
        if (panelContainer.isPanelVisible(id))
        {
            hasPanels = true;
            break;
        }
    }

    // Apply left/bottom margins
    bounds.removeFromLeft(margin);
    bounds.removeFromBottom(margin);

    // No sidebar; main content starts after outer margin

    if (hasPanels)
    {
        // Panel on right, consistent margin between sidebar and panel
        auto panelBounds = bounds.removeFromRight(panelContainerWidth);
        bounds.removeFromRight(margin); // Gap between piano roll and panel
        panelContainer.setBounds(panelBounds);
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

    panelContainer.setVisible(hasPanels);
    resized();
}
