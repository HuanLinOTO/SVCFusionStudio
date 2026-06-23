#include "TrackListComponent.h"
#include "../Audio/EditorController.h"
#include "StyledComponents.h"

static juce::Colour getTypeColor(TrackType t) {
    return (t == TrackType::Vocal) ? juce::Colour(0xff6ab0ff) : juce::Colour(0xff88cc88);
}

// ── TrackItem ──

TrackListComponent::TrackItem::TrackItem(TrackListComponent& o, int idx)
    : owner(o), trackIndex(idx),
      muteButton(TR("track.mute")), soloButton(TR("track.solo"))
{
    muteButton.setClickingTogglesState(true);
    muteButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffe05848));
    muteButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffc04030));
    muteButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    muteButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    muteButton.setTooltip(TR("track.mute"));

    soloButton.setClickingTogglesState(true);
    soloButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xffe8b33d));
    soloButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffc89830));
    soloButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    soloButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    soloButton.setTooltip(TR("track.solo"));

    typeCombo.addItem(TR("track.accompaniment"), 1);
    typeCombo.addItem(TR("track.vocal"), 2);
    typeCombo.setSelectedId(1, juce::dontSendNotification);

    addAndMakeVisible(muteButton);
    addAndMakeVisible(soloButton);
    addAndMakeVisible(typeCombo);

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
        if (newType != trackType && owner.onTrackTypeChanged)
            owner.onTrackTypeChanged(trackIndex, newType);
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
    volume = track->getVolume();

    typeCombo.setSelectedId((trackType == TrackType::Vocal) ? 2 : 1, juce::dontSendNotification);

    auto* proj = track->getProject();
    if (proj) {
        auto& audio = proj->getAudioData();
        waveformPreview.makeCopyOf(audio.waveform);
    } else {
        waveformPreview = {};
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

    g.setColour(APP_COLOR_BORDER_SUBTLE);
    g.drawHorizontalLine(bounds.getBottom() - 0.5f, bounds.getX(), bounds.getRight());

    if (isActive) {
        g.setColour(APP_COLOR_PRIMARY);
        g.fillRect(bounds.getX(), bounds.getY(), 3.0f, bounds.getHeight());
    }

    // ── Left: Header area (name + type + mute/solo) ──
    int hw = owner.headerWidth;
    int leftPad = 12;

    // Type dot
    g.setColour(getTypeColor(trackType));
    g.fillEllipse(static_cast<float>(leftPad), bounds.getY() + 10, 10.0f, 10.0f);

    // Track name
    g.setColour(APP_COLOR_TEXT_PRIMARY);
    g.setFont(AppFont::getFont(12.0f));
    g.drawText(trackName,
               leftPad + 16, bounds.getY() + 6, hw - leftPad - 16, 18,
               juce::Justification::left);

    // ── Right: Waveform area ──
    int wfLeft = hw;
    int wfWidth = getWidth() - hw;
    int wfHeight = 40;
    int wfY = 12;

    if (waveformPreview.getNumSamples() > 0 && wfWidth > 10) {
        g.setColour(APP_COLOR_WAVEFORM.withAlpha(0.45f));
        const float* data = waveformPreview.getReadPointer(0);
        int numSamples = waveformPreview.getNumSamples();
        float mid = wfY + wfHeight * 0.5f;
        for (int x = 0; x < wfWidth; ++x) {
            int startIdx = static_cast<int>((static_cast<float>(x) / wfWidth) * numSamples);
            int endIdx = static_cast<int>((static_cast<float>(x + 1) / wfWidth) * numSamples);
            if (endIdx <= startIdx) endIdx = startIdx + 1;
            float maxVal = 0.0f;
            for (int i = startIdx; i < endIdx && i < numSamples; ++i) {
                float v = std::abs(data[i]);
                if (v > maxVal) maxVal = v;
            }
            float h = maxVal * wfHeight * 0.5f;
            g.drawVerticalLine(wfLeft + x, mid - h, mid + h);
        }

        // Playhead line in waveform area
        double ratio = (owner.totalDuration > 0.0)
            ? (owner.playheadPosition / owner.totalDuration)
            : 0.0;
        if (ratio > 0.0 && ratio < 1.0) {
            int phX = wfLeft + static_cast<int>(ratio * wfWidth);
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.drawVerticalLine(phX, bounds.getY(), bounds.getBottom());
        }
    }

    if (isMuted) {
        g.setColour(juce::Colour(0xffe05848).withAlpha(0.12f));
        g.fillAll();
    }
}

void TrackListComponent::TrackItem::resized()
{
    int hw = owner.headerWidth;
    typeCombo.setBounds(12, 28, hw - 80, 18);
    int btnW = 22;
    int btnH = 16;
    int btnY = 28;
    muteButton.setBounds(hw - btnW - 8, btnY, btnW, btnH);
    soloButton.setBounds(hw - btnW * 2 - 14, btnY, btnW, btnH);
}

void TrackListComponent::TrackItem::mouseDown(const juce::MouseEvent& e)
{
    if (owner.onTrackSelected)
        owner.onTrackSelected(trackIndex);
}

// ── TrackListComponent ──

TrackListComponent::TrackListComponent()
{
}

TrackListComponent::~TrackListComponent() = default;

void TrackListComponent::refresh()
{
    items.clear();

    if (!editorController) return;

    int count = editorController->getTrackCount();
    items.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        auto item = std::make_unique<TrackItem>(*this, i);
        item->updateFromTrack();
        addAndMakeVisible(*item);
        items.push_back(std::move(item));
    }

    resized();
    repaint();
}

void TrackListComponent::setPlayheadPosition(double timeSeconds, double totalDurationSeconds)
{
    playheadPosition = timeSeconds;
    totalDuration = totalDurationSeconds;
    repaint();
}

void TrackListComponent::paint(juce::Graphics& g)
{
    g.setColour(APP_COLOR_BACKGROUND);
    g.fillAll();

    if (items.empty()) {
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.setFont(AppFont::getFont(13.0f));
        g.drawText(TR("tracks.empty"),
                   getLocalBounds(), juce::Justification::centred);
    }
}

void TrackListComponent::resized()
{
    int y = 0;
    int w = getWidth();
    for (auto& item : items) {
        item->setBounds(0, y, w, laneHeight);
        y += laneHeight;
    }
}

int TrackListComponent::getTotalHeight() const
{
    int count = static_cast<int>(items.size());
    return count * laneHeight;
}
