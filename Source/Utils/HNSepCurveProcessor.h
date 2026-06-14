#pragma once

#include "../Models/Project.h"

namespace HNSepCurveProcessor {
constexpr float kDefaultVoicing = 100.0f;
constexpr float kDefaultBreath = 100.0f;
constexpr float kDefaultTension = 0.0f;
constexpr float kDefaultShfc = 0.0f;

void initializeCurves(Project &project);
void rebuildCurvesFromNotes(Project &project);
void rebuildCurvesForRange(Project &project, int startFrame, int endFrame);
void extractNoteCurvesFromMaster(Project &project);
void extractNoteCurvesFromMasterForRange(Project &project, int startFrame,
                                         int endFrame);
bool hasActiveEdits(const Project &project, int startFrame, int endFrame);
std::vector<float> applyShfcToF0(const std::vector<float> &f0,
                                 const std::vector<float> &shfcCurve,
                                 int shfcStartFrame = 0);
} // namespace HNSepCurveProcessor
