#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Models/Track.h"
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

/**
 * Audio engine for playback and synthesis.
 */
class AudioEngine : public juce::AudioSource, public juce::ChangeListener {
public:
  AudioEngine();
  ~AudioEngine() override;

  // AudioSource interface
  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
  void releaseResources() override;
  void
  getNextAudioBlock(const juce::AudioSourceChannelInfo &bufferToFill) override;

  // ChangeListener
  void changeListenerCallback(juce::ChangeBroadcaster *source) override;

  // Playback control
  void setProject(Project *proj) { project = proj; }
  void loadWaveform(const juce::AudioBuffer<float> &buffer, int sampleRate,
                    bool preservePosition = false);

  // Multi-track mixing
  void setTracks(const std::vector<Track*> &newTracks) { tracks = newTracks; }
  const std::vector<Track*>& getTracks() const { return tracks; }
  void rebuildMixedWaveform(bool preservePosition = false);

  bool play();
  void pause();
  void stop();
  void seek(double timeSeconds);

  // Loop control (seconds)
  void setLoopRange(double startSeconds, double endSeconds);
  void setLoopEnabled(bool enabled);
  void clearLoopRange();
  bool isLoopEnabled() const { return loopEnabled.load(); }

  bool isPlaying() const { return playing; }
  double getPosition() const; // Returns position in seconds
  double getDuration() const;

  // Access to current (mixed) waveform for export
  const juce::AudioBuffer<float>& getCurrentWaveform() const { return currentWaveform; }
  int getWaveformSampleRate() const { return waveformSampleRate; }

  // Callbacks
  using PositionCallback = std::function<void(double)>;
  using FinishCallback = std::function<void()>;

  void setPositionCallback(PositionCallback callback) {
    std::atomic_store(&positionCallback,
                      std::make_shared<PositionCallback>(std::move(callback)));
  }
  void setFinishCallback(FinishCallback callback) {
    std::atomic_store(&finishCallback,
                      std::make_shared<FinishCallback>(std::move(callback)));
  }
  void clearCallbacks() {
    std::atomic_store(&positionCallback, std::shared_ptr<PositionCallback>{});
    std::atomic_store(&finishCallback, std::shared_ptr<FinishCallback>{});
  }

  // Audio device management
  juce::AudioDeviceManager &getDeviceManager() { return deviceManager; }
  void initializeAudio();
  void shutdownAudio();

  // Volume control (dB, -60 to +12)
  void setVolumeDb(float dB);
  float getVolumeDb() const;

private:
  struct PositionUpdateState {
    std::atomic<double> latestSeconds{0.0};
    std::atomic<bool> callbackPending{false};
  };

  juce::AudioDeviceManager deviceManager;
  juce::AudioSourcePlayer audioSourcePlayer;

  Project *project = nullptr;
  std::vector<Track*> tracks;
  juce::AudioBuffer<float> currentWaveform;
  int waveformSampleRate = 44100;

  std::atomic<int64_t> currentPosition{0}; // Position in waveform samples
  std::atomic<bool> playing{false};
  std::atomic<bool> shouldStop{false};

  std::shared_ptr<PositionCallback> positionCallback;
  std::shared_ptr<FinishCallback> finishCallback;

  std::shared_ptr<PositionUpdateState> positionUpdateState =
      std::make_shared<PositionUpdateState>();

  double currentSampleRate = 44100.0;

  // For sample rate conversion
  juce::LagrangeInterpolator interpolator;
  double playbackRatio = 1.0;      // waveformSampleRate / deviceSampleRate
  double fractionalPosition = 0.0; // Sub-sample position for interpolation

  // Thread safety for waveform updates
  juce::SpinLock waveformLock;

  // Volume control (linear gain, lock-free for audio thread)
  std::atomic<float> volumeGain{1.0f};

  // Loop range (in waveform samples)
  std::atomic<bool> loopEnabled{false};
  std::atomic<int64_t> loopStartSample{0};
  std::atomic<int64_t> loopEndSample{0};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
