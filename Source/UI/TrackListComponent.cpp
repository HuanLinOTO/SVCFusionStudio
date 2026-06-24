#include "TrackListComponent.h"
#include "../Audio/EditorController.h"
#include "StyledComponents.h"

static juce::Colour getTypeColor(TrackType t) {
    return (t == TrackType::Vocal) ? juce::Colour(0xff6ab0ff) : juce::Colour(0xff88cc88);
}

// ── TrackItem ──

TrackListComponent::TrackItem::TrackItem(TrackListComponent& o, int idx)
    : owner(o), trackIndex(idx),
      muteButton("M"), soloButton("S"),
      deleteButton("x")
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

    deleteButton.setColour(juce::TextButton::buttonColourId, APP_COLOR_SURFACE);
    deleteButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffe05848));
    deleteButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    deleteButton.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_MUTED);
    deleteButton.setTooltip("Delete track");

    typeCombo.addItem(TR("track.accompaniment"), 1);
    typeCombo.addItem(TR("track.vocal"), 2);
    typeCombo.setSelectedId(1, juce::dontSendNotification);

    volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange(-24.0, 6.0, 0.1);
    volumeSlider.setValue(0.0);

    addAndMakeVisible(muteButton);
    addAndMakeVisible(soloButton);
    addAndMakeVisible(deleteButton);
    addAndMakeVisible(typeCombo);
    addAndMakeVisible(volumeSlider);

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

    deleteButton.onClick = [this]() {
        if (owner.onTrackDeleted)
            owner.onTrackDeleted(trackIndex);
    };

    volumeSlider.onValueChange = [this]() {
        if (!owner.editorController) return;
        auto* track = owner.editorController->getTrack(trackIndex);
        if (!track) return;
        track->setVolume(static_cast<float>(volumeSlider.getValue()));
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
        waveformPreview.makeCopyOf(audio.waveform);
        volumeSlider.setValue(track->getVolume(), juce::dontSendNotification);
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

    // ── Left: Header area ──
    int hw = owner.headerWidth;
    int leftPad = 12;

    // Type dot
    g.setColour(getTypeColor(trackType));
    g.fillEllipse(static_cast<float>(leftPad), bounds.getY() + 8, 10.0f, 10.0f);

    // Track name
    g.setColour(APP_COLOR_TEXT_PRIMARY);
    g.setFont(AppFont::getFont(14.0f));
    g.drawText(trackName,
               leftPad + 16, bounds.getY() + 4, hw - leftPad - 40, 20,
               juce::Justification::left);

    // ── Right: Waveform area (fit-to-width, independent of piano roll zoom) ──
    int wfLeft = hw;
    int wfWidth = getWidth() - hw;
    int wfHeight = getHeight() - 16;
    int wfY = 8;

    if (waveformPreview.getNumSamples() > 0 && wfWidth > 10) {
        g.setColour(juce::Colour(0xff8a9bbf));
        const float* data = waveformPreview.getReadPointer(0);
        int numSamples = waveformPreview.getNumSamples();
        float mid = wfY + wfHeight * 0.5f;
        for (int x = 0; x < wfWidth; ++x) {
            int startIdx = static_cast<int>((static_cast<float>(x) / wfWidth) * numSamples);
            int endIdx = static_cast<int>((static_cast<float>(x + 1) / wfWidth) * numSamples);
            if (endIdx <= startIdx) endIdx = startIdx + 1;
            if (endIdx > numSamples) endIdx = numSamples;
            float maxVal = 0.0f;
            for (int i = startIdx; i < endIdx; ++i) {
                float v = std::abs(data[i]);
                if (v > maxVal) maxVal = v;
            }
            float h = maxVal * wfHeight * 0.5f;
            g.drawVerticalLine(wfLeft + x, mid - h, mid + h);
        }

        // Playhead line (ratio-based, independent of piano roll zoom)
        double ratio = (owner.totalDuration > 0.0)
            ? (owner.playheadPosition / owner.totalDuration)
            : 0.0;
        if (ratio >= 0.0 && ratio <= 1.0) {
            int phX = wfLeft + static_cast<int>(ratio * wfWidth);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
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
    // Row 1: name (y=4, h=20)
    deleteButton.setBounds(hw - 28, 4, 20, 18);

    // Row 2: type combo + M/S buttons (y=28, h=22)
    int btnW = 26;
    int btnH = 22;
    int btnY = 28;
    typeCombo.setBounds(12, btnY, 70, btnH);
    muteButton.setBounds(12 + 70 + 4, btnY, btnW, btnH);
    soloButton.setBounds(12 + 70 + 4 + btnW + 4, btnY, btnW, btnH);

    // Row 3: volume slider (y=54, h=14)
    volumeSlider.setBounds(12, 54, hw - 24, 14);
}

void TrackListComponent::TrackItem::mouseDown(const juce::MouseEvent& e)
{
    if (e.eventComponent == this) {
        if (owner.onTrackSelected)
            owner.onTrackSelected(trackIndex);

        // Click in waveform area = seek (ratio-based)
        int hw = owner.headerWidth;
        if (e.x >= hw && owner.totalDuration > 0.0 && owner.onSeek) {
            double ratio = static_cast<double>(e.x - hw) / (getWidth() - hw);
            ratio = juce::jlimit(0.0, 1.0, ratio);
            owner.onSeek(ratio * owner.totalDuration);
        }
    }
}

void TrackListComponent::TrackItem::mouseDrag(const juce::MouseEvent& e)
{
    int hw = owner.headerWidth;
    if (e.x >= hw && owner.totalDuration > 0.0 && owner.onSeek) {
        double ratio = static_cast<double>(e.x - hw) / (getWidth() - hw);
        ratio = juce::jlimit(0.0, 1.0, ratio);
        owner.onSeek(ratio * owner.totalDuration);
    }
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
        g.setFont(AppFont::getFont(14.0f));
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
