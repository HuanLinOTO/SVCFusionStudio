# SVC 转换后音频缺段 Bug 分析与修复规格

## 现象

1. 加载音频 → 波形完整无缺段
2. 激活 SVC 模型（DDSP-SVC 6.3）→ 部分 segment 缺段：
   - 播放无声音（对应位置波形为全零）
   - 编辑器中 note 可见但下方预览条无波形
   - note 可编辑（拖拽等操作）
3. 拖拽两个 note 后该 segment 恢复播放，但**无 SVC 效果**（原始音色）

## 日志证据

文件：`%APPDATA%\SVCFusion Studio\Logs\debug_20260612_153223.log`

```
Line 229: 20609 frames, 10551296 samples  ← 音频总长 ~239s
Line 230: 17 segments                      ← 切片器切出 17 段
```

### 各 segment 处理结果

| Seg | startFrame | frames | 16kHz samples | 结果 |
|-----|-----------|--------|---------------|------|
| 1   | 840       | 327    | 60,602        | OK   |
| 2   | 1,312     | 308    | 57,084        | OK   |
| 3   | 1,750     | 323    | 59,881        | OK   |
| 4   | 2,201     | 323    | 59,944        | OK   |
| 5   | 2,709     | 975    | 180,938       | OK   |
| 6   | 3,683     | 941    | 174,728       | OK   |
| 7   | 4,630     | 1,913  | 355,293       | OK   |
| **8** | **6,547** | **3,757** | **697,752** | **✗ 失败** |
| 9   | 10,329    | 981    | 182,089       | OK   |
| 10  | 11,309    | 938    | 174,125       | OK   |
| 11  | 12,246    | 118    | 21,828        | OK   |
| 12  | 15,073    | 454    | 84,205        | OK   |
| 13  | 15,545    | 428    | 79,407        | OK   |
| 14  | 15,982    | 463    | 85,910        | OK   |
| 15  | 16,444    | 423    | 78,489        | OK   |
| 16  | 16,883    | 2,782  | 516,621       | OK   |
| 17  | 19,664    | 944    | 175,358       | OK   |

关键日志行：

```
Line 337: Segment 8/17 -- startFrame=6547 frames=3757 samples=1923178
Line 339: [Timer] Resample 16kHz: 1 ms (697752 samples)
Line 340: ContentVec inference failed: Non-zero status code returned
           while running MatMul node. Name:'/encoder/layers.0/fc1/MatMul'
Line 341: ContentVec extraction failed
Line 342: [Timer] Segment 8 total: 40 ms -> 0 samples    ← 输出为 0
Line 519: Stored sliced SVC mel [16 segments]            ← 只存了 16 段！
Line 522: Full SVC conversion complete -- 10551296 samples replaced
```

## 根因

### 一级根因：ContentVec ONNX 模型输入长度限制

`contentvec768l12.onnx` 模型内部的 MatMul 节点（`/encoder/layers.0/fc1/MatMul`）在输入序列长度超过约 **600k samples at 16kHz** 时返回 `Non-zero status code`。

阈值推断：Segment 16（516,621 samples @ 16kHz）成功 → Segment 8（697,752 samples）失败 → 阈值约在 **520k ~ 697k** 之间。

### 二级根因：失败处理不当

流程链：

1. `SVCInferenceEngine.cpp:782-787`: `extractContentVec()` 捕获 `Ort::Exception`，返回空 vector
2. `SVCInferenceEngine.cpp:783`: `hubertRaw.empty()` → `infer()` 返回空 mel
3. `EditorController.cpp:361`: `segMel.empty()` → 不调用 vocoder，不存入 `collectedSegMels`
4. `EditorController.cpp:377`: `segAudio.empty()` → `continue`，跳过重建
5. `EditorController.cpp:382-383`: `resultAudio.resize(resultAudio.size() + silentLength, 0.f)` → segment 位置被填为全零
6. `EditorController.cpp:666`: `audioData.melFromSVC = true` — **无条件设置！** 即使只有 16/17 个 segments 成功

### 三级根因：melFromSVC 无条件为 true 导致拖拽后无 SVC 效果

- 失败的 segment 8 的原始分析 mel 留在 `audioData.melSpectrogram` 中
- `melFromSVC = true` 导致拖拽后 `synthesizeRegion()` 走 `useSvcMelDirect` 快速路径
- 快速路径从 `melSpectrogram` 读取原始分析 mel → vocoder 输出原始音色

## 代码位置

| 文件 | 行号 | 问题 |
|------|------|------|
| `Source/Audio/SVCInferenceEngine.cpp` | 146-198 | `extractContentVec()` 对大输入无分块处理 |
| `Source/Audio/EditorController.cpp` | 377 | `if (segAudio.empty()) continue;` — 失败 segment 直接跳过 |
| `Source/Audio/EditorController.cpp` | 382-390 | 重建逻辑依赖 `currentLength` 跟踪，skip 后不同步 |
| `Source/Audio/EditorController.cpp` | 657-666 | `melFromSVC = true` 无条件设置 |
| `Source/Audio/EditorController.cpp` | 520-525 | blend 时失败 segment 位置 `finalAudio[i] == 0` → `blendedAudio[i] == 0` |

## 修复方案

### 方案 A：ContentVec 分块推理（根本修复）

**文件：** `Source/Audio/SVCInferenceEngine.cpp`

**目标：** 使 ContentVec 能处理任意长度的输入。

**实现思路：**
- 在 `extractContentVec()` 中，当输入 `numSamples16k` 超过阈值（建议 **500,000 samples**，留安全余量）时
- 将 16kHz 音频切分为重叠块（overlap = inference hop 的整数倍，如 320 samples）
- 每块独立运行 ContentVec ONNX 推理
- 在重叠区域取平均或丢弃尾部
- 拼接所有块的特征为连续输出

**阈值选择依据：** Segment 16（516,621 samples）刚好成功。使用 500,000 作为安全上限，留约 3% 余量。

### 方案 B：失败 segment 的容错处理（必须修复）

**文件：** `Source/Audio/EditorController.cpp`

**目标：** 当 segment 推理失败时，不产生静音，且正确影响后续流程。

**子项 B1：失败时回退到原始音频（替代 `continue`）**

位置：`EditorController.cpp:377`

当前代码：
```cpp
if (segAudio.empty()) continue;
```

改为：
```cpp
if (segAudio.empty()) {
    // 回退到原始音频
    segAudio.assign(origPtr + seg.startSample,
                    origPtr + seg.startSample + seg.numSamples);
    // 不存入 collectedSegMels → SVC mel 保持原始分析 mel
}
```

**子项 B2：追踪失败 segment**

位置：`EditorController.cpp:319-391`

添加 `bool anySegmentFailed = false;` 在循环前。当 `segAudio` 为空且使用原始音频回退时，设置 `anySegmentFailed = true`。

**子项 B3：条件设置 melFromSVC**

位置：`EditorController.cpp:666`

当前代码：
```cpp
audioData.melFromSVC = true;
```

改为：
```cpp
if (!anySegmentFailed)
    audioData.melFromSVC = true;
```

**子项 B4：确保 currentLength 正确跟踪**

当使用原始音频回退时，segAudio 大小是 `seg.numSamples`，但 SVC 成功时 segAudio 大小是 `segFrames * hopSize`。两者可能不同。

位置：`EditorController.cpp:390`

```cpp
currentLength = currentLength + silentLength + static_cast<int>(segAudio.size());
```
`seg.startSample`（在 silentLength 中）基于原始音频位置。当 segAudio 大小不等于 `seg.numSamples` 时（SVC 成功路径），`currentLength` 会产生漂移。

解决方案：在 B1 的原始音频回退中，确保 `segAudio` 取 `seg.numSamples` 长度（而非 `segFrames * hopSize`），以匹配 `silentLength` 使用的 `seg.startSample` 参考系。

### 方案 C：AudioSlicer 最大 segment 限制（预防性）

**文件：** `Source/Utils/AudioSlicer.h/.cpp`

**目标：** 从源头防止过大的 segment。

在 `AudioSlicer::slice()` 的参数中添加 `maxSegmentSamples`（默认 600,000 samples = ~13.6s @ 44100Hz = ~520k @ 16kHz）。当 segment 超过此大小，在内部进一步切割。

但此方案不能覆盖所有场景（非切片路径、未来模型变更），建议作为辅助措施。

## 优先实施顺序

1. **方案 B（容错处理）** — 最高优先级，阻止静音产生和 melFromSVC 误设置
2. **方案 A（ContentVec 分块）** — 中等优先级，使大 segment 也能正常进行 SVC 转换
3. **方案 C（slicer 限制）** — 低优先级，额外预防措施

## 验证方法

1. 加载 >30 秒音频（确保触发切片路径）
2. 激活 DDSP-SVC 6.3 模型
3. 确认日志不出现 "ContentVec extraction failed" 或 "0 samples"
4. 确认波形预览条无空白区域
5. 拖拽 note 后确认 SVC 效果仍保留
6. 确认日志显示 "Stored sliced SVC mel [17 segments]"（非 16）

## 参考

- ContentVec 模型：contentvec768l12.onnx（~180MB）
- 模型输入：`source[1, audio_length]` at 16kHz
- 模型输出：`features[1, T, 768]` where T ≈ audio_length / 320
- ONNX Runtime 版本：1.17.3
- 执行设备：DirectML (device 1)
- SVC 模型：DDSP-SVC 6.3 (model_type_index=4)
