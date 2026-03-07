#include "AudioEngine.h"
#include "../Utils/AppLogger.h"
#include <algorithm>

AudioEngine::AudioEngine() {}

AudioEngine::~AudioEngine() { shutdownAudio(); }

void AudioEngine::initializeAudio() {
  // Initialize audio device
  auto result = deviceManager.initialiseWithDefaultDevices(
      0, 2); // No input, stereo output

  if (result.isNotEmpty()) {
    LOG("AudioEngine: device initialization FAILED: " + result);
  } else {
    auto *device = deviceManager.getCurrentAudioDevice();
    if (device) {
      LOG("AudioEngine: device OK -- name=" + device->getName() +
          " sr=" + juce::String(device->getCurrentSampleRate()) +
          " bufSize=" + juce::String(device->getCurrentBufferSizeSamples()));
    } else {
      LOG("AudioEngine: device init succeeded but getCurrentAudioDevice() returned null!");
    }
  }

  deviceManager.addAudioCallback(&audioSourcePlayer);
  audioSourcePlayer.setSource(this);
}

void AudioEngine::shutdownAudio() {
  audioSourcePlayer.setSource(nullptr);
  deviceManager.removeAudioCallback(&audioSourcePlayer);
  deviceManager.closeAudioDevice();
}

void AudioEngine::prepareToPlay(int samplesPerBlockExpected,
                                double sampleRate) {
  currentSampleRate = sampleRate;
  playbackRatio = static_cast<double>(waveformSampleRate) / sampleRate;
  interpolator.reset();
  fractionalPosition = 0.0;

  LOG("AudioEngine::prepareToPlay -- deviceSR=" + juce::String(sampleRate) +
      " waveformSR=" + juce::String(waveformSampleRate) +
      " ratio=" + juce::String(playbackRatio) +
      " blockSize=" + juce::String(samplesPerBlockExpected));
}

void AudioEngine::releaseResources() {}

void AudioEngine::getNextAudioBlock(
    const juce::AudioSourceChannelInfo &bufferToFill) {
  if (!playing || currentWaveform.getNumSamples() == 0) {
    bufferToFill.clearActiveBufferRegion();
    return;
  }

  const juce::SpinLock::ScopedTryLockType lock(waveformLock);
  if (!lock.isLocked()) {
    // Waveform is being updated, output silence to avoid glitches
    bufferToFill.clearActiveBufferRegion();
    return;
  }

  auto *outputBuffer = bufferToFill.buffer;
  auto numOutputSamples = bufferToFill.numSamples;
  auto startSample = bufferToFill.startSample;

  int64_t pos = currentPosition.load();
  int64_t waveformLength = currentWaveform.getNumSamples();

  bool loopActive = loopEnabled.load();
  int64_t loopStart = loopStartSample.load();
  int64_t loopEnd = loopEndSample.load();

  if (loopActive) {
    loopStart = juce::jlimit<int64_t>(0, waveformLength, loopStart);
    loopEnd = juce::jlimit<int64_t>(0, waveformLength, loopEnd);
    if (loopEnd <= loopStart)
      loopActive = false;
  }

  if (!loopActive && pos >= waveformLength) {
    bufferToFill.clearActiveBufferRegion();
    playing = false;

    // Schedule callback on message thread
    if (auto cb = std::atomic_load(&finishCallback))
      juce::MessageManager::callAsync([cb]() { (*cb)(); });
    return;
  }

  if (loopActive && pos >= loopEnd) {
    pos = loopStart;
    interpolator.reset();
  }

  // Use interpolator for sample rate conversion
  const float *inputData = currentWaveform.getReadPointer(0);
  float *outputData = outputBuffer->getWritePointer(0, startSample);

  outputBuffer->clear(startSample, numOutputSamples);

  int samplesRemaining = numOutputSamples;
  int writeOffset = 0;

  while (samplesRemaining > 0) {
    int64_t segmentEnd = loopActive ? loopEnd : waveformLength;
    int64_t inputAvailable = segmentEnd - pos;

    if (inputAvailable <= 0) {
      if (loopActive) {
        pos = loopStart;
        interpolator.reset();
        continue;
      }
      break;
    }

    int maxOutput = static_cast<int>(inputAvailable / playbackRatio);
    if (maxOutput <= 0) {
      if (loopActive) {
        pos = loopStart;
        interpolator.reset();
        continue;
      }
      break;
    }

    int outCount = std::min(samplesRemaining, maxOutput);
    int samplesUsed = interpolator.process(playbackRatio, inputData + pos,
                                           outputData + writeOffset, outCount,
                                           static_cast<int>(inputAvailable),
                                           0 // No wrap
    );

    pos += samplesUsed;
    samplesRemaining -= outCount;
    writeOffset += outCount;

    if (loopActive && pos >= loopEnd) {
      pos = loopStart;
      interpolator.reset();
    }
  }

  // Apply volume gain (lock-free read)
  float gain = volumeGain.load();
  if (std::abs(gain - 1.0f) > 0.0001f) // Only apply if not unity gain
  {
    juce::FloatVectorOperations::multiply(outputData, gain, numOutputSamples);
  }

  currentPosition.store(pos);

  // Copy to other channels (if stereo output)
  for (int ch = 1; ch < outputBuffer->getNumChannels(); ++ch) {
    outputBuffer->copyFrom(ch, startSample,
                           outputBuffer->getReadPointer(0, startSample),
                           numOutputSamples);
  }

  if (!loopActive && samplesRemaining > 0) {
    playing = false;
    if (auto cb = std::atomic_load(&finishCallback))
      juce::MessageManager::callAsync([cb]() { (*cb)(); });
  }

  // Update position callback
  if (auto cb = std::atomic_load(&positionCallback)) {
    auto state = positionUpdateState;
    state->latestSeconds.store(static_cast<double>(currentPosition.load()) /
                               waveformSampleRate);

    // Schedule at most one pending callback to avoid flooding the message
    // thread
    if (!state->callbackPending.exchange(true)) {
      juce::MessageManager::callAsync([cb, state]() {
        state->callbackPending.store(false);
        (*cb)(state->latestSeconds.load());
      });
    }
  }
}

void AudioEngine::changeListenerCallback(juce::ChangeBroadcaster *source) {}

void AudioEngine::loadWaveform(const juce::AudioBuffer<float> &buffer,
                               int sampleRate, bool preservePosition) {
  // Save playing state if we need to preserve it
  bool wasPlaying = playing.load();

  LOG("AudioEngine::loadWaveform -- samples=" + juce::String(buffer.getNumSamples()) +
      " sr=" + juce::String(sampleRate) +
      " preservePos=" + juce::String(preservePosition ? 1 : 0) +
      " wasPlaying=" + juce::String(wasPlaying ? 1 : 0) +
      " currentDeviceSR=" + juce::String(currentSampleRate));

  // Stop playback first to safely update waveform
  playing = false;

  {
    const juce::SpinLock::ScopedLockType lock(waveformLock);
    currentWaveform = buffer;
    waveformSampleRate = sampleRate;

    if (!preservePosition) {
      currentPosition.store(0);
      fractionalPosition = 0.0;
    }

    // Update playback ratio for sample rate conversion
    if (currentSampleRate > 0)
      playbackRatio =
          static_cast<double>(waveformSampleRate) / currentSampleRate;
    else
      playbackRatio = 1.0;

    interpolator.reset();
  }

  // Restore playing state if preserving position (e.g., during incremental
  // synthesis)
  if (preservePosition && wasPlaying) {
    playing = true;
    LOG("AudioEngine: Restored playback state after waveform update");
  }

  LOG("AudioEngine: waveform loaded -- currentWaveform.samples=" +
      juce::String(currentWaveform.getNumSamples()) +
      " playbackRatio=" + juce::String(playbackRatio) +
      " playing=" + juce::String(playing.load() ? 1 : 0));

  if (loopEnabled.load()) {
    auto loopStart = loopStartSample.load();
    auto loopEnd = loopEndSample.load();
    const int64_t waveformLength = currentWaveform.getNumSamples();
    loopStart = juce::jlimit<int64_t>(0, waveformLength, loopStart);
    loopEnd = juce::jlimit<int64_t>(0, waveformLength, loopEnd);
    loopStartSample.store(loopStart);
    loopEndSample.store(loopEnd);
    if (loopEnd <= loopStart)
      loopEnabled.store(false);
  }
}

bool AudioEngine::play() {
  int numSamples = currentWaveform.getNumSamples();
  if (numSamples == 0) {
    LOG("AudioEngine::play FAILED -- no waveform loaded (0 samples)");
    return false;
  }

  auto *device = deviceManager.getCurrentAudioDevice();
  LOG("AudioEngine::play -- samples=" + juce::String(numSamples) +
      " pos=" + juce::String(currentPosition.load()) +
      " deviceSR=" + juce::String(currentSampleRate) +
      " ratio=" + juce::String(playbackRatio) +
      " deviceOpen=" + juce::String(device != nullptr ? 1 : 0));
  playing = true;
  return true;
}

void AudioEngine::pause() { playing = false; }

void AudioEngine::stop() {
  playing = false;

  const juce::SpinLock::ScopedLockType lock(waveformLock);
  currentPosition.store(0);
  interpolator.reset();
  fractionalPosition = 0.0;
}

void AudioEngine::seek(double timeSeconds) {
  const juce::SpinLock::ScopedLockType lock(waveformLock);
  int64_t newPos = static_cast<int64_t>(timeSeconds * waveformSampleRate);
  newPos = juce::jlimit<int64_t>(0, currentWaveform.getNumSamples(), newPos);
  currentPosition.store(newPos);
  interpolator.reset();
  fractionalPosition = 0.0;
}

void AudioEngine::setLoopRange(double startSeconds, double endSeconds) {
  if (startSeconds > endSeconds)
    std::swap(startSeconds, endSeconds);

  const juce::SpinLock::ScopedLockType lock(waveformLock);
  const int64_t waveformLength = currentWaveform.getNumSamples();
  int64_t startSample =
      static_cast<int64_t>(startSeconds * waveformSampleRate);
  int64_t endSample = static_cast<int64_t>(endSeconds * waveformSampleRate);

  startSample = juce::jlimit<int64_t>(0, waveformLength, startSample);
  endSample = juce::jlimit<int64_t>(0, waveformLength, endSample);

  loopStartSample.store(startSample);
  loopEndSample.store(endSample);
  loopEnabled.store(endSample > startSample);
}

void AudioEngine::setLoopEnabled(bool enabled) {
  if (enabled && loopEndSample.load() <= loopStartSample.load())
    loopEnabled.store(false);
  else
    loopEnabled.store(enabled);
}

void AudioEngine::clearLoopRange() {
  loopEnabled.store(false);
  loopStartSample.store(0);
  loopEndSample.store(0);
}

double AudioEngine::getPosition() const {
  return static_cast<double>(currentPosition.load()) / waveformSampleRate;
}

double AudioEngine::getDuration() const {
  if (currentWaveform.getNumSamples() == 0)
    return 0.0;
  return static_cast<double>(currentWaveform.getNumSamples()) /
         waveformSampleRate;
}

void AudioEngine::setVolumeDb(float dB) {
  // Clamp dB to range: -12 dB to +12 dB (symmetric around 0)
  dB = juce::jlimit(-12.0f, 12.0f, dB);
  // Convert dB to linear gain: gain = 10^(dB/20)
  float gain = juce::Decibels::decibelsToGain(dB, -60.0f);
  volumeGain.store(gain);
}

float AudioEngine::getVolumeDb() const {
  float gain = volumeGain.load();
  return juce::Decibels::gainToDecibels(gain, -60.0f);
}
