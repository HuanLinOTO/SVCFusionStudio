#include "TrackListComponent.h"
#include "../Audio/EditorController.h"
#include "StyledComponents.h"

static juce::Colour getTypeColor(TrackType t) {
    return (t == TrackType::Vocal) ? juce::Colour(0xff6ab0ff) : juce::Colour(0xff88cc88);
}

static juce::Colour colormapFor(int idx, float v) {
    v = juce::jlimit(0.0f, 1.0f, v);
    switch (idx) {
        case 1: { float h = 0.55f + v * 0.12f; return juce::Colour::fromHSV(h, 0.5f + v * 0.2f, 0.55f + v * 0.35f, 0.85f); }
        case 2: { float h = 0.02f + v * 0.08f; return juce::Colour::fromHSV(h, 0.6f + v * 0.2f, 0.7f + v * 0.3f, 0.85f); }
        case 3: { float h = 0.75f - v * 0.75f; return juce::Colour::fromHSV(h, 0.3f, 0.9f, 0.7f); }
        case 4: { float h = 0.85f - v * 0.85f; return juce::Colour::fromHSV(h, 0.5f + v * 0.3f, 0.5f + v * 0.45f, 0.85f); }
        case 5: { float h = 0.75f - v * 0.55f; return juce::Colour::fromHSV(h, 0.45f + v * 0.25f, 0.55f + v * 0.4f, 0.85f); }
        default: { float h = 0.7f - v * 0.7f; return juce::Colour::fromHSV(h, 0.45f, 0.8f, 0.85f); }
    }
}

TrackListComponent::TrackItem::TrackItem(TrackListComponent& o, int idx)
    : owner(o), trackIndex(idx),
      muteButton("M"), soloButton("S")
{
    muteButton.setClickingTogglesState(true);
    muteButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3a3a));
    muteButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffe05848));
    muteButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    muteButton.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_MUTED);
    muteButton.setTooltip(TR("track.mute"));

    soloButton.setClickingTogglesState(true);
    soloButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3a3a));
    soloButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffe8b33d));
    soloButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    soloButton.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_MUTED);
    soloButton.setTooltip(TR("track.solo"));

    typeCombo.addItem(TR("track.accompaniment"), 1);
    typeCombo.addItem(TR("track.vocal"), 2);
    typeCombo.setSelectedId(1, juce::dontSendNotification);

    volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 36, 16);
    volumeSlider.setRange(-24.0, 6.0, 0.1);
    volumeSlider.setValue(0.0);
    volumeSlider.setColour(juce::Slider::textBoxTextColourId, APP_COLOR_TEXT_PRIMARY);
    volumeSlider.setColour(juce::Slider::textBoxBackgroundColourId, APP_COLOR_SURFACE);
    volumeSlider.setColour(juce::Slider::textBoxOutlineColourId, APP_COLOR_BORDER_SUBTLE);
    volumeSlider.setColour(juce::Slider::thumbColourId, juce::Colour(0xff4fc3f7));
    volumeSlider.setColour(juce::Slider::trackColourId, juce::Colour(0xff5a8acc));
    volumeSlider.setColour(juce::Slider::backgroundColourId, juce::Colour(0xff333344));

    addAndMakeVisible(muteButton);
    addAndMakeVisible(soloButton);
    addAndMakeVisible(typeCombo);
    addAndMakeVisible(volumeSlider);

    setInterceptsMouseClicks(true, false);

    muteButton.onClick = [this]() {
        if (!owner.editorController) return;
        auto* track = owner.editorController->getTrack(trackIndex);
        if (!track) return;
        track->mute = !track->mute;
        owner.editorController->refreshAudioEngine(true);
        updateFromTrack();
        if (owner.onTracksChanged) owner.onTracksChanged();
    };

    soloButton.onClick = [this]() {
        if (!owner.editorController) return;
        auto* track = owner.editorController->getTrack(trackIndex);
        if (!track) return;
        track->solo = !track->solo;
        owner.editorController->refreshAudioEngine(true);
        updateFromTrack();
        if (owner.onTracksChanged) owner.onTracksChanged();
    };

    typeCombo.onChange = [this]() {
        TrackType newType = (typeCombo.getSelectedId() == 2) ? TrackType::Vocal : TrackType::Accompaniment;
        // Always notify user changes. During accompaniment->vocal analysis the
        // model type remains Accompaniment until completion, so switching back
        // to Accompaniment would otherwise be filtered out and fail to abort.
        if (owner.onTrackTypeChanged)
            owner.onTrackTypeChanged(trackIndex, newType);
    };

    volumeSlider.onValueChange = [this]() {
        if (!owner.editorController) return;
        auto* track = owner.editorController->getTrack(trackIndex);
        if (!track) return;
        track->setVolume(static_cast<float>(volumeSlider.getValue()));
    };
    volumeSlider.onDragEnd = [this]() {
        if (owner.editorController)
            owner.editorController->refreshAudioEngine(true);
    };
}

TrackListComponent::TrackItem::~TrackItem() = default;

void TrackListComponent::TrackItem::updateFromTrack()
{
    if (!owner.editorController) return;
    auto* track = owner.editorController->getTrack(trackIndex);
    if (!track) return;

    trackName = track->name;
    trackType = track->type;
    isActive = (owner.editorController->getActiveTrackIndex() == trackIndex);
    isMuted = track->mute;
    isSoloed = track->solo;

    typeCombo.setSelectedId((trackType == TrackType::Vocal) ? 2 : 1, juce::dontSendNotification);

    auto* proj = track->getProject();
    if (proj) {
        auto& audio = proj->getAudioData();
        audioData = audio.waveform.getReadPointer(0);
        audioNumSamples = audio.waveform.getNumSamples();
        audioSampleRate = audio.sampleRate;
        volumeSlider.setValue(track->getVolume(), juce::dontSendNotification);
    } else {
        audioData = nullptr;
        audioNumSamples = 0;
    }

    muteButton.setToggleState(isMuted, juce::dontSendNotification);
    soloButton.setToggleState(isSoloed, juce::dontSendNotification);

    repaint();
}

void TrackListComponent::TrackItem::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    auto bg = isActive ? APP_COLOR_SURFACE_RAISED : APP_COLOR_SURFACE;
    if (trackIndex % 2 == 0)
        bg = bg.darker(0.03f);
    g.setColour(bg);
    g.fillAll();

    // Clear separator line between tracks
    g.setColour(APP_COLOR_BORDER);
    g.drawHorizontalLine(bounds.getBottom() - 1.0f, bounds.getX(), bounds.getRight());

    // Vertical separator between header and waveform
    int hw = owner.headerWidth;
    g.setColour(APP_COLOR_BORDER);
    g.drawVerticalLine(static_cast<float>(hw), bounds.getY(), bounds.getBottom());

    if (isActive) {
        g.setColour(APP_COLOR_PRIMARY);
        g.fillRect(bounds.getX(), bounds.getY(), 3.0f, bounds.getHeight());
    }

    int leftPad = 12;

    g.setColour(getTypeColor(trackType));
    g.fillEllipse(static_cast<float>(leftPad), bounds.getY() + 8, 10.0f, 10.0f);

    g.setColour(APP_COLOR_TEXT_PRIMARY);
    g.setFont(AppFont::getFont(14.0f));
    {
        int nameWidth = hw - leftPad - 40;
        juce::String displayName = trackName;
        if (g.getCurrentFont().getStringWidth(displayName) > nameWidth) {
            while (g.getCurrentFont().getStringWidth(displayName + "...") > nameWidth &&
                   displayName.length() > 1) {
                displayName = displayName.dropLastCharacters(1);
            }
            displayName += "...";
        }
        g.drawText(displayName,
                   leftPad + 16, bounds.getY() + 4, hw - leftPad - 40, 20,
                   juce::Justification::left);
    }

    g.setColour(APP_COLOR_TEXT_MUTED);
    g.setFont(AppFont::getFont(11.0f));
    g.drawText("Vol", leftPad, bounds.getY() + 57, 20, 14,
               juce::Justification::left);

    int wfLeft = hw;
    int wfWidth = getWidth() - hw;
    int wfHeight = getHeight() - 16;
    int wfY = 8;

    if (audioData != nullptr && audioNumSamples > 0 && wfWidth > 10) {
        float mid = wfY + wfHeight * 0.5f;
        float pps = owner.getEffectivePps();
        double scrollSec = owner.trackScrollSec;
        double samplesPerSec = static_cast<double>(audioSampleRate);
        double totalSamples = static_cast<double>(audioNumSamples);

        for (int x = 0; x < wfWidth; ++x) {
            double timeStart = scrollSec + (static_cast<double>(x) / pps);
            double timeEnd = scrollSec + (static_cast<double>(x + 1) / pps);
            if (timeEnd < 0.0 || timeStart > owner.totalDuration) continue;

            int sampleStart = static_cast<int>(timeStart * samplesPerSec);
            int sampleEnd = static_cast<int>(timeEnd * samplesPerSec);
            if (sampleStart < 0) sampleStart = 0;
            if (sampleEnd >= audioNumSamples) sampleEnd = audioNumSamples - 1;
            if (sampleStart > sampleEnd) sampleStart = sampleEnd;

            float maxVal = 0.0f;
            for (int i = sampleStart; i <= sampleEnd; ++i) {
                float v = std::abs(audioData[i]);
                if (v > maxVal) maxVal = v;
            }

            float h = maxVal * wfHeight * 0.5f;
            if (owner.rainbowWaveform) {
                g.setColour(colormapFor(owner.colormapIndex, maxVal));
            } else {
                g.setColour(juce::Colour(0xff8a9bbf));
            }
            g.drawVerticalLine(wfLeft + x, mid - h, mid + h);
        }

        // Playhead: pixel position based on zoom/scroll
        float phX = wfLeft + owner.timeToPixel(owner.playheadPosition);
        if (phX >= wfLeft && phX <= wfLeft + wfWidth) {
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawVerticalLine(static_cast<int>(phX), bounds.getY(), bounds.getBottom());
        }
    }

    if (isMuted) {
        g.setColour(juce::Colour(0xffe05848).withAlpha(0.12f));
        g.fillAll();
    }

    // Progress overlay (active = bars, queued = badge; both can coexist)
    if (progress.active || progress.queued) {
        int wfLeft = hw;
        int wfWidth = getWidth() - hw;

        // Dim the waveform area when progress bars are shown
        if (progress.active) {
            g.setColour(juce::Colour(0xff000000).withAlpha(0.5f));
            g.fillRect(wfLeft, 0, wfWidth, getHeight());
        }

        // Queued pill badge — rendered independent of progress bars
        if (progress.queued) {
            juce::String label = progress.queuedLabel.isNotEmpty()
                                     ? progress.queuedLabel
                                     : TR("tracks.queued_analysis");
            g.setFont(AppFont::getFont(12.0f));
            int textW = juce::jmin(wfWidth - 16,
                                   g.getCurrentFont().getStringWidth(label) + 28);
            int badgeH = 22;
            int badgeX = wfLeft + 8;
            int badgeY = (getHeight() - badgeH) / 2;
            juce::Rectangle<float> badge((float)badgeX, (float)badgeY,
                                         (float)textW, (float)badgeH);
            g.setColour(APP_COLOR_SURFACE_RAISED);
            g.fillRoundedRectangle(badge, badgeH * 0.5f);
            g.setColour(APP_COLOR_BORDER_HIGHLIGHT);
            g.drawRoundedRectangle(badge.reduced(0.5f), badgeH * 0.5f, 1.0f);
            g.setColour(APP_COLOR_PRIMARY_GLOW);
            g.drawText(label, badge.toNearestInt(), juce::Justification::centred);
            if (!progress.active)
                return; // badge only, nothing else to draw
        }

        // Step text: "2/6 Mel spectrogram"
        juce::String stepText = juce::String(progress.step) + "/" + juce::String(progress.totalSteps) + " " + progress.message;
        g.setColour(juce::Colours::white);
        g.setFont(AppFont::getFont(12.0f));
        g.drawText(stepText, wfLeft + 8, 4, wfWidth - 16, 16,
                   juce::Justification::left);

        // Main progress bar (step level)
        int barY = 24;
        int barH = 6;
        int barW = wfWidth - 16;
        int barX = wfLeft + 8;

        float stepRatio = (progress.totalSteps > 0)
            ? static_cast<float>(progress.step - 1) / progress.totalSteps
            : 0.0f;
        float subRatio = (progress.subProgress >= 0.0f)
            ? static_cast<float>(progress.subProgress)
            : 0.0f;
        float totalRatio = stepRatio + subRatio / progress.totalSteps;

        // Bar background
        g.setColour(juce::Colour(0xff333344));
        g.fillRect(barX, barY, barW, barH);

        // Bar fill
        g.setColour(juce::Colour(0xff4fc3f7));
        g.fillRect(barX, barY, static_cast<int>(barW * totalRatio), barH);

        // Sub-progress bar (chunk level) — only if subProgress >= 0
        if (progress.subProgress >= 0.0f) {
            int subBarY = barY + barH + 2;
            int subBarH = 3;
            g.setColour(juce::Colour(0xff2a3a4a));
            g.fillRect(barX, subBarY, barW, subBarH);
            g.setColour(juce::Colour(0xff88ccff));
            g.fillRect(barX, subBarY, static_cast<int>(barW * subRatio), subBarH);
        }
    }
}

void TrackListComponent::TrackItem::resized()
{
    int hw = owner.headerWidth;
    int btnW = 26;
    int btnH = 22;
    int btnY = 28;
    typeCombo.setBounds(12, btnY, 70, btnH);
    muteButton.setBounds(12 + 70 + 4, btnY, btnW, btnH);
    soloButton.setBounds(12 + 70 + 4 + btnW + 4, btnY, btnW, btnH);
    int volLabelW = 22;
    volumeSlider.setBounds(12 + volLabelW, 54, hw - 24 - volLabelW, 20);
}

void TrackListComponent::TrackItem::mouseDown(const juce::MouseEvent& e)
{
    if (e.eventComponent == this) {
        if (e.mods.isRightButtonDown()) {
            // Right-click: show context menu
            juce::PopupMenu menu;
            menu.addSectionHeader(trackName);
            menu.addSeparator();
            menu.addItem(TR("track.copy"), [this]() {
                if (owner.onTrackCopied)
                    owner.onTrackCopied(trackIndex);
            });
            menu.addSeparator();
            menu.addItem(TR("track.delete"), [this]() {
                if (owner.onTrackDeleted)
                    owner.onTrackDeleted(trackIndex);
            });
            menu.showMenuAsync(juce::PopupMenu::Options()
                .withTargetScreenArea(juce::Rectangle<int>(
                    e.getScreenX(), e.getScreenY(), 1, 1)));
            return;
        }

        if (owner.onTrackSelected)
            owner.onTrackSelected(trackIndex);
        int hw = owner.headerWidth;
        if (e.x >= hw && owner.totalDuration > 0.0 && owner.onSeek) {
            double time = owner.pixelToTime(static_cast<float>(e.x - hw));
            time = juce::jlimit(0.0, owner.totalDuration, time);
            owner.onSeek(time);
        }
    }
}

void TrackListComponent::TrackItem::mouseDrag(const juce::MouseEvent& e)
{
    int hw = owner.headerWidth;
    if (e.x >= hw && owner.totalDuration > 0.0 && owner.onSeek) {
        double time = owner.pixelToTime(static_cast<float>(e.x - hw));
        time = juce::jlimit(0.0, owner.totalDuration, time);
        owner.onSeek(time);
    }
}

void TrackListComponent::TrackItem::mouseUp(const juce::MouseEvent& e)
{
    // Intentionally empty — kept for future use
}

void TrackListComponent::TrackItem::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
        // Ctrl/Cmd + wheel = zoom centered on mouse position
        int hw = owner.headerWidth;
        if (e.x < hw || owner.totalDuration <= 0.0) return;

        float wfWidth = static_cast<float>(getWidth() - hw);
        float oldPps = owner.getEffectivePps();
        float mouseX = static_cast<float>(e.x - hw);

        // Time at mouse position before zoom
        double timeAtMouse = owner.pixelToTime(mouseX);

        // Zoom factor
        float zoomFactor = 1.0f + wheel.deltaY * 0.3f;
        if (zoomFactor < 0.5f) zoomFactor = 0.5f;
        if (zoomFactor > 2.0f) zoomFactor = 2.0f;

        float newPps = oldPps * zoomFactor;
        // Clamp: don't zoom out beyond fit-to-width, don't zoom in beyond 2000 pps
        float fitPps = (owner.totalDuration > 0.0)
            ? wfWidth / static_cast<float>(owner.totalDuration) : 100.0f;
        if (newPps < fitPps) newPps = fitPps;
        if (newPps > 2000.0f) newPps = 2000.0f;

        owner.trackPps = newPps;

        // Adjust scroll to keep mouse position stable
        owner.trackScrollSec = timeAtMouse - (mouseX / newPps);
        if (owner.trackScrollSec < 0.0) owner.trackScrollSec = 0.0;

        double maxScroll = owner.totalDuration - (wfWidth / newPps);
        if (maxScroll < 0.0) maxScroll = 0.0;
        if (owner.trackScrollSec > maxScroll) owner.trackScrollSec = maxScroll;

        owner.updateScrollBarRange();
        owner.lastPlayheadX = -1;
        owner.repaint();
    } else {
        // Normal scroll: horizontal
        if (owner.totalDuration <= 0.0) return;
        float wfWidth = static_cast<float>(getWidth() - owner.headerWidth);
        float effectivePps = owner.getEffectivePps();
        double visibleSecs = wfWidth / effectivePps;
        double maxScroll = owner.totalDuration - visibleSecs;
        if (maxScroll <= 0.0) return;

        double scrollDelta = -wheel.deltaY * visibleSecs * 0.1;
        owner.trackScrollSec = juce::jlimit(0.0, maxScroll, owner.trackScrollSec + scrollDelta);
        owner.updateScrollBarRange();
        owner.lastPlayheadX = -1;
        owner.repaint();
    }
}

void TrackListComponent::TrackItem::mouseDoubleClick(const juce::MouseEvent& e)
{
    int hw = owner.headerWidth;
    if (e.x >= hw) {
        // Reset to fit-to-width
        owner.trackPps = 0.0f;
        owner.trackScrollSec = 0.0;
        owner.lastPlayheadX = -1;
        owner.updateScrollBarRange();
        owner.repaint();
    }
}

TrackListComponent::TrackListComponent()
{
    viewport.setViewedComponent(&contentContainer, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    hScrollBar.setColour(juce::ScrollBar::backgroundColourId, APP_COLOR_SURFACE);
    hScrollBar.setColour(juce::ScrollBar::thumbColourId, APP_COLOR_PRIMARY.withAlpha(0.6f));
    hScrollBar.setColour(juce::ScrollBar::trackColourId, APP_COLOR_SURFACE_ALT);
    hScrollBar.addListener(this);
    addAndMakeVisible(hScrollBar);
}

TrackListComponent::~TrackListComponent() = default;

void TrackListComponent::computeTotalDuration()
{
    totalDuration = 0.0;
    if (!editorController) return;
    int count = editorController->getTrackCount();
    for (int i = 0; i < count; ++i) {
        auto* t = editorController->getTrack(i);
        if (t && t->project)
            totalDuration = std::max(totalDuration,
                static_cast<double>(t->project->getAudioData().getDuration()));
    }
}

void TrackListComponent::refresh()
{
    if (!editorController) {
        items.clear();
        return;
    }

    int count = editorController->getTrackCount();

    // Incremental update: avoid destroying/recreating items unnecessarily
    while (static_cast<int>(items.size()) < count) {
        auto item = std::make_unique<TrackItem>(*this, static_cast<int>(items.size()));
        contentContainer.addAndMakeVisible(*item);
        items.push_back(std::move(item));
    }
    while (static_cast<int>(items.size()) > count) {
        items.pop_back();
    }

    for (int i = 0; i < count; ++i) {
        items[static_cast<size_t>(i)]->trackIndex = i;
        items[static_cast<size_t>(i)]->updateFromTrack();
    }

    computeTotalDuration();
    resized();
    repaint();
}

void TrackListComponent::setPlayheadPosition(double timeSeconds)
{
    playheadPosition = timeSeconds;

    int wfWidth = viewport.getWidth() - headerWidth;
    if (wfWidth <= 0) return;

    // Auto-scroll to follow playhead when zoomed in
    if (trackPps > 0.0f && totalDuration > 0.0) {
        float pps = getEffectivePps();
        double visibleSecs = wfWidth / pps;
        double playheadEnd = trackScrollSec + visibleSecs;

        // If playhead is outside visible range, scroll to keep it at 20% from left edge
        if (playheadPosition < trackScrollSec || playheadPosition > playheadEnd) {
            trackScrollSec = playheadPosition - visibleSecs * 0.2;
            if (trackScrollSec < 0.0) trackScrollSec = 0.0;
            double maxScroll = totalDuration - visibleSecs;
            if (trackScrollSec > maxScroll) trackScrollSec = maxScroll;
            lastPlayheadX = -1;
            updateScrollBarRange();
            repaint();
            return;
        }
    }

    // Repaint only the dirty playhead regions on each item
    int newPhX = static_cast<int>(timeToPixel(playheadPosition));

    if (newPhX == lastPlayheadX) return;

    int oldPhX = lastPlayheadX;
    lastPlayheadX = newPhX;

    for (auto& item : items) {
        int wfLeft = headerWidth;
        int minX = wfLeft + juce::jmin(oldPhX, newPhX) - 1;
        int maxX = wfLeft + juce::jmax(oldPhX, newPhX) + 1;
        item->repaint(juce::Rectangle<int>(minX, 0, maxX - minX, item->getHeight()));
    }
}

void TrackListComponent::setTrackProgress(int trackIndex, const TrackProgress& progress)
{
    if (trackIndex < 0) {
        // Pending track (loading)
        pendingProgress = progress;
    } else {
        pendingProgress.active = false;
        if (trackIndex >= 0 && trackIndex < static_cast<int>(items.size())) {
            auto& dst = items[static_cast<size_t>(trackIndex)]->progress;
            bool preserveQueued = dst.queued;
            juce::String preserveQueuedLabel = dst.queuedLabel;
            dst = progress;
            dst.queued = preserveQueued;
            dst.queuedLabel = preserveQueuedLabel;
            items[static_cast<size_t>(trackIndex)]->repaint();
        }
    }

    // Ensure the pending progress item is visible / hidden
    resized();
    if (pendingProgress.active)
        contentContainer.repaint();
    else
        repaint();
}

void TrackListComponent::setTrackQueued(int trackIndex, bool queued, const juce::String& label)
{
    if (trackIndex < 0 || trackIndex >= static_cast<int>(items.size()))
        return;
    auto& item = items[static_cast<size_t>(trackIndex)];
    item->progress.queued = queued;
    item->progress.queuedLabel = queued ? label : juce::String();
    item->repaint();
}

void TrackListComponent::paint(juce::Graphics& g)
{
    g.setColour(APP_COLOR_BACKGROUND);
    g.fillAll();

    if (items.empty() && !pendingProgress.active) {
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.setFont(AppFont::getFont(14.0f));
        g.drawText(TR("tracks.empty"),
                   getLocalBounds(), juce::Justification::centred);
    }

    // Draw pending track progress (for audio being loaded, track not yet created)
    if (pendingProgress.active) {
        int y = static_cast<int>(items.size()) * laneHeight;
        int w = viewport.getWidth();
        auto bounds = juce::Rectangle<int>(0, y, w, laneHeight);

        g.setColour(APP_COLOR_SURFACE);
        g.fillRect(bounds);

        g.setColour(APP_COLOR_BORDER_SUBTLE);
        g.drawHorizontalLine(static_cast<float>(bounds.getBottom() - 0.5f),
                             static_cast<float>(bounds.getX()),
                             static_cast<float>(bounds.getRight()));

        int hw = headerWidth;
        int wfLeft = hw;
        int wfWidth = w - hw;

        // "Loading..." label in header area
        g.setColour(APP_COLOR_TEXT_PRIMARY);
        g.setFont(AppFont::getFont(14.0f));
        g.drawText(TR("progress.loading"),
                   12, y + 4, hw - 40, 20, juce::Justification::left);

        // Dim waveform area
        g.setColour(juce::Colour(0xff000000).withAlpha(0.5f));
        g.fillRect(wfLeft, y, wfWidth, laneHeight);

        // Step text
        juce::String stepText = juce::String(pendingProgress.step) + "/" +
            juce::String(pendingProgress.totalSteps) + " " + pendingProgress.message;
        g.setColour(juce::Colours::white);
        g.setFont(AppFont::getFont(12.0f));
        g.drawText(stepText, wfLeft + 8, y + 4, wfWidth - 16, 16,
                   juce::Justification::left);

        // Progress bar
        int barY = y + 24;
        int barH = 6;
        int barW = wfWidth - 16;
        int barX = wfLeft + 8;

        float stepRatio = (pendingProgress.totalSteps > 0)
            ? static_cast<float>(pendingProgress.step - 1) / pendingProgress.totalSteps
            : 0.0f;
        float subRatio = (pendingProgress.subProgress >= 0.0f)
            ? static_cast<float>(pendingProgress.subProgress)
            : 0.0f;
        float totalRatio = stepRatio + subRatio / pendingProgress.totalSteps;

        g.setColour(juce::Colour(0xff333344));
        g.fillRect(barX, barY, barW, barH);
        g.setColour(juce::Colour(0xff4fc3f7));
        g.fillRect(barX, barY, static_cast<int>(barW * totalRatio), barH);

        if (pendingProgress.subProgress >= 0.0f) {
            int subBarY = barY + barH + 2;
            int subBarH = 3;
            g.setColour(juce::Colour(0xff2a3a4a));
            g.fillRect(barX, subBarY, barW, subBarH);
            g.setColour(juce::Colour(0xff88ccff));
            g.fillRect(barX, subBarY, static_cast<int>(barW * subRatio), subBarH);
        }
    }
}

void TrackListComponent::resized()
{
    int scrollH = kHScrollBarHeight;
    int viewH = getHeight() - scrollH;

    viewport.setBounds(0, 0, getWidth(), viewH);

    int y = 0;
    int w = viewport.getWidth();
    for (auto& item : items) {
        item->setBounds(0, y, w, laneHeight);
        y += laneHeight;
    }
    // Reserve space for pending track progress
    if (pendingProgress.active)
        y += laneHeight;
    contentContainer.setSize(w, juce::jmax(y, viewport.getMaximumVisibleHeight()));

    hScrollBar.setBounds(0, viewH, getWidth(), scrollH);
    updateScrollBarRange();
}

float TrackListComponent::getEffectivePps() const
{
    if (trackPps > 0.0f)
        return trackPps;

    // Fit-to-width
    int wfWidth = viewport.getWidth() - headerWidth;
    if (wfWidth <= 0 || totalDuration <= 0.0)
        return 100.0f;
    return static_cast<float>(wfWidth / totalDuration);
}

float TrackListComponent::getPixelsPerSecond() const
{
    return trackPps;
}

void TrackListComponent::setPixelsPerSecond(float pps)
{
    trackPps = pps;
    lastPlayheadX = -1;
    updateScrollBarRange();
    repaint();
}

void TrackListComponent::setScrollSeconds(double sec)
{
    trackScrollSec = sec;
    lastPlayheadX = -1;
    updateScrollBarRange();
    repaint();
}

double TrackListComponent::pixelToTime(float x) const
{
    return trackScrollSec + static_cast<double>(x) / getEffectivePps();
}

float TrackListComponent::timeToPixel(double time) const
{
    return static_cast<float>((time - trackScrollSec) * getEffectivePps());
}

void TrackListComponent::updateScrollBarRange()
{
    int wfWidth = viewport.getWidth() - headerWidth;
    if (wfWidth <= 0 || totalDuration <= 0.0) {
        hScrollBar.setRangeLimits(0.0, 0.0, juce::dontSendNotification);
        return;
    }

    float pps = getEffectivePps();
    double visibleSecs = wfWidth / pps;
    double maxScroll = totalDuration - visibleSecs;
    if (maxScroll < 0.0) maxScroll = 0.0;

    // Clamp scroll position
    if (trackScrollSec > maxScroll) trackScrollSec = maxScroll;

    // Range limits = full content duration; thumb = visible portion
    hScrollBar.setRangeLimits(0.0, totalDuration, juce::dontSendNotification);
    hScrollBar.setCurrentRange(trackScrollSec, visibleSecs, juce::dontSendNotification);
}

void TrackListComponent::onHScrollChanged()
{
    trackScrollSec = hScrollBar.getCurrentRangeStart();
    lastPlayheadX = -1;
    repaint();
}

void TrackListComponent::scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart)
{
    if (scrollBarThatHasMoved == &hScrollBar)
        onHScrollChanged();
}
