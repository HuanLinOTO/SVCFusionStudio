# HachiTune Remote Commit Sync Spec

## Goal

Identify the latest HachiTune commits that have not been pulled into the local `HachiTune` checkout yet, and assess which of those commits should later be synchronized into `SVCFusionStudio`.

This document is an exploration spec only. It does not implement or modify application code.

## Repository Context

- `HachiTune`: `D:\Projects\SVCFusionStudio\HachiTune`
- `SVCFusionStudio`: `D:\Projects\SVCFusionStudio\SVCFusionStudio`
- Both repositories are currently on `master`

Important constraint:

- `HachiTune` and `SVCFusionStudio` are not being treated as a single clean shared git history for synchronization purposes.
- Practical sync work will need to be driven by commit content and file-level comparison, not by assuming `cherry-pick` can be applied mechanically across repositories.

## Current Git State

`HachiTune` local status:

- `master...origin/master [behind 13]`

Interpretation:

- There are no local-only commits in `HachiTune` waiting to be pushed.
- The unsynced candidate commits are the 13 commits present on `HachiTune` remote `origin/master` but not present in the local `HachiTune` checkout.

Common ancestor between local `HachiTune` and `origin/master`:

- `da038d37f805d0ac32c2732cd133d9299056f4d2`

## Remote-Only HachiTune Commits

The following commits exist in `HachiTune` `origin/master` and are missing from the local checkout:

1. `a780f51` Initial implementation
2. `c777649` Delay curve drawing and pass wheel scrolling through editors
3. `004ecbc` Keep the playhead visible above the parameter overlay
4. `393546d` Refactor hnsep curve synthesis and fix reset undo behavior
5. `1c16372` Optimize HNSep lane scrolling and curve rendering
6. `e447347` Normalize tension tilt in true dB with RMS matching
7. `da36c26` Add HNSep RMS overlays and localized lane controls
8. `9af5fd6` Fix piano roll zoom anchoring
9. `d119371` Add model
10. `ff3a545` Merge pull request #13 from hrukalive/HNSep3
11. `017da30` fix:修复无法导出的问题
12. `4fceb06` Add tips section about project refactoring status
13. `7b18806` Merge pull request #22 from Evi233/master

## File-Level Scope Observed In HachiTune

### HNSep feature line

The following commits touch a coherent HNSep feature stream rather than isolated one-line fixes:

- `a780f51`
- `c777649`
- `004ecbc`
- `393546d`
- `1c16372`
- `e447347`
- `da36c26`
- `d119371`

Observed affected areas:

- `Source/Audio/EditorController.*`
- `Source/Audio/HNSepModel.*`
- `Source/Audio/TensionProcessor.*`
- `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`
- `Source/Models/Note.*`
- `Source/Models/Project.*`
- `Source/Models/ProjectSerializer.cpp`
- `Source/UI/HNSepLaneComponent.*`
- `Source/UI/PianoRollComponent.*`
- `Source/UI/PianoRollWorkspaceView.*`
- `Source/UI/MainComponent.cpp`
- `Source/Utils/HNSepCurveProcessor.*`
- `Resources/models/hnsep/*`
- `Resources/lang/*.json`

### Isolated or lower-risk items

- `017da30` export fix in `Source/UI/MainComponent.cpp`
- `9af5fd6` zoom anchoring changes in piano roll scroll/zoom code
- `4fceb06` README update

### Merge commits

- `ff3a545`
- `7b18806`

These should be treated as containers for included work, not as standalone sync targets.

## Current SVCFusionStudio Coverage

Exploration shows that `SVCFusionStudio` already contains corresponding HNSep-related modules:

- `Source/Audio/EditorController.*`
- `Source/Audio/HNSepModel.*`
- `Source/UI/HNSepLaneComponent.*`
- `Source/UI/PianoRollComponent.*`
- `Source/UI/PianoRollWorkspaceView.*`
- `Source/Utils/HNSepCurveProcessor.*`
- `Resources/models/hnsep/config.json`
- `Resources/models/hnsep/hnsep_VR.onnx`

This implies:

- Most remote-only `HachiTune` commits are not simple missing features.
- They are more likely refinements, alternate implementations, or behavior deltas against logic that already exists in `SVCFusionStudio`.

## Sync Classification

### Group A: likely good first-pass sync candidates

These are the easiest items to evaluate first because they are narrower in scope or low risk.

1. `4fceb06` README refactoring-status tips
2. `017da30` export fix
3. `9af5fd6` piano roll zoom anchoring fix
4. `da36c26` HNSep localized lane labels and RMS overlays

Reasoning:

- They are easier to verify independently.
- They are less likely to require broad architectural movement.
- They can be checked directly against existing `SVCFusionStudio` behavior.

### Group B: medium-risk sync candidates

1. `d119371` HNSep model asset addition
2. `004ecbc` keep playhead above parameter overlay
3. `c777649` wheel/curve drawing interaction refinement

Reasoning:

- These are still bounded changes, but they interact with runtime resources or input/rendering behavior.
- `SVCFusionStudio` already has adjacent implementations, so content-level comparison is required.

### Group C: high-risk feature-line commits

1. `a780f51` Initial implementation
2. `393546d` HNSep curve synthesis refactor and undo fix
3. `1c16372` HNSep rendering and scrolling optimization
4. `e447347` tension normalization changes
5. `ff3a545` merge of HNSep3 line
6. `7b18806` later merge commit that may bundle additional changes

Reasoning:

- These commits appear to be part of a multi-commit feature evolution.
- `SVCFusionStudio` already contains overlapping HNSep systems.
- Direct transplantation would be error-prone without careful file-level and behavior-level reconciliation.

## Notable Findings

### Export-related change

- `017da30` only touches `Source/UI/MainComponent.cpp` in `HachiTune`.
- `SVCFusionStudio` also has extensive export logic in `Source/UI/MainComponent.cpp`.
- This is a good candidate for a focused diff because the blast radius is small, but the local file is already heavily customized.

### Zoom anchoring change

- `9af5fd6` touches `ScrollZoomController` and related piano roll files in `HachiTune`.
- `SVCFusionStudio` already contains a `Source/UI/PianoRoll/ScrollZoomController.cpp` with custom zoom and scroll logic.
- This suggests the fix may already be partially present or may need selective adaptation rather than wholesale porting.

### HNSep overlays and lane controls

- `da36c26` adds localized lane labels and RMS overlays in `HachiTune`.
- `SVCFusionStudio` already computes and draws RMS/dB-related HNSep overlay behavior in `Source/UI/HNSepLaneComponent.cpp`.
- This item likely needs comparison for parity rather than simple import.

### Model asset addition

- `d119371` adds `Resources/models/hnsep/config.json` and `Resources/models/hnsep/hnsep_VR.onnx` in `HachiTune`.
- Those files already exist in `SVCFusionStudio`.
- This commit is therefore probably already covered at the resource level, though version parity is still unknown.

## Recommended Next Exploration Steps

If sync work proceeds later, the next read-only exploration should be:

1. Diff `017da30` against `SVCFusionStudio/Source/UI/MainComponent.cpp`
2. Diff `9af5fd6` against `SVCFusionStudio/Source/UI/PianoRoll/ScrollZoomController.cpp`
3. Diff `da36c26` against `SVCFusionStudio/Source/UI/HNSepLaneComponent.cpp` and `Resources/lang/*.json`
4. Check whether `d119371` model files differ by content hash, not just by path presence
5. Expand `ff3a545` and `7b18806` into their underlying non-merge commit content before evaluating them as sync targets

## Sync Strategy Recommendation

Recommended order for future actual sync work:

1. README and documentation parity
2. Export bug fix
3. Zoom anchoring behavior
4. HNSep UI parity items
5. Resource/version parity checks for HNSep model assets
6. Only then evaluate the larger HNSep refactor chain

This order minimizes regression risk and avoids mixing large behavioral rewrites with small correctness fixes.

## Status

Exploration complete for the current scope:

- Unsynced remote commit list identified
- File-level areas of impact identified
- Initial `SVCFusionStudio` coverage mapping completed
- Sync priority groups established

No application code was modified during this exploration.
