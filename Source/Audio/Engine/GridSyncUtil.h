#pragma once

#include "HostSyncService.h"

/**
 * Grid Sync Utility
 *
 * Provides utilities for synchronizing the piano roll grid with host tempo.
 * Enables beat-aligned editing and display.
 *
 * Usage:
 *   GridSyncUtil grid(hostSyncService);
 *   double snapTime = grid.snapToGrid(cursorTime, GridSyncUtil::GridResolution::Beat);
 *   auto markers = grid.getGridMarkers(startTime, endTime);
 */
class GridSyncUtil
{
public:
    // Grid resolution options
    enum class GridResolution
    {
        Bar,            // Snap to bar boundaries
        Beat,           // Snap to beat boundaries
        HalfBeat,       // 1/2 beat (eighth note in 4/4)
        QuarterBeat,    // 1/4 beat (sixteenth note in 4/4)
        EighthBeat,     // 1/8 beat (thirty-second note in 4/4)
        Triplet,        // Triplet subdivision
        None            // No snapping (free)
    };

    // Grid marker for display
    struct GridMarker
    {
        double timeSeconds;
        double ppqPosition;
        int barNumber;
        int beatNumber;
        bool isBarLine;
        bool isBeatLine;
        bool isSubdivision;
    };

    explicit GridSyncUtil(HostSyncService* syncService = nullptr)
        : hostSync(syncService)
    {
    }

    void setHostSyncService(HostSyncService* syncService)
    {
        hostSync = syncService;
    }

    // ========== Grid Snapping ==========

    /**
     * Snap a time value to the nearest grid position.
     * @param timeSeconds Time in seconds to snap
     * @param resolution Grid resolution to snap to
     * @return Snapped time in seconds
     */
    double snapToGrid(double timeSeconds, GridResolution resolution) const
    {
        if (hostSync == nullptr || resolution == GridResolution::None)
            return timeSeconds;

        auto state = hostSync->getCurrentState();
        if (!state.tempo.hasBpm || state.tempo.bpm <= 0.0)
            return timeSeconds;

        double gridInterval = getGridIntervalSeconds(resolution, state.tempo);
        if (gridInterval <= 0.0)
            return timeSeconds;

        // Round to nearest grid position
        return std::round(timeSeconds / gridInterval) * gridInterval;
    }

    /**
     * Snap a time value to the previous grid position.
     */
    double snapToPreviousGrid(double timeSeconds, GridResolution resolution) const
    {
        if (hostSync == nullptr || resolution == GridResolution::None)
            return timeSeconds;

        auto state = hostSync->getCurrentState();
        if (!state.tempo.hasBpm || state.tempo.bpm <= 0.0)
            return timeSeconds;

        double gridInterval = getGridIntervalSeconds(resolution, state.tempo);
        if (gridInterval <= 0.0)
            return timeSeconds;

        return std::floor(timeSeconds / gridInterval) * gridInterval;
    }

    /**
     * Snap a time value to the next grid position.
     */
    double snapToNextGrid(double timeSeconds, GridResolution resolution) const
    {
        if (hostSync == nullptr || resolution == GridResolution::None)
            return timeSeconds;

        auto state = hostSync->getCurrentState();
        if (!state.tempo.hasBpm || state.tempo.bpm <= 0.0)
            return timeSeconds;

        double gridInterval = getGridIntervalSeconds(resolution, state.tempo);
        if (gridInterval <= 0.0)
            return timeSeconds;

        return std::ceil(timeSeconds / gridInterval) * gridInterval;
    }

    // ========== Grid Information ==========

    /**
     * Get the grid interval in seconds for a given resolution.
     */
    double getGridIntervalSeconds(GridResolution resolution) const
    {
        if (hostSync == nullptr)
            return getDefaultGridInterval(resolution);

        auto state = hostSync->getCurrentState();
        return getGridIntervalSeconds(resolution, state.tempo);
    }

    /**
     * Get grid markers for a time range.
     * @param startSeconds Start of range in seconds
     * @param endSeconds End of range in seconds
     * @param resolution Minimum resolution to include
     * @return Vector of grid markers
     */
    std::vector<GridMarker> getGridMarkers(double startSeconds, double endSeconds,
                                           GridResolution resolution = GridResolution::Beat) const
    {
        std::vector<GridMarker> markers;

        if (hostSync == nullptr)
            return markers;

        auto state = hostSync->getCurrentState();
        if (!state.tempo.hasBpm || state.tempo.bpm <= 0.0)
            return markers;

        double beatInterval = state.tempo.getSecondsPerBeat();
        double barInterval = state.tempo.getSecondsPerBar();
        double gridInterval = getGridIntervalSeconds(resolution, state.tempo);

        if (gridInterval <= 0.0)
            return markers;

        // Start from the previous bar
        double startBar = std::floor(startSeconds / barInterval) * barInterval;

        for (double time = startBar; time <= endSeconds; time += gridInterval)
        {
            if (time < startSeconds)
                continue;

            GridMarker marker;
            marker.timeSeconds = time;
            marker.ppqPosition = state.secondsToPpq(time);

            // Calculate bar and beat numbers
            int totalBeats = static_cast<int>(std::round(time / beatInterval));
            int beatsPerBar = state.tempo.timeSigNumerator;

            marker.barNumber = (totalBeats / beatsPerBar) + 1;
            marker.beatNumber = (totalBeats % beatsPerBar) + 1;

            // Determine marker type
            double barRemainder = std::fmod(time, barInterval);
            double beatRemainder = std::fmod(time, beatInterval);

            marker.isBarLine = std::abs(barRemainder) < 0.001 || std::abs(barRemainder - barInterval) < 0.001;
            marker.isBeatLine = std::abs(beatRemainder) < 0.001 || std::abs(beatRemainder - beatInterval) < 0.001;
            marker.isSubdivision = !marker.isBarLine && !marker.isBeatLine;

            markers.push_back(marker);
        }

        return markers;
    }

    // ========== Time Conversion ==========

    /**
     * Convert seconds to bar:beat:tick format string.
     */
    juce::String formatBarBeatTick(double timeSeconds, int ticksPerBeat = 480) const
    {
        if (hostSync == nullptr)
            return formatTimeOnly(timeSeconds);

        auto state = hostSync->getCurrentState();
        if (!state.tempo.hasBpm || state.tempo.bpm <= 0.0)
            return formatTimeOnly(timeSeconds);

        double beatInterval = state.tempo.getSecondsPerBeat();
        int beatsPerBar = state.tempo.timeSigNumerator;

        double totalBeats = timeSeconds / beatInterval;
        int bar = static_cast<int>(totalBeats / beatsPerBar) + 1;
        double beatInBar = std::fmod(totalBeats, static_cast<double>(beatsPerBar));
        int beat = static_cast<int>(beatInBar) + 1;
        int tick = static_cast<int>((beatInBar - std::floor(beatInBar)) * ticksPerBeat);

        return juce::String::formatted("%d.%d.%03d", bar, beat, tick);
    }

    /**
     * Convert seconds to time format string (MM:SS.mmm).
     */
    static juce::String formatTimeOnly(double timeSeconds)
    {
        int minutes = static_cast<int>(timeSeconds) / 60;
        int seconds = static_cast<int>(timeSeconds) % 60;
        int millis = static_cast<int>((timeSeconds - std::floor(timeSeconds)) * 1000);

        return juce::String::formatted("%02d:%02d.%03d", minutes, seconds, millis);
    }

    // ========== Tempo Utilities ==========

    /**
     * Check if host tempo is available.
     */
    bool hasHostTempo() const
    {
        if (hostSync == nullptr)
            return false;
        return hostSync->getCurrentState().tempo.hasBpm;
    }

    /**
     * Get current BPM from host.
     */
    double getBpm() const
    {
        if (hostSync == nullptr)
            return 120.0;
        auto state = hostSync->getCurrentState();
        return state.tempo.hasBpm ? state.tempo.bpm : 120.0;
    }

    /**
     * Get current time signature from host.
     */
    void getTimeSignature(int& numerator, int& denominator) const
    {
        if (hostSync == nullptr)
        {
            numerator = 4;
            denominator = 4;
            return;
        }

        auto state = hostSync->getCurrentState();
        if (state.tempo.hasTimeSignature)
        {
            numerator = state.tempo.timeSigNumerator;
            denominator = state.tempo.timeSigDenominator;
        }
        else
        {
            numerator = 4;
            denominator = 4;
        }
    }

private:
    HostSyncService* hostSync = nullptr;

    double getGridIntervalSeconds(GridResolution resolution, const HostSyncService::TempoInfo& tempo) const
    {
        if (tempo.bpm <= 0.0)
            return getDefaultGridInterval(resolution);

        double beatInterval = tempo.getSecondsPerBeat();
        double barInterval = tempo.getSecondsPerBar();

        switch (resolution)
        {
            case GridResolution::Bar:
                return barInterval;
            case GridResolution::Beat:
                return beatInterval;
            case GridResolution::HalfBeat:
                return beatInterval / 2.0;
            case GridResolution::QuarterBeat:
                return beatInterval / 4.0;
            case GridResolution::EighthBeat:
                return beatInterval / 8.0;
            case GridResolution::Triplet:
                return beatInterval / 3.0;
            case GridResolution::None:
            default:
                return 0.0;
        }
    }

    static double getDefaultGridInterval(GridResolution resolution)
    {
        // Default to 120 BPM, 4/4 time
        constexpr double defaultBeatInterval = 0.5; // 120 BPM

        switch (resolution)
        {
            case GridResolution::Bar:
                return defaultBeatInterval * 4.0;
            case GridResolution::Beat:
                return defaultBeatInterval;
            case GridResolution::HalfBeat:
                return defaultBeatInterval / 2.0;
            case GridResolution::QuarterBeat:
                return defaultBeatInterval / 4.0;
            case GridResolution::EighthBeat:
                return defaultBeatInterval / 8.0;
            case GridResolution::Triplet:
                return defaultBeatInterval / 3.0;
            case GridResolution::None:
            default:
                return 0.0;
        }
    }
};
