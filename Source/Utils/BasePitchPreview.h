#pragma once

#include "../Models/Note.h"
#include <functional>
#include <vector>

struct BasePitchPreviewRange {
  int startFrame = -1;
  int endFrame = -1; // exclusive
  std::vector<float> weights; // size = endFrame - startFrame
};

BasePitchPreviewRange computeBasePitchPreviewRange(
    const std::vector<Note> &notes, int totalFrames,
    const std::function<bool(const Note &)> &isSelected);
