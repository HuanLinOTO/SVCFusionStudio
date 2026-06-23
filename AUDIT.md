# SVCFusion Studio 全面质量审计报告

> **审计日期**：2026-06-23  
> **审计范围**：`D:\Projects\SVCFusionStudio\SVCFusionStudio`（C++ 子项目）  
> **源码规模**：155 文件 / ~39,615 行 / 85 头文件  
> **技术栈**：JUCE 8 + ONNX Runtime 1.17.3 + C++17 + CMake  
> **方法论**：38 个独立 subagent 进行初轮审计 + 38 个对抗性 subagent 进行批判性复核

---

## 目录

- [验证结果总览](#验证结果总览)
- [一、致命问题（必须立即修复）](#一致命问题必须立即修复)
- [二、高优先级问题](#二高优先级问题)
- [三、中等问题](#三中等问题)
- [四、被推翻的结论（REFUTED）](#四被推翻的结论refuted)
- [五、严重级别被高估的结论（OVERSTATED）](#五严重级别被高估的结论overstated)
- [六、通过项（表现优秀）](#六通过项表现优秀)
- [七、修复优先级建议](#七修复优先级建议)

---

## 验证结果总览

经 38 个对抗性验证 subagent 独立复核后，原审计结论的裁定分布如下：

| 裁定 | 数量 | 含义 |
|---|---|---|
| **CONFIRMED** | 14 | 结论准确，无需修正 |
| **OVERSTATED** | 15 | 事实正确但严重级别被高估 |
| **REFUTED** | 4 | 结论错误，需推翻 |
| **混合** | 5 | 部分确认、部分推翻 |

---

## 一、致命问题（必须立即修复）

以下结论经过严格对抗性验证，**确认无误**，是项目中最高优先级的修复项。

### 1. 非 Windows 平台 ORT InitApi 缺失 → 崩溃 [#4 #26]

**严重级别**：致命（CRITICAL）

**验证状态**：CONFIRMED — 经多角度反驳尝试均失败

**问题**：
- `ORT_API_MANUAL_INIT=1` 在 `CMakeLists.txt:328` 全局定义（PUBLIC，未用 `WIN32` 限制），所有平台生效
- `OnnxRuntimeLoader.cpp:53-108` 中 `ensureLoaded()` 的非 Windows 分支（`:105-107`）直接 `return true`，**从不调用 `Ort::InitApi()`**
- ONNX Runtime 头文件 `onnxruntime_cxx_api.h:89-121` 确认：`ORT_API_MANUAL_INIT=1` 下 `Global<T>::api_` 仅 null 初始化，必须手动调 `InitApi()`
- `Ort::Env` 构造函数（`onnxruntime_cxx_inline.h:470-471`）调用 `GetApi().CreateEnv(...)`，`GetApi()` 返回 `*Global<void>::api_` — **解引用空指针**
- 代码库中有 13+ 处 `Ort::Env` 构造（由 `#ifdef HAVE_ONNXRUNTIME` 守护，全平台生效）

**影响**：macOS/Linux 上首次使用任何 ONNX 功能（音高检测/人声分离/SVC 推理）即触发空指针解引用崩溃。

**未被 CI 发现的原因**：macOS/Linux CI 只 build+package，从不运行应用。

**修复方案**：在 `OnnxRuntimeLoader.cpp` 的 `#else` 分支补 `Ort::InitApi();`。

**证据**：
- `CMakeLists.txt:328` — `ORT_API_MANUAL_INIT=1` PUBLIC 定义
- `OnnxRuntimeLoader.cpp:105-107` — 非 Windows `return true`
- `onnxruntime_cxx_api.h:89-121` — `ORT_API_MANUAL_INIT` 语义
- `onnxruntime_cxx_inline.h:470-471` — `Env` 构造调 `GetApi()`

---

### 2. IncrementalSynthesizer detached 线程 Use-After-Free [#16 #18]

**严重级别**：致命（CRITICAL）

**验证状态**：CONFIRMED — 调用链完整验证

**问题**：
- `IncrementalSynthesizer.cpp:631` 创建临时 `std::thread` 并立即 `.detach()`（`:788`）
- 头文件 `IncrementalSynthesizer.h:79` 声明了 `std::thread applyThread` 成员，但**从未赋值**（全仓 grep 确认 0 处引用）
- 析构函数 `~IncrementalSynthesizer()`（`:14`）仅调用 `cancel()`（设置 cancelFlag），**不 join**
- `~EditorController`（`EditorController.cpp:104-127`）join 的是 `incrementalSynthThread`（启动器线程，已退出），**不是 detached 的工作线程**
- detached 线程捕获 `this` 并访问 `this->jobId`（`:637`）、`this->isBusy`（`:640,655,668,784`）、`capturedProject->getAudioData()`（`:647`）

**触发条件**：用户在增量合成进行中关闭应用 / 切换 project / 触发新的 `resynthesizeIncrementalAsync`。

**cancelFlag 无法阻止 UAF**：线程仅在开始处检查一次（`:639`），检查后的工作循环（`:647-787`）不再检查。即使 cancel 路径中 `:640` 的 `isBusy = false` 本身也是 UAF。

**修复方案**：将 `.detach()` 改为赋值给 `applyThread` 成员，析构中 `cancel() + join()`。

**证据**：
- `IncrementalSynthesizer.cpp:631,788` — `.detach()`
- `IncrementalSynthesizer.h:79` — `applyThread` 声明但从未使用
- `IncrementalSynthesizer.cpp:14` — 析构仅 `cancel()`

---

### 3. SVCInferenceEngine load/unloadContentVec 不加锁 → Session UAF [#18]

**严重级别**：致命（CRITICAL）

**验证状态**：CONFIRMED — 具体触发路径已验证

**问题**：
- `SVCInferenceEngine::loadContentVec`（`:46-135`）和 `unloadContentVec`（`:142-149`）**不获取 `inferenceMutex`**
- `infer()`（`:786`）、`inferRVC()`（`:1051`）、`inferSoVITS()`（`:1289`）持 `inferenceMutex` 使用 `contentVecSession`
- `extractContentVec`（`:182`）在持锁的 `infer*` 内调用 `contentVecSession->Run()`

**具体触发路径**（`EditorController.cpp`）：
1. `runFullSVCConversionAsync`（`:348-352`）：`cancelSVCConversion()` + 将旧线程移入 `svcOldThreads`（**不 join**）+ 启动新线程
2. 旧线程 A 在 `infer()` 返回后 `:771` post `callAsync`
3. 消息线程执行 lambda，检查 `svcGeneration.load() != myGen`（`:783`）→ 为真
4. `:787` 调用 `svcEngine->unloadContentVec()` — 此时线程 B 极有可能正在 `infer()` 内持 `inferenceMutex` 使用 `contentVecSession`
5. `unloadContentVec` 不加锁地 `contentVecSession.reset()` → 销毁正在被使用的 session → **UAF**

**修复方案**：在 `loadContentVec` 和 `unloadContentVec` 内添加 `std::lock_guard<std::mutex> lock(inferenceMutex);`。

**证据**：
- `SVCInferenceEngine.cpp:142-149` — `unloadContentVec` 无锁
- `SVCInferenceEngine.cpp:46-135` — `loadContentVec` 无锁
- `SVCInferenceEngine.cpp:786` — `infer()` 持锁
- `EditorController.cpp:348-352,787` — 重触发 + 过期回调

---

### 4. SVCModelSession 完全无同步 [#18]

**严重级别**：高（HIGH）→ 经验证确认为致命

**验证状态**：CONFIRMED

**问题**：
- `SVCModelSession.h:101-111` — 无任何 mutex/atomic 成员
- `unload()`（`SVCModelSession.cpp:16-31`）直接 `encoderSession.reset()` 等，瞬间释放 `Ort::Session` 内存
- `getXxxSession()` 在 `SVCInferenceEngine.cpp` 的持锁推理路径中被调用，返回裸 `Ort::Session*`
- 但 `inferenceMutex` 在 `SVCInferenceEngine` 侧，`SVCModelSession::unload()` 对其一无所知
- `unload()` 在多个线程被调用：`loaderThread`（`:2070`）、`hnsepPrewarmThread`（`:1190`）、`svcConversionThread`（`:746`）、消息线程（`:239`）

**可达的 UAF 场景**：`incrementalSynthThread` 在 `infer(*svcModel,...)` 内持 `inferenceMutex`，手握 `getEncoderSession()` 返回的裸指针，正跑 `Session::Run()`。此时另一线程执行 `svcModel->unload()` → `encoderSession.reset()` 释放正在被用的 session → UAF。

**修复方案**：`SVCModelSession` 应内嵌 mutex，保护 `unload()`/`load*()`/`getXxxSession()`。

**证据**：
- `SVCModelSession.h:101-111` — 无同步成员
- `SVCModelSession.cpp:16-31` — `unload()` 无锁
- `EditorController.cpp:1072-1073,239,2070,1190` — 多线程调用 unload

---

### 5. mainComponent 裸指针跨线程竞态 [#18 #30]

**严重级别**：致命（CRITICAL）

**验证状态**：CONFIRMED

**问题**：
- `PluginProcessor.h:137` — `IMainView *mainComponent = nullptr;` 裸指针，无 atomic、无锁
- **音频线程**（`processNonARAMode`，由 `processBlock` 调用）：
  - `:148` — `mainComponent && mainComponent->hasAnalyzedProject()` 每 block 调用，`hasAnalyzedProject()` 内部读取 `project->getAudioData().waveform` 等消息线程数据结构
  - `:199` — `mainComponent->getComponent()` 解引用
- **消息线程**：`setMainComponent`（`:282`）由 `PluginEditor.cpp:24`（构造）和 `:43`（析构 `setMainComponent(nullptr)`）调用
- SafePointer 仅在 lambda 内部验证 `juce::Component*` 存活，**不保护** `:148` 的 `hasAnalyzedProject()` 和 `:199` 的 `getComponent()` 解引用（这些在 SafePointer 创建之前）

**触发场景**：host 关闭窗口/插件移除时，editor 析构线程将 `mainComponent` 置 `nullptr` 并销毁 `MainComponent`，而音频线程正执行 `processNonARAMode` 读 `mainComponent` → 竞态 UB/崩溃。

**修复方案**：audio thread 永远不解引用 `mainComponent`，改为把所有需通知 UI 的状态写入无锁结构，由 message-thread Timer 轮询刷新 UI。

**证据**：
- `PluginProcessor.h:137` — 裸指针声明
- `PluginProcessor.cpp:148,199` — 音频线程读
- `PluginProcessor.cpp:282` — 消息线程写
- `PluginEditor.cpp:24,43` — 构造/析构触发

---

### 6. RealtimePitchProcessor 音频线程阻塞锁 [#22 #23]

**严重级别**：致命（CRITICAL）— 实时安全违规

**验证状态**：CONFIRMED

**问题**：
- `RealtimePitchProcessor.h:54` — `juce::CriticalSection bufferLock;`
- `RealtimePitchProcessor.cpp:60` — `const juce::ScopedLock sl(bufferLock);`（阻塞，非 tryLock）
- JUCE 源码（`juce_CriticalSection.h:78`）确认：`enter()` 在 Windows 上等价于 Win32 `EnterCriticalSection()`，争用时进入内核等待，**无 tryLock 回退机制**

**竞争方**（持同一锁且持锁时间长）：
- `invalidate()`（`:102-109`）：持锁 `waveformSnapshot.makeCopyOf(audioData.waveform)` — 整段波形内存拷贝（可达数十 MB）
- `computeInBackground()`（`:212-223`）：持锁 mel 频谱图拷贝
- `computeInBackground()`（`:288`）：持锁 `processedBuffer = std::move(output)`

**调用链确认**：
- 非 ARA 路径：`PluginProcessor.cpp:114 processBlock` → `:135 processNonARAMode` → `:225 realtimeProcessor.processBlock` → `:60 ScopedLock`
- ARA 路径：`ARADocumentController.cpp:108 processBlock` → `:193 realtimeProcessor->processBlock` → `:60 ScopedLock`

**对比**：`AudioEngine.cpp:59` 正确使用 `ScopedTryLockType`，失败时输出静音 — 这是正确的音频线程模式。

**修复方案**：改为双缓冲 + 原子指针交换，或最小修复改用 `ScopedTryLock`（获取失败时 passthrough）。

**证据**：
- `RealtimePitchProcessor.h:54` — `CriticalSection bufferLock`
- `RealtimePitchProcessor.cpp:60` — `ScopedLock`（阻塞）
- `juce_CriticalSection.h:78` — `enter()` 阻塞语义确认

---

## 二、高优先级问题

### 7. ARA noexcept 回调含可抛异常代码 [#17]

**严重级别**：中高（触发概率低但机制确实存在）

**验证状态**：CONFIRMED（严重级别从"致命"降为"中高"）

**问题**：
- `ARADocumentController.h:29` — `processBlock(...) noexcept override`
- `ARADocumentController.cpp:155` — noexcept 函数内构造 `juce::AudioBuffer<float>` 
- JUCE 源码链：`AudioBuffer` 构造 → `allocateData()` → `HeapBlock<char,true>::malloc` → `ThrowOnFail<true>::checkPointer` → `throw std::bad_alloc`（当 malloc 返回 nullptr 时）
- 异常逃逸 noexcept → `std::terminate()` → 宿主 DAW 崩溃

**严重级别修正**：`std::bad_alloc` 在小块分配（2-8KB）时极少发生。但这是 ARA `noexcept` 契约的违反 + 实时音频线程分配反模式，应修复。

**其他抛出点**：
- `:193` — 调用非 noexcept `RealtimePitchProcessor::processBlock`
- `:310` — `new SVCFusionStudioPlaybackRenderer`
- `:322-323` — `data.setSize()` 大块分配

**修复方案**：在 noexcept 函数内加 try/catch，失败时 `buffer.clear(); return true;`。

---

### 8. CI 完全未执行测试 [#9 #14 #15]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- 6 个 CI job 全部只 build+package，无一执行测试
- `CMakeLists.txt:251` — `EXCLUDE_FROM_ALL` 排除默认构建
- 项目 CMakeLists.txt 无 `enable_testing()`/`add_test()`
- 测试/源码比 0.91%（360/39615）
- `ProjectSerializer`（289 行，数据安全关键）零测试
- 仅 3 个测试函数（2 单测 + 1 env-gated 集成）
- 自实现测试框架：首次失败即中止全部后续（`main` 单一 try/catch）

**修复建议**：
1. 在 Linux CPU job 添加 `cmake --build build --target SVCFusionStudioLogicTests` + 执行
2. CMakeLists.txt 添加 `enable_testing()` + `add_test()`
3. 优先补测 `ProjectSerializer`（round-trip 测试）、`BasePitchCurve`/`F0Smoother`

---

### 9. CI 缺少静态分析 / 格式化 / sanitizer [#10]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- CI 无 clang-format/clang-tidy/cppcheck/ASan/UBSan 步骤
- 项目根目录无 `.clang-format`/`.clang-tidy`/`.cppcheck`
- `svcfusionstudio_core`/`svcfusionstudio_ui` 未链接 `juce_recommended_warning_flags`
- 无 sanitizer 构建选项

**修复建议**：引入 CI lint job + sanitizer job（聚焦 `SVCFusionStudioLogicTests`）。

---

### 10. PCH 未启用 [#20]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- CMakeLists.txt 无 `target_precompile_headers`
- JUCE `JUCEUtils.cmake` 确认不自动启用 PCH
- 70/85 头文件 include `JuceHeader.h`（11 个 JUCE 模块，~50,000 行）
- ~70 个 TU 重复解析 JUCE 头文件

**修复方案**：添加 `target_precompile_headers(... "Source/JuceHeader.h")`，预期编译时间减少 40-70%。

---

### 11. CI 模型脚本重复 280 行 + HNSep 缺失 [#7]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- 模型准备脚本在 6 个 job 中重复 280 行（168 行 pwsh + 112 行 bash）
- macOS/Linux 3 个 job **缺少 HNSep 模型复制**
- HNSep 在所有平台编译并使用（CoreML/CUDA/CPU），是实际功能缺失
- Windows 主包包含 HNSep 模型，macOS/Linux 主包不包含

---

### 12. Lang 文件 Windows 分支缺失 VST3 [#3]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- `CMakeLists.txt:715-719`（Lang Windows else 分支）仅复制到 `SVCFusionStudio`
- 对比 Fonts Windows 分支（`:738-743`）同时复制到 `SVCFusionStudio` 和 `SVCFusionStudioPlugin_VST3`
- 对比 Lang macOS 分支（`:709-711`）复制到 VST3
- Windows VST3 插件只有嵌入英语（BinaryData），zh/zh-TW/ja 不可用

**注**：原审计中 "AAX 资源缺失" 被 OVERSTATED — AAX 是条件构建（需 `AAX_SDK_PATH`），默认不生成。

---

### 13. 状态持久化无版本头 + ARA 指针悬空 [#29]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- `getStateInformation`/`setStateInformation`（`PluginProcessor.cpp:300-322`）直接存取裸 UTF-8 JSON，无版本号/魔数/长度前缀
- `ARADocumentController.h:97` — `currentAudioSource` 裸指针，在 `didAddAudioSourceToDocument` 赋值，**无 `willRemoveAudioSourceFromDocument` 清理**
- `reanalyze()`（`:303-306`）直接解引用 `currentAudioSource`，若宿主已移除 audio source → UAF

**注**：原审计中 "无 APVTS 严重" 被 OVERSTATED — Melodyne 式 ARA 音高编辑器不需要宿主自动化参数，逐音符编辑应存入 ARA 文档。

---

### 14. i18n 硬编码 + onLanguageChanged 未赋值 [#33]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- ~17-19 处 AlertWindow/StyledMessageBox 硬编码英文（`EditorController.cpp`、`MainComponent.cpp`）
- `onLanguageChanged` 回调**从未被赋值**（全仓 3 处匹配均为声明/调用）
- 运行时切换语言后，仅 `TR()` 在 paint 中调用的字符串刷新，构造函数中 `setText(TR(...))` 的标签停留在旧语言
- 4 语言 184 key 完全一致
- `toolbar.stretch` 在所有语言中均为英文 "Stretch"（确认是遗漏）

---

### 15. 采样率变化未失效 processedBuffer [#25]

**严重级别**：高（HIGH）

**验证状态**：CONFIRMED

**事实**：
- `RealtimePitchProcessor::prepareToPlay`（`:30-33`）仅设 `sampleRate` 和 `position`，不重置 `processedBuffer`/`ready`
- `processBlock` 用新 SR 计算偏移（`pos * sampleRate`），但 `processedBuffer` 按旧 SR 生成
- SR 44100→48000 后：读索引 `t*48000` 对应音频 `t*48000/44100 ≈ t*1.088` 秒 → 速度 ×1.088，音高上移约 1.46 半音
- 无其他机制在 SR 变化后触发 `invalidate()`

---

### 16. ONNX Runtime EP 回退后 sessionOptions 未重置 [#27]

**严重级别**：中高（HIGH）

**验证状态**：CONFIRMED（部分 OVERSTATED）

**事实**：
- `HNSepModel.cpp:235-239` — `DisableMemPattern()` + `SetExecutionMode(ORT_SEQUENTIAL)` 在 catch 前已执行
- catch 块（`:242-246`）仅改 `effectiveProvider` 变量，不重置 `sessionOptions`
- 后续用被污染的 sessionOptions（GPU 风格）创建 CPU session
- `GAMEDetector.cpp:154-167` — 三处空 catch 块，无日志、不回退 provider

**严重级别修正**：`SetExecutionMode(ORT_SEQUENTIAL)` 是默认值无影响；真正有影响的是 `IntraOpNumThreads(1)`（应为 `hardware_concurrency/2`），可造成 2-4× 退化。

---

## 三、中等问题

| # | 问题 | 验证状态 | 关键修正 |
|---|---|---|---|
| #2 | GLOB_RECURSE 无 CONFIGURE_DEPENDS；APPEND 重复 | OVERSTATED | APPEND 重复被 Ninja 生成器去重，实际构建无影响 |
| #6 | .gitignore 缺失 out/ 和 .playwright-mcp/ | CONFIRMED(字面) | `out/` 内容被 `build*/` 间接覆盖；`.playwright-mcp/` 为空目录 |
| #11 | 模型缓存 key 不自动失效 | CONFIRMED | ccache 用 `github.sha` 是社区做法非官方"最佳实践" |
| #13 | macOS CoreML 缺失 | OVERSTATED | CoreML 是 runtime EP，标准 ORT 包已内置，8+ 源文件实现运行时注册 |
| #19 | 常量重复定义；FMIN/FMAX 遮蔽 | CONFIRMED(设计异味) | "44100 硬编码 15+ 处"大多正当（模型原生率/默认值） |
| #21 | 代码风格不一致 | OVERSTATED(低) | 应为"低"而非"高"；5+ 文件有差异，非仅 Project.h |
| #24 | svc-audio-gap-bug | CONFIRMED(已修复) | 三级根因均已修复，方案 C 缺失由方案 A 合理覆盖 |
| #28 | runReflowSampling 每步拷贝 cond | OVERSTATED | 实际 0.3-6MB/次（非 ~100MB），总 6-600MB（非 2-10GB） |
| #30 | 非 ARA 采集时序问题 | 混合 | mainComponent 竞态 CONFIRMED；hostIsPlaying "nullopt 卡死" OVERSTATED |
| #32 | WaveformComponent 无缓存 | 混合 | 代码层面 CONFIRMED，但组件从未被实例化（死代码），运行时影响为零 |
| #34 | 可访问性 | OVERSTATED | 1062 个 JUCE 内置控件自动提供 a11y；真正缺口是自绘组件（PianoRoll 等）。评分应 5-6/10 |
| #35 | 模型文件无 SHA256 校验 | OVERSTATED | 本地桌面应用威胁模型不同；HF LFS 已用内容寻址 |
| #36 | HNSep config.json 无效 + no_stft 未声明 | OVERSTATED | config.json 是惰性文件（代码不读取）；no_stft 缺失时回退是优雅降级设计 |
| #37 | wchar_t+ORT 模式重复 15+ 处 | CONFIRMED | 重复属实；但"ORT API 变更风险"被高估，真正风险是一致性维护 |
| #38 | 顶层工作区清理 | OVERSTATED | `out/` 被间接覆盖；`.playwright-mcp/` 为空 |

---

## 四、被推翻的结论（REFUTED）

以下 4 项原审计结论经对抗性验证后被**推翻**：

| # | 原结论 | 推翻理由 |
|---|---|---|
| **#1** | 3 个 target 未设 cxx_std_17 有风险 | JUCE 模块通过 INTERFACE 传递 `cxx_std_17`（`JUCEModuleSupport.cmake:599-605`），三个 target 通过 PUBLIC link 继承。`DOWNLOAD_EXTRACT_TIMESTAMP TRUE` 在 CMake 4.x 下锁定旧行为，更安全。**不存在"以低于 C++17 编译"的风险** |
| **#5** | build.ps1 有多个 bug | PowerShell 中换行是合法的数组元素分隔符（`about_Arrays` 文档），第 149 行缺逗号不是 bug。`[bool] $Launch` 功能正确，`-Launch:$true` 可正常调用。`exit 0` 在构建成功后是合理的。**4 项指控中 2 项完全错误，2 项是可辩护的设计选择** |
| **#16** | 内存管理"优秀" | 审计员遗漏了两个真实 UAF：(a) `IncrementalSynthesizer.cpp:631` detached 线程；(b) `EditorController.cpp` 5 处 `callAsync([this])`。代码库其他部分已正确使用 WeakReference（`MainComponent.cpp`），说明这是遗漏而非设计选择。**"RAII 优秀"结论被推翻** |
| **#26** | 8 个独立 Env 实例违反最佳实践 | `CreateEnv` 是进程内单例（`onnxruntime_c_api.h:750-753` 明确记录"return the same instance"）。8 个 C++ wrapper 指向同一 `OrtEnv`。Env 不拥有线程池（线程池是 per-session 的）。**"浪费内存并加剧线程过订阅"不成立** |

---

## 五、严重级别被高估的结论（OVERSTATED）

| # | 原级别 | 修正级别 | 高估原因 |
|---|---|---|---|
| #8 | 高 | 信息 | 守卫 `if (Test-Path $required) { exit 0 }` 在 CI 中总是命中（submodule 含 `extras/Build/`），下载分支为死代码 |
| #15 | 5 项高 | 1 项高 + 4 项低 | 3 个测试不需要 filter/setup-teardown；GoogleTest 建议不当（项目已链接 JUCE，自带 UnitTest） |
| #22 | 致命(必定xrun) | 中(应修复但非必定) | 2-8KB malloc 在现代分配器上通常亚微秒级，"必定造成 xrun/爆音"被夸大 |
| #28 | Critical(~100MB/次) | Medium(0.3-6MB/次) | 审计员自述"256×数百"计算出来仅 0.3-5MB，与声称的 100MB 相差 20-300 倍 |
| #31 | 严重 | 中(设计折中) | Melodyne 式 ARA 编辑器不需要宿主自动化参数；`SmoothedValue` 缺失合理（有专用 F0Smoother） |
| #34 | 3/10 | 5-6/10 | 1062 个 JUCE 内置控件自动提供 a11y；WCAG 对桌面音频插件无强制法规要求 |
| #35 | 高 | 低/信息 | HF LFS 已用 SHA256 内容寻址；能改模型的攻击者也能改二进制和哈希 |
| #36 | 高 | 信息 | config.json 是惰性文件；no_stft 缺失时回退是优雅降级设计 |
| #38 | 中 | 低 | `out/` 被间接覆盖；`.playwright-mcp/` 为空 |

---

## 六、通过项（表现优秀）

| 领域 | 评价 |
|---|---|
| 内存管理（同步路径） | 零裸 `delete`/`malloc`/`free`，零 `ScopedPointer`，RAII 使用优秀（注：异步路径有 UAF） |
| 锁获取顺序 | 无嵌套锁，无死锁风险 |
| svc-audio-gap-bug | 三级根因均已修复 ✅ |
| i18n key 一致性 | en/zh/zh-TW/ja 四语言 184 key 完全一致 |
| PianoRollComponent 优化 | 静态层缓存+桶化滚动+LOD+视口剔除+profiling，工程成熟度高（注：垂直轴无桶化） |
| JUCE ARA Factory | 骨架符合规范 |
| 并发原子模式 | `HostSyncService`/`PluginTransportController` 的 release/acquire + exchange 去重模式正确 |
| AudioEngine tryLock | `AudioEngine.cpp:59` 正确使用 `ScopedTryLockType`，是音频线程锁的正确模式 |
| CMake 选项互斥 | CUDA 与 DirectML 互斥检查（`CMakeLists.txt:22-26`）正确 |

---

## 七、修复优先级建议

| 优先级 | 检查点 | 行动 | 工作量 |
|---|---|---|---|
| **P0** | #4 | 非 Windows 补 `Ort::InitApi()` | 0.5h |
| **P0** | #18 (IncrementalSynth) | detached 改为 joinable 成员 | 2h |
| **P0** | #18b (SVCInferenceEngine) | `load/unloadContentVec` 加 `inferenceMutex` | 1h |
| **P0** | #18c (SVCModelSession) | 内嵌 mutex 保护 session 指针 | 4h |
| **P0** | #18d (mainComponent) | audio thread 改为无锁通道 | 8h |
| **P0** | #23 (RealtimePitchProcessor) | 改用 tryLock 或双缓冲 | 8h |
| **P1** | #17 (ARA noexcept) | noexcept 回调加 try/catch | 2h |
| **P1** | #9 #15 | CI 添加测试执行 + CTest 集成 | 4h |
| **P1** | #20 | 启用 PCH | 1h |
| **P1** | #10 | 引入 clang-format + clang-tidy + ASan | 1d |
| **P1** | #3 | Lang Windows 分支补 VST3 | 0.5h |
| **P1** | #29 | 状态持久化加版本头 + ARA willRemove 回调 | 1d |
| **P2** | #7 | CI 模型脚本抽成 composite action | 1d |
| **P2** | #33 | 补全硬编码字符串 i18n + 接通 onLanguageChanged | 1d |
| **P2** | #25 | prepareToPlay 中 invalidate processedBuffer | 1h |
| **P2** | #27 | EP 回退后重置 sessionOptions | 2h |
| **P2** | #14 | 优先补 ProjectSerializer round-trip 测试 | 1d |
| **P3** | #32 | PianoRollComponent 垂直滚动不重建静态层 | 4h |
| **P3** | #37 | 抽取 OrtSessionLoader 统一 wchar_t 模式 | 4h |
| **P3** | #34 | 核心自绘组件实现 AccessibilityHandler | 2d |

---

> **报告说明**：本报告由 38 个独立审计 subagent + 38 个对抗性验证 subagent 生成。每项结论均附有 `file:line` 证据。被标注为 CONFIRMED 的结论经过反驳尝试仍成立；被标注为 OVERSTATED 的结论事实正确但严重级别下调；被标注为 REFUTED 的结论被推翻。
