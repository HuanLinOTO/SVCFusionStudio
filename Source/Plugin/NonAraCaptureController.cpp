#include "NonAraCaptureController.h"

#include <algorithm>
#include <cmath>

void NonAraCaptureController::prepare(double sampleRate, int numChannels,
                                      int maxCaptureSeconds) {
  juce::ignoreUnused(sampleRate);

  const int maxSamples = static_cast<int>(sampleRate * maxCaptureSeconds);

  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    captureBuffer.setSize(numChannels, maxSamples);
    captureBuffer.clear();
    capturePosition = 0;
    finalLength = 0;
    stopDebounceBlocks = 0;
  }

  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  state.store(State::WaitingForAudio);
}

void NonAraCaptureController::resetToWaiting() {
  const juce::SpinLock::ScopedLockType lock(bufferLock);
  captureBuffer.clear();
  capturePosition = 0;
  finalLength = 0;
  stopDebounceBlocks = 0;
  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  state.store(State::WaitingForAudio);
}

void NonAraCaptureController::processBlock(
    const juce::AudioBuffer<float> &input, bool hostIsPlaying) {
  if (analysisPending.load())
    return;

  auto currentState = state.load();

  // Wait for audio
  if (currentState == State::WaitingForAudio && hostIsPlaying) {
    float maxLevel = 0.0f;
    for (int ch = 0; ch < input.getNumChannels(); ++ch) {
      const float *data = input.getReadPointer(ch);
      for (int i = 0; i < input.getNumSamples(); ++i)
        maxLevel = std::max(maxLevel, std::abs(data[i]));
    }

    if (maxLevel > audioThreshold) {
      const juce::SpinLock::ScopedLockType lock(bufferLock);
      capturePosition = 0;
      stopDebounceBlocks = 0;
      state.store(State::Capturing);
      shouldFinalizeFlag.store(false);
      return;
    }
  }

  currentState = state.load();

  if (currentState != State::Capturing)
    return;

  if (hostIsPlaying) {
    stopDebounceBlocks = 0;

    const juce::SpinLock::ScopedLockType lock(bufferLock);
    int spaceLeft = captureBuffer.getNumSamples() - capturePosition;
    int toCopy = std::min(input.getNumSamples(), spaceLeft);

    if (toCopy > 0) {
      int channelsToCopy =
          std::min(input.getNumChannels(), captureBuffer.getNumChannels());
      for (int ch = 0; ch < channelsToCopy; ++ch)
        captureBuffer.copyFrom(ch, capturePosition, input, ch, 0, toCopy);
      capturePosition += toCopy;
    }

    if (capturePosition >= captureBuffer.getNumSamples()) {
      shouldFinalizeFlag.store(true);
    }
  } else {
    ++stopDebounceBlocks;
    if (stopDebounceBlocks >= kStopDebounceBlocks)
      shouldFinalizeFlag.store(true);
  }
}

bool NonAraCaptureController::finalizeCapture(double hostSampleRate,
                                              FinalizeResult &out) {
  if (state.load() != State::Capturing)
    return false;

  const int minSamples = static_cast<int>(hostSampleRate * minCaptureSeconds);

  int captured = 0;
  int channels = 0;
  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    captured = capturePosition;
    channels = captureBuffer.getNumChannels();
  }

  if (captured < minSamples) {
    resetToWaiting();
    return false;
  }

  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    finalLength = capturePosition;
  }

  analysisPending.store(true);
  shouldFinalizeFlag.store(false);
  state.store(State::Complete);

  out.numChannels = channels;
  out.numSamples = finalLength;
  out.sampleRate = hostSampleRate;
  return true;
}

juce::AudioBuffer<float>
NonAraCaptureController::copyCapturedAudio(int numSamples) const {
  juce::AudioBuffer<float> trimmed;

  const juce::SpinLock::ScopedLockType lock(bufferLock);
  int length = std::min(numSamples, captureBuffer.getNumSamples());
  trimmed.setSize(captureBuffer.getNumChannels(), length);
  for (int ch = 0; ch < captureBuffer.getNumChannels(); ++ch)
    trimmed.copyFrom(ch, 0, captureBuffer, ch, 0, length);

  return trimmed;
}

void NonAraCaptureController::onAnalysisDispatched() {
  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  stopDebounceBlocks = 0;
  state.store(State::WaitingForAudio);
}

void NonAraCaptureController::stop() {
  {
    const juce::SpinLock::ScopedLockType lock(bufferLock);
    captureBuffer.clear();
    capturePosition = 0;
    finalLength = 0;
    stopDebounceBlocks = 0;
  }
  analysisPending.store(false);
  shouldFinalizeFlag.store(false);
  state.store(State::Idle);
}
