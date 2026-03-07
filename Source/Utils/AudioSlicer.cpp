/**
 * AudioSlicer.cpp -- RMS-based audio slicer for chunked SVC inference.
 *
 * Faithfully ports the openvpi/audio-slicer algorithm used in SVCFusion
 * Python (ddspsvc_6_3/slicer.py) so that long audio can be processed
 * in manageable chunks rather than a single giant tensor.
 */

#include "AudioSlicer.h"
#include "AppLogger.h"

#include <algorithm>
#include <cmath>
#include <numeric>

// ═══════════════════════════════════════════════════════════════════════
// RMS computation  (matches Python  get_rms  but without padding)
// ═══════════════════════════════════════════════════════════════════════

std::vector<float> AudioSlicer::computeRMS(
    const float* audio, int numSamples, int hopSize, int winSize)
{
    if (numSamples <= 0 || hopSize <= 0 || winSize <= 0)
        return {};

    int numFrames = (numSamples + hopSize - 1) / hopSize;
    std::vector<float> rms(numFrames, 0.f);
    int halfWin = winSize / 2;

    for (int f = 0; f < numFrames; ++f) {
        int center = f * hopSize;
        int start  = std::max(0, center - halfWin);
        int end    = std::min(numSamples, center + halfWin);
        int n      = end - start;
        if (n <= 0) continue;

        float sumSq = 0.f;
        for (int i = start; i < end; ++i)
            sumSq += audio[i] * audio[i];
        rms[f] = std::sqrt(sumSq / static_cast<float>(n));
    }

    return rms;
}

// ═══════════════════════════════════════════════════════════════════════
// Slicer -- based on openvpi/audio-slicer (SVCFusion Python version)
// ═══════════════════════════════════════════════════════════════════════

std::vector<AudioSegment> AudioSlicer::slice(
    const float* audio, int numSamples, int sampleRate, int modelHopSize,
    float thresholdDb, int minLengthMs, int minIntervalMs,
    int hopSizeMs, int maxSilKeptMs)
{
    if (numSamples <= 0 || sampleRate <= 0 || modelHopSize <= 0)
        return {};

    // ── Convert parameters from ms to samples / slicer-frames ──
    float threshold = std::pow(10.f, thresholdDb / 20.f);

    int hopSize = std::max(1,
        static_cast<int>(std::round(sampleRate * hopSizeMs / 1000.0)));

    int minIntervalSamples = static_cast<int>(
        sampleRate * minIntervalMs / 1000.0);
    int winSize = std::min(minIntervalSamples, 4 * hopSize);
    if (winSize < 1) winSize = hopSize;

    // In slicer-frame units
    int minLength   = std::max(1, static_cast<int>(std::round(
        sampleRate * minLengthMs / 1000.0 / hopSize)));
    int minInterval = std::max(1, static_cast<int>(std::round(
        static_cast<double>(minIntervalSamples) / hopSize)));
    int maxSilKept  = std::max(1, static_cast<int>(std::round(
        sampleRate * maxSilKeptMs / 1000.0 / hopSize)));

    // ── Compute RMS ──
    auto rms = computeRMS(audio, numSamples, hopSize, winSize);
    int totalFrames = static_cast<int>(rms.size());
    if (totalFrames == 0) return {};

    // Helper: find frame with minimum RMS in [from..to] (inclusive)
    auto argmin = [&](int from, int to) -> int {
        from = std::max(0, from);
        to   = std::min(totalFrames - 1, to);
        int best = from;
        for (int j = from + 1; j <= to; ++j)
            if (rms[j] < rms[best]) best = j;
        return best;
    };

    // ── Find silence cut-points (sil_tags) ──
    // Each tag is (cutStart, cutEnd) in slicer-frame units.
    // Regions [cutStart .. cutEnd] are removed (silence gaps).
    std::vector<std::pair<int,int>> silTags;
    int silenceStart = -1;
    int clipStart    = 0;

    for (int i = 0; i < totalFrames; ++i) {
        if (rms[i] < threshold) {
            if (silenceStart < 0)
                silenceStart = i;
            continue;
        }

        // Non-silent frame
        if (silenceStart < 0)
            continue;

        bool isLeading = (silenceStart == 0 && clipStart == 0);
        bool needSlice = (i - silenceStart >= minInterval &&
                          i - clipStart    >= minLength);

        if (!isLeading && !needSlice) {
            silenceStart = -1;
            continue;
        }

        int silLen = i - silenceStart;

        if (silLen <= maxSilKept) {
            // Short silence: find quietest point in entire silence
            int pos = argmin(silenceStart, i);
            silTags.push_back(isLeading
                ? std::pair<int,int>{0, pos}
                : std::pair<int,int>{pos, pos});
            clipStart = pos;
        }
        else if (silLen <= maxSilKept * 2) {
            // Medium silence
            int posR = argmin(i - maxSilKept, i);
            if (isLeading) {
                silTags.push_back({0, posR});
            } else {
                int posL = argmin(silenceStart, silenceStart + maxSilKept);
                silTags.push_back({posL, posR});
            }
            clipStart = posR;
        }
        else {
            // Long silence: find positions at both ends
            int posL = argmin(silenceStart, silenceStart + maxSilKept);
            int posR = argmin(i - maxSilKept, i);
            silTags.push_back(isLeading
                ? std::pair<int,int>{0, posR}
                : std::pair<int,int>{posL, posR});
            clipStart = posR;
        }

        silenceStart = -1;
    }

    // Handle trailing silence
    if (silenceStart >= 0 && totalFrames - silenceStart >= minInterval) {
        int searchEnd = std::min(totalFrames - 1, silenceStart + maxSilKept);
        int pos = argmin(silenceStart, searchEnd);
        silTags.push_back({pos, totalFrames});
    }

    // ── Build segments from sil_tags ──
    // Segments are the voiced regions between cut-points.
    std::vector<std::pair<int,int>> chunkFrames; // (startSlicerFrame, endSlicerFrame)

    if (silTags.empty()) {
        chunkFrames.push_back({0, totalFrames});
    } else {
        if (silTags[0].first > 0)
            chunkFrames.push_back({0, silTags[0].first});

        for (size_t i = 0; i + 1 < silTags.size(); ++i) {
            int segStart = silTags[i].second;
            int segEnd   = silTags[i + 1].first;
            if (segEnd > segStart)
                chunkFrames.push_back({segStart, segEnd});
        }

        if (silTags.back().second < totalFrames)
            chunkFrames.push_back({silTags.back().second, totalFrames});
    }

    // ── Convert slicer-frame segments to sample-aligned AudioSegments ──
    std::vector<AudioSegment> result;
    for (auto& [sf, ef] : chunkFrames) {
        int startSample = sf * hopSize;
        int endSample   = std::min(ef * hopSize, numSamples);
        if (endSample <= startSample) continue;

        // Align startSample to model hop boundary
        int startFrame  = startSample / modelHopSize;
        startSample     = startFrame * modelHopSize;

        result.push_back({startFrame, startSample, endSample - startSample});
    }

    LOG("AudioSlicer: " + juce::String(numSamples) + " samples -> " +
        juce::String(result.size()) + " segments" +
        " (slicer hop=" + juce::String(hopSize) +
        " win=" + juce::String(winSize) +
        " thresh=" + juce::String(threshold, 6) + ")");

    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// Cross-fade  (matches Python  cross_fade  exactly)
// ═══════════════════════════════════════════════════════════════════════

std::vector<float> AudioSlicer::crossFade(
    const std::vector<float>& a,
    const std::vector<float>& b,
    int idx)
{
    if (idx < 0) idx = 0;
    int aLen = static_cast<int>(a.size());
    int bLen = static_cast<int>(b.size());

    int resultLen = idx + bLen;
    int fadeLen   = aLen - idx;

    if (fadeLen <= 0) {
        // No overlap: concatenate with possible zero-gap
        std::vector<float> result(std::max(resultLen, aLen), 0.f);
        std::copy(a.begin(), a.end(), result.begin());
        std::copy(b.begin(), b.end(), result.begin() + idx);
        return result;
    }

    std::vector<float> result(resultLen);

    // Part before overlap: copy from a
    std::copy(a.begin(), a.begin() + idx, result.begin());

    // Overlap region: linear cross-fade
    int overlapLen = std::min(fadeLen, bLen);
    for (int i = 0; i < overlapLen; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(fadeLen);
        result[idx + i] = (1.f - t) * a[idx + i] + t * b[i];
    }

    // Remaining part of b after overlap
    if (fadeLen < bLen)
        std::copy(b.begin() + fadeLen, b.end(), result.begin() + aLen);

    return result;
}
