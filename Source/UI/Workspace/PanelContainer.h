#pragma once

#include "../../JuceHeader.h"
#include "DraggablePanel.h"
#include <vector>
#include <memory>

/**
 * Container that manages multiple draggable panels.
 * Panels can be reordered by dragging their headers.
 */
class PanelContainer : public juce::Component, private juce::Timer
{
public:
    PanelContainer();
    ~PanelContainer() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void addPanel(std::unique_ptr<DraggablePanel> panel);
    void removePanel(const juce::String& panelId);
    void showPanel(const juce::String& panelId, bool show);
    bool isPanelVisible(const juce::String& panelId) const;
    void setRevealProgress(float progress);

    DraggablePanel* getPanel(const juce::String& panelId);
    const std::vector<juce::String>& getPanelOrder() const { return panelOrder; }

    void updateLayout();
    void handlePanelDrag(DraggablePanel* panel, const juce::MouseEvent& e);
    void handlePanelDragEnd(DraggablePanel* panel);

    std::function<void(const std::vector<juce::String>&)> onPanelOrderChanged;

private:
    void timerCallback() override;
    void reorderPanels();
    int findPanelIndexAt(int y) const;
    juce::String getFirstVisiblePanelId() const;
    void startPanelSwitchAnimation();

    std::map<juce::String, std::unique_ptr<DraggablePanel>> panels;
    std::vector<juce::String> panelOrder;
    std::set<juce::String> visiblePanels;
    juce::String activePanelId;

    // Drag state
    DraggablePanel* draggedPanel = nullptr;
    int dragInsertIndex = -1;
    int dragStartY = 0;
    float revealProgress = 0.0f;
    bool switchAnimationActive = false;
    float switchAnimationProgress = 1.0f;
    double switchAnimationStartTimeMs = 0.0;
    int switchAnimationDurationMs = 130;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PanelContainer)
};
