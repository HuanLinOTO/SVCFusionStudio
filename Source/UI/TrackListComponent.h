#pragma once

#include "../JuceHeader.h"
#include "../Models/Track.h"
#include "../Utils/UI/Theme.h"

class EditorController;

class TrackListComponent : public juce::Component
{
public:
    TrackListComponent();
    ~TrackListComponent() override;

    void setEditorController(EditorController* ec) { editorController = ec; refresh(); }

    void refresh();
    void paint(juce::Graphics& g) override;
    void resized() override;

    std::function<void(int trackIndex)> onTrackDoubleClicked;
    std::function<void(int trackIndex)> onTrackTypeChangeRequested;
    std::function<void()> onTracksChanged;

private:
    EditorController* editorController = nullptr;

    struct TrackItem : public juce::Component
    {
        TrackItem(TrackListComponent& owner, int index);
        ~TrackItem() override;

        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseDoubleClick(const juce::MouseEvent& e) override;
        void mouseDown(const juce::MouseEvent& e) override;

        void updateFromTrack();

        TrackListComponent& owner;
        int trackIndex;
        juce::String trackName;
        TrackType trackType = TrackType::Accompaniment;
        bool isActive = false;
        bool isMuted = false;
        bool isSoloed = false;
        float volume = 0.0f;
        juce::AudioBuffer<float> waveformPreview;

        juce::ShapeButton muteButton;
        juce::ShapeButton soloButton;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackItem)
    };

    std::vector<std::unique_ptr<TrackItem>> items;
    int itemHeight = 72;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackListComponent)
};
