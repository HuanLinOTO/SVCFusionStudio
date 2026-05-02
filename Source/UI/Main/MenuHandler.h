#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/UndoManager.h"
#include "../../Utils/Localization.h"
#include "../Commands.h"
#include <functional>

/**
 * Handles menu bar creation and menu item selection.
 */
class MenuHandler : public juce::MenuBarModel {
public:
    static constexpr int kRecentFileMenuBaseId = 0x3000;
    static constexpr int kMaxRecentMenuItems = 10;

    MenuHandler();
    ~MenuHandler() override = default;

    void setPluginMode(bool isPlugin) {
        pluginMode = isPlugin;
    }
    void setUndoManager(PitchUndoManager* mgr) { undoManager = mgr; }
    void setCommandManager(juce::ApplicationCommandManager* mgr) { commandManager = mgr; }
    void setRecentFiles(const juce::StringArray& files) { recentFiles = files; }
    void setCommandInfoProvider(std::function<bool(juce::CommandID, juce::ApplicationCommandInfo&)> provider)
    {
        commandInfoProvider = std::move(provider);
    }
    void setOnRecentFileSelected(std::function<void(const juce::File&)> cb) { onRecentFileSelected = std::move(cb); }

    // MenuBarModel interface
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int menuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    void addCommandMenuItem(juce::PopupMenu& menu, juce::CommandID commandID) const;
    bool fillCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& info) const;
    juce::PopupMenu buildMenuForIndex(int menuIndex) const;

    bool pluginMode = false;
    PitchUndoManager* undoManager = nullptr;
    juce::ApplicationCommandManager* commandManager = nullptr;
    juce::StringArray recentFiles;
    std::function<bool(juce::CommandID, juce::ApplicationCommandInfo&)> commandInfoProvider;
    std::function<void(const juce::File&)> onRecentFileSelected;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MenuHandler)
};
