#pragma once

#include "../JuceHeader.h"

class AppMenuBarComponent : public juce::Component,
                            private juce::MenuBarModel::Listener {
public:
  AppMenuBarComponent();
  ~AppMenuBarComponent() override;

  void setModel(juce::MenuBarModel *newModel);

  void paint(juce::Graphics &g) override;
  void resized() override;
  void mouseMove(const juce::MouseEvent &e) override;
  void mouseExit(const juce::MouseEvent &e) override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  bool keyPressed(const juce::KeyPress &key) override;
  bool hitTest(int x, int y) override;

  bool isPopupOpen() const { return openMenuIndex >= 0; }
  void closeMenu();

  static constexpr int kMenuBarHeight = 24;
  static constexpr int kItemHeight = 30;
  static constexpr int kSeparatorHeight = 9;

private:
  static constexpr int kMenuMinWidth = 210;
  static constexpr int kMenuMaxWidth = 560;
  static constexpr int kHorizontalPadding = 12;
  static constexpr int kIconColumnWidth = 22;

  struct MenuItemView {
    juce::PopupMenu::Item item;
    juce::Rectangle<int> bounds;
    int sourceIndex = -1;
  };

  void menuBarItemsChanged(juce::MenuBarModel *menuBarModel) override;
  void menuCommandInvoked(
      juce::MenuBarModel *menuBarModel,
      const juce::ApplicationCommandTarget::InvocationInfo &info) override;
  void menuBarActivated(juce::MenuBarModel *menuBarModel, bool isActive) override;

  void refreshMenuNames();
  void updateMenuBounds();
  int getTopLevelItemAt(juce::Point<int> pos) const;
  int getPopupItemAt(juce::Point<int> pos) const;
  void setHoveredTopLevelIndex(int index);
  void setHoveredPopupIndex(int index);
  void setHoveredSubMenuIndex(int index);
  void openMenu(int index);
  void rebuildOpenMenuItems();
  void rebuildSubMenuItems(int parentIndex);
  void invokeItem(const MenuItemView &view);
  bool isPointInActiveArea(juce::Point<int> pos) const;
  int getSubMenuItemAt(juce::Point<int> pos) const;

  juce::MenuBarModel *model = nullptr;
  juce::StringArray menuNames;
  juce::Array<juce::Rectangle<int>> topLevelBounds;
  juce::OwnedArray<juce::PopupMenu> openMenuStorage;
  juce::OwnedArray<juce::PopupMenu> subMenuStorage;
  std::vector<MenuItemView> openMenuItems;
  std::vector<MenuItemView> subMenuItems;
  juce::Rectangle<int> popupBounds;
  juce::Rectangle<int> subMenuBounds;
  int hoveredTopLevelIndex = -1;
  int openMenuIndex = -1;
  int hoveredPopupIndex = -1;
  int activeSubMenuParentIndex = -1;
  int hoveredSubMenuIndex = -1;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppMenuBarComponent)
};
