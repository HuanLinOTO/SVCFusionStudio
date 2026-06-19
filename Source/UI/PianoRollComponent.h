#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/DrawCurve.h"
#include "../Utils/BasePitchPreview.h"
#include "../Utils/UndoManager.h"
#include "Commands.h"
#include "../Utils/CenteredMelSpectrogram.h"
#include "PianoRoll/BoxSelector.h"
#include "PianoRoll/CoordinateMapper.h"
#include "PianoRoll/NoteSplitter.h"
#include "PianoRoll/PianoRollRenderer.h"
#include "PianoRoll/PitchEditor.h"
#include "PianoRoll/ScrollZoomController.h"

#include <array>
#include <deque>
#include <limits>
#include <memory>
#include <unordered_map>

class PitchUndoManager;

/**
 * Edit mode for the piano roll.
 */
enum class EditMode {
  Select, // Normal selection and dragging
  Stretch, // Stretch note timing
  Draw,   // Pitch drawing mode
  Split   // Note splitting mode
};

enum class PitchDrawingTarget {
  Output,
  Svc
};

struct SvcPitchFrameEdit {
  int idx = -1;
  float oldValue = 0.0f;
  float newValue = 0.0f;
};

/**
 * Piano roll component for displaying and editing notes.
 * Supports DPI-aware scaling for multi-monitor setups.
 */
class PianoRollComponent : public juce::Component,
                           public juce::ScrollBar::Listener,
                           public juce::KeyListener,
                           private juce::Timer {
public:
  using juce::Component::keyPressed;
  PianoRollComponent();
  ~PianoRollComponent() override;

  void paint(juce::Graphics &g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseMove(const juce::MouseEvent &e) override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void mouseWheelMove(const juce::MouseEvent &e,
                      const juce::MouseWheelDetails &wheel) override;
  void mouseMagnify(const juce::MouseEvent &e, float scaleFactor) override;

  // Focus handling - re-grab focus when lost (important for plugin mode)
  void focusLost(FocusChangeType cause) override;
  void focusGained(FocusChangeType cause) override;

  // KeyListener
  bool keyPressed(const juce::KeyPress &key,
                  juce::Component *originatingComponent) override;

  // ScrollBar::Listener
  void scrollBarMoved(juce::ScrollBar *scrollBar,
                      double newRangeStart) override;

  // Project
  void setProject(Project *proj);
  Project *getProject() const { return project; }

  // Undo Manager
  void setUndoManager(PitchUndoManager *manager);
  PitchUndoManager *getUndoManager() const { return undoManager; }

  // Cursor
  void setCursorTime(double time);
  double getCursorTime() const { return cursorTime; }

  // Zoom with optional center point
  void setPixelsPerSecond(float pps, bool centerOnCursor = false);
  void setPixelsPerSemitone(float pps, float anchorContentY = -1.0f);
  float getPixelsPerSecond() const { return pixelsPerSecond; }
  float getPixelsPerSemitone() const { return pixelsPerSemitone; }

  // Scroll
  void setScrollX(double x);
  double getScrollX() const { return scrollX; }
  int getPianoKeysWidth() const { return pianoKeysWidth; }
  void centerOnPitchRange(float minMidi, float maxMidi);
  int getVisibleContentWidth() const;
  int getVisibleContentHeight() const;

  // Edit mode
  void setEditMode(EditMode mode);
  EditMode getEditMode() const { return editMode; }

  // Cancel current drawing operation (used when undo is triggered during
  // drawing)
  void cancelDrawing();

  // View settings
  void setShowDeltaPitch(bool show) {
    showDeltaPitch = show;
    invalidateStaticPianoLayer();
    repaint();
  }
  void setShowBasePitch(bool show) {
    showBasePitch = show;
    invalidateStaticPianoLayer();
    repaint();
  }
  void setShowSomeSegmentsDebug(bool show) {
    showSomeSegmentsDebug = show;
    invalidateStaticPianoLayer();
    repaint();
  }
  void setShowSomeValuesDebug(bool show) {
    showSomeValuesDebug = show;
    invalidateStaticPianoLayer();
    repaint();
  }
  void setShowUvInterpolationDebug(bool show) {
    showUvInterpolationDebug = show;
    invalidateStaticPianoLayer();
    repaint();
  }
  void setShowActualF0Debug(bool show) {
    showActualF0Debug = show;
    invalidateStaticPianoLayer();
    repaint();
  }
  void setShowFpsOverlay(bool show) {
    showFpsOverlay = show;
    if (!show) {
      stopTimer();
      fpsHistory.fill(0.0f);
      fpsHistoryIndex = 0;
      fpsHistoryCount = 0;
      currentFps = 0.0f;
      lastFpsSampleTimeMs = 0.0;
    } else {
      startTimerHz(10);
    }
    repaint();
  }
  void setShowBackgroundWaveform(bool show) {
    showBackgroundWaveform = show;
    invalidateStaticPianoLayer();
    if (!show) {
      waveformCache = {};
      cachedScrollX = -1.0;
      cachedWaveformBucketScrollX = -1.0;
      cachedPixelsPerSecond = -1.0f;
    }
    repaint();
  }
  bool getShowDeltaPitch() const { return showDeltaPitch; }
  bool getShowBasePitch() const { return showBasePitch; }

  // Invalidate the background waveform rendering cache (call after waveform data changes)
  void invalidateWaveformCache();

  // Callbacks
  std::function<void(Note *)> onNoteSelected;
  std::function<void()> onPitchEdited;
  std::function<void()> onPitchEditFinished; // Called when dragging ends
  std::function<void()> onCursorMoved;
  std::function<void(double)> onSeek;
  std::function<void(float)> onZoomChanged;
  std::function<void(double)> onScrollChanged;
  std::function<void(const LoopRange &)> onLoopRangeChanged;
  std::function<void(int, int)>
      onReinterpolateUV; // Called to re-infer UV regions (startFrame, endFrame)
  std::function<bool()> isSVCActive; // Returns true if SVC model is active

private:
  void drawBackgroundWaveform(juce::Graphics &g,
                              const juce::Rectangle<int> &visibleArea);
  void drawGrid(juce::Graphics &g);
  void drawTimeline(juce::Graphics &g);
  void drawLoopTimeline(juce::Graphics &g);
  void drawNotes(juce::Graphics &g);
  void drawPitchCurves(juce::Graphics &g);
  void drawCursor(juce::Graphics &g);
  void drawPianoKeys(juce::Graphics &g);
  void drawDrawingCursor(juce::Graphics &g); // Draw mode indicator
  void drawSelectionRect(juce::Graphics &g); // Box selection rectangle
  void drawLoopOverlay(juce::Graphics &g);
  void drawSomeSegmentDebugOverlay(juce::Graphics &g);
  void drawSomeValuesDebugOverlay(juce::Graphics &g);
  void recordFpsSample();
  juce::Rectangle<int> getFpsOverlayBounds() const;
  void drawFpsOverlay(juce::Graphics &g);
  void timerCallback() override;
  void drawStretchGuides(juce::Graphics &g);
  juce::Rectangle<int> getNoteDirtyBounds(const Note &note) const;
  void drawStaticPianoLayer(juce::Graphics &g,
                            const juce::Rectangle<int> &mainArea);
  void drawStaticPianoContentDirect(juce::Graphics &g,
                                    const juce::Rectangle<int> &mainArea);
  void invalidateStaticPianoLayer();
  bool isStaticPianoLayerValid(const juce::Rectangle<int> &mainArea) const;
  void rebuildStaticPianoLayer(const juce::Rectangle<int> &mainArea);
  static double getStaticLayerRenderScrollX(double sourceScrollX);
  void rebuildDragOverlayCache();
  void drawDragOverlay(juce::Graphics &g);
  void drawDragPitchOverlay(juce::Graphics &g);
  juce::Rectangle<int> getDragPitchDirtyBounds() const;
  juce::Rectangle<int> getHorizontalScrollDirtyBounds() const;
  bool isRenderProfilingEnabled();
  void recordRenderProfilePaint(juce::int64 startTicks,
                                const juce::Rectangle<int> &clipBounds,
                                bool interactivePaint,
                                bool fpsOverlayOnly);
  void recordRenderProfileStaticLayerDraw(double elapsedMs);
  void recordRenderProfileDynamicOverlay(double elapsedMs);
  void recordRenderProfileStaticLayerCache(bool cacheHit);
  void recordRenderProfileStaticContentSections(double backgroundMs,
                                                double gridMs,
                                                double notesMs,
                                                double pitchMs);
  void recordRenderProfileStaticLayerRebuild(double totalMs,
                                             double backgroundMs,
                                             double gridMs,
                                             double notesMs,
                                             double pitchMs);
  void flushRenderProfileIfNeeded(juce::int64 nowTicks);
  void resetRenderProfileWindow(juce::int64 nowTicks);
  static double ticksToMs(juce::int64 ticks);

  float midiToY(float midiNote) const;
  float yToMidi(float y) const;
  float timeToX(double time) const;
  double xToTime(float x) const;

  Note *findNoteAt(float x, float y);
  void updateScrollBars();
  void rebuildWaveformPeakCacheIfNeeded(const AudioData &audioData);
  float getWaveformPeakForSampleRange(const AudioData &audioData,
                                      int startSample,
                                      int endSample) const;
  void updateBasePitchCacheIfNeeded();
  void reapplyBasePitchForNote(
      Note *note); // Recalculate F0 from base pitch + delta after undo/redo
  void prepareDragBasePreview();
  void restoreDragBasePreview();
  struct StretchBoundary {
    Note *left = nullptr;
    Note *right = nullptr;
    int frame = 0;
  };

  struct StretchDragState {
    bool active = false;
    bool changed = false;
    StretchBoundary boundary;
    int originalBoundary = 0;
    int originalLeftStart = 0;
    int originalLeftEnd = 0;
    int originalRightStart = 0;
    int originalRightEnd = 0;
    int rangeStartFull = 0;
    int rangeEndFull = 0;
    int rangeStart = 0;
    int rangeEnd = 0;
    int minFrame = 0;
    int maxFrame = 0;
    int currentBoundary = 0;
    std::vector<float> leftDelta;
    std::vector<float> rightDelta;
    std::vector<bool> leftVoiced;
    std::vector<bool> rightVoiced;
    std::vector<float> originalLeftClip;
    std::vector<float> originalRightClip;
    std::vector<float> originalLeftHarmonicClip;
    std::vector<float> originalRightHarmonicClip;
    std::vector<float> originalLeftNoiseClip;
    std::vector<float> originalRightNoiseClip;
    std::vector<std::vector<float>> originalMelRangeFull;
    std::vector<float> originalDeltaRangeFull;
    std::vector<bool> originalVoicedRangeFull;
  };

  std::vector<StretchBoundary> collectStretchBoundaries() const;
  int findStretchBoundaryIndex(float worldX, float tolerancePx) const;
  void startStretchDrag(const StretchBoundary &boundary);
  void updateStretchDrag(int targetFrame);
  void finishStretchDrag();
  void cancelStretchDrag();

  // Pitch drawing helpers
  void applyPitchDrawing(float x, float y, PitchDrawingTarget target,
                         bool resetToReference);
  void commitPitchDrawing();
  void applyPitchPoint(int frameIndex, int midiCents,
                       PitchDrawingTarget target, bool resetToReference);
  void startNewPitchCurve(int frameIndex, int midiCents);

  Project *project = nullptr;
  PitchUndoManager *undoManager = nullptr;

  // New modular components
  std::unique_ptr<CoordinateMapper> coordMapper;
  std::unique_ptr<PianoRollRenderer> renderer;
  std::unique_ptr<ScrollZoomController> scrollZoomController;
  std::unique_ptr<PitchEditor> pitchEditor;
  std::unique_ptr<BoxSelector> boxSelector;
  std::unique_ptr<NoteSplitter> noteSplitter;

  float pixelsPerSecond = DEFAULT_PIXELS_PER_SECOND;
  float pixelsPerSemitone = DEFAULT_PIXELS_PER_SEMITONE;

  double cursorTime = 0.0;
  double scrollX = 0.0;
  double scrollY = 0.0;

  // Layout constants
  static constexpr int pianoKeysWidth = 60;
  static constexpr int timelineHeight = 24;
  static constexpr int loopTimelineHeight = 16;
  static constexpr int headerHeight = timelineHeight + loopTimelineHeight;

  // Edit mode
  EditMode editMode = EditMode::Select;

  // View settings
  bool showDeltaPitch = true;
  bool showBasePitch = false;
  bool showSomeSegmentsDebug = false;
  bool showSomeValuesDebug = false;
  bool showUvInterpolationDebug = false;
  bool showActualF0Debug = false;
  bool showFpsOverlay = false;
  bool showBackgroundWaveform = false;

  static constexpr int fpsHistorySize = 120;
  std::array<float, fpsHistorySize> fpsHistory{};
  int fpsHistoryIndex = 0;
  int fpsHistoryCount = 0;
  float currentFps = 0.0f;
  double lastFpsSampleTimeMs = 0.0;

  // Dragging state
  bool isDragging = false;
  Note *draggedNote = nullptr;
  float dragStartY = 0.0f;
  float originalPitchOffset = 0.0f;
  float originalMidiNote = 60.0f; // Original MIDI note before drag
  float boundaryF0Start =
      0.0f; // F0 value before note start (for smooth transition)
  float boundaryF0End = 0.0f; // F0 value after note end (for smooth transition)
  std::vector<float> originalF0Values; // F0 values before drag for undo
  juce::Rectangle<int> lastPaintedDragBounds;
  juce::Rectangle<int> lastPaintedPitchBounds;
  juce::Image dragOverlayImage;
  juce::Rectangle<int> dragOverlaySourceBounds;
  struct DragPitchPoint {
    float x = 0.0f;
    float midi = 0.0f;
    float weight = 0.0f;
  };
  std::vector<DragPitchPoint> dragPitchPoints;
  int dragPreviewStartFrame = -1;
  int dragPreviewEndFrame = -1;
  float dragPreviewMinMidi = 0.0f;
  float dragPreviewMaxMidi = 0.0f;
  std::vector<float> dragPreviewWeights;
  std::vector<float> dragBasePitchSnapshot;
  std::vector<float> dragF0Snapshot;

  // Delta pitch scale drag state (handle below selected note outline)
  bool isDeltaScaleDragging = false;
  float deltaScaleDragStartY = 0.0f;
  float deltaScaleFactor = 1.0f;
  int deltaScaleMinFrame = std::numeric_limits<int>::max();
  int deltaScaleMaxFrame = std::numeric_limits<int>::min();
  std::vector<Note *> deltaScaleTargetNotes;
  std::vector<F0FrameEdit> deltaScaleEdits;

  // Delta pitch offset drag state (vertical shift of delta curve)
  bool isDeltaOffsetDragging = false;
  float deltaOffsetDragStartY = 0.0f;
  float deltaOffsetSemitones = 0.0f;
  int deltaOffsetMinFrame = std::numeric_limits<int>::max();
  int deltaOffsetMaxFrame = std::numeric_limits<int>::min();
  std::vector<Note *> deltaOffsetTargetNotes;
  std::vector<F0FrameEdit> deltaOffsetEdits;

  // Pitch drawing state
  bool isDrawing = false;
  bool isPendingDraw = false;
  PitchDrawingTarget drawingTarget = PitchDrawingTarget::Output;
  bool pendingDrawReset = false;
  float pendingDrawStartX = 0.0f;
  float pendingDrawStartY = 0.0f;
  std::vector<F0FrameEdit> drawingEdits; // unique edits per frame
  std::unordered_map<int, size_t> drawingEditIndexByFrame;
  std::vector<SvcPitchFrameEdit> svcDrawingEdits;
  std::unordered_map<int, size_t> svcDrawingEditIndexByFrame;
  int lastDrawFrame = -1;
  int lastDrawValueCents = 0;
  DrawCurve *activeDrawCurve = nullptr;
  std::deque<std::unique_ptr<DrawCurve>> drawCurves;

  // Split mode guide line
  float splitGuideX =
      -1.0f; // World X coordinate for split guide line (-1 = hidden)
  Note *splitGuideNote = nullptr; // Note being hovered for split

  // Stretch mode state
  StretchDragState stretchDrag;
  int hoveredStretchBoundaryIndex = -1;
  static constexpr float stretchHandleHitPadding = 6.0f;
  static constexpr int minStretchNoteFrames = 3;
  std::unique_ptr<CenteredMelSpectrogram> centeredMelComputer;

  // Loop range drag state
  enum class LoopDragMode {
    None,
    Create,
    ResizeStart,
    ResizeEnd,
    Move
  };
  LoopDragMode loopDragMode = LoopDragMode::None;
  float loopDragStartX = 0.0f;
  double loopDragStartSeconds = 0.0;
  double loopDragEndSeconds = 0.0;
  double loopDragAnchorSeconds = 0.0;
  double loopDragOriginalStart = 0.0;
  double loopDragOriginalEnd = 0.0;
  static constexpr float loopHandleHitPadding = 6.0f;

  // Scrollbars
  juce::ScrollBar horizontalScrollBar{false};
  juce::ScrollBar verticalScrollBar{true};

  // Waveform cache for performance
  juce::Image waveformCache;
  double cachedScrollX = -1.0;
  double cachedWaveformBucketScrollX = -1.0;
  float cachedPixelsPerSecond = -1.0f;
  int cachedWidth = 0;
  int cachedHeight = 0;

  struct WaveformPeakCache {
    std::vector<float> peaks;
    std::vector<float> coarsePeaks;
    int samplesPerPeak = 256;
    int peaksPerCoarsePeak = 16;
    int sourceNumSamples = 0;
    int sourceSampleRate = 0;
    int sourceNumChannels = 0;
    bool valid = false;
  } waveformPeakCache;

  juce::Image staticPianoLayer;
  static constexpr double staticLayerBucketPx = 256.0;
  static constexpr int staticLayerOverscanPx = 384;
  static constexpr int staticLayerWidthBucketPx = 512;
  bool staticPianoLayerValid = false;
  int staticPianoLayerWidth = 0;
  int staticPianoLayerHeight = 0;
  double staticPianoLayerScrollX = -1.0;
  double staticPianoLayerScrollY = -1.0;
  float staticPianoLayerPixelsPerSecond = -1.0f;
  float staticPianoLayerPixelsPerSemitone = -1.0f;
  bool staticPianoLayerShowBackgroundWaveform = false;
  bool staticPianoLayerShowDeltaPitch = false;
  bool staticPianoLayerShowBasePitch = false;
  bool staticPianoLayerShowDebug = false;
  bool staticPianoLayerInteractivePitch = false;
  Note *staticPianoLayerSkippedDragNote = nullptr;
  bool skipDraggedNoteInStaticLayer = false;
  bool renderingDirectStaticPianoContent = false;

  // Scratch buffers reused across paints to avoid allocator churn while
  // rebuilding the CPU-backed static layer.
  std::vector<Note *> noteRenderVisibleNotes;
  std::vector<const Note *> pitchRenderVisibleNotes;
  std::vector<float> noteWaveValues;
  std::vector<float> noteSmoothedWaveValues;
  std::vector<float> noteLowDetailPeaks;
  juce::Path noteWaveformPath;
  juce::Path noteWaveformOutlinePath;
  juce::Path noteLowDetailWaveformPath;
  juce::Path pitchRenderPath;
  juce::Path pitchActualPath;
  juce::Path pitchBasePath;
  juce::Path pitchSvcPath;
  juce::Path pitchDashedPath;

  struct RenderProfileStats {
    juce::int64 windowStartTicks = 0;
    int paintCount = 0;
    int interactivePaintCount = 0;
    int fpsOverlayPaintCount = 0;
    int staticLayerHits = 0;
    int staticLayerMisses = 0;
    int staticLayerRebuilds = 0;
    int backgroundCalls = 0;
    int gridCalls = 0;
    int notesCalls = 0;
    int pitchCalls = 0;
    int staticLayerDrawCalls = 0;
    int staticDirectDraws = 0;
    int dynamicOverlayCalls = 0;
    double paintTotalMs = 0.0;
    double paintMaxMs = 0.0;
    double staticLayerDrawTotalMs = 0.0;
    double staticLayerDrawMaxMs = 0.0;
    double staticLayerRebuildTotalMs = 0.0;
    double staticLayerRebuildMaxMs = 0.0;
    double backgroundTotalMs = 0.0;
    double gridTotalMs = 0.0;
    double notesTotalMs = 0.0;
    double pitchTotalMs = 0.0;
    double dynamicOverlayTotalMs = 0.0;
    double dynamicOverlayMaxMs = 0.0;
    juce::Rectangle<int> lastClipBounds;
  } renderProfileStats;
  bool renderProfilingInitialized = false;
  bool renderProfilingEnabled = false;

  // Base pitch curve cache for performance
  // Only recalculates when notes change, not on every repaint
  std::vector<float> cachedBasePitch;
  size_t cachedNoteCount = 0;
  int cachedTotalFrames = 0;
  bool cacheInvalidated = true; // Start invalidated, force first calculation

public:
  void invalidateBasePitchCache() {
    cacheInvalidated = true;
    cachedNoteCount = 0;
    cachedBasePitch.clear();
    cachedBasePitch.shrink_to_fit(); // Release memory
  }

private:
  // Optional: disable base pitch rendering for performance testing
  static constexpr bool ENABLE_BASE_PITCH_DEBUG =
      true; // Set to false to disable

  // Mouse drag throttling
  juce::int64 lastDragRepaintTime = 0;
  static constexpr juce::int64 minDragRepaintInterval = 16; // ~60fps max
  juce::int64 lastStretchPreviewTime = 0;
  static constexpr juce::int64 minStretchPreviewInterval = 120;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoRollComponent)
};
