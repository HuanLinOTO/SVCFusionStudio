#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/Constants.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

/**
 * Manages audio file loading, saving, and export operations.
 * Handles async file operations with progress callbacks.
 */
class AudioFileManager {
public:
  using ProgressCallback =
      std::function<void(double progress, const juce::String &message)>;
  using LoadCompleteCallback =
      std::function<void(juce::AudioBuffer<float> &&buffer, int sampleRate,
                         const juce::File &file)>;
  using ExportCompleteCallback = std::function<void(bool success)>;

  AudioFileManager();
  ~AudioFileManager();

  // File dialogs
  void showOpenDialog(std::function<void(const juce::File &)> onFileSelected);
  void showSaveDialog(const juce::File &defaultPath,
                      std::function<void(const juce::File &)> onFileSelected);
  void showExportDialog(const juce::File &defaultPath,
                        std::function<void(const juce::File &)> onFileSelected);

  // Async file operations
  void loadAudioFileAsync(const juce::File &file, ProgressCallback onProgress,
                          LoadCompleteCallback onComplete);

  void exportAudioFileAsync(const juce::File &file,
                            const juce::AudioBuffer<float> &buffer,
                            int sampleRate, ProgressCallback onProgress,
                            ExportCompleteCallback onComplete);

  // State
  bool isLoading() const { return isLoadingAudio.load(); }
  void cancelLoading();

  // Drag and drop support
  static bool isInterestedInFileDrag(const juce::StringArray &files);
  static juce::File getFirstAudioFile(const juce::StringArray &files);

private:
  struct LoadTask {
    juce::File file;
    ProgressCallback onProgress;
    LoadCompleteCallback onComplete;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
  };

  // Resample audio to target sample rate
  static juce::AudioBuffer<float>
  resampleIfNeeded(const juce::AudioBuffer<float> &buffer, int srcSampleRate,
                   int targetSampleRate);

  // Convert stereo to mono
  static juce::AudioBuffer<float>
  convertToMono(const juce::AudioBuffer<float> &stereoBuffer);

  std::unique_ptr<juce::FileChooser> fileChooser;
  std::thread workerThread;
  std::atomic<bool> isLoadingAudio{false};
  std::atomic<bool> isShuttingDown{false};

  std::mutex queueMutex;
  std::condition_variable queueCv;
  std::deque<LoadTask> loadQueue;

  // Current task cancel flag (if any)
  std::shared_ptr<std::atomic<bool>> currentCancelFlag;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioFileManager)
  JUCE_DECLARE_WEAK_REFERENCEABLE(AudioFileManager)
};
