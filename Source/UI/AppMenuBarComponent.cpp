#include "AppMenuBarComponent.h"

#include "StyledComponents.h"
#include "../Utils/UI/Theme.h"

#include <algorithm>

namespace {
int getTextWidth(const juce::Font &font, const juce::String &text) {
  return juce::GlyphArrangement::getStringWidthInt(font, text);
}

int getItemHeight(const juce::PopupMenu::Item &item) {
  return item.isSeparator ? AppMenuBarComponent::kSeparatorHeight
                          : AppMenuBarComponent::kItemHeight;
}

juce::String normaliseShortcutText(juce::String text) {
  text = text.replace(" + ", "+").replace(" +", "+").replace("+ ", "+");
  juce::StringArray parts;
  parts.addTokens(text, "+", "");

  for (auto &part : parts) {
    part = part.trim();
    if (part.equalsIgnoreCase("ctrl") || part.equalsIgnoreCase("control"))
      part = "Ctrl";
    else if (part.equalsIgnoreCase("cmd") || part.equalsIgnoreCase("command"))
      part = "Cmd";
    else if (part.equalsIgnoreCase("shift"))
      part = "Shift";
    else if (part.equalsIgnoreCase("alt") || part.equalsIgnoreCase("option"))
      part = "Alt";
    else if (part.equalsIgnoreCase("escape"))
      part = "Esc";
    else if (part.equalsIgnoreCase("return"))
      part = "Enter";
    else if (part.length() == 1)
      part = part.toUpperCase();
  }

  return parts.joinIntoString("+");
}

void drawSubMenuChevron(juce::Graphics &g, juce::Rectangle<int> area,
                        juce::Colour colour) {
  auto r = area.toFloat().withSizeKeepingCentre(5.0f, 8.0f);
  juce::Path chevron;
  chevron.startNewSubPath(r.getX(), r.getY());
  chevron.lineTo(r.getRight(), r.getCentreY());
  chevron.lineTo(r.getX(), r.getBottom());
  g.setColour(colour.withAlpha(0.72f));
  g.strokePath(chevron, juce::PathStrokeType(1.35f));
}
}

AppMenuBarComponent::AppMenuBarComponent() {
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);
}

AppMenuBarComponent::~AppMenuBarComponent() { setModel(nullptr); }

void AppMenuBarComponent::setModel(juce::MenuBarModel *newModel) {
  if (model == newModel)
    return;

  if (model != nullptr)
    model->removeListener(this);

  closeMenu();
  model = newModel;

  if (model != nullptr)
    model->addListener(this);

  refreshMenuNames();
}

void AppMenuBarComponent::paint(juce::Graphics &g) {
  g.setColour(APP_COLOR_SURFACE_ALT);
  g.fillRect(0, 0, getWidth(), kMenuBarHeight);
  g.setColour(APP_COLOR_BORDER_SUBTLE);
  g.drawLine(0.0f, static_cast<float>(kMenuBarHeight) - 0.5f,
             static_cast<float>(getWidth()),
             static_cast<float>(kMenuBarHeight) - 0.5f, 1.0f);

  const auto textFont = AppFont::getFont(13.5f);
  g.setFont(textFont);

  for (int i = 0; i < topLevelBounds.size(); ++i) {
    const auto bounds = topLevelBounds.getReference(i);
    if (i == openMenuIndex || i == hoveredTopLevelIndex) {
      g.setColour(APP_COLOR_PRIMARY.withAlpha(0.18f));
      g.fillRoundedRectangle(bounds.reduced(1, 2).toFloat(), 5.0f);
    }

    g.setColour(APP_COLOR_TEXT_PRIMARY);
    g.drawFittedText(menuNames[i], bounds, juce::Justification::centred, 1);
  }

  if (openMenuIndex < 0 || popupBounds.isEmpty())
    return;

  g.setColour(APP_COLOR_SURFACE);
  g.fillRoundedRectangle(popupBounds.toFloat(), 6.0f);
  g.setColour(APP_COLOR_BORDER);
  g.drawRoundedRectangle(popupBounds.toFloat().reduced(0.5f), 6.0f, 1.0f);

  const auto menuFont = AppFont::getFont(13.5f);
  const auto shortcutFont = AppFont::getFont(12.0f);
  g.setFont(menuFont);
  for (int i = 0; i < static_cast<int>(openMenuItems.size()); ++i) {
    const auto &view = openMenuItems[static_cast<size_t>(i)];
    const auto &item = view.item;
    auto row = view.bounds;

    if (item.isSeparator) {
      g.setColour(APP_COLOR_GRID_BAR);
      const auto line = row.withHeight(1).withY(row.getCentreY());
      g.fillRect(line.reduced(8, 0));
      continue;
    }

    if (i == hoveredPopupIndex && item.isEnabled) {
      g.setColour(APP_COLOR_PRIMARY.withAlpha(0.22f));
      g.fillRoundedRectangle(row.reduced(4, 2).toFloat(), 5.0f);
    }

    const auto textColour = item.isEnabled ? APP_COLOR_TEXT_PRIMARY
                                           : APP_COLOR_TEXT_MUTED.withAlpha(0.55f);
    g.setColour(textColour);

    auto textArea = row.reduced(8, 0);
    auto iconArea = textArea.removeFromLeft(kIconColumnWidth);

    if (item.isTicked) {
      juce::Path tick;
      const auto tickBounds = iconArea.reduced(6, 8).toFloat();
      tick.startNewSubPath(tickBounds.getX(), tickBounds.getCentreY());
      tick.lineTo(tickBounds.getCentreX() - 1.0f, tickBounds.getBottom());
      tick.lineTo(tickBounds.getRight(), tickBounds.getY());
      g.strokePath(tick, juce::PathStrokeType(1.8f));
    }

    if (item.subMenu != nullptr)
      drawSubMenuChevron(g, textArea.removeFromRight(18), textColour);

    if (item.shortcutKeyDescription.isNotEmpty()) {
      const auto shortcutText = normaliseShortcutText(item.shortcutKeyDescription);
      const auto shortcutArea = textArea.removeFromRight(78);
      g.setColour(textColour.withAlpha(item.isEnabled ? 0.75f : 0.45f));
      g.setFont(shortcutFont);
      g.drawText(shortcutText, shortcutArea, juce::Justification::centredRight,
                 true);
      g.setColour(textColour);
      g.setFont(menuFont);
    }

    g.drawText(item.text, textArea, juce::Justification::centredLeft, true);
  }

  if (activeSubMenuParentIndex < 0 || subMenuBounds.isEmpty())
    return;

  g.setColour(APP_COLOR_SURFACE);
  g.fillRoundedRectangle(subMenuBounds.toFloat(), 6.0f);
  g.setColour(APP_COLOR_BORDER);
  g.drawRoundedRectangle(subMenuBounds.toFloat().reduced(0.5f), 6.0f, 1.0f);

  for (int i = 0; i < static_cast<int>(subMenuItems.size()); ++i) {
    const auto &view = subMenuItems[static_cast<size_t>(i)];
    const auto &item = view.item;
    const auto row = view.bounds;

    if (item.isSeparator) {
      g.setColour(APP_COLOR_GRID_BAR);
      g.fillRect(row.withHeight(1).withY(row.getCentreY()).reduced(8, 0));
      continue;
    }

    if (i == hoveredSubMenuIndex && item.isEnabled) {
      g.setColour(APP_COLOR_PRIMARY.withAlpha(0.22f));
      g.fillRoundedRectangle(row.reduced(4, 2).toFloat(), 5.0f);
    }

    const auto textColour = item.isEnabled ? APP_COLOR_TEXT_PRIMARY
                                           : APP_COLOR_TEXT_MUTED.withAlpha(0.55f);
    g.setColour(textColour);

    auto textArea = row.reduced(8, 0);
    auto iconArea = textArea.removeFromLeft(kIconColumnWidth);
    if (item.isTicked) {
      juce::Path tick;
      const auto tickBounds = iconArea.reduced(6, 8).toFloat();
      tick.startNewSubPath(tickBounds.getX(), tickBounds.getCentreY());
      tick.lineTo(tickBounds.getCentreX() - 1.0f, tickBounds.getBottom());
      tick.lineTo(tickBounds.getRight(), tickBounds.getY());
      g.strokePath(tick, juce::PathStrokeType(1.8f));
    }

    g.setFont(menuFont);
    g.drawText(item.text, textArea, juce::Justification::centredLeft, true);
  }
}

void AppMenuBarComponent::resized() { updateMenuBounds(); }

void AppMenuBarComponent::mouseMove(const juce::MouseEvent &e) {
  const auto pos = e.getPosition();
  const auto topIndex = getTopLevelItemAt(pos);
  setHoveredTopLevelIndex(topIndex);

  if (openMenuIndex >= 0 && topIndex >= 0 && topIndex != openMenuIndex) {
    openMenu(topIndex);
    return;
  }

  const auto popupIndex = getPopupItemAt(pos);
  if (popupIndex >= 0) {
    setHoveredSubMenuIndex(-1);
    setHoveredPopupIndex(popupIndex);
    const auto &item = openMenuItems[static_cast<size_t>(popupIndex)].item;
    if (item.isEnabled && item.subMenu != nullptr)
      rebuildSubMenuItems(popupIndex);
    else if (activeSubMenuParentIndex >= 0) {
      activeSubMenuParentIndex = -1;
      hoveredSubMenuIndex = -1;
      subMenuBounds = {};
      subMenuItems.clear();
      subMenuStorage.clear();
      repaint();
    }
    return;
  }

  setHoveredSubMenuIndex(getSubMenuItemAt(pos));
}

void AppMenuBarComponent::mouseExit(const juce::MouseEvent &e) {
  juce::ignoreUnused(e);
  setHoveredTopLevelIndex(-1);
  if (openMenuIndex < 0)
    setHoveredPopupIndex(-1);
}

void AppMenuBarComponent::mouseDown(const juce::MouseEvent &e) {
  grabKeyboardFocus();
  const auto pos = e.getPosition();
  const auto topIndex = getTopLevelItemAt(pos);

  if (topIndex >= 0) {
    if (openMenuIndex == topIndex)
      closeMenu();
    else
      openMenu(topIndex);
    return;
  }

  if (getPopupItemAt(pos) < 0 && getSubMenuItemAt(pos) < 0)
    closeMenu();
}

void AppMenuBarComponent::mouseUp(const juce::MouseEvent &e) {
  const auto itemIndex = getPopupItemAt(e.getPosition());
  if (itemIndex < 0) {
    const auto subItemIndex = getSubMenuItemAt(e.getPosition());
    if (subItemIndex < 0)
      return;

    const auto &view = subMenuItems[static_cast<size_t>(subItemIndex)];
    if (!view.item.isEnabled || view.item.isSeparator || view.item.isSectionHeader)
      return;

    invokeItem(view);
    return;
  }

  const auto &view = openMenuItems[static_cast<size_t>(itemIndex)];
  if (!view.item.isEnabled || view.item.isSeparator || view.item.isSectionHeader)
    return;

  if (view.item.subMenu != nullptr)
    return;

  invokeItem(view);
}

bool AppMenuBarComponent::keyPressed(const juce::KeyPress &key) {
  if (key.isKeyCode(juce::KeyPress::escapeKey) && openMenuIndex >= 0) {
    closeMenu();
    return true;
  }

  return false;
}

bool AppMenuBarComponent::hitTest(int x, int y) {
  return isPointInActiveArea({x, y});
}

void AppMenuBarComponent::closeMenu() {
  if (model != nullptr && openMenuIndex >= 0)
    model->handleMenuBarActivate(false);

  openMenuIndex = -1;
  hoveredPopupIndex = -1;
  popupBounds = {};
  openMenuItems.clear();
  openMenuStorage.clear();
  subMenuStorage.clear();
  subMenuItems.clear();
  subMenuBounds = {};
  activeSubMenuParentIndex = -1;
  hoveredSubMenuIndex = -1;
  repaint();
}

void AppMenuBarComponent::menuBarItemsChanged(juce::MenuBarModel *menuBarModel) {
  juce::ignoreUnused(menuBarModel);
  refreshMenuNames();
}

void AppMenuBarComponent::menuCommandInvoked(
    juce::MenuBarModel *menuBarModel,
    const juce::ApplicationCommandTarget::InvocationInfo &info) {
  juce::ignoreUnused(menuBarModel, info);
  closeMenu();
}

void AppMenuBarComponent::menuBarActivated(juce::MenuBarModel *menuBarModel,
                                           bool isActive) {
  juce::ignoreUnused(menuBarModel, isActive);
}

void AppMenuBarComponent::refreshMenuNames() {
  menuNames.clear();
  if (model != nullptr)
    menuNames = model->getMenuBarNames();

  updateMenuBounds();
  repaint();
}

void AppMenuBarComponent::updateMenuBounds() {
  topLevelBounds.clear();
  auto x = 0;
  const auto font = AppFont::getFont(13.5f);

  for (const auto &name : menuNames) {
    const auto width = juce::jmax(48, getTextWidth(font, name) + 28);
    topLevelBounds.add({x, 0, width, kMenuBarHeight});
    x += width;
  }

  if (openMenuIndex >= 0)
    rebuildOpenMenuItems();
}

int AppMenuBarComponent::getTopLevelItemAt(juce::Point<int> pos) const {
  if (pos.y < 0 || pos.y >= kMenuBarHeight)
    return -1;

  for (int i = 0; i < topLevelBounds.size(); ++i)
    if (topLevelBounds.getReference(i).contains(pos))
      return i;

  return -1;
}

int AppMenuBarComponent::getPopupItemAt(juce::Point<int> pos) const {
  if (openMenuIndex < 0 || !popupBounds.contains(pos))
    return -1;

  for (int i = 0; i < static_cast<int>(openMenuItems.size()); ++i)
    if (openMenuItems[static_cast<size_t>(i)].bounds.contains(pos))
      return i;

  return -1;
}

int AppMenuBarComponent::getSubMenuItemAt(juce::Point<int> pos) const {
  if (activeSubMenuParentIndex < 0 || !subMenuBounds.contains(pos))
    return -1;

  for (int i = 0; i < static_cast<int>(subMenuItems.size()); ++i)
    if (subMenuItems[static_cast<size_t>(i)].bounds.contains(pos))
      return i;

  return -1;
}

void AppMenuBarComponent::setHoveredTopLevelIndex(int index) {
  if (hoveredTopLevelIndex == index)
    return;

  const auto old = hoveredTopLevelIndex;
  hoveredTopLevelIndex = index;

  if (old >= 0 && old < topLevelBounds.size())
    repaint(topLevelBounds.getReference(old).expanded(2, 0));
  if (index >= 0 && index < topLevelBounds.size())
    repaint(topLevelBounds.getReference(index).expanded(2, 0));
}

void AppMenuBarComponent::setHoveredPopupIndex(int index) {
  if (hoveredPopupIndex == index)
    return;

  const auto repaintRow = [this](int row) {
    if (row >= 0 && row < static_cast<int>(openMenuItems.size()))
      repaint(openMenuItems[static_cast<size_t>(row)].bounds.expanded(4, 2));
  };

  repaintRow(hoveredPopupIndex);
  hoveredPopupIndex = index;
  repaintRow(hoveredPopupIndex);
}

void AppMenuBarComponent::setHoveredSubMenuIndex(int index) {
  if (hoveredSubMenuIndex == index)
    return;

  const auto repaintRow = [this](int row) {
    if (row >= 0 && row < static_cast<int>(subMenuItems.size()))
      repaint(subMenuItems[static_cast<size_t>(row)].bounds.expanded(4, 2));
  };

  repaintRow(hoveredSubMenuIndex);
  hoveredSubMenuIndex = index;
  repaintRow(hoveredSubMenuIndex);
}

void AppMenuBarComponent::openMenu(int index) {
  if (model == nullptr || !juce::isPositiveAndBelow(index, menuNames.size()))
    return;

  if (openMenuIndex < 0)
    model->handleMenuBarActivate(true);

  openMenuIndex = index;
  hoveredPopupIndex = -1;
  activeSubMenuParentIndex = -1;
  hoveredSubMenuIndex = -1;
  subMenuItems.clear();
  subMenuStorage.clear();
  subMenuBounds = {};
  rebuildOpenMenuItems();
  repaint();
}

void AppMenuBarComponent::rebuildOpenMenuItems() {
  openMenuItems.clear();
  openMenuStorage.clear();
  subMenuItems.clear();
  subMenuStorage.clear();
  popupBounds = {};
  subMenuBounds = {};
  activeSubMenuParentIndex = -1;
  hoveredSubMenuIndex = -1;

  if (model == nullptr || !juce::isPositiveAndBelow(openMenuIndex, menuNames.size()) ||
      !juce::isPositiveAndBelow(openMenuIndex, topLevelBounds.size()))
    return;

  auto menu = model->getMenuForIndex(openMenuIndex, menuNames[openMenuIndex]);
  auto *storedMenu = openMenuStorage.add(new juce::PopupMenu(std::move(menu)));

  juce::PopupMenu::MenuItemIterator iter(*storedMenu, false);
  int rowCount = 0;
  int maxTextWidth = 0;
  const auto font = AppFont::getFont(13.5f);

  while (iter.next()) {
    const auto &item = iter.getItem();
    MenuItemView view;
    view.item = item;
    view.sourceIndex = rowCount;
    openMenuItems.push_back(std::move(view));

    if (!item.isSeparator) {
      maxTextWidth = std::max(maxTextWidth, getTextWidth(font, item.text));
      if (item.shortcutKeyDescription.isNotEmpty())
        maxTextWidth += getTextWidth(AppFont::getFont(12.0f),
                                     normaliseShortcutText(item.shortcutKeyDescription)) + 22;
      if (item.subMenu != nullptr)
        maxTextWidth += 22;
    }

    ++rowCount;
  }

  if (openMenuItems.empty())
    return;

  const auto menuWidth = juce::jlimit(
      kMenuMinWidth, kMenuMaxWidth,
      maxTextWidth + kIconColumnWidth + kHorizontalPadding * 2 + 24);
  int menuHeight = 8;
  for (const auto &itemView : openMenuItems)
    menuHeight += getItemHeight(itemView.item);
  auto x = topLevelBounds.getReference(openMenuIndex).getX();
  x = juce::jlimit(0, juce::jmax(0, getWidth() - menuWidth - 2), x);
  popupBounds = {x, kMenuBarHeight + 2, menuWidth, menuHeight};

  auto row = popupBounds.reduced(4, 4).withHeight(kItemHeight);
  for (auto &itemView : openMenuItems) {
    row.setHeight(getItemHeight(itemView.item));
    itemView.bounds = row;
    row.translate(0, row.getHeight());
  }
}

void AppMenuBarComponent::rebuildSubMenuItems(int parentIndex) {
  if (parentIndex == activeSubMenuParentIndex)
    return;

  activeSubMenuParentIndex = parentIndex;
  hoveredSubMenuIndex = -1;
  subMenuItems.clear();
  subMenuStorage.clear();
  subMenuBounds = {};

  if (parentIndex < 0 || parentIndex >= static_cast<int>(openMenuItems.size())) {
    repaint();
    return;
  }

  const auto &parent = openMenuItems[static_cast<size_t>(parentIndex)];
  if (parent.item.subMenu == nullptr) {
    repaint();
    return;
  }

  auto *storedMenu = subMenuStorage.add(new juce::PopupMenu(*parent.item.subMenu));
  juce::PopupMenu::MenuItemIterator iter(*storedMenu, false);
  int maxTextWidth = 0;
  const auto font = AppFont::getFont(13.5f);

  while (iter.next()) {
    const auto &item = iter.getItem();
    MenuItemView view;
    view.item = item;
    subMenuItems.push_back(std::move(view));

    if (!item.isSeparator)
      maxTextWidth = std::max(maxTextWidth, getTextWidth(font, item.text));
  }

  if (subMenuItems.empty()) {
    repaint();
    return;
  }

  const auto menuWidth = juce::jlimit(
      kMenuMinWidth, kMenuMaxWidth,
      maxTextWidth + kIconColumnWidth + kHorizontalPadding * 2 + 18);
  int menuHeight = 8;
  for (const auto &itemView : subMenuItems)
    menuHeight += getItemHeight(itemView.item);
  auto x = popupBounds.getRight() - 2;
  if (x + menuWidth > getWidth())
    x = popupBounds.getX() - menuWidth + 2;
  auto y = parent.bounds.getY() - 4;
  y = juce::jlimit(kMenuBarHeight + 2,
                   juce::jmax(kMenuBarHeight + 2, getHeight() - menuHeight - 2), y);
  subMenuBounds = {x, y, menuWidth, menuHeight};

  auto row = subMenuBounds.reduced(4, 4).withHeight(kItemHeight);
  for (auto &itemView : subMenuItems) {
    row.setHeight(getItemHeight(itemView.item));
    itemView.bounds = row;
    row.translate(0, row.getHeight());
  }

  repaint();
}

void AppMenuBarComponent::invokeItem(const MenuItemView &view) {
  const auto itemId = view.item.itemID;
  const auto topLevelIndex = openMenuIndex;
  closeMenu();

  if (model != nullptr && itemId != 0)
    model->menuItemSelected(itemId, topLevelIndex);
}

bool AppMenuBarComponent::isPointInActiveArea(juce::Point<int> pos) const {
  if (pos.y >= 0 && pos.y < kMenuBarHeight)
    return true;

  if (openMenuIndex >= 0)
    return true;

  return false;
}
