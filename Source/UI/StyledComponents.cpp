#include "StyledComponents.h"

DarkLookAndFeel::DarkLookAndFeel()
{
    // PopupMenu
    setColour(juce::PopupMenu::backgroundColourId, APP_COLOR_SURFACE);
    setColour(juce::PopupMenu::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, APP_COLOR_PRIMARY);
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);

    // ComboBox
    setColour(juce::ComboBox::backgroundColourId, APP_COLOR_SURFACE);
    setColour(juce::ComboBox::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::ComboBox::outlineColourId, APP_COLOR_BORDER);
    setColour(juce::ComboBox::arrowColourId, APP_COLOR_PRIMARY);
    setColour(juce::ComboBox::focusedOutlineColourId, APP_COLOR_PRIMARY);

    // Label
    setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

    // TextButton
    setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE);
    setColour(juce::TextButton::buttonOnColourId, APP_COLOR_PRIMARY);
    setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);

    // ListBox
    setColour(juce::ListBox::backgroundColourId, APP_COLOR_SURFACE_ALT);
    setColour(juce::ListBox::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::ListBox::outlineColourId, APP_COLOR_BORDER);

    // ScrollBar
    setColour(juce::ScrollBar::thumbColourId, APP_COLOR_PRIMARY.withAlpha(0.7f));
    setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);

    // TextEditor
    setColour(juce::TextEditor::backgroundColourId, APP_COLOR_SURFACE_ALT);
    setColour(juce::TextEditor::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::TextEditor::outlineColourId, APP_COLOR_BORDER);
    setColour(juce::TextEditor::focusedOutlineColourId, APP_COLOR_PRIMARY);
    setColour(juce::CaretComponent::caretColourId, APP_COLOR_PRIMARY);

    // AlertWindow / DialogWindow
    setColour(juce::AlertWindow::backgroundColourId, APP_COLOR_BACKGROUND);
    setColour(juce::AlertWindow::textColourId, APP_COLOR_TEXT_PRIMARY);
    setColour(juce::AlertWindow::outlineColourId, APP_COLOR_BORDER);
}

void DarkLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    g.fillAll(APP_COLOR_SURFACE);
    g.setColour(APP_COLOR_BORDER);
    g.drawRect(0, 0, width, height, 1);
}

void DarkLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area,
                                        bool isSeparator, bool isActive, bool isHighlighted,
                                        bool isTicked, bool hasSubMenu,
                                        const juce::String& text, const juce::String& shortcutKeyText,
                                        const juce::Drawable* icon, const juce::Colour* textColour)
{
    juce::ignoreUnused(hasSubMenu, shortcutKeyText, icon, textColour);

    if (isSeparator)
    {
        auto r = area.reduced(5, 0).withHeight(1).withY(area.getCentreY());
        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.fillRect(r);
        return;
    }

    auto textArea = area.reduced(10, 0);

    if (isHighlighted && isActive)
    {
        g.setColour(APP_COLOR_PRIMARY);
        g.fillRect(area);
        g.setColour(juce::Colours::white);
    }
    else
    {
        g.setColour(isActive ? APP_COLOR_TEXT_PRIMARY : APP_COLOR_TEXT_MUTED);
    }

    g.setFont(juce::Font(juce::FontOptions(15.0f)));
    g.drawFittedText(text, textArea, juce::Justification::centredLeft, 1);

    if (isTicked)
    {
        auto tickArea = area.withLeft(area.getRight() - area.getHeight()).reduced(6);
        g.drawText(juce::String::charToString(0x2713), tickArea, juce::Justification::centred);
    }
}

void DarkLookAndFeel::drawTickBox(juce::Graphics& g, juce::Component& component,
                                   float x, float y, float w, float h,
                                   bool ticked, bool isEnabled, bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown)
{
    juce::ignoreUnused(component, shouldDrawButtonAsDown);

    auto boxSize = std::min(w, h) * 0.9f;
    auto boxX = x + (w - boxSize) * 0.5f;
    auto boxY = y + (h - boxSize) * 0.5f;
    auto cornerSize = boxSize * 0.2f;

    juce::Rectangle<float> boxBounds(boxX, boxY, boxSize, boxSize);

    if (ticked)
    {
        g.setColour(APP_COLOR_PRIMARY);
        g.fillRoundedRectangle(boxBounds, cornerSize);

        g.setColour(juce::Colours::white);
        auto tick = boxBounds.reduced(boxSize * 0.25f);
        juce::Path path;
        path.startNewSubPath(tick.getX(), tick.getCentreY());
        path.lineTo(tick.getX() + tick.getWidth() * 0.35f, tick.getBottom());
        path.lineTo(tick.getRight(), tick.getY());
        g.strokePath(path, juce::PathStrokeType(2.0f));
    }
    else
    {
        auto alpha = isEnabled ? (shouldDrawButtonAsHighlighted ? 1.0f : 0.7f) : 0.4f;
        g.setColour(APP_COLOR_BORDER.withAlpha(alpha));
        g.drawRoundedRectangle(boxBounds, cornerSize, 1.5f);
    }
}

void DarkLookAndFeel::drawProgressBar(juce::Graphics& g, juce::ProgressBar& bar,
                                       int width, int height, double progress,
                                       const juce::String& textToShow)
{
    auto background = bar.findColour(juce::ProgressBar::backgroundColourId);
    auto foreground = bar.findColour(juce::ProgressBar::foregroundColourId);

    auto barBounds = juce::Rectangle<float>(0.0f, 0.0f, (float)width, (float)height);
    auto cornerSize = 4.0f;

    // Background
    g.setColour(background);
    g.fillRoundedRectangle(barBounds, cornerSize);

    // Foreground bar
    if (progress >= 0.0 && progress <= 1.0)
    {
        auto fillBounds = barBounds.withWidth(barBounds.getWidth() * (float)progress);
        g.setColour(foreground);
        g.fillRoundedRectangle(fillBounds, cornerSize);
    }
    else
    {
        // Indeterminate: draw animated bar
        auto time = juce::Time::getMillisecondCounter();
        auto pos = (float)(time % 1000) / 1000.0f;
        auto barWidth = barBounds.getWidth() * 0.3f;
        auto x = barBounds.getX() + (barBounds.getWidth() - barWidth) * pos;
        g.setColour(foreground);
        g.fillRoundedRectangle(x, barBounds.getY(), barWidth, barBounds.getHeight(), cornerSize);
    }

    // Text (white color)
    if (textToShow.isNotEmpty())
    {
        g.setColour(juce::Colours::white);
        g.setFont((float)height * 0.6f);
        g.drawText(textToShow, barBounds, juce::Justification::centred, false);
    }
}
