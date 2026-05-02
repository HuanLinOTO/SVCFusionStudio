#include "PanelContainer.h"

PanelContainer::PanelContainer()
{
    setOpaque(true);
}

void PanelContainer::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient bgGradient(
        APP_COLOR_SURFACE_ALT, bounds.getX(), bounds.getY(),
        APP_COLOR_BACKGROUND, bounds.getX(), bounds.getBottom(), false);
    g.setGradientFill(bgGradient);
    g.setOpacity(revealProgress);
    g.fillAll();
    g.setOpacity(1.0f);
}

void PanelContainer::resized()
{
    updateLayout();
}

void PanelContainer::addPanel(std::unique_ptr<DraggablePanel> panel)
{
    auto id = panel->getPanelId();
    panel->setPanelContainer(this);
    addChildComponent(panel.get());

    panels[id] = std::move(panel);
    panelOrder.push_back(id);
}

void PanelContainer::removePanel(const juce::String& panelId)
{
    auto it = panels.find(panelId);
    if (it != panels.end())
    {
        removeChildComponent(it->second.get());
        panels.erase(it);

        panelOrder.erase(std::remove(panelOrder.begin(), panelOrder.end(), panelId), panelOrder.end());
        visiblePanels.erase(panelId);

        updateLayout();
    }
}

void PanelContainer::showPanel(const juce::String& panelId, bool show)
{
    auto it = panels.find(panelId);
    if (it == panels.end())
        return;

    if (show)
    {
        if (activePanelId != panelId)
        {
            activePanelId = panelId;
            startPanelSwitchAnimation();
        }

        visiblePanels.insert(panelId);
        it->second->setVisible(true);
    }
    else
    {
        visiblePanels.erase(panelId);

        if (activePanelId == panelId)
            activePanelId = getFirstVisiblePanelId();

        if (it->second != nullptr)
            it->second->setVisible(false);
    }

    updateLayout();
}

bool PanelContainer::isPanelVisible(const juce::String& panelId) const
{
    return visiblePanels.find(panelId) != visiblePanels.end();
}

DraggablePanel* PanelContainer::getPanel(const juce::String& panelId)
{
    auto it = panels.find(panelId);
    return it != panels.end() ? it->second.get() : nullptr;
}

void PanelContainer::setRevealProgress(float progress)
{
    revealProgress = juce::jlimit(0.0f, 1.0f, progress);
    updateLayout();
    repaint();
}

void PanelContainer::updateLayout()
{
    int width = getWidth();
    int height = getHeight();

    if (activePanelId.isEmpty())
        activePanelId = getFirstVisiblePanelId();

    const auto contentAlpha = revealProgress * (switchAnimationActive ? switchAnimationProgress : 1.0f);
    const auto contentOffset = (1.0f - (switchAnimationActive ? switchAnimationProgress : 1.0f)) * 18.0f;

    for (auto& entry : panels)
    {
        auto* panel = entry.second.get();
        if (panel == nullptr)
            continue;

        const bool isActive = !activePanelId.isEmpty() && entry.first == activePanelId;
        panel->setBounds(0, 0, width, height);
        panel->setVisible(isActive && width > 0 && height > 0 && contentAlpha > 0.001f);
        panel->setAlpha(isActive ? contentAlpha : 1.0f);
        panel->setTransform(isActive
                                ? juce::AffineTransform::translation(contentOffset, 0.0f)
                                : juce::AffineTransform());
        panel->setEnabled(isActive && revealProgress >= 0.999f &&
                          (!switchAnimationActive || switchAnimationProgress >= 0.999f));
    }
}

void PanelContainer::timerCallback()
{
    if (!switchAnimationActive)
        return;

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - switchAnimationStartTimeMs;
    const auto t = juce::jlimit(0.0, 1.0, elapsedMs / static_cast<double>(switchAnimationDurationMs));
    switchAnimationProgress = static_cast<float>(t * t * (3.0 - 2.0 * t));
    updateLayout();
    repaint();

    if (t >= 1.0)
    {
        switchAnimationActive = false;
        switchAnimationProgress = 1.0f;
        stopTimer();
        updateLayout();
        repaint();
    }
}

void PanelContainer::handlePanelDrag(DraggablePanel* panel, const juce::MouseEvent& e)
{
    if (draggedPanel == nullptr)
    {
        draggedPanel = panel;
        dragStartY = panel->getY();
    }

    // Find insert position
    int newIndex = findPanelIndexAt(e.y);
    if (newIndex != dragInsertIndex)
    {
        dragInsertIndex = newIndex;
        repaint();
    }
}

void PanelContainer::handlePanelDragEnd(DraggablePanel* panel)
{
    if (draggedPanel != panel || dragInsertIndex < 0)
    {
        draggedPanel = nullptr;
        dragInsertIndex = -1;
        return;
    }

    // Find current index
    auto it = std::find(panelOrder.begin(), panelOrder.end(), panel->getPanelId());
    if (it != panelOrder.end())
    {
        int currentIndex = static_cast<int>(std::distance(panelOrder.begin(), it));

        if (currentIndex != dragInsertIndex)
        {
            // Remove from current position
            panelOrder.erase(it);

            // Adjust insert index if needed
            if (dragInsertIndex > currentIndex)
                dragInsertIndex--;

            // Insert at new position
            panelOrder.insert(panelOrder.begin() + dragInsertIndex, panel->getPanelId());

            if (onPanelOrderChanged)
                onPanelOrderChanged(panelOrder);
        }
    }

    draggedPanel = nullptr;
    dragInsertIndex = -1;
    updateLayout();
}

int PanelContainer::findPanelIndexAt(int y) const
{
    int index = 0;
    int currentY = 8;

    for (const auto& id : panelOrder)
    {
        if (visiblePanels.find(id) == visiblePanels.end())
            continue;

        auto it = panels.find(id);
        if (it == panels.end())
            continue;

        int height = it->second->getPreferredHeight();
        int midY = currentY + height / 2;

        if (y < midY)
            return index;

        currentY += height + 8;
        index++;
    }

    return index;
}

void PanelContainer::reorderPanels()
{
    updateLayout();
}

juce::String PanelContainer::getFirstVisiblePanelId() const
{
    for (const auto& id : panelOrder)
    {
        if (visiblePanels.find(id) != visiblePanels.end())
            return id;
    }

    return {};
}

void PanelContainer::startPanelSwitchAnimation()
{
    switchAnimationActive = true;
    switchAnimationProgress = 0.0f;
    switchAnimationStartTimeMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(60);
}
