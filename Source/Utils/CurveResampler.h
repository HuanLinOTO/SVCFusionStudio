#pragma once

#include <vector>

namespace CurveResampler {
  // Frame-based resampling (hop-size aligned), modeled after DiffSinger's
  // resample_align_curve utility.
  // Resample a dense curve to a target length using linear interpolation.
  std::vector<float> resampleLinear(const std::vector<float>& points,
                                    int targetLength);

  // Resample a boolean mask to a target length using nearest-neighbor mapping.
  std::vector<bool> resampleNearest(const std::vector<bool>& points,
                                    int targetLength);

  // Resample a 2D curve [T, C] to a target length using linear interpolation.
  std::vector<std::vector<float>> resampleLinear2D(
      const std::vector<std::vector<float>>& points,
      int targetLength);

  // Resample a 2D curve [T, C] to a target length using nearest-neighbor.
  std::vector<std::vector<float>> resampleNearest2D(
      const std::vector<std::vector<float>>& points,
      int targetLength);
} // namespace CurveResampler
