English: [README_EN.md](README_EN.md)

<div align="center">
  <h1>SVCFusion Studio</h1>
  <img src="efc4695dc0bf6bdf922dd174b6acf3d1_720.png" alt="Logo" width="300" />
  <p>基于 JUCE 构建的轻量级音高编辑器，具备神经音高检测和声码器重合成，支持实时预览。</p>
</div>

> **AI/代理注意事项**：代理相关的构建说明维护在 `AGENTS.md` 中。有关本地构建默认值、CI 不变量和纯 CPU 构建覆盖，请参阅该文件。


## 声明
本项目基于 [HachiTune](https://github.com/KCKT0112/HachiTune)，在其基础上加入了 SVC 功能

感谢原始作者的贡献

## 概述

SVCFusion Studio 是一款独立应用及音频插件，用于在钢琴卷帘中编辑人声音高。它通过神经音高检测器分析音频、分割音符，并使用神经声码器重合成编辑结果。插件同时支持 ARA 宿主和非 ARA 采集工作流程。
可显著改善调音后的失真问题。

## 特性

- 钢琴卷帘编辑：音高、时值和音符参数
- 神经音高检测（默认 RMVPE，可选 FCPE）
- 基于 SOME 的音符分割，附 F0 回退
- 神经声码器重合成实时预览
- 导出音频和 MIDI
- 独立应用 + VST3/AU 插件（可选 AAX）
- GPU 加速：Windows 上使用 CUDA 或 DirectML；macOS 上使用 CoreML
- 多语言界面

## 系统要求

- CMake 3.22+
- C++17 编译器
- Git（用于子模块）

### 平台说明

| 平台    | 要求                                         |
| ------- | -------------------------------------------- |
| Windows | Visual Studio 2019+（含 C++ 工作负载）、Windows SDK |
| macOS   | Xcode + 命令行工具                           |
| Linux   | GCC/Clang、ALSA 开发库                       |

## 模型与资源

构建时需要以下模型文件位于 `Resources/models` 中，缺失将导致构建失败。CI 从 Hugging Face（`SVCFusion/things`）自动下载模型；本地构建需手动放置。

### 核心模型

- `pc_nsf_hifigan.onnx`（声码器）
- `contentvec768l12.onnx`（内容编码器）
- `rmvpe.onnx`
- `fcpe.onnx`
- `some.onnx`
- `cent_table.bin`
- `mel_filterbank.bin`

### HNSep（谐波/打击乐分离）

- `hnsep/config.json`
- `hnsep/hnsep_VR.onnx`
- `hnsep/hnsep_VR_convstft.onnx`
- `hnsep/split/hnsep_pre.onnx`
- `hnsep/split/hnsep_core.onnx`
- `hnsep/split/hnsep_post.onnx`

### GAME（可选，用于基于 GAME 的分析）

- `GAME/bd2dur.onnx`
- `GAME/config.json`
- `GAME/dur2bd.onnx`
- `GAME/encoder.onnx`
- `GAME/estimator.onnx`
- `GAME/segmenter.onnx`

CI 会自动解析模型变体（优先顺序 `_fp32_slim.onnx` → `_fp32.onnx` → 基础名称）。

其他运行时资源位于 `Resources/lang` 和 `Resources/fonts` 中。

## 构建

### 构建辅助脚本（Windows）

在 Windows 上，最简单的方式是使用附带的 `build.ps1` 脚本，默认使用 DirectML 构建：

```powershell
.\build.ps1                              # 配置 + 编译（DirectML, Release）
.\build.ps1 -Configuration Debug         # Debug 构建
.\build.ps1 -Reconfigure                 # 强制重新运行 CMake 配置
.\build.ps1 -CMakeArgs @('-DUSE_DIRECTML=OFF')  # 纯 CPU 构建
```

详见 `.\build.ps1 -h` 或阅读脚本内容。

### 手动构建

#### 子模块

```bash
git submodule update --init --recursive
```

#### 配置

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

#### 编译

```bash
cmake --build build --config Release
```

### 可选标志

- `-DUSE_CUDA=ON` 启用 CUDA 执行提供程序（Windows）
- `-DUSE_DIRECTML=ON` 启用 DirectML 执行提供程序（Windows）
- `-DUSE_ASIO=ON` 启用 ASIO 支持（Windows）
- `-DUSE_BUNDLED_CUDA_RUNTIME=ON` 打包 CUDA 运行时（Windows）
- `-DUSE_BUNDLED_DIRECTML_RUNTIME=ON` 打包 DirectML 运行时（Windows）
- `-DONNXRUNTIME_VERSION=1.17.3` 覆盖 ONNX Runtime 版本

注意：

- CUDA 和 DirectML 不能同时启用。
- ONNX Runtime 会在 CMake 配置期间自动下载。

## 插件格式

- 默认构建 VST3 和 AU。
- 仅当设置了有效的 `AAX_SDK_PATH` 时才构建 AAX。
- 当 `third_party/ARA_SDK` 子模块存在时启用 ARA。

## 使用方法

1. 打开音频文件（WAV/MP3/FLAC/OGG）。
2. 分析音频以提取音高和音符。
3. 在钢琴卷帘中编辑音符和音高曲线。
4. 实时预览更改效果。
5. 导出音频或 MIDI。

### 插件模式

- **ARA 模式**：在支持的宿主中直接访问音频（如 Studio One、Cubase、Logic）。
- **非 ARA 模式**：在没有 ARA 的宿主中自动采集和处理。

## 项目结构

```
SVCFusion Studio/
  Source/
    Audio/        # 音频引擎、音高检测、声码器
    Models/       # 项目和音符数据
    UI/           # 界面组件
    Utils/        # 工具、本地化、撤销
    Plugin/       # VST3/AU/AAX/ARA 集成
  Resources/
    models/       # 必需的 ONNX 和数据文件
    lang/         # 本地化 JSON
    fonts/        # 界面字体
  third_party/
    JUCE/         # JUCE 子模块
    ARA_SDK/      # ARA SDK 子模块（可选）
  CMakeLists.txt
```

## 许可协议

参见 `LICENSE` 文件。
