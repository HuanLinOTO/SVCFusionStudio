#include "CurveResampler.h"

#include <algorithm>
#include <cmath>

namespace CurveResampler {
  std::vector<float> resampleLinear(const std::vector<float>& points,
                                    int targetLength) {
    if (targetLength <= 0)
      return {};
    if (points.empty())
      return std::vector<float>(static_cast<size_t>(targetLength), 0.0f);
    if (targetLength == 1)
      return {points.front()};
    if (points.size() == 1)
      return std::vector<float>(static_cast<size_t>(targetLength), points.front());

    const float tMax = static_cast<float>(points.size() - 1);
    std::vector<float> out(static_cast<size_t>(targetLength), 0.0f);
    for (int i = 0; i < targetLength; ++i) {
      const float t = tMax * static_cast<float>(i) /
                      static_cast<float>(targetLength - 1);
      const int idx0 = std::clamp(static_cast<int>(std::floor(t)), 0,
                                  static_cast<int>(points.size() - 1));
      const int idx1 =
          std::min(idx0 + 1, static_cast<int>(points.size() - 1));
      const float frac = t - static_cast<float>(idx0);
      const float v0 = points[static_cast<size_t>(idx0)];
      const float v1 = points[static_cast<size_t>(idx1)];
      out[static_cast<size_t>(i)] = v0 + (v1 - v0) * frac;
    }
    return out;
  }

  std::vector<bool> resampleNearest(const std::vector<bool>& points,
                                    int targetLength) {
    if (targetLength <= 0)
      return {};
    if (points.empty())
      return std::vector<bool>(static_cast<size_t>(targetLength), false);
    if (targetLength == 1)
      return {points.front()};
    if (points.size() == 1)
      return std::vector<bool>(static_cast<size_t>(targetLength), points.front());

    const float tMax = static_cast<float>(points.size() - 1);
    std::vector<bool> out(static_cast<size_t>(targetLength), false);
    for (int i = 0; i < targetLength; ++i) {
      const float t = tMax * static_cast<float>(i) /
                      static_cast<float>(targetLength - 1);
      const int idx =
          std::clamp(static_cast<int>(std::floor(t)), 0,
                     static_cast<int>(points.size() - 1));
      out[static_cast<size_t>(i)] = points[static_cast<size_t>(idx)];
    }
    return out;
  }

  std::vector<std::vector<float>> resampleLinear2D(
      const std::vector<std::vector<float>>& points,
      int targetLength) {
    if (targetLength <= 0)
      return {};
    if (points.empty()) {
      return std::vector<std::vector<float>>(static_cast<size_t>(targetLength));
    }
    const size_t numChannels = points.front().size();
    if (points.size() == 1) {
      return std::vector<std::vector<float>>(
          static_cast<size_t>(targetLength), points.front());
    }
    if (targetLength == 1)
      return {points.front()};

    const float tMax = static_cast<float>(points.size() - 1);
    std::vector<std::vector<float>> out(
        static_cast<size_t>(targetLength),
        std::vector<float>(numChannels, 0.0f));
    for (int i = 0; i < targetLength; ++i) {
      const float t = tMax * static_cast<float>(i) /
                      static_cast<float>(targetLength - 1);
      const int idx0 = std::clamp(static_cast<int>(std::floor(t)), 0,
                                  static_cast<int>(points.size() - 1));
      const int idx1 =
          std::min(idx0 + 1, static_cast<int>(points.size() - 1));
      const float frac = t - static_cast<float>(idx0);
      const auto &p0 = points[static_cast<size_t>(idx0)];
      const auto &p1 = points[static_cast<size_t>(idx1)];
      for (size_t ch = 0; ch < numChannels; ++ch) {
        const float v0 = ch < p0.size() ? p0[ch] : 0.0f;
        const float v1 = ch < p1.size() ? p1[ch] : 0.0f;
        out[static_cast<size_t>(i)][ch] = v0 + (v1 - v0) * frac;
      }
    }
    return out;
  }

  std::vector<std::vector<float>> resampleNearest2D(
      const std::vector<std::vector<float>>& points,
      int targetLength) {
    if (targetLength <= 0)
      return {};
    if (points.empty()) {
      return std::vector<std::vector<float>>(static_cast<size_t>(targetLength));
    }
    const size_t numChannels = points.front().size();
    if (points.size() == 1) {
      return std::vector<std::vector<float>>(
          static_cast<size_t>(targetLength), points.front());
    }
    if (targetLength == 1)
      return {points.front()};

    const float tMax = static_cast<float>(points.size() - 1);
    std::vector<std::vector<float>> out(
        static_cast<size_t>(targetLength),
        std::vector<float>(numChannels, 0.0f));
    for (int i = 0; i < targetLength; ++i) {
      const float t = tMax * static_cast<float>(i) /
                      static_cast<float>(targetLength - 1);
      const int idx =
          std::clamp(static_cast<int>(std::round(t)), 0,
                     static_cast<int>(points.size() - 1));
      const auto &src = points[static_cast<size_t>(idx)];
      for (size_t ch = 0; ch < numChannels; ++ch)
        out[static_cast<size_t>(i)][ch] =
            ch < src.size() ? src[ch] : 0.0f;
    }
    return out;
  }
} // namespace CurveResampler
