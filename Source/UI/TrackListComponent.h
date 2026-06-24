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

    void setPlayheadPosition(double timeSeconds);
    void setRainbowWaveform(bool enabled) { rainbowWaveform = enabled; repaint(); }
    void setColormapIndex(int idx) { colormapIndex = idx; repaint(); }

    // Per-track progress: trackIndex=-1 means pending track shown at bottom
    struct TrackProgress {
        bool active = false;
        int step = 0;
        int totalSteps = 0;
        juce::String message;
        double subProgress = -1.0; // -1 = indeterminate, 0-1 = chunk progress
    };
    void setTrackProgress(int trackIndex, const TrackProgress& progress);

    std::function<void(int trackIndex)> onTrackSelected;
    std::function<void(int trackIndex, TrackType newType)> onTrackTypeChanged;
    std::function<void(int trackIndex)> onTrackDeleted;
    std::function<void()> onTracksChanged;
    std::function<void(double timeSeconds)> onSeek;

    int getHeaderWidth() const { return headerWidth; }
    int getLaneHeight() const { return laneHeight; }

private:
    EditorController* editorController = nullptr;

    static constexpr int kPeakBuckets = 2048;

    struct TrackItem : public juce::Component, public juce::TooltipClient
    {
        TrackItem(TrackListComponent& owner, int index);
        ~TrackItem() override;

        void paint(juce::Graphics& g) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        juce::String getTooltip() override { return trackName; }

        void updateFromTrack();

        TrackListComponent& owner;
        int trackIndex;
        juce::String trackName;
        TrackType trackType = TrackType::Accompaniment;
        bool isActive = false;
        bool isMuted = false;
        bool isSoloed = false;

        // Pre-computed peak data: min/max abs amplitude per bucket
        std::vector<float> peakMin;  // min abs value per bucket
        std::vector<float> peakMax;  // max abs value per bucket

        juce::TextButton muteButton;
        juce::TextButton soloButton;
        juce::TextButton deleteButton;
        juce::ComboBox typeCombo;
        juce::Slider volumeSlider;

        TrackProgress progress;
    };

    void computeTotalDuration();
    int lastPlayheadX = -1;

    juce::Viewport viewport;
    juce::Component contentContainer;
    std::vector<std::unique_ptr<TrackItem>> items;
    int headerWidth = 200;
    int laneHeight = 80;
    double playheadPosition = 0.0;
    double totalDuration = 0.0;
    bool rainbowWaveform = false;
    int colormapIndex = 0;
    TrackProgress pendingProgress; // for trackIndex=-1 (loading)
};
