#pragma once

#include "../Models/Project.h"

namespace HNSepCurveProcessor {
constexpr float kDefaultVoicing = 100.0f;
constexpr float kDefaultBreath = 100.0f;
constexpr float kDefaultTension = 0.0f;

void initializeCurves(Project &project);
void rebuildCurvesFromNotes(Project &project);
void rebuildCurvesForRange(Project &project, int startFrame, int endFrame);
void extractNoteCurvesFromMaster(Project &project);
bool hasActiveEdits(const Project &project, int startFrame, int endFrame);
} // namespace HNSepCurveProcessor