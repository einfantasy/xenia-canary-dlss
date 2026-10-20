# Milestone 0 — Pre-integration Audit（NVIDIA DLSS 集成前置审计基线）

> 本文档是 Xenia Canary 集成 **DLSS SR / Optical Flow (NVOF) / 帧生成 / 实验性神经渲染** 的**只读前置审计基线**，记录审计时的仓库状态、源码事实与 SDK 契约，作为后续开发的可追溯证据。
>
> **本阶段性质**：静态审计。未修改任何 C++ 源码、头文件、测试或构建脚本；未执行构建；未安装依赖；未加载任何 NVIDIA DLL；未执行真实 GPU / NVOF 渲染；未做画质评估。
>
> 本文档只记录**审计时已核实**的状态，不将 SDK 文件存在等同于运行时可用，不将静态审计等同于编译通过或运行时验证。后续任何状态升级都必须以新证据为前提。

| 项目 | 值 |
| --- | --- |
| 总体状态 | `PASS WITH FINDINGS` |
| 里程碑 | `Milestone 0 — Pre-integration Audit` |
| 审计日期 | 2026-10-20 |
| 审计方式 | 只读（源码、头文件、SDK 文档、构建配置、运行时文件） |
| 仓库分支 | `dev/dlss5-integration` |
| HEAD 提交 | `c28af1125d7a50c1cdfc4ba0c7e6c1df5b00f295` |
| 工作区状态 | 干净（0 已修改、0 未跟踪） |
| 目标文件 | `docs/dlss-integration/milestone-0-audit.md` |

> **后续更新（Milestone 0.1 / 0.2）**：Milestone 0.1 已完成开发基线修正与关键结论复核（含 `effect_count = 0` 语义修正、`PerfQuality` 无 `Off` 值修正、D3D12/D3D11 头文件引用修正），见 [`milestone-0.1-baseline-review.md`](milestone-0.1-baseline-review.md)；Milestone 0.2 许可证与输入格式可行性审查见 [`milestone-0.2-feasibility-review.md`](milestone-0.2-feasibility-review.md)。

---

## 1. 审计摘要与总体状态

**总体状态：`PASS WITH FINDINGS`**

审计确认了集成所需的技术基线：插入点、回退机制、SDK 契约、输入数据边界和许可证约束均可从源码与文档直接核实。同时存在若干必须处理的发现（Findings），主要集中在输入数据缺口与许可证：

1. **插入点已确认**：`GuestOutputPaintEffect` / `GetGuestOutputPaintFlow` 链路是 DLSS SR 的自然插入位置，且已有 `effect_count = 0` 的原始路径回退模式可复用。
2. **运动矢量完全缺失**：`src/xenia/gpu` 零命中。这是高质量 SR 的硬性阻塞项，也是帧生成的前置条件（见第 6 节的细化说明，不作"所有 SR 模式绝对不可用"的断言）。
3. **深度缓冲存在但无公开访问器**：需上游访问器及资源状态/分辨率验证。
4. **`kOriginalPath` / `kUseBackend` 语义尚不存在**：全源码零命中，属拟议语义，需在后续阶段引入，不得当作现有代码。
5. **NVOF 所需 `nvofapi64.dll` 缺失**：由 NVIDIA 驱动提供，当前环境不可加载。
6. **AGENTS.md 目录映射过时**：其指向的 `\\EIN-9900k\DLSS5_Research\...` 不可解析，实际布局在 `\\ein-9900k\Code\DLSS5_Research\`。
7. **许可证需复核**：将 SDK 目标代码嵌入开源 Xenia 并重分发触发 `LICENSE REVIEW REQUIRED`。

---

## 2. 审计环境、仓库分支和提交基线

**仓库位置（已确认）**
- 主工作仓库：`\\ein-9900k\Code\DLSS5_Research\xenia-canary`
- 参考资料根目录：`\\ein-9900k\Code\DLSS5_Research\`，含 `xenia-canary`、`nvidia-dlss-reference`、`runtime`、`Veyra-NRVideo`、`AGENTS.md`。

**Git 基线（已确认）**
- 分支：`dev/dlss5-integration`
- HEAD：`c28af1125d7a50c1cdfc4ba0c7e6c1df5b00f295`
- `git status --short --branch` 仅输出 `## dev/dlss5-integration`（无文件行），`git ls-files --others --exclude-standard` 计数为 `0` —— 工作区干净。
- 审计前后工作区均保持干净，本阶段**零修改、零构建产物**。

**路径差异（Findings，需更新 AGENTS.md）**
- AGENTS.md 将主仓库与参考目录映射为 `\\EIN-9900k\DLSS5_Research\...`，该 UNC 路径**不存在**。
- 实际可访问的布局为 `\\ein-9900k\Code\DLSS5_Research\...`。
- DLSS SDK 与 Optical Flow SDK 位于本机 `C:\SDKs\NVIDIA\...`，与 AGENTS.md 记载一致。
- 结论：AGENTS.md 的网络共享目录映射需更新为 `Code\DLSS5_Research` 前缀。本阶段未改动 AGENTS.md（遵循修改范围约束）。

**开发环境限制**
- 运行于 Hyper-V，无法安装 NVIDIA 驱动，无法执行真实 GPU / NVOF 渲染。
- 本地可执行：源码检查、SDK 分析、架构设计、编译期集成、输入验证、基于 Mock 的主机侧测试、静态分析。
- 本地不可执行：真实 GPU 初始化、NVOF 执行、DLSS 质量评估。所有硬件相关项在远程验证前必须标记为 `UNVERIFIED`。

---

## 3. 渲染输出调用链与候选接入点

**guest output 链路（源码级，`STATIC_VERIFIED`）**

```
D3D12CommandProcessor::IssueSwap(frontbuffer_ptr, w, h)
  src/xenia/gpu/d3d12/d3d12_command_processor.cc:2250
  └─ texture_cache_->RequestSwapTexture(...)
       src/xenia/gpu/d3d12/d3d12_command_processor.cc:2270
  └─ presenter->RefreshGuestOutput(w, h, aspect, refresher)
       src/xenia/gpu/d3d12/d3d12_command_processor.cc:2279
       └─ refresher：gamma-ramp + 可选 FXAA 计算通道，写入 guest_output_resource
           (R10G10B10A2_UNORM，UAV-capable)
Presenter::PaintAndPresent
  └─ D3D12Presenter::PaintAndPresentImpl(execute_ui_drawers)
       src/xenia/ui/d3d12/d3d12_presenter.cc:463
       └─ GetGuestOutputPaintFlow(...)   src/xenia/ui/presenter.cc:651
           构建放大 / 锐化效果链（effects[]、effect_output_sizes[]、letterbox）
       └─ 绘制到交换链后缓冲（B8G8R8A8_UNORM）→ 呈现
```

**关键类型与位置**
- `GuestOutputPaintEffect`（枚举）：`src/xenia/ui/presenter.h:389`。现有取值含 `kBilinear`、`kBilinearDither`、`kCasSharpen(+Dither)`、`kCasResample(+Dither)`、`kFsrEasu`、`kFsrRcas(+Dither)`。**DLSS SR 效果的候选插入位置（候选 B）**。
- `GuestOutputPaintConfig`：`src/xenia/ui/presenter.h:196`（effect 类型、CAS 锐度、FSR 通道/锐度、dither、overscan）。
- `GuestOutputPaintFlow`：`src/xenia/ui/presenter.h:439`，含 `effect_count`、`effects[]`、`effect_output_sizes[]`、letterbox。由 `Presenter::GetGuestOutputPaintFlow`（`presenter.cc:651`）构建。
- `kGuestOutputMailboxSize = 3`：无锁 SPSC 邮箱。
- `guest_output_resource_paint_refs`（数组）：`src/xenia/ui/d3d12/d3d12_presenter.h:267`。
- `guest_output_resources_`（邮箱本体）：`src/xenia/ui/d3d12/d3d12_presenter.h:320`。
  - 以上即 AGENTS.md 明令**禁止擅自接管**的 guest 输出资源引用，已确认为真实存在的结构，必须保留其所有权与 Fence 依赖。

**现有回退模式（可复用，`STATIC_VERIFIED`）**
- 在 `PaintAndPresentImpl` 中，若某 paint effect 的管线为 null，则将 `guest_output_flow.effect_count = 0`，guest 输出不带任何效果直接显示（`src/xenia/ui/d3d12/d3d12_presenter.cc:530`、`:535`、`:671`）。
- 这是 "kOriginalPath" 回退，也是 DLSS SR 的自然回退点。关闭功能 / 初始化失败 / 处理失败时，复用此路径即可保证原始渲染不受影响。

**结果语义（现状 vs 拟议）**
- `PaintResult`（枚举，`src/xenia/ui/presenter.h:340`）：`kPresented`、`kPresentedSuboptimal`、`kNotPresented`、`kNotPresentedConnectionOutdated`、`kGpuLostExternally`、`kGpuLostResponsible`。
- **`kOriginalPath` / `kUseBackend` 全源码零命中**：属拟议语义，非现有代码。后续阶段需引入（或复用 `PaintResult`-style 模式），不得当作既有能力。

**格式与分辨率**
- guest 输出：`R10G10B10A2_UNORM`；交换链后缓冲：`B8G8R8A8_UNORM`；guest 前缓冲为 Xenos 8888 或 2_10_10_10。
- 存在 `IsDrawResolutionScaled` / `GetScaledAspectRatio` 等分辨率/纵横比查询。

**UNKNOWN（待后续核实）**
- 插入点获得的资源在每一阶段的确切 `D3D12_RESOURCE_STATE`（需逐段核实状态迁移）。
- 深度缓冲的具体资源状态与分辨率（见第 6 节）。

---

## 4. DLSS SDK 与 Optical Flow SDK 的版本和 API 契约

### 4.1 DLSS SDK 310.9.1（`STATIC_VERIFIED`）

- 路径：`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\`，含 `doc/`、`include/`、`lib/`、`NVIDIAImageScaling/`、`utils/`、`LICENSE.txt`、`README.md`、`.gitmodules`。
- `include/` 头文件（24 个），关键者：`nvsdk_ngx_helpers_d3d.h`、`nvsdk_ngx_helpers_dlssg_d3d.h`、`nvsdk_ngx_helpers_dlssd_d3d.h`、`nvsdk_ngx_params*.h`、`nvsdk_ngx_loader.h`。
- **`lib/` 目录非空（修正先前误判）**：`lib\Windows_x86_64\dev\` 与 `\rel\` 均含 `nvngx_dlss.dll`、`nvngx_dlssd.dll`、`nvngx_dlssg.dll`，并附导入库（`nvsdk_ngx_*.lib`，含 `x64/`、`vs2010/`、`vs2012/`、`vs2013/`、`khr/x64/`、`uwp/x64/` 等）。Linux `aarch64`/`x86_64` 与 Windows `aarch64`/`arm64ec` 布局齐全。
- **版本偏差**：SR 开发指导文档版本为 310.6.0，本地安装头文件为 310.9.1。**以头文件为权威**。

**Eval 参数结构（`NVSDK_NGX_D3D12_DLSS_Eval_Params`，`include\nvsdk_ngx_helpers_d3d.h:37-69`，已核实字段）**

| 行号 | 字段 | 说明 |
| --- | --- | --- |
| 37 | `pInDepth` | 深度输入 |
| 38 | `pInMotionVectors` | 运动矢量输入 |
| 39-40 | `InJitterOffsetX` / `InJitterOffsetY` | 抖动偏移（输入/渲染像素空间） |
| 43 | `InReset` | 场景完全变化时置 1 |
| 55 | `InPreExposure` | 预曝光 |
| 62 | `pInMotionVectors3D` | 3D 运动矢量 |
| 65 | `pInDepthHighRes` | 高分辨率深度 |
| 69 | `pInMotionVectorsReflections` | 反射对象运动矢量 |

**SR 输入契约**
- 必需：`pInColor`、`pInOutput`、逐帧 jitter。
- 条件/可选：深度（`pInDepth`）、运动矢量（场景运动时必需）、曝光。
- DLL 分工：`nvngx_dlss.dll`（SR）、`nvngx_dlssd.dll`（Denoise/RR）、`nvngx_dlssg.dll`（Frame Generation）。

### 4.2 Optical Flow SDK 5.0.7（`STATIC_VERIFIED`）

- 路径：`C:\SDKs\NVIDIA\Optical_Flow_SDK_5.0.7\`，含 `NvOFInterface/`、`Doc/`、`NvOFBasicSamples/`、`NvOFFRUC/`、`NvOFTracker/`、`LicenseAgreement.pdf`、`Release_Notes.pdf` 等。
- `NvOFInterface/` 头文件（5 个）：`nvOpticalFlowCommon.h`、`nvOpticalFlowCuda.h`、`nvOpticalFlowD3D11.h`、`nvOpticalFlowD3D12.h`、`nvOpticalFlowVulkan.h`。
- **API 版本**（`nvOpticalFlowCommon.h:56-58`）：`NV_OF_API_MAJOR_VERSION 5`、`NV_OF_API_MINOR_VERSION 0`、`NV_OF_API_VERSION = (5 << 4) | 0` = API 5.0。
- **D3D12 集成 API**（`nvOpticalFlowD3D12.h`）：
  - `nvCreateOpticalFlowD3D12`（创建实例）
  - `nvOFRegisterResourceD3D12`（注册 `ID3D12Resource`，typedef `PFNNVOFREGISTERRESOURCED3D12`）
  - `nvOFExecuteD3D12`（执行，typedef `PFNNVOFEXECUTED3D12`；输入结构 `NV_OF_EXECUTE_INPUT_PARAMS_D3D12`、输出结构 `NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12`）
  - 注意：执行符号为 `nvOFExecuteD3D12`（小写 `nv` 前缀），并非独立拼写的 "NvOFExecuteD3D12"。
- **运行时依赖**：`nvofapi64.dll` 由 NVIDIA 驱动提供（当前环境缺失，见第 5 节）。要求 Turing+ GPU、D3D12（Windows 10 20H1+）。
- 输入格式 NV12 / ABGR8 / GRAYSCALE8；输出光流为 int16 `SHORT2`（S10.5）。

### 4.3 能力边界（不得混淆）

以下五项**必须分别调查**，不得视为同一功能，也不得假设一个 SDK 自动提供其他能力：
- DLSS Super Resolution（SR）— `nvngx_dlss.dll`
- NVIDIA Optical Flow（NVOF）— `nvofapi64.dll`（驱动提供）
- 帧生成（Frame Generation）— `nvngx_dlssg.dll`
- 实验性神经渲染（RR / Denoise 相关）— `nvngx_dlssd.dll`
- Presenter 集成 — Xenia 侧接入，独立于以上 SDK

**"DLSS 5"** 在本项目中是目标方向的称呼。依据本地真实 SDK（DLSS 310.9.1、OF 5.0.7）与官方文档，当前可确认的能力以第 4.1 / 4.2 节为准，不虚构不存在的 API 或承诺未获证实的功能。

---

## 5. 运行时文件与依赖检查

**运行时部署目录**：`\\ein-9900k\Code\DLSS5_Research\runtime`（使用前须校验存在性与版本兼容性）。

**已确认存在的文件（49 个文件）**

| 文件 | 作用 |
| --- | --- |
| `nvngx_dlss.dll` | DLSS SR |
| `nvngx_dlssd.dll`（含 `.bak`） | Denoise / RR |
| `nvngx_dlssg.dll` | Frame Generation |
| `nvngx_dlssnr.dll` | 神经渲染 |
| `nvngx_truehdr.dll` | True HDR |
| `nvngx_vsr.dll` | 可变速率 |
| `nvngxruntime.dll` | NGX 运行时 |
| `nvVFXVideoFrameGeneration.dll` | 帧生成 |
| `NVVideoEffects.dll` | 视频特效 |
| `NVCVImage.dll` | 图像库 |
| `cudart64_12.dll` | CUDA 运行时 12 |
| `LibMWCapture.dll` | 捕获库 |

**缺失项（Findings）**
- **`nvofapi64.dll` 缺失**：由 NVIDIA 驱动提供，当前 Hyper-V 环境无驱动，故不存在。NVOF 在本环境不可加载。这是 NVOF 的 `GPU_UNVERIFIED` 前提。
- `nvofapi64.dll.bak` 等备份文件亦不在列。

**关键结论**
- DLSS 系列 DLL 存在于运行时目录，**不等于**运行时可用——实际加载与执行需真实 GPU + 驱动，属 `GPU_UNVERIFIED`。
- 本阶段**未加载任何 DLL**，仅核实文件存在性与版本。

---

## 6. 颜色、深度、运动矢量、抖动及曝光输入分析

**颜色前缓冲（可得，`STATIC_VERIFIED`）**
- 经 `RequestSwapTexture` 获取，格式 8888 或 2_10_10_10。这是 SR 的核心输入 `pInColor`。

**深度缓冲（存在但需访问器，待核实）**
- `d3d12_render_target_cache` 内部跟踪深度/模板，但**无公开 `ID3D12Resource` 深度访问器**（检索 `GetDepthTexture` / `RequestDepth` / `DepthTexture` / `z_texture` 均无结果）。
- 深度作为 guest 纹理存在于缓存中。需上游访问器 + 资源状态/分辨率验证后方可用于 `pInDepth`。
- 状态：`UNKNOWN`（可用性待核实，需上游改动）。

**运动矢量（缺失，硬性阻塞项）**
- `src/xenia/gpu` 中 `motion_vector` / `motion vector` / `movector` / `pInMotionVectors` **零命中**。
- Xenos（Xbox 360）无运动矢量硬件，宿主侧亦无生成。
- **细化结论（不作过度断言）**：
  - 运动矢量缺失是**高质量 SR 的硬性阻塞项**（场景运动时必需），也是**帧生成的前置条件**。
  - 但**不能据此断言"所有 SR 模式绝对不可用"**：SR 在静态场景、或配合零矢量回退（`InReset` / 零 MV）时理论上仍可运行（质量退化）。该运行时可行性仍属 `GPU_UNVERIFIED`。
  - **不得**伪造运动矢量、用颜色差分或相邻帧估算冒充真实游戏运动矢量、或用 Mock 数据描述为真实 MV。
- 若需真实 MV，需上游改动：guest 渲染 / 着色器翻译层注入，或由 NVOF 从相邻帧计算（NVOF 输出可作为 SR 的 MV 来源，见第 7 节）。

**抖动（jitter）**
- 缺失，可主机侧生成。SR 每帧需 jitter 偏移。

**曝光（exposure）**
- 无直接信号。可默认/估算（`InPreExposure` / `InExposureScale`）。非硬阻塞。

**帧生成 / 神经渲染所需的其余输入（均缺失，Xbox 360 非 RT）**
- 相机矩阵、相机向量（帧生成需 ×5）。
- UI/HUD 分离（帧生成需）。
- G-Buffer（albedo/normal/roughness）与 RT 命中距离（神经渲染 RR 需）。
- Reflex + HAGS（帧生成呈现机制相关）。
- 结论：这些信号在 Presenter 层仅能拿到最终颜色纹理，无法满足帧生成 / RR 的输入要求，需上游改动或不可行。

---

## 7. Veyra-NRVideo 参考架构与可移植性分析

> `Veyra-NRVideo` 位于 `\\ein-9900k\Code\DLSS5_Research\Veyra-NRVideo`，**只读参考**，本阶段严禁修改。以下为可通过实际源码证实的架构经验。

**可借鉴的架构经验**
- **无 CPU 等待的 Fence 同步**：调用方在输入就绪时 signal `inFence/inValue`；NVOF 在光流完成时 signal `outFence/nextOutValue`；调用方 `ID3D12CommandQueue::Wait` 于 `outFence`。不阻塞 CPU。
- **1:1 SR 旁路**：输入==输出时跳过整个 SR 处理（`shouldBypass`）。
- **单进程核心 host + 参数块追踪**：一个 device 一次 Init；SR/NR/FG 作为消费者，从不直接调用核心 Init/Shutdown；参数块通过 host 分配/销毁（保证逆序拆序）；追踪存活块与 `healthy()`。
- **显式 `Status` 失败分类**：内部 `Status` 枚举（Ok、InvalidArgument、FileNotFound、IoFailure、LoadLibraryFailure、MissingExport、DeviceFailure…），与 HRESULT / NGX 结果分离（按原始十六进制 + 名称记录）。
- **严格拆序**：`unregisterAll()` → 释放纹理 → `shutdown()`。在 `nvOFDestroy` + `FreeLibrary` 之后释放已注册资源会导致段错误。
- **NVOF 作为 SR 的 MV 生成器**：ABGR8 输入，SHORT2（S10.5）输出，cost 门控。这为 Xenia 缺失的 MV 提供了一条真实来源路径。

**不可移植的部分（依赖不同引擎架构）**
- Veyra 是**视频增强应用**（Moonlight / remoteplay）。其相机矩阵 / G-Buffer / RT 信号源自**源视频流**，而非 guest 渲染。
- Xenia 是**模拟器**，必须**自行生成/估算**这些信号（或从 guest 状态推导），不能直接沿用 Veyra 的信号来源。
- 因此：Fence 同步、1:1 旁路、参数块管理、显式 Status 等**机制**可移植；相机/G-Buffer/RT **数据来源**不可移植。

---

## 8. 配置、诊断和回退设计约束

> 以下为**设计约束**，本阶段**不实施**。

1. **主开关控制所有子功能**：主开关关闭时，所有新增功能保持关闭，不执行任何新增处理。
2. **SR、NVOF、帧生成、实验性神经渲染具有独立的启用与诊断状态**：各自可单独启用与诊断。
3. **子功能受主开关控制**：主开关关闭后不得继续执行新增处理。
4. **失败分类必须明确**：配置关闭、SDK 缺失、运行时不兼容、初始化失败、输入无效、资源状态错误、同步失败，各有独立分类。
5. **仅当后端实际成功处理并满足输出契约，才允许返回 `kUseBackend`**；其余情况返回 `kOriginalPath` 或明确的失败/回退结果，**不得误报成功**。
6. **不得擅自接管 `guest_output_resource_paint_refs`**，不得破坏现有资源所有权与 Fence 依赖。
7. **新增功能不得改变原始渲染路径的行为**。

**当前代码是否已具备表达这些语义的类型**
- `kOriginalPath` / `kUseBackend` **不存在**（零命中），需引入。
- 现有 `effect_count = 0` 回退模式（第 3 节）可复用为 "kOriginalPath" 路径。
- `GuestOutputPaintConfig`（`presenter.h:196`）是主开关 + 各子功能独立开关的自然落点。
- 优先复用现有 `PaintResult`-style 模式，不重复造抽象。

---

## 9. 验证证据及其等级

**本阶段证据等级**
- `STATIC_VERIFIED`：全部第 3–6 节结论（源码路径、行号、类型、函数名、SDK 版本、头文件字段、DLL 存在性、MV 缺失、回退模式）。
- `COMPILE_VERIFIED`：**未达到**（本阶段未构建，不得将静态审计等同于编译通过）。
- `MOCK_VERIFIED`：**未达到**（本阶段未做 Mock 主机侧测试）。
- `GPU_UNVERIFIED`：所有真实 GPU 行为（DLSS init/evaluate、NVOF 执行、图像输出）。
- `RUNTIME_UNVERIFIED`：真实目标环境上的运行时行为。

**关键源码证据（可追溯）**
| 结论 | 证据位置 |
| --- | --- |
| guest output 链路 | `d3d12_command_processor.cc:2250/2270/2279`、`d3d12_presenter.cc:463`、`presenter.cc:651` |
| DLSS SR 候选插入点 | `presenter.h:389`（`GuestOutputPaintEffect`） |
| 回退模式 | `d3d12_presenter.cc:530/535/671`（`effect_count = 0`） |
| 禁止接管的资源 | `d3d12_presenter.h:267`（`guest_output_resource_paint_refs`）、`:320`（`guest_output_resources_`） |
| 结果语义现状 | `presenter.h:340`（`PaintResult`） |
| DLSS Eval 契约 | `nvsdk_ngx_helpers_d3d.h:37-69` |
| OF API 版本 | `nvOpticalFlowCommon.h:56-58` |
| OF D3D12 API | `nvOpticalFlowD3D12.h`（`nvCreateOpticalFlowD3D12` / `nvOFRegisterResourceD3D12` / `nvOFExecuteD3D12`） |
| 运动矢量缺失 | `rg` 在 `src/xenia/gpu` 零命中 |
| `kOriginalPath`/`kUseBackend` 缺失 | `rg` 在 `src/` 零命中 |

**证据区分**
- **已核实事实**：上表所有源码/头文件/SDK 版本/DLL 存在性/MV 缺失。
- **合理推断**：插入点选 `GuestOutputPaintEffect` 链路（基于现有 FXAA/gamma 与放大链的对称性）。
- **UNKNOWN**：深度缓冲资源状态与分辨率；曝光可否由寄存器推导；`kOriginalPath`/`kUseBackend` 最终类型落点。
- **待复核**：许可证（第 11 节）、AGENTS.md 路径映射（第 2 节）。

---

## 10. 环境限制与未验证事项

**Hyper-V 环境无法验证**
- 真实 GPU 初始化。
- DLSS SR 的 `Evaluate` 执行。
- NVOF 光流执行。
- DLSS 输出画质与帧生成性能。

**未验证事项清单**
- 所有真实 GPU 行为：`GPU_UNVERIFIED`。
- 真实目标环境运行时行为：`RUNTIME_UNVERIFIED`。
- 编译通过：未验证（本阶段未构建）。
- Mock 主机侧逻辑：未验证（本阶段未做 Mock 测试）。
- **不得**将未经执行的构建标记为 `COMPILE_VERIFIED`，不得将静态审计标记为编译或运行时通过。

---

## 11. API、ABI、同步、许可证等风险

| 风险 | 说明 | 等级 |
| --- | --- | --- |
| SDK API / ABI 兼容 | 文档 310.6.0 vs 头文件 310.9.1，以头文件为准，需核对 ABI 稳定性 | 中 |
| 运行时 DLL 版本/加载 | `nvofapi64.dll` 缺失（驱动提供）；DLSS 系列 DLL 存在但未加载验证 | 中 |
| D3D12 资源状态/格式 | guest 输出 R10G10B10A2 与 NVOF 要求 ABGR8（B8G8R8A8_UNORM）不一致，需转换通道 | 中 |
| GPU 队列 / Fence / 跨阶段同步 | refresher 与 paint 时间线依赖，不得破坏现有 Fence | 高 |
| 游戏 MV / 深度可用性 | MV 缺失（硬阻塞）；深度需访问器 | 高 |
| 帧生成呈现机制 | 交错生成/渲染帧、Reflex+HAGS 在模拟器中可行性未定 | 高 |
| 许可证 / 分发 | 嵌入 SDK 目标代码入开源 Xenia 并重分发触发 `LICENSE REVIEW REQUIRED` | 高 |
| 失败回退 / 原始路径兼容 | 新路径关闭/失败必须无缝回到 `kOriginalPath`，不得误报成功 | 高 |

**许可证（审慎措辞，保留结论）**
- 依据 `nvidia-dlss-reference`（00）：**本地研究允许**；**将 SDK 目标代码嵌入开源 Xenia 并重分发触发 `LICENSE REVIEW REQUIRED`**；公开/商业发布需提前通知 NVIDIA；禁止反向工程与独立重分发。
- 该结论为**待复核**事项，需法律/合规确认后方可决定分发方式。本阶段不作最终裁定。

---

## 12. 各项能力状态清单

> 每项只填写**当前证据支持的最高状态**。无法确认处注明 `UNKNOWN`，不猜测。

| 能力 | 当前状态 | 说明 |
| --- | --- | --- |
| DLSS SR | **未实现** | SDK 头文件已核实，无集成代码 |
| NVOF | **未实现** | API 契约已核实，`nvofapi64.dll` 缺失 |
| 帧生成 | **未实现** | 运动矢量缺失（硬阻塞），帧生成呈现机制未定 |
| 实验性神经渲染 | **未实现** | RR 需 RT G-Buffer，对 Xenia 非 RT 架构不适用 |
| Presenter 集成 | **尚未开始** | 插入点已识别，无代码 |
| 最终画质与性能 | **尚未评估** | 需目标硬件验证 |

**状态定义**
- 未实现 / 尚未开始 / 尚未评估：本阶段仅完成静态审计。
- 编译期已集成 / 运行时已验证 / 执行已验证 / 输出已验证 / 回退已验证 / 后端输出已验证 / 本地评估 / 目标硬件验证：均需后续阶段以新证据达成，本阶段**不得**预先标记。

---

## 13. 下一步建议与前置条件

**最小且安全的下一步（推荐）**
在 `GuestOutputPaintConfig` 中引入**纯配置数据结构**：主开关 + SR / NVOF / 帧生成 / 实验性神经渲染各自独立的启用与诊断状态，并定义 `kOriginalPath` / `kUseBackend` 语义类型。
- **范围**：仅 `src/xenia/ui/presenter.h` 与 `src/xenia/ui/presenter.cc` 中新增枚举/配置字段与默认关闭的 getter。
- **不接入**任何后端、**不加载** DLL、**不改动**渲染行为。
- **前置条件**：
  1. 用户确认 AGENTS.md 路径映射更新（`DLSS5_Research` → `Code\DLSS5_Research`）。
  2. 确认许可证复核结论（`LICENSE REVIEW REQUIRED`）。
- **验收标准**：
  1. 构建通过（届时方可标记 `COMPILE_VERIFIED`）。
  2. 主开关默认关闭。
  3. 单测验证主开关关闭时 `effect_count` 逻辑不受影响。
  4. 零 GPU 依赖、零 DLL 加载。
- **停止条件**：完成本步后停止，不自动执行 Milestone 0.1 或 Milestone 1。

**明确不做的断言**
- 不将 SDK 文件存在等同于运行时可用。
- 不将静态审计等同于编译通过或运行时验证。
- 不将运动矢量缺失直接推导为所有 DLSS SR 模式绝对不可用。
- 不在没有新证据的情况下把任何状态升级为已验证。

---

## 附录：修改范围与 Git 验证

- **本阶段允许**：创建 `docs/dlss-integration/` 目录、创建 `docs/dlss-integration/milestone-0-audit.md`。
- **本阶段禁止**：修改 C++ 源码/头文件/测试/构建脚本、修改 `AGENTS.md`、修改 SDK/运行时/Veyra-NRVideo、执行构建、安装依赖、加载 NVIDIA DLL、创建其他报告或实现文件。
- **Git 验证**：本阶段仅新增本文档。`git status --short --branch` 应仅显示 `## dev/dlss5-integration` 与新增的未跟踪文档 `docs/dlss-integration/milestone-0-audit.md`，无其他差异。
