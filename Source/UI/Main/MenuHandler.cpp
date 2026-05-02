#include "MenuHandler.h"

MenuHandler::MenuHandler() = default;

juce::StringArray MenuHandler::getMenuBarNames() {
    if (pluginMode)
        return {TR("menu.edit"), TR("menu.view"), TR("menu.settings")};
    return {TR("menu.file"), TR("menu.edit"), TR("menu.view"), TR("menu.settings")};
}

juce::PopupMenu MenuHandler::getMenuForIndex(int menuIndex, const juce::String& /*menuName*/) {
    return buildMenuForIndex(menuIndex);
}

void MenuHandler::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/) {
    // Command items are handled automatically by ApplicationCommandManager
    if (menuItemID >= kRecentFileMenuBaseId &&
        menuItemID < kRecentFileMenuBaseId + kMaxRecentMenuItems) {
        const int idx = menuItemID - kRecentFileMenuBaseId;
        if (idx >= 0 && idx < recentFiles.size() && onRecentFileSelected) {
            onRecentFileSelected(juce::File(recentFiles[idx]));
        }
        return;
    }

    if (commandManager != nullptr)
        commandManager->invokeDirectly(menuItemID, false);
}

void MenuHandler::addCommandMenuItem(juce::PopupMenu& menu, juce::CommandID commandID) const {
    juce::ApplicationCommandInfo info(commandID);
    if (!fillCommandInfo(commandID, info))
        return;

    juce::PopupMenu::Item item(info.shortName.isNotEmpty() ? info.shortName
                                                           : commandManager->getNameOfCommand(commandID));
    item.setID(commandID);
    item.setEnabled((info.flags & juce::ApplicationCommandInfo::isDisabled) == 0);
    item.setTicked((info.flags & juce::ApplicationCommandInfo::isTicked) != 0);

    if (commandManager != nullptr && !info.defaultKeypresses.isEmpty())
        item.shortcutKeyDescription = info.defaultKeypresses.getFirst().getTextDescription();

    menu.addItem(std::move(item));
}

bool MenuHandler::fillCommandInfo(juce::CommandID commandID,
                                  juce::ApplicationCommandInfo& info) const {
    if (commandInfoProvider != nullptr)
        return commandInfoProvider(commandID, info);

    if (commandManager == nullptr)
        return false;

    if (const auto* registered = commandManager->getCommandForID(commandID)) {
        info = *registered;
        return true;
    }

    return false;
}

juce::PopupMenu MenuHandler::buildMenuForIndex(int menuIndex) const {
    juce::PopupMenu menu;

    if (pluginMode) {
        if (menuIndex == 0) {
            addCommandMenuItem(menu, CommandIDs::undo);
            addCommandMenuItem(menu, CommandIDs::redo);
            menu.addSeparator();
            addCommandMenuItem(menu, CommandIDs::selectAll);
        } else if (menuIndex == 1) {
            addCommandMenuItem(menu, CommandIDs::showDeltaPitch);
            addCommandMenuItem(menu, CommandIDs::showBasePitch);
        } else if (menuIndex == 2) {
            addCommandMenuItem(menu, CommandIDs::showSettings);
        }

        return menu;
    }

    if (menuIndex == 0) {
        addCommandMenuItem(menu, CommandIDs::openFile);
        juce::PopupMenu recentMenu;
        if (recentFiles.isEmpty()) {
            recentMenu.addItem(1, "No Recent Files", false, false);
        } else {
            const int count = juce::jmin(kMaxRecentMenuItems, recentFiles.size());
            for (int i = 0; i < count; ++i) {
                juce::File file(recentFiles[i]);
                const juce::String label =
                    juce::String(i + 1) + "  " + file.getFullPathName();
                recentMenu.addItem(kRecentFileMenuBaseId + i, label);
            }
        }
        menu.addSubMenu("Recent Files", recentMenu);
        addCommandMenuItem(menu, CommandIDs::saveProject);
        menu.addSeparator();
        addCommandMenuItem(menu, CommandIDs::exportAudio);
        addCommandMenuItem(menu, CommandIDs::exportMidi);
        menu.addSeparator();
        addCommandMenuItem(menu, CommandIDs::clearProject);
        menu.addSeparator();
        addCommandMenuItem(menu, CommandIDs::quit);
    } else if (menuIndex == 1) {
        addCommandMenuItem(menu, CommandIDs::undo);
        addCommandMenuItem(menu, CommandIDs::redo);
        menu.addSeparator();
        addCommandMenuItem(menu, CommandIDs::selectAll);
    } else if (menuIndex == 2) {
        addCommandMenuItem(menu, CommandIDs::showDeltaPitch);
        addCommandMenuItem(menu, CommandIDs::showBasePitch);
    } else if (menuIndex == 3) {
        addCommandMenuItem(menu, CommandIDs::showSettings);
    }

    return menu;
}
