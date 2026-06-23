#pragma once

#include "../JuceHeader.h"
#include "../Models/Track.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Localization.h"

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

    void setPlayheadPosition(double timeSeconds, double totalDurationSeconds);

    std::function<void(int trackIndex)> onTrackSelected;
    std::function<void(int trackIndex, TrackType newType)> onTrackTypeChanged;
    std::function<void()> onTracksChanged;

    int getHeaderWidth() const { return headerWidth; }
    int getLaneHeight() const { return laneHeight; }
    int getTotalHeight() const;

private:
    EditorController* editorController = nullptr;

    struct TrackItem : public juce::Component
    {
        TrackItem(TrackListComponent& owner, int index);
        ~TrackItem() override;

        void paint(juce::Graphics& g) override;
        void resized() override;
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

        juce::TextButton muteButton;
        juce::TextButton soloButton;
        juce::ComboBox typeCombo;
    };

    std::vector<std::unique_ptr<TrackItem>> items;
    int headerWidth = 150;
    int laneHeight = 64;
    double playheadPosition = 0.0;
    double totalDuration = 0.0;
};
