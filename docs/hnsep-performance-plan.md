# HNSep 推理性能诊断与优化计划

状态：诊断完成，未动手修改。本文档给出根因、证据与分阶段修复计划。

## 结论（TLDR）

"hnsep 推理极慢"不是单一 bug，而是三层问题叠加。当前开发机上（RTX 4060 Ti，DML device 1，split pipeline + external STFT）**稳态单 chunk 推理并不慢**：最新日志（debug_20260705_222309.log）显示每 10s chunk 约 pre=110ms / stft=25ms / core=62ms / post=70ms，4:14 音频约 11s 分离完成。慢的来源按严重度排序：

1. **生命周期策略（主因，所有用户）**：模型每次用完即卸载，下次冷加载，且同一操作流程内全波形分离被重复执行多次。
2. **发布版缺关键模型文件（主因，CI 打包用户）**：`hnsep_pre_no_stft.onnx` 不在 CI 模型下载清单里，发布版 pre 阶段退回 CPU 跑内部 ConvSTFT，每 chunk 2.0–2.6s（占 85% 耗时），比 external-STFT 路径慢 8.2x。
3. **静默降级路径（灾难性，条件触发）**：DML 初始化失败后，被 GPU 配置污染的 session options（单线程、无 arena）直接拿去创建 CPU session，跑的还是最重的 conv-STFT 模型，可慢 10–50x，且上层完全无感知。

## 根因详解与证据

### A. 生命周期：unload-after-use + 重复全曲分离（dominant）

1. **每次用完即卸载**：`SVCFUSION_HNSEP_UNLOAD_AFTER_USE` 默认 `"1"`（`Source/Audio/EditorController.cpp:33-41`），`maybeUnloadHNSepAfterUse` 在所有完成路径调用（973、1640、1663、2771、2846）。`HNSepModel::unload()` 销毁全部 3 个 Ort::Session 连同 onnxEnv（`Source/Audio/HNSepModel.cpp:521-538`），下次使用重付完整 session 创建 + DML JIT。实测：首 chunk 预热 ~3.2s（pre=2539ms + core=640ms），约占单次分离总时长 30%；最新一个会话里发生了 **5 次** load/unload 循环。该行为来自 commit `ef1fb6d`（为省 VRAM：462MB vs 2690MB），是显存换时间的刻意取舍，但没有任何缓存/超时机制。
2. **analyze + SVC 转换 = 2 次全曲分离**：分析路径在 `EditorController.cpp:2520` 加载（load #1）、2752-2764 全曲分离、2769-2771 卸载；SVC 转换完成后在 961-973 **再次冷加载并对整条混合后波形重新全曲分离**（load #2）——即使 SVC 只替换了有声区间。一次"分析 3 分钟音轨 + SVC 转换"= 2 次冷加载 + 约 40 次 padded 10s chunk 推理 + 2 次卸载。
3. **曲线编辑的最坏交互路径**：`ensureHNSepBases`（`EditorController.cpp:1614-1667`）没有 dirty-range 概念，永远全曲分离；而 `IncrementalSynthesizer.cpp:558-571` 在任何增量 SVC 推理后设置 `waveformFromSVC=true` 使 bases 缓存失效。用户交替进行音高编辑和 HNSep 曲线编辑时，**每次曲线编辑 = 一次冷加载 + 全曲分离 + 卸载**。
4. **模型互逐（thrash）**：`ensureHNSepModelLoadedForAnalysis`（1569-1579）每次都驱逐 SVC 模型和 ContentVec，SVC 流程随后（977-978）再重建——一次转换可致 SVC session 重建最多 2 次、ContentVec 2 次。
5. **GPU 下分析全串行**：`allowConcurrentModelInference = (provider == CPU)`（2501），DirectML 下 hnsep 在 mel/F0 之前就加载并占住 VRAM，分离也不与其他阶段并行。
6. **切轨触发 prewarm**：`setActiveTrack`/`setProject` 都会触发 `prewarmHNSepBasesAsync`（153-188 → 1669-1705），条件满足时同样是完整的 load+全曲分离+unload，且经 `hnsepBasesMutex` 与 SVC 转换互相阻塞。

### B. 发布版缺 `hnsep_pre_no_stft.onnx`（dominant，仅影响打包产物）

- 模型架构：hnsep 是 vocal-remover CascadedNet；因 DML 不支持 ONNX STFT 算子，`scripts/convert_hnsep_stft_to_conv.py` 把 STFT 重写为 2050×1×2048 stride-512 的 DFT Conv，`scripts/split_hnsep_pipeline.py` 再拆为 pre（ConvSTFT + stage-1 band UNets + 4 个 LSTM，约 60% 参数量）/ core（mask head）/ post（复数掩码乘 + 两个 kernel-2048 ConvTranspose 逆 STFT，恒在 CPU）。
- **pre 想上 GPU 的唯一条件是 `hnsep_pre_no_stft.onnx` 存在**（`HNSepModel.cpp:332-338, 416-425`）；缺失时静默回退 `hnsep_pre.onnx` 全 CPU。
- **CI 下载清单没有它**：`.github/workflows/build.yml:66-71` 只下载 `hnsep_pre.onnx/hnsep_core.onnx/hnsep_post.onnx`，三处必需文件校验（245、479、703）同样不含 `pre_no_stft`。仓库 `Resources/models/hnsep/split/` 里有该文件（36MB，commit `ef3827d` 提交），但 CI 打包从模型缓存取，不走 Resources。
- 实测差距（日志 + commit `ef3827d` 基准）：pre 2314ms → 283ms（8.2x），单 30s chunk 总耗时 2772ms → 821ms（3.4x）。日志"三个时代"清晰可见：6 月 10–21 日的版本（split pipeline 但无 external STFT）每 chunk pre 就要 2.0–2.6s——发布版用户现在体验的就是这个。

### C. DML 失败后的 session options 污染（灾难性，条件触发；AUDIT.md #16 已记录）

- `loadSingleSessionModel` 在 append DML **之前**按 GPU profile 配置 options：`DisableCpuMemArena` + `SetIntraOpNumThreads(1)` + `SetInterOpNumThreads(1)`（`HNSepModel.cpp:196-200`）+ `DisableMemPattern`（235）。
- DML append 失败的 catch（242-246）只把 `effectiveProvider` 改回 CPU，**不重建 options**，然后用污染的 options 创建 CPU session 并返回 true（308-311）。
- 后果：单线程 CPU 跑 `hnsep_VR_convstft.onnx`（其 conv 化 STFT 约为 FFT 版 40x FLOPs），估算比预期路径慢 10–50x；且因 `loadModel` 返回 true，`EditorController.cpp:1604-1608` 的 CPU 模型（`hnsep_VR.onnx`）回退**永远不会触发**。这是最符合"极慢"描述的静默故障模式。
- 相关风险：本机 DML 枚举出 5 个同名 "RTX 4060 Ti" 适配器（远程桌面虚拟显示驱动），代码固定选 device 1；日志证实当前 device 1 是真 GPU，但适配器顺序变化或在别的远程环境下选中虚拟适配器会触发代码中已有的 "performance may be severely degraded" 警告路径（`HNSepModel.cpp:45-49, 219-221`）。

### D. 稳态开销（minor，各 10–25%）

- **无流水线**：stft→pre→core→post 单线程严格串行，chunk 间也串行（`HNSepModel.cpp:577-716, 782-829`）；CPU 阶段时 GPU 空转，反之亦然。CPU 段约占 30–50%，流水线化理论收益 1.4–2x。
- **10s padded chunk + 1s overlap**：180s 文件 = 20 个 chunk，每个都 pad 满 10s，冗余计算 +11–16%（CPU 路径 30s chunk 仅 7 块 +3.3%）。固定 shape 是为避免 DML per-shape 重编译与 VRAM 爆炸（`d9b12d7`：8.5GB→2.7GB），方向正确，但尾块全额 pad 与偏小的 chunk 尺寸仍是成本。
- **CPU 线程数减半**：`hardware_concurrency()/2`（192-194、360-361），SVCModelSession 用满核（`SVCModelSession.cpp:499-506`），无 SMT 机器上 CPU 路径最多损失 ~2x。
- **进度反馈粒度**：每 chunk 才回调一次且映射进总进度条 5% 区间（`EditorController.cpp:2760-2761`），造成"卡住了"的观感。
- 已排除的非问题：外部 STFT 帧数向上取整到 32 的浪费仅 ~0.23%；无 IoBinding 的 CPU↔GPU 拷贝对 180s 文件仅 ~40–80ms。

## 修复计划

### 阶段 0：可复现度量（改动前先固定基线）

- 用现成 CLI：`SVCFusionStudio --benchmark-hnsep`（`Source/Main.cpp:197`）与 `--test-inference <wav>`，分别记录冷加载耗时、首 chunk、稳态每 chunk 分段耗时（stft/pre/core/post）。
- 对照两个场景：split 目录含/不含 `hnsep_pre_no_stft.onnx`（模拟发布版）。
- 验收：有一份基线数字表，后续每阶段用同一命令验证。

### 阶段 1：快赢（预计一天内，收益最大）

1. **把 `hnsep_pre_no_stft.onnx` 加入 CI**：`.github/workflows/build.yml` 下载清单（66-71）+ 三处校验循环（245、479、703）+ README 模型清单。同时确认模型下载源（`MODEL_BASE_URL`）已上传该文件。
   - 收益：发布版 pre 8.2x 提速、单 chunk 3.4x。风险：几乎为零。
2. **修复 DML 失败回退的 options 污染**：catch 中要么用 CPU profile 重建 `sessionOptions` 再创建 session，要么直接让 `loadSingleSessionModel` 返回 false，把回退决策交还 `EditorController.cpp:1604-1608`（它会换用更适合 CPU 的 `hnsep_VR.onnx` + 正确线程数）。`loadSplitPipelineModels` 的 cpuOptions/gpuOptions 分离已经是正确写法，可参照。
   - 收益：消除 10–50x 静默降级。验收：人为让 DML append 抛异常（如伪造 device id），确认走 CPU 模型且线程数正确、日志明示。
3. **生命周期从"用完即卸"改为"空闲超时卸载"**：保留 VRAM 收益但不重复付 JIT。方案：卸载改为延迟任务（如 60–120s 无 hnsep 使用才卸载；期间再次使用则取消卸载）。已知连续流程（analyzeAudio → SVC convert 的 961 行重分离）内强制保持常驻。保留 `SVCFUSION_HNSEP_UNLOAD_AFTER_USE=1` 作为立即卸载的兼容开关。
   - 收益：每次操作省 ~3.2s 预热 + session 创建；曲线编辑交互延迟大幅下降。风险：VRAM 常驻 ~0.5–2.7GB 时长增加，需在低显存机器验证（shader cache 使 10s chunk 的重 JIT 仅 ~2.4s，超时卸载兜底）。

### 阶段 2：消除重复计算（结构性收益）

4. **SVC 转换后的重分离限定 dirty range**：`EditorController.cpp:961-973` 只对 SVC 实际替换的样本区间（±overlap 余量）重跑分离，与已有 harmonic/noise bases 拼接，而不是整条重来。
5. **`ensureHNSepBases` 支持区间分离**：给 `HNSepModel::separate` 增加区间入口（内部本来就是 chunk 化的，天然支持）；`IncrementalSynthesizer.cpp:558-571` 失效标记从"全部作废"改为记录受影响帧范围。
   - 收益：曲线编辑从"全曲分离"降为"编辑区间分离"，3 分钟音轨的单次编辑从 ~10s+ 降到 ~1s 级。风险：chunk 边界混叠需用与现有 overlap-blend 相同的策略，需 A/B 验证输出一致性。
6. **减少模型互逐**：hnsep 与 SVC/ContentVec 的驱逐改为基于实际 VRAM 压力（或至少在同一转换流程内不反复逐出/重建）。

### 阶段 3：稳态吞吐（可选，前两阶段完成后按 profiler 决定）

7. **三级流水线**：chunk t 的 core（GPU）与 chunk t+1 的 stft+pre（CPU）、chunk t-1 的 post（CPU）并行。DML 用独立线程喂即可，无需 IoBinding。预期 1.4–2x。
8. **重评 DML chunk 尺寸**：在 VRAM 允许时试 15–20s chunk（d9b12d7 数据外推 ~4GB@15s），chunk 数减半、overlap 冗余减半；不可行则维持 10s。
9. **小项**：外部 STFT 多线程化（目前单线程 ~25ms/chunk，流水线化后可能无所谓）；CPU 路径线程数改用物理核数而非 `hardware_concurrency()/2`。

### 阶段 4：观测与守护（防回归）

10. **启动/加载期自检日志与 UI 提示**：加载 hnsep 后打印一行结论性日志（provider、adapter 名、是否 external STFT、pre 落在 CPU 还是 GPU）；当 pre 意外落在 CPU 或发生 DML→CPU 回退时，在分析进度 UI 上显示"性能降级"警告而非静默。
11. **进度粒度**：分离阶段进度按 chunk 内分段（stft/pre/core/post）上报，并给它超过 5% 的进度条区间，消除"卡死"观感。
12. （可选）CI 冒烟：对 1 个短 wav 跑 `--benchmark-hnsep`，断言 external STFT 路径被启用（防止清单文件再丢）。

## 验收矩阵

| 场景 | 现状（估） | 目标 |
|---|---|---|
| 发布版单 30s chunk（DML） | ~2.8–3.1s（CPU pre） | ~0.8s（阶段 1.1） |
| 分析 4 分钟音轨（本机 DML） | ~11s + 3.2s 预热 | ~8–11s，无重复预热（1.3） |
| 分析 + SVC 转换全流程 hnsep 部分 | 2×(冷加载+全曲) | 1×冷加载 + 1 次全曲 + 1 次区间（1.3+2.4） |
| 单次 HNSep 曲线编辑（已有 SVC） | 冷加载+全曲分离 | 区间分离，秒级（2.5） |
| DML 初始化失败时 | 单线程 CPU conv-STFT（10–50x 慢，静默） | 多线程 CPU + FFT 模型 + UI 警告（1.2+4.10） |

## 涉及文件索引

- `Source/Audio/HNSepModel.cpp/.h` — session options 污染、chunking、流水线、STFT
- `Source/Audio/EditorController.cpp` — unload-after-use（33-41）、分析路径（2489-2846）、SVC 重分离（961-973）、`ensureHNSepBases`（1614-1667）、模型互逐（1569-1579）、prewarm（1669-1705）
- `Source/Audio/Synthesis/IncrementalSynthesizer.cpp:558-571` — bases 失效逻辑
- `.github/workflows/build.yml:66-71, 245, 479, 703` — 模型清单
- `scripts/split_hnsep_pipeline.py`、`scripts/convert_hnsep_stft_to_conv.py`、`tools/make_hnsep_pre_no_stft.py` — 模型生成链
- `Source/Main.cpp:197, 577-682` — `--benchmark-hnsep` / `--test-inference`
- `AUDIT.md` issue #16 — options 污染问题的既有记录
