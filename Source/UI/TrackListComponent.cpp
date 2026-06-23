#include "TrackListComponent.h"
#include "../Audio/EditorController.h"
#include "StyledComponents.h"

TrackListComponent::TrackItem::TrackItem(TrackListComponent& o, int idx)
    : owner(o), trackIndex(idx),
      muteButton("M", juce::Colour(0xffe05848), juce::Colour(0xffc04030), juce::Colour(0xffe05848)),
      soloButton("S", juce::Colour(0xffe8b33d), juce::Colour(0xffc89830), juce::Colour(0xffe8b33d))
{
    auto makeButtonShape = []() {
        juce::Path p;
        p.addRoundedRectangle(0, 0, 20, 16, 3);
        return p;
    };

    auto onShape = makeButtonShape();

    muteButton.setShape(onShape, false, true, false);
    muteButton.setOutline(juce::Colour(0xff666666), 1.0f);
    muteButton.setButtonText("M");

    soloButton.setShape(onShape, false, true, false);

    addAndMakeVisible(muteButton);
    addAndMakeVisible(soloButton);

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

    auto* proj = track->getProject();
    if (proj) {
        auto& audio = proj->getAudioData();
        waveformPreview.makeCopyOf(audio.waveform);
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

    float left = 12.0f;

    auto typeColor = (trackType == TrackType::Vocal)
        ? juce::Colour(0xff6ab0ff)
        : juce::Colour(0xff88cc88);

    g.setColour(typeColor);
    g.fillEllipse(left, bounds.getY() + 10, 10, 10);

    g.setColour(APP_COLOR_TEXT_PRIMARY);
    g.setFont(AppFont::getFont(13.0f));
    g.drawText(trackName, left + 20, bounds.getY() + 6, getWidth() - 120, 20,
               juce::Justification::left);

    juce::String typeStr = (trackType == TrackType::Vocal) ? "Vocal" : "Accompaniment";
    g.setColour(APP_COLOR_TEXT_MUTED);
    g.setFont(AppFont::getFont(11.0f));
    g.drawText(typeStr, left + 20, bounds.getY() + 26, getWidth() - 120, 16,
               juce::Justification::left);

    int waveformY = bounds.getY() + 46;
    int waveformH = 18;
    int waveformW = getWidth() - 80;
    if (waveformPreview.getNumSamples() > 0 && waveformW > 10) {
        g.setColour(APP_COLOR_WAVEFORM.withAlpha(0.5f));
        const float* data = waveformPreview.getReadPointer(0);
        int numSamples = waveformPreview.getNumSamples();
        float mid = waveformY + waveformH * 0.5f;
        for (int x = 0; x < waveformW; ++x) {
            int startIdx = (int)((float)x / waveformW * numSamples);
            int endIdx = (int)((float)(x + 1) / waveformW * numSamples);
            if (endIdx <= startIdx) endIdx = startIdx + 1;
            float maxVal = 0.0f;
            for (int i = startIdx; i < endIdx && i < numSamples; ++i) {
                float v = std::abs(data[i]);
                if (v > maxVal) maxVal = v;
            }
            float h = maxVal * waveformH * 0.5f;
            g.drawVerticalLine(left + x, mid - h, mid + h);
        }
    }

    if (isMuted) {
        g.setColour(juce::Colour(0xffe05848).withAlpha(0.15f));
        g.fillAll();
    }
}

void TrackListComponent::TrackItem::resized()
{
    int btnSize = 20;
    int y = getHeight() - 28;
    muteButton.setBounds(getWidth() - 60, y, btnSize, 16);
    soloButton.setBounds(getWidth() - 36, y, btnSize, 16);
}

void TrackListComponent::TrackItem::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (owner.onTrackDoubleClicked)
        owner.onTrackDoubleClicked(trackIndex);
}

void TrackListComponent::TrackItem::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown() && owner.onTrackTypeChangeRequested)
        owner.onTrackTypeChangeRequested(trackIndex);
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

void TrackListComponent::paint(juce::Graphics& g)
{
    g.setColour(APP_COLOR_BACKGROUND);
    g.fillAll();

    if (items.empty()) {
        g.setColour(APP_COLOR_TEXT_MUTED);
        g.setFont(AppFont::getFont(14.0f));
        g.drawText("No tracks. Import audio to create a track.",
                   getLocalBounds(), juce::Justification::centred);
    }
}

void TrackListComponent::resized()
{
    int y = 0;
    for (auto& item : items) {
        item->setBounds(0, y, getWidth(), itemHeight);
        y += itemHeight;
    }
}
