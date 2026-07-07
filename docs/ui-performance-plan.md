# 页面全局卡顿修复 Spec

目标：降低 JUCE message thread 压力，让滚动、面板调整、设置动画、播放、调参恢复可交互流畅。

## 问题定义

用户观察：

- 滚动卡。
- 调整面板卡。
- 打开设置动画卡。
- 播放卡。
- 调参卡。

诊断判断：

- 不是单点 bug，而是 UI/message thread 系统性超预算。
- 多个 30/60Hz timer、全量 repaint、每帧 layout、paint 内重计算、调参后的合成/混音重任务叠加。
- `TrackList` 是播放卡顿的强热点，但不足以解释所有交互都卡。
- 必须分层处理：先止血，再缓存重绘热点，再迁出 UI 线程重任务。

## 核心根因

### 1. 过多 60Hz UI 动画

涉及：

- `Source/UI/SettingsComponent.cpp`
- `Source/UI/Workspace/WorkspaceComponent.cpp`
- `Source/UI/Workspace/PanelContainer.cpp`
- `Source/UI/VoicebankPanel.cpp`

表现：

- 每帧 `setAlpha()` / `setTransform()` / `repaint()`。
- 有些路径每帧 `resized()` / `updateLayout()`。
- Settings overlay 每帧全屏 dim + drop shadow。

### 2. paint 内做大量实时计算

涉及：

- `Source/UI/TrackListComponent.cpp`
- `Source/UI/PianoRoll/OverviewPanel.cpp`
- `Source/UI/HNSepLaneComponent.cpp`
- `Source/UI/PianoRollComponent.cpp`

表现：

- TrackList 局部 repaint 仍全宽扫描 waveform。
- Overview 每次 repaint 扫 waveform/f0/notes。
- HNSep lane 滚动/缩放时重算曲线/能量。
- PianoRoll static layer miss/direct render 时成本高。

### 3. UI 线程执行音频/模型/IO 重任务

涉及：

- `Source/UI/MainComponent.cpp`
- `Source/Audio/EditorController.cpp`
- `Source/Audio/AudioEngine.cpp`
- `Source/Utils/SHA256Utils.cpp`
- `Source/Models/ProjectSerializer.cpp`

表现：

- 调参结束后增量合成完成会 `refreshAudioEngine(true)`。
- `AudioEngine::rebuildMixedWaveform()` 整段重混音。
- 打开工程完成阶段在 message thread 同步 load SVC/vocoder。
- 保存时同步 SHA256 和 JSON 写盘。
- 多处 `thread.join()` 可能阻塞 UI。

## 实施原则

- 先做最小可验证改动。
- 每一阶段都能独立回滚。
- 优先修共享 UI 线程压力，而不是只优化某一个组件。
- 先降频/裁剪/缓存，再考虑大规模架构改造。
- 不改变音频结果、不改变项目数据格式、不改 CI/build provider 默认。

## 阶段 0：诊断开关

目的：能快速证明卡顿来源并量化改善。

新增运行时开关：

- `SVCFS_DISABLE_UI_ANIMATIONS=1`
- `SVCFS_LIGHT_TRACKLIST=1`
- `SVCFS_UI_PROFILING=1`

行为：

- `SVCFS_DISABLE_UI_ANIMATIONS=1` 禁用或降级 SettingsOverlay、Settings tab、Workspace panel、PanelContainer、VoicebankPanel 的 60Hz 动画。
- `SVCFS_LIGHT_TRACKLIST=1` 临时跳过 TrackList waveform 绘制，仅保留背景、播放头、进度 overlay。
- `SVCFS_UI_PROFILING=1` 输出关键 timer/paint/resized 耗时和频率日志。

验收：

- 开关默认关闭，不影响用户默认行为。
- 开启 `SVCFS_DISABLE_UI_ANIMATIONS=1` 后设置打开、面板切换明显更稳。
- 开启 `SVCFS_LIGHT_TRACKLIST=1` 后播放时卡顿明显下降则确认 TrackList 热点。

## 阶段 1：TrackList 立即止血

文件：

- `Source/UI/TrackListComponent.cpp`
- `Source/UI/TrackListComponent.h`

改动：

- 在 `TrackItem::paint()` 中读取 `g.getClipBounds()`。
- waveform 循环只遍历 clip 覆盖的 x 范围。
- 修正播放头首次 `lastPlayheadX == -1` 时 dirty rect 过大的情况。
- 保持视觉输出一致。

当前热点：

```cpp
for (int x = 0; x < wfWidth; ++x)
```

目标逻辑：

```cpp
auto clip = g.getClipBounds();
int startX = std::max(0, clip.getX() - wfLeft);
int endX = std::min(wfWidth, clip.getRight() - wfLeft);
for (int x = startX; x < endX; ++x)
```

验收：

- 播放时 TrackList 播放头 repaint 不再触发全宽 waveform 计算。
- 多轨、长音频、最大化窗口下播放明显更顺。
- waveform 视觉无回归。

风险：

- clip 与 header 区域坐标需谨慎处理。
- 进度 overlay、muted overlay、playhead 不能被错误裁掉。

## 阶段 2：UI 动画降级

文件：

- `Source/UI/SettingsComponent.cpp`
- `Source/UI/Workspace/WorkspaceComponent.cpp`
- `Source/UI/Workspace/PanelContainer.cpp`
- `Source/UI/VoicebankPanel.cpp`

改动：

- 增加统一 helper：判断是否禁用 UI 动画。
- SettingsOverlay 禁用动画时直接 `animationProgress = target`。
- SettingsOverlay 减少每帧全屏 repaint。
- SettingsComponent tab 禁用动画时直接切换 tab，不启动 60Hz timer。
- SettingsComponent tab 默认也可将 60Hz 降到 30Hz。
- WorkspaceComponent 动画期间避免每帧 `resized()`。
- WorkspaceComponent 改为 panel transform-only reveal，结束时再 layout。
- PanelContainer 避免每帧对所有 panel `setBounds()`。
- PanelContainer panel switch 动画只影响 active panel transform/alpha。
- VoicebankPanel 避免每帧 `listBox.repaint()`。
- VoicebankPanel 仅 repaint info 区域。

验收：

- 设置打开/关闭不卡顿。
- 面板展开/收起不卡顿。
- Voicebank 切换不卡顿。
- 禁用动画开关打开时所有动画立即完成。
- 默认动画仍存在但负载明显下降。

风险：

- transform-only 动画可能与布局 reserved width 有视觉差异。
- Workspace 主内容宽度是否跟随动画需产品选择。

建议取舍：

- 优先性能：动画期间 main content 不动态缩放，只 panel 滑入覆盖或最终一次布局。
- 优先视觉：保留 main content 缩放，但必须接受较高 UI 成本。

推荐选择：优先性能。

## 阶段 3：Overview 和 HNSep 缓存

文件：

- `Source/UI/PianoRoll/OverviewPanel.cpp`
- `Source/UI/PianoRoll/OverviewPanel.h`
- `Source/UI/HNSepLaneComponent.cpp`
- `Source/UI/HNSepLaneComponent.h`

OverviewPanel 改动：

- 增加 waveform overview peak cache。
- 增加 f0 overview cache。
- cache key 包含 waveform sample count、sample rate、component width/height、f0 size、relevant display flags。
- repaint 时直接画 cache，不再每次扫描整段音频。
- 滚动只更新 viewport overlay，不重算 waveform/f0。

HNSepLane 改动：

- 能量 overlay 增加 per-frame/per-pixel cache。
- `drawCurve()` 避免每个 point 调 `Project::getNoteAtFrame()` 线性扫描。
- 可预构造 audible frame ranges，二分查找。
- 只在 curve/audio/notes/zoom 改变时重建。

验收：

- 水平滚动和缩放时 Overview 不再成为热点。
- HNSep 打开后滚动/缩放不卡。
- 长音频、多 note 情况下稳定。

风险：

- cache invalidation 容易漏。
- HNSep 的 SHFC/voicing/breath/tension 不同曲线需分别处理。

## 阶段 4：调参路径防抖与局部 repaint

文件：

- `Source/UI/MainComponent.cpp`
- `Source/UI/ParameterPanel.cpp`
- `Source/UI/PianoRollComponent.cpp`
- `Source/UI/PianoRoll/PitchEditor.cpp`

改动：

- `MainComponent::onPitchEdited()` 不再直接 `pianoRoll.repaint()` 全量。
- 使用 selected note / dirty range 计算 repaint rect。
- ParameterPanel 拖动时只更新当前 note 数据和局部视觉。
- ParameterPanel 拖动时不触发合成。
- slider drag ended 后 debounce 触发一次 `resynthesizeIncremental()`。
- 如果已有合成在跑，只保留最后一次 pending。
- 全局 pitch 拖动时只更新显示，松手后触发重合成/转换。
- `parameterPanel.updateFromNote()` 避免拖动中反复 setValue 造成控件额外刷新。

验收：

- 拖动 pitch/volume/global pitch slider 时不卡。
- 松手后可以后台合成。
- UI 不因合成完成瞬间卡死。
- Undo/redo 行为不回归。

风险：

- dirty rect 计算需覆盖 pitch curve 上下边界。
- 全局 pitch 影响全局 F0，局部 repaint 不一定适用；可先全量 repaint 但合成必须防抖。

## 阶段 5：重混音异步化

文件：

- `Source/Audio/AudioEngine.cpp`
- `Source/Audio/AudioEngine.h`
- `Source/Audio/EditorController.cpp`
- `Source/UI/MainComponent.cpp`

当前问题：

- `AudioEngine::rebuildMixedWaveform()` 在调用线程整段分配、clear、遍历 tracks、`addWithMultiply()`。
- 多处从 UI 线程触发：mute/solo、volume drag end、调参合成完成、SVC 完成、打开工程完成、导入音频完成。

改动方案：

- 新增 `requestRebuildMixedWaveformAsync(preservePosition)`。
- 后台构建新的 mix buffer。
- 完成后 message thread 或 audio-safe swap 指针。
- 合并重复请求，只保留最后一次。
- UI 只显示轻量状态，不等待 rebuild 完成。

验收：

- mute/solo/volume release 不再卡。
- 调参合成完成不再卡一下。
- 多轨长音频 rebuild 不阻塞 UI。
- 播放中不出现长时间静音。

风险：

- audio thread 数据交换需线程安全。
- 当前 `waveformLock` 是 SpinLock，不适合长时间持有。
- 需要谨慎处理播放位置 preserve。

## 阶段 6：设置/IO 重任务迁移

文件：

- `Source/UI/MainComponent.cpp`
- `Source/UI/SettingsComponent.cpp`
- `Source/UI/Main/SettingsManager.cpp`
- `Source/Utils/SHA256Utils.cpp`
- `Source/Models/ProjectSerializer.cpp`
- `Source/Audio/EditorController.cpp`

改动：

- settings `saveConfig()` 合并/延迟写。
- toggle/combobox 变化不立即同步写盘，延迟 300-500ms flush。
- 打开工程 final lambda 里 SVC/vocoder 模型恢复迁出 UI 线程。
- `onPitchDetectorChanged` 避免 `reloadInferenceModels(false)` 同步执行。
- `saveProject()` 的 SHA256 和 JSON 写盘改为后台任务。
- UI 显示 saving/loading progress，完成后回 message thread 更新状态。

验收：

- 设置切换不再卡。
- 保存工程 UI 不冻结。
- 打开工程最后阶段不卡死。
- 模型恢复期间 UI 可交互。

风险：

- 保存期间关闭应用要 flush pending config。
- 后台保存需要防止项目数据被修改，可复制快照。

## 阶段 7：线程 join 风险治理

文件：

- `Source/Audio/EditorController.cpp`
- `Source/Audio/EditorController.h`

改动：

- UI 可达路径避免直接 `thread.join()`。
- `setActiveTrack()` 中 HNSep prewarm 不阻塞 UI。
- 模型 reload、loader、render、incremental synth 的旧线程 join 移到 joiner/background。
- 统一 cancellation + generation token。
- 旧任务完成后丢弃 stale result。

验收：

- 切轨不卡。
- 分析/模型加载期间再操作 UI 不阻塞。
- 不出现 use-after-free 或 stale result 覆盖新状态。

风险：

- 生命周期复杂，需要 SafePointer/generation 检查。
- 析构时仍需安全 join。

## 验证矩阵

基础场景：

- 空工程打开设置。
- 已加载短音频打开设置。
- 已加载 5 分钟音频打开设置。
- 1 track 播放。
- 6 tracks 播放。
- 最大化窗口播放。
- TrackList 拖高后播放。
- Overview 开启滚动/缩放。
- HNSep 显示后滚动/缩放。
- 调参 pitch slider 拖动。
- 调参 global pitch slider 拖动。
- mute/solo 切换。
- 保存工程。
- 打开包含 SVC 状态的工程。

验收目标：

- 设置打开动画不掉帧或明显卡顿。
- 播放时 UI 可拖动、可点按钮、播放头流畅。
- 滚动和缩放不出现连续冻结。
- 调参拖动响应即时。
- 保存/打开/模型恢复有进度但不冻结 UI。
- CPU 占用下降，UI thread profiler 不再长期停在 paint/rebuild/model/IO。

## 推荐优先级

第一批最小改动：

1. `SVCFS_DISABLE_UI_ANIMATIONS` 开关。
2. TrackList clip-aware paint。
3. Workspace animation 移除每帧 `resized()`。
4. SettingsOverlay 动画降级。
5. 调参 debounce。

第二批性能结构改动：

1. TrackList peak cache。
2. Overview cache。
3. HNSep cache。
4. 异步 mix rebuild。

第三批稳定性改动：

1. 异步保存/配置 flush。
2. 模型恢复迁出 UI。
3. 清理 UI 线程 join。

## 实施顺序建议

建议按以下 PR/commit 粒度推进：

1. `Add UI performance toggles and profiling logs`
2. `Make TrackList waveform painting clip-aware`
3. `Reduce UI animation layout/repaint pressure`
4. `Debounce parameter edits and limit repaint scope`
5. `Cache overview waveform and f0 rendering`
6. `Cache HNSep lane rendering`
7. `Make mixed waveform rebuild asynchronous`
8. `Move settings/project IO off the message thread`
9. `Remove UI-thread blocking joins`

## 验收标准

完成后应满足：

- 设置面板打开/关闭动画不卡。
- 面板展开/收起不卡。
- 播放不卡，尤其长音频和多轨。
- 滚动和缩放不卡。
- 调参拖动不卡。
- 打开/保存不冻结 UI。
- 无音频输出回归。
- 无项目保存/加载格式回归。
- 默认行为保留，性能开关仅用于诊断或低性能模式。

## 当前最可行落地方案

最先做这四项，收益最大、风险最低：

1. TrackList clip-aware paint。
2. 全局禁用/降级 UI 动画开关。
3. Workspace 动画去掉每帧 `resized()`。
4. 调参拖动 debounce，松手后合成。

这四项完成后，再用 profiler 判断是否继续做 Overview/HNSep cache 和 async mix rebuild。
