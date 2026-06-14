#include "ToolbarComponent.h"
#include "PianoRollComponent.h"  // For EditMode enum
#include "StyledComponents.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/SvgUtils.h"
#include "../Utils/UI/TimecodeFont.h"
#include "BinaryData.h"

ToolbarComponent::ToolbarComponent()
{
    // Load SVG icons with white tint
    auto playIcon = SvgUtils::loadSvg(BinaryData::playline_svg, BinaryData::playline_svgSize, juce::Colours::white);
    auto pauseIcon = SvgUtils::loadSvg(BinaryData::pauseline_svg, BinaryData::pauseline_svgSize, juce::Colours::white);
    auto stopIcon = SvgUtils::loadSvg(BinaryData::stopline_svg, BinaryData::stopline_svgSize, juce::Colours::white);
    auto startIcon = SvgUtils::loadSvg(BinaryData::movestartline_svg, BinaryData::movestartline_svgSize, juce::Colours::white);
    auto endIcon = SvgUtils::loadSvg(BinaryData::moveendline_svg, BinaryData::moveendline_svgSize, juce::Colours::white);
    auto cursorIcon = SvgUtils::loadSvg(BinaryData::cursor_24_filled_svg, BinaryData::cursor_24_filled_svgSize, juce::Colours::white);
    auto stretchIcon = SvgUtils::loadSvg(BinaryData::stretch_24_filled_svg, BinaryData::stretch_24_filled_svgSize, juce::Colours::white);
    auto pitchEditIcon = SvgUtils::loadSvg(BinaryData::pitch_edit_24_filled_svg, BinaryData::pitch_edit_24_filled_svgSize, juce::Colours::white);
    auto scissorsIcon = SvgUtils::loadSvg(BinaryData::scissors_24_filled_svg, BinaryData::scissors_24_filled_svgSize, juce::Colours::white);
    auto followIcon = SvgUtils::loadSvg(BinaryData::follow24filled_svg, BinaryData::follow24filled_svgSize, juce::Colours::white);
    auto loopIcon = SvgUtils::loadSvg(BinaryData::loop24filled_svg, BinaryData::loop24filled_svgSize, juce::Colours::white);
    const juce::String parametersIconSvg =
        R"(<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><rect x="3" y="2" width="2" height="20" rx="1"/><circle cx="4" cy="9" r="3"/><rect x="11" y="2" width="2" height="20" rx="1"/><circle cx="12" cy="15" r="3"/><rect x="19" y="2" width="2" height="20" rx="1"/><circle cx="20" cy="6" r="3"/></svg>)";
    auto parametersIcon = SvgUtils::createDrawableFromSvg(parametersIconSvg, juce::Colours::white);
    const juce::String voicebankIconSvg =
        R"(<svg viewBox="0 0 24 24" fill="currentColor" xmlns="http://www.w3.org/2000/svg"><path d="M12 2a3.5 3.5 0 0 0-3.5 3.5v5a3.5 3.5 0 0 0 7 0v-5A3.5 3.5 0 0 0 12 2z"/><path d="M6 9.5a1 1 0 0 0-2 0A8 8 0 0 0 11 17.43V20H9a1 1 0 1 0 0 2h6a1 1 0 1 0 0-2h-2v-2.57A8 8 0 0 0 20 9.5a1 1 0 0 0-2 0 6 6 0 0 1-12 0z"/></svg>)";
    auto voicebankIcon = SvgUtils::createDrawableFromSvg(voicebankIconSvg, juce::Colours::white);

    playButton.setImages(playIcon.get());
    stopButton.setImages(stopIcon.get());
    goToStartButton.setImages(startIcon.get());
    goToEndButton.setImages(endIcon.get());
    selectModeButton.setImages(cursorIcon.get());
    stretchModeButton.setImages(stretchIcon.get());
    drawModeButton.setImages(pitchEditIcon.get());
    splitModeButton.setImages(scissorsIcon.get());
    hnsepModeButton.setImages(parametersIcon.get());
    followButton.setImages(followIcon.get());
    loopButton.setImages(loopIcon.get());
    parametersButton.setImages(parametersIcon.get());
    voicebankButton.setImages(voicebankIcon.get());

    // Set edge indent for icon padding (makes icons smaller within button bounds)
    goToStartButton.setEdgeIndent(4);
    playButton.setEdgeIndent(6);
    stopButton.setEdgeIndent(6);
    goToEndButton.setEdgeIndent(4);
    selectModeButton.setEdgeIndent(6);
    stretchModeButton.setEdgeIndent(6);
    drawModeButton.setEdgeIndent(6);
    splitModeButton.setEdgeIndent(6);
    hnsepModeButton.setEdgeIndent(6);
    followButton.setEdgeIndent(6);
    loopButton.setEdgeIndent(6);
    parametersButton.setEdgeIndent(6);
    voicebankButton.setEdgeIndent(6);

    // Store pause icon for later use
    pauseDrawable = std::move(pauseIcon);
    playDrawable = SvgUtils::loadSvg(BinaryData::playline_svg, BinaryData::playline_svgSize, juce::Colours::white);

    // Configure buttons
    addAndMakeVisible(goToStartButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(goToEndButton);
    addAndMakeVisible(selectModeButton);
    addAndMakeVisible(stretchModeButton);
    addAndMakeVisible(drawModeButton);
    addAndMakeVisible(splitModeButton);
    addAndMakeVisible(hnsepModeButton);
    addAndMakeVisible(followButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(parametersButton);
    addAndMakeVisible(voicebankButton);

    // Plugin mode buttons (hidden by default)
    addChildComponent(reanalyzeButton);
    addChildComponent(araModeLabel);

    // ARA mode label style (background drawn in paint() for rounded corners)
    araModeLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    araModeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    araModeLabel.setJustificationType(juce::Justification::centred);
    araModeLabel.setFont(
        juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));

    goToStartButton.addListener(this);
    playButton.addListener(this);
    stopButton.addListener(this);
    goToEndButton.addListener(this);
    selectModeButton.addListener(this);
    stretchModeButton.addListener(this);
    drawModeButton.addListener(this);
    splitModeButton.addListener(this);
    hnsepModeButton.addListener(this);
    followButton.addListener(this);
    loopButton.addListener(this);
    parametersButton.addListener(this);
    voicebankButton.addListener(this);
    reanalyzeButton.addListener(this);

    // Set localized text (tooltips for icon buttons)
    selectModeButton.setTooltip(TR("toolbar.select") + " (1)");
    stretchModeButton.setTooltip(TR("toolbar.stretch") + " (2)");
    drawModeButton.setTooltip(TR("toolbar.draw") + " (3)");
    splitModeButton.setTooltip(TR("toolbar.split") + " (4)");
    hnsepModeButton.setTooltip("HNSep Parameters (5)");
    followButton.setTooltip(TR("toolbar.follow") + " (6)");
    loopButton.setTooltip(TR("toolbar.loop") + " (7)");
    parametersButton.setTooltip(TR("panel.parameters"));
    voicebankButton.setTooltip(TR("panel.voicebank"));
    reanalyzeButton.setButtonText(TR("toolbar.reanalyze"));
    zoomLabel.setText(TR("toolbar.zoom"), juce::dontSendNotification);

    // Style reanalyze button
    reanalyzeButton.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE);
    reanalyzeButton.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_PRIMARY);

    // Set default active states
    selectModeButton.setActive(true);
    followButton.setActive(true);  // Follow is on by default
    loopButton.setActive(false);
    parametersButton.setActive(false);
    voicebankButton.setActive(false);

    // Time label with app font (larger and bold for readability)
    addAndMakeVisible(timeLabel);
    timeLabel.setText("00:00.000 / 00:00.000", juce::dontSendNotification);
    timeLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    timeLabel.setJustificationType(juce::Justification::centred);
    timeLabel.setFont(TimecodeFont::getBoldFont(21.0f).withHorizontalScale(0.92f));

    // Zoom slider
    addAndMakeVisible(zoomLabel);
    addAndMakeVisible(zoomSlider);

    zoomLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);

    zoomSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 1.0);
    zoomSlider.setValue(100.0);
    zoomSlider.setSkewFactorFromMidPoint(200.0);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.addListener(this);

    zoomSlider.setColour(juce::Slider::backgroundColourId, APP_COLOR_SURFACE_ALT);
    zoomSlider.setColour(juce::Slider::trackColourId, APP_COLOR_PRIMARY.withAlpha(0.75f));
    zoomSlider.setColour(juce::Slider::thumbColourId, APP_COLOR_PRIMARY);

    // Progress bar (hidden by default)
    addChildComponent(progressBar);
    addChildComponent(progressLabel);

    progressLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    progressLabel.setJustificationType(juce::Justification::centredLeft);
    progressBar.setColour(juce::ProgressBar::foregroundColourId, APP_COLOR_PRIMARY);
    progressBar.setColour(juce::ProgressBar::backgroundColourId, APP_COLOR_SURFACE_ALT);
    progressBar.setLookAndFeel(&DarkLookAndFeel::getInstance());
    
    // Status label (hidden by default)
    addChildComponent(statusLabel);
    statusLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
}

ToolbarComponent::~ToolbarComponent()
{
    progressBar.setLookAndFeel(nullptr);
}

void ToolbarComponent::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    juce::ColourGradient bgGradient(
        APP_COLOR_SURFACE_ALT, bounds.getX(), bounds.getY(),
        APP_COLOR_BACKGROUND, bounds.getX(), bounds.getBottom(), false);
    g.setGradientFill(bgGradient);
    g.fillAll();

    // Draw rounded background for tool buttons container
    if (!toolContainerBounds.isEmpty())
    {
        auto toolBounds = toolContainerBounds.toFloat();
        juce::ColourGradient toolGradient(
            APP_COLOR_SURFACE_RAISED, toolBounds.getX(), toolBounds.getY(),
            APP_COLOR_SURFACE, toolBounds.getX(), toolBounds.getBottom(), false);
        g.setGradientFill(toolGradient);
        g.fillRoundedRectangle(toolBounds, 8.0f);
        g.setColour(APP_COLOR_BORDER);
        g.drawRoundedRectangle(toolBounds.reduced(0.5f), 8.0f, 1.0f);
    }

    // Draw rounded background for time label
    if (timeLabel.isVisible())
    {
        auto timeBounds = timeLabel.getBounds().toFloat();
        juce::ColourGradient timeGradient(
            APP_COLOR_SURFACE_RAISED, timeBounds.getX(), timeBounds.getY(),
            APP_COLOR_SURFACE, timeBounds.getX(), timeBounds.getBottom(), false);
        g.setGradientFill(timeGradient);
        g.fillRoundedRectangle(timeBounds, 8.0f);
        g.setColour(APP_COLOR_BORDER);
        g.drawRoundedRectangle(timeBounds.reduced(0.5f), 8.0f, 1.0f);
    }

    // Draw rounded background for ARA mode label
    if (pluginMode && araModeLabel.isVisible())
    {
        auto araBounds = araModeLabel.getBounds().toFloat();
        juce::ColourGradient araGradient(
            APP_COLOR_PRIMARY, araBounds.getX(), araBounds.getY(),
            APP_COLOR_PRIMARY.darker(0.2f), araBounds.getX(), araBounds.getBottom(), false);
        g.setGradientFill(araGradient);
        g.fillRoundedRectangle(araBounds, 8.0f);
    }
}

void ToolbarComponent::resized()
{
    auto bounds = getLocalBounds().reduced(10, 6);

    // Right side - voicebank + parameters buttons + status/progress
    const int rightButtonSize = 28;
    const int rightButtonSpacing = 4;
    auto rightBounds = bounds.removeFromRight(240);
    auto rightButtonArea = rightBounds.removeFromRight(rightButtonSize * 2 + rightButtonSpacing + 10);
    const int rightButtonY =
        rightButtonArea.getY() + (rightButtonArea.getHeight() - rightButtonSize) / 2;
    int rightBtnX = rightButtonArea.getRight() - rightButtonSize;
    parametersButton.setBounds(rightBtnX, rightButtonY,
                               rightButtonSize, rightButtonSize);
    rightBtnX -= (rightButtonSize + rightButtonSpacing);
    voicebankButton.setBounds(rightBtnX, rightButtonY,
                              rightButtonSize, rightButtonSize);

    if (showingStatus && !showingProgress)
    {
        statusLabel.setBounds(rightBounds.removeFromLeft(140));
    }
    if (showingProgress)
    {
        auto progressArea = rightBounds.withWidth(std::min(200, rightBounds.getWidth()));
        const int progressBarHeight = progressArea.getHeight() / 2;
        progressLabel.setBounds(progressArea.removeFromTop(progressArea.getHeight() - progressBarHeight));
        progressBar.setBounds(progressArea.withHeight(progressBarHeight));
    }

    // Hide zoom controls
    zoomLabel.setVisible(false);
    zoomSlider.setVisible(false);

    // Left section - playback + time
    int currentX = bounds.getX();
    const int timeWidth = 170;

    // Playback controls (or plugin mode buttons)
    if (pluginMode)
    {
        araModeLabel.setBounds(currentX, bounds.getY(), 90, bounds.getHeight());
        currentX += 98;
        reanalyzeButton.setBounds(currentX, bounds.getY(), 110, bounds.getHeight());
        currentX += 118;
    }
    else
    {
        goToStartButton.setBounds(currentX, bounds.getY() + 4, 28, bounds.getHeight() - 8);
        currentX += 32;
        playButton.setBounds(currentX, bounds.getY() + 4, 28, bounds.getHeight() - 8);
        currentX += 32;
        stopButton.setBounds(currentX, bounds.getY() + 4, 28, bounds.getHeight() - 8);
        currentX += 32;
        goToEndButton.setBounds(currentX, bounds.getY() + 4, 28, bounds.getHeight() - 8);
        currentX += 36;
    }

    // Time display on the left cluster
    timeLabel.setBounds(currentX, bounds.getY() + 2, timeWidth, bounds.getHeight() - 4);
    currentX += timeWidth + 16;

    // Center section - edit mode buttons
    const int toolButtonSize = 32;
    const int toolContainerPadding = 4;
    const int numToolButtons = pluginMode ? 5 : 7;
    const int toolContainerWidth = toolButtonSize * numToolButtons + toolContainerPadding * 2;
    const int centerWidth = bounds.getRight() - currentX;
    const int centerX = currentX + std::max(0, (centerWidth - toolContainerWidth) / 2);
    toolContainerBounds = juce::Rectangle<int>(centerX, bounds.getY() + 2, toolContainerWidth, bounds.getHeight() - 4);
    auto toolArea = toolContainerBounds.reduced(toolContainerPadding, toolContainerPadding);
    int toolX = toolArea.getX();
    selectModeButton.setBounds(toolX, toolArea.getY(), toolButtonSize, toolArea.getHeight());
    toolX += toolButtonSize;
    stretchModeButton.setBounds(toolX, toolArea.getY(), toolButtonSize, toolArea.getHeight());
    toolX += toolButtonSize;
    drawModeButton.setBounds(toolX, toolArea.getY(), toolButtonSize, toolArea.getHeight());
    toolX += toolButtonSize;
    splitModeButton.setBounds(toolX, toolArea.getY(), toolButtonSize, toolArea.getHeight());
    toolX += toolButtonSize;
    hnsepModeButton.setBounds(toolX, toolArea.getY(), toolButtonSize, toolArea.getHeight());
    toolX += toolButtonSize;
    if (!pluginMode)
        followButton.setBounds(toolX, toolArea.getY(), toolButtonSize, toolArea.getHeight());
    if (!pluginMode)
    {
        toolX += toolButtonSize;
        loopButton.setBounds(toolX, toolArea.getY(), toolButtonSize, toolArea.getHeight());
    }
}

void ToolbarComponent::buttonClicked(juce::Button* button)
{
    if (button == &goToStartButton && onGoToStart)
        onGoToStart();
    else if (button == &goToEndButton && onGoToEnd)
        onGoToEnd();
    else if (button == &playButton)
    {
        if (isPlaying)
        {
            if (onPause)
                onPause();
        }
        else
        {
            if (onPlay)
                onPlay();
        }
    }
    else if (button == &stopButton && onStop)
        onStop();
    else if (button == &reanalyzeButton && onReanalyze)
        onReanalyze();
    else if (button == &selectModeButton)
    {
        setEditMode(EditMode::Select);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Select);
    }
    else if (button == &stretchModeButton)
    {
        setEditMode(EditMode::Stretch);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Stretch);
    }
    else if (button == &drawModeButton)
    {
        setEditMode(EditMode::Draw);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Draw);
    }
    else if (button == &splitModeButton)
    {
        setEditMode(EditMode::Split);
        if (onEditModeChanged)
            onEditModeChanged(EditMode::Split);
    }
    else if (button == &hnsepModeButton)
    {
        if (onHNSepRequested)
            onHNSepRequested();
    }
    else if (button == &followButton)
    {
        followPlayback = !followPlayback;
        followButton.setActive(followPlayback);
    }
    else if (button == &loopButton)
    {
        loopEnabled = !loopEnabled;
        loopButton.setActive(loopEnabled);
        if (onLoopToggled)
            onLoopToggled(loopEnabled);
    }
    else if (button == &parametersButton)
    {
        parametersVisible = !parametersVisible;
        parametersButton.setActive(parametersVisible);
        if (onToggleParameters)
            onToggleParameters(parametersVisible);
    }
    else if (button == &voicebankButton)
    {
        voicebankVisible = !voicebankVisible;
        voicebankButton.setActive(voicebankVisible);
        if (onToggleVoicebank)
            onToggleVoicebank(voicebankVisible);
    }
}

void ToolbarComponent::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &zoomSlider && onZoomChanged)
        onZoomChanged(static_cast<float>(slider->getValue()));
}

void ToolbarComponent::setPlaying(bool playing)
{
    isPlaying = playing;
    playButton.setImages(playing ? pauseDrawable.get() : playDrawable.get());
}

void ToolbarComponent::setCurrentTime(double time)
{
    currentTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setTotalTime(double time)
{
    totalTime = time;
    updateTimeDisplay();
}

void ToolbarComponent::setEditMode(EditMode mode)
{
    currentEditModeInt = static_cast<int>(mode);
    selectModeButton.setActive(mode == EditMode::Select);
    stretchModeButton.setActive(mode == EditMode::Stretch);
    drawModeButton.setActive(mode == EditMode::Draw);
    splitModeButton.setActive(mode == EditMode::Split);
    hnsepModeButton.setActive(hnsepVisible);
}

void ToolbarComponent::setZoom(float pixelsPerSecond)
{
    // Update slider without triggering callback
    zoomSlider.setValue(pixelsPerSecond, juce::dontSendNotification);
}

void ToolbarComponent::setLoopEnabled(bool enabled)
{
    loopEnabled = enabled;
    loopButton.setActive(loopEnabled);
}

void ToolbarComponent::setParametersVisible(bool visible)
{
    parametersVisible = visible;
    parametersButton.setActive(parametersVisible);
}

void ToolbarComponent::setVoicebankVisible(bool visible)
{
    voicebankVisible = visible;
    voicebankButton.setActive(voicebankVisible);
}

void ToolbarComponent::setHNSepVisible(bool visible)
{
    hnsepVisible = visible;
    hnsepModeButton.setActive(hnsepVisible);
}

void ToolbarComponent::showProgress(const juce::String& message)
{
    showingProgress = true;
    progressLabel.setText(message, juce::dontSendNotification);
    progressLabel.setVisible(true);
    progressBar.setVisible(true);
    progressValue = -1.0;  // Indeterminate
    resized();
    repaint();
}

void ToolbarComponent::hideProgress()
{
    showingProgress = false;
    progressLabel.setVisible(false);
    progressBar.setVisible(false);
    resized();
    repaint();
}

void ToolbarComponent::setProgress(float progress)
{
    if (progress < 0)
        progressValue = -1.0;  // Indeterminate
    else
        progressValue = static_cast<double>(juce::jlimit(0.0f, 1.0f, progress));
}

void ToolbarComponent::setStatusMessage(const juce::String& message)
{
    if (message.isEmpty())
    {
        showingStatus = false;
        statusLabel.setVisible(false);
    }
    else
    {
        showingStatus = true;
        statusLabel.setText(message, juce::dontSendNotification);
        statusLabel.setVisible(true);
    }
    resized();
    repaint();
}

void ToolbarComponent::updateTimeDisplay()
{
    timeLabel.setText(formatTime(currentTime) + " / " + formatTime(totalTime),
                      juce::dontSendNotification);
}

juce::String ToolbarComponent::formatTime(double seconds)
{
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int ms = static_cast<int>((seconds - std::floor(seconds)) * 1000);

    return juce::String::formatted("%02d:%02d.%03d", mins, secs, ms);
}

void ToolbarComponent::mouseDown(const juce::MouseEvent& e)
{
#if JUCE_MAC
    if (auto* window = getTopLevelComponent())
        dragger.startDraggingComponent(window, e.getEventRelativeTo(window));
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDrag(const juce::MouseEvent& e)
{
#if JUCE_MAC
    if (auto* window = getTopLevelComponent())
        dragger.dragComponent(window, e.getEventRelativeTo(window), nullptr);
#else
    juce::ignoreUnused(e);
#endif
}

void ToolbarComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
}

void ToolbarComponent::setPluginMode(bool isPlugin)
{
    pluginMode = isPlugin;

    goToStartButton.setVisible(!isPlugin);
    playButton.setVisible(!isPlugin);
    stopButton.setVisible(!isPlugin);
    goToEndButton.setVisible(!isPlugin);
    reanalyzeButton.setVisible(isPlugin);
    araModeLabel.setVisible(isPlugin);

    // In plugin mode, hide follow button (host controls playback)
    followButton.setVisible(!isPlugin);
    loopButton.setVisible(!isPlugin);

    resized();
}

void ToolbarComponent::setARAMode(bool isARA)
{
    araMode = isARA;
    araModeLabel.setText(isARA ? TR("toolbar.ara_mode") : TR("toolbar.non_ara"), juce::dontSendNotification);
}
