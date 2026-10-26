# Milestone 5-A：DLSS 帧生成（FG / DLSSG / DLSS-MFG）可行性研究报告

- **文档性质**：INVESTIGATION / 可行性研究（**只读**）。本轮**未修改任何代码**，仅做：官方 FG SDK 文档精读、SDK 头文件（310.9.1）核对、Veyra-NRVideo FG 实现只读研究、Xenia 当前源码（SR / NVOF / Presenter）核对，并输出本报告 + 实施计划（`milestone-5-a-FG_IMPLEMENTATION_PLAN.md`）。**未编译、未运行、未加载任何 DLL、未执行任何 GPU 操作。**
- **对应任务**：`architecture-design.md` K4.2.1 Milestone 5-A。
- **仓库状态**：分支 `dev/dlss5-integration`，HEAD `67ccea8`（ui: add Milestone 4 integrate NVOF optical flow into D3D12 paint path）。工作树仅 `docs/dlss-integration/architecture-design.md` 被修改（用户既有改动，未回退）。
- **证据标签约定**：`STATIC_VERIFIED`（当前源码已核实）｜`SDK-DOC-SUPPORTED`（官方文档支持）｜`SDK-VERIFIED`（SDK 头文件支持）｜`VEYRA-REFERENCE`（Veyra 只读参考，非本仓库代码）｜`DESIGN PROPOSAL`（设计建议，未实施）｜`UNVERIFIED`（未验证）｜`RUNTIME_VERIFIED_ON_RTX4090`（仅在真实 RTX 4090 证据存在时使用）。

---

## A. 总结

- **总体状态：`CONDITIONALLY FEASIBLE`（有条件可行）。** 所有 FG SDK API 契约已用官方文档 + 310.9.1 头文件证实；FG 运行时 DLL（`nvngx_dlssg.dll` 310.9.1，Authenticode 有效）**已部署**于 `runtime/experimental/`；FG 子开关（`frame_generation_enabled` + `IsFrameGenerationRequested()`）**已在 M2 配置结构体中定义**；SR 后端的 NGX 核心初始化/SEH 边界模式可直接复用；NVOF 的 MV 输出**可直接作为 FG 的 `pMVecs`**（格式/约定/分辨率三者全匹配）。**无硬性阻断项。**
- **三个前置条件（非阻断，但 M5-B 必须处理）**：
  1. **共享 NGX 核心属主**：当前 SR 后端 `Shutdown()` 调用 `NVSDK_NGX_D3D12_Shutdown1(device)`，这会销毁整个 NGX 核心（含任何存活的 FG 特性）。SR 与 FG 共用同一设备 → 必须引入**引用计数的共享 NGX 核心属主**（Veyra 的 `NgxCoreHost` 模式），使 `Shutdown1` 仅在最后一个消费者释放时执行。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`
  2. **第二次呈现（2x 呈现时序）**：FG 2x 在真实帧 N 之前生成插值帧 N-0.5，须先呈现 N-0.5、再呈现 N → 每个 guest 帧产生 **2 次 Present**。须修改呈现时序（`PaintAndPresentImpl` + 呈现 timeline），并新增一次「FG 输出 → backbuffer → Present」的绘制传递。这是呈现路径（最敏感区域）的改动。`DESIGN PROPOSAL`
  3. **零深度占位**：Xbox 360 不提供深度缓冲，FG 的 `pDepth` 沿用 SR 后端的零深度占位（诚实回退，非伪造）。深度质量限制生成帧质量，但**非硬性阻断**。`STATIC_VERIFIED`
- **RTX 4090 原生支持 2x FG**（`MultiFrameCountMax = 1`），无需任何解锁/补丁/架构伪装。Veyra 的 `FgCompatibilitySession`（AdaMfgUnlock / AmpereMfgUnlock / DlssgTransfusion）是**补丁/伪装机制**，按 AGENTS.md 红线第 3 条（严禁 Mock 伪装真实 GPU 执行）**不可**用于 Xenia 主线。`SDK-DOC-SUPPORTED` + `VEYRA-REFERENCE`
- **所有硬件相关项标记 `UNVERIFIED`**（Hyper-V 无 NVIDIA 驱动）：FG 特性真实创建/评估、生成帧画质、呈现时序、设备丢失回退。

---

## 1. FG SDK API 契约（官方文档 + 310.9.1 头文件）

### 1.1 NGX 核心初始化 / 关闭
- `NVSDK_NGX_D3D12_Init_with_ProjectID(projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, engineVersion, appDataPath, device, featureInfo, NVSDK_NGX_Version_API)` —— 每设备一次。**对已初始化的 API + 设备再次调用无效果**（no-op）。`SDK-DOC-SUPPORTED`（p.70）`STATIC_VERIFIED`（SR 后端 `dlss_super_sampling_backend.cc:139` 已用此模式）
- `NVSDK_NGX_D3D12_Shutdown1(device)` —— 关闭**该设备**的 SDK 并**释放所有资源**（含所有特性）。`Shutdown()`（无参）已弃用。`SDK-DOC-SUPPORTED`（p.71）**关键**：这是销毁性的，SR 与 FG 必须共享一个引用计数的核心属主。`STATIC_VERIFIED`
- 核心属主是**进程级 / 设备级单例**：SR / FG / NR 都是消费者，**各自不得自行 Init/Shutdown**。`VEYRA-REFERENCE`（`NgxCoreHost`）

### 1.2 FG 特性创建（`NGX_D3D12_CREATE_DLSSG`）
- 包装 `NVSDK_NGX_D3D12_CreateFeature(cmdList, NVSDK_NGX_Feature_FrameGeneration, params, &handle)`。`SDK-DOC-SUPPORTED`（p.119）`SDK-VERIFIED`（`nvsdk_ngx_helpers_dlssg_d3d.h`）
- `cmdList` 须**打开且正在记录**、**非拷贝队列**。`SDK-DOC-SUPPORTED`（p.76）
- 创建参数 `NVSDK_NGX_DLSSG_Create_Params`：`Width`/`Height`（颜色，4-8192/4-7680）、`NativeBackbufferFormat`（≥3 通道 FLOAT/UNORM/SNORM，如 `R10G10B10A2_UNORM`）、`RenderWidth`/`RenderHeight`（内部/渲染分辨率 = 送入 SR 的输入分辨率）、`DynamicResolutionScaling`。`SDK-DOC-SUPPORTED`（p.122）`SDK-VERIFIED`
- 310.9.1 头文件**新增**（310.7 PDF 未含）：`NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags` / `ResourceNeverProvided_Flags`（创建期资源标志位掩码，可启用 VRAM 优化；backbuffer/mvecs/depth/output 恒提供，HUDLess/UI/UIAlpha/BidirectionalDistortionField 恒不提供）。`SDK-VERIFIED`（`nvsdk_ngx_defs_dlssg.h:109-110`）`VEYRA-REFERENCE`
- **创建后宽高不可变**：改分辨率须 `ReleaseFeature` 后重建。`SDK-DOC-SUPPORTED`（p.92-93）

### 1.3 FG 评估（`NGX_D3D12_EVALUATE_DLSSG`）
- 包装 `NVSDK_NGX_D3D12_EvaluateFeature(cmdList, handle, params, callback)`。`SDK-DOC-SUPPORTED`（p.120）`SDK-VERIFIED`
- **必填资源**：`pBackbuffer`（输入颜色）、`pDepth`（当前帧深度，硬件单位）、`pMVecs`（逐像素运动矢量，当前帧→前一帧）、`pOutputInterpFrame`（输出插值帧）。`SDK-DOC-SUPPORTED`（p.123）`SDK-VERIFIED`（`NVSDK_NGX_D3D12_DLSSG_Eval_Params`）
- **可选资源**：`pHudless`、`pUI`、`pUIAlpha`（310.9.1 新增）、`pBidirectionalDistortionField`、`pOutputRealFrame`（输出真实帧，推荐用于保留真实帧）、`pOutputDisableInterpolation`（4 字节状态，指示本帧是否禁用插值）。`SDK-DOC-SUPPORTED`（p.123）`SDK-VERIFIED`
- **引擎数据**（`NVSDK_NGX_DLSSG_Opt_Eval_Params`）：`reset`（场景变化重置）、`depthInverted`、`mvecScale`、`cameraViewToClip` 等 5 个 4x4 矩阵、`cameraFOV`（**弧度**）、`cameraAspectRatio`、`colorBuffersHDR`、`motionVectorsInvalidValue`、`multiFrameCount`/`multiFrameIndex`（**310.9.1 头文件已含此字段**，310.7 PDF 结构体未含——文档矛盾已解决，以头文件为权威）。`SDK-DOC-SUPPORTED`（p.124-125）`SDK-VERIFIED`（`nvsdk_ngx_params_dlssg.h`）
- **资源状态要求**：输入资源 `Evaluate` 前须处于 `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`；输出资源须处于 `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`。**执行命令列表是应用责任**。`SDK-DOC-SUPPORTED`（p.18, 21, 95-102）
- **MVec 约定**：像素单位，**当前帧→前一帧**（把某像素的 MV 加到该像素位置，得到它在前一帧的位置）。与 NVOF 输出约定**一致**。`SDK-DOC-SUPPORTED`（p.96-97）`STATIC_VERIFIED`（NVOF `nvof_backend_test.cc` MvContract current→previous）
- **深度约定**：硬件/非线性单位，**不支持线性深度**；`depthInverted` 标记。`SDK-DOC-SUPPORTED`（p.97-98）

### 1.4 能力查询
- `NVSDK_NGX_Parameter_FrameGeneration_Available`（当前系统是否可用 FG）；`NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax`（每对渲染帧间可生成最大帧数；**RTX 40 系 = 1 → 仅 2x；RTX 50 系 ≥ 3 → 最高 4x/6x**）；`NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult` / `NeedsUpdatedDriver` / `MinDriverVersionMajor/Minor`。`SDK-DOC-SUPPORTED`（p.117-118）
- `NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, discovery, &supported)` —— **无需先 Init**，可用适配器判定 FG 支持。`SDK-DOC-SUPPORTED`（p.77）
- VRAM 估算：`NGX_D3D12_ESTIMATE_VRAM_DLSSG(...)`。`SDK-DOC-SUPPORTED`（p.121）

### 1.5 错误处理与线程安全
- `NVSDK_NGX_Result` 返回码（`0x1` 成功，`0xBAD00000` 系列失败）+ `NVSDK_NGX_SUCCEED/FAILED` 宏。完整 20 个错误码见 `sdk-docs/api/errors.md`。`SDK-DOC-SUPPORTED`（p.82-85）
- **NGX API 非线程安全**：所有调用须单线程或妥善同步。**命令列表状态不被 NGX 保存**，须自行管理。`SDK-DOC-SUPPORTED`（p.8）
- **SDK 可崩溃**（access violation），不仅是返回错误码 → 每个 SDK 入口须 SEH 边界包裹。`STATIC_VERIFIED`（SR / NVOF 后端均如此）`VEYRA-REFERENCE`

### 1.6 运行时 DLL 部署
- `nvngx_dlssg.dll`（310.9.1，7,460,976 字节，SHA256 `FF6E90EB...`，Authenticode 有效）**已部署**于 `runtime/experimental/nvngx_dlssg.dll`；另有 `fallback-dlssg-310.7/` 与 `dlssg-lite/`（310.9.16 性能模式）。`STATIC_VERIFIED`（`release-runtime-manifest.json`）
- 头文件：`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx*.h`（含 `_dlssg` 后缀的 FG 专用头）。`STATIC_VERIFIED`

---

## 2. 需求—证据—实现位置追踪（任务 1）

| # | 需求（来自 K4.2.1 + SDK 契约） | 证据 | 实现位置（M5-B / M5-C） | 状态 |
|---|---|---|---|---|
| R1 | FG 默认关闭，主开关控制所有子功能 | `GuestOutputEnhancementConfig.frame_generation_enabled=false`；`IsFrameGenerationRequested()=master&&fg` | `guest_output_enhancement.h`（**已存在**） | `STATIC_VERIFIED` 复用 |
| R2 | FG 子开关 UI | M2 配置已有，但 **UI 未画 FG 复选框**（现仅 SR + Optical Flow） | `emulator_window.cc`（**需新增**） | `STATIC_VERIFIED` 新增 |
| R3 | FG 后端模块（NGX 封装，SEH 边界） | SR 后端 `DLSSSuperSamplingBackend` 的完整模式可复用 | `src/xenia/ui/fg/`（**需新增**，仿 `dlss/`） | `STATIC_VERIFIED` 新增 |
| R4 | 共享 NGX 核心属主（引用计数） | SR `Shutdown()` 调 `Shutdown1` 销毁整个核心 | **需新增** `NgxCoreHost` 等价物 | `STATIC_VERIFIED` 新增（前置条件 1） |
| R5 | FG 创建参数（Width/Height/Format/RenderW/H） | `NVSDK_NGX_DLSSG_Create_Params` | FG 后端 `Create` | `SDK-VERIFIED` |
| R6 | FG 评估输入（backbuffer/depth/mvecs/output） | `NVSDK_NGX_D3D12_DLSSG_Eval_Params` | FG 后端 `Evaluate` | `SDK-VERIFIED` |
| R7 | 资源状态转换（NPSR/UAV）+ 命令列表执行 | p.18/21/95-102 | FG 后端 + Presenter 屏障 | `SDK-DOC-SUPPORTED` |
| R8 | MV 输入（当前帧→前一帧，像素单位） | NVOF `MotionVectorOutput()`（R16G16_FLOAT，guest 输出分辨率） | **复用 NVOF 输出**作为 `pMVecs` | `STATIC_VERIFIED` 复用 |
| R9 | 深度输入（硬件单位） | 无真实深度 → 零占位（诚实回退） | 复用 SR 零深度占位 | `STATIC_VERIFIED` 复用 |
| R10 | 2x 呈现时序（先 N-0.5 后 N） | p.24 呈现时序是应用责任 | **需修改**呈现路径 + 新增第二呈现 | `STATIC_VERIFIED` 修改（前置条件 2） |
| R11 | 失败/关闭 → 原始路径（仅呈现真实帧） | `BackendResult` 枚举 + `IsOriginalPathFallback` | FG 后端 `TryProcess` 返回 `BackendResult` | `STATIC_VERIFIED` 复用 |
| R12 | 能力查询（Available / MultiFrameCountMax） | `GetCapabilityParameters` + `GetFeatureRequirements` | FG 后端 `Initialize` | `SDK-DOC-SUPPORTED` |
| R13 | 运行时缺失 / 驱动过旧 → 确定性 kUnavailable | 错误码表 | FG 后端 `Initialize` 失败分类 | `SDK-DOC-SUPPORTED` |
| R14 | 许可证隔离（SDK 头仅 fg/* 包含） | SR/NVOF 许可隔离模式 | FG 后端模块 | `STATIC_VERIFIED` 复用 |
| R15 | 禁止 Mock/伪装真实 GPU 执行（红线 3） | Veyra `FgCompatibilitySession` 是补丁机制 | **不引入** AdaMfgUnlock 等 | `VEYRA-REFERENCE` 排除 |

---

## 3. 未解决问题按阻断级别分类（任务 2）

### 阻断级（必须解决才能继续 M5-B）
- **P0-1 共享 NGX 核心属主缺失**：SR 的 `Shutdown1` 会销毁 FG 特性。必须引入引用计数的核心属主。**无技术难点**，纯架构改动。`STATIC_VERIFIED`
- **P0-2 呈现时序改动**：2x 需每 guest 帧 2 次 Present，须改 `PaintAndPresentImpl` 与呈现 timeline。**这是最敏感的改动**，但设计清晰（见 §6）。`DESIGN PROPOSAL`

### 高优（影响质量，非阻断）
- **P1-1 零深度占位**：生成帧质量受限（无真实深度引导）。诚实回退，非伪造。可后续用深度估计改进。`STATIC_VERIFIED`
- **P1-2 MV 来源为 NVOF 估算光流**：非游戏原生运动矢量，语义与原生 MV 不同（红线 3 不得冒充）。对 FG 是**可用输入**（FG 接受引擎运动信息），但生成帧质量取决于 MV 质量。`STATIC_VERIFIED`
- **P1-3 禁用插值状态读取**：`pOutputDisableInterpolation`（4 字节）需 CPU 读回才能决定是否跳过插值帧呈现。读回引入 CPU-GPU 同步点（延迟）。最小版本可**始终呈现两帧**（禁用时生成帧=真实帧拷贝，短暂重复帧/卡顿），读回控制留作 M5-C 优化。`DESIGN PROPOSAL`

### 中优（可延后）
- **P2-1 动态分辨率**：`DynamicResolutionScaling` 当前不支持，guest 输出分辨率固定。留作后续。`SDK-DOC-SUPPORTED`
- **P2-2 多帧生成（MFG）**：RTX 4090 仅 2x（`MultiFrameCountMax=1`），4x/6x 需 RTX 50 系。**不在本期范围**。`SDK-DOC-SUPPORTED`
- **P2-3 HDR 输出**：guest 输出是 SDR（`colorBuffersHDR=false`），即便格式是 RGB10A2。HDR 留作后续。`STATIC_VERIFIED`

### 低优（文档/诊断）
- **P3-1 scratch buffer**：文档说「大多数 NGX 特性不使用 scratch buffer」，未明确 FG。`SDK-DOC-SUPPORTED`（待验证，FG 大概率不需要）
- **P3-2 `PFN_NVSDK_NGX_ProgressCallback` 完整 typedef**：文档未给出。`SDK-DOC-SUPPORTED`（待验证，最小版本不用进度回调）

---

## 4. 现有模块复用 / 修改 / 新增（任务 3）

### 复用（不改）
| 模块 | 复用点 |
|---|---|
| `guest_output_enhancement.h` | `GuestOutputEnhancementConfig.frame_generation_enabled` / `IsFrameGenerationRequested()`；`BackendResult` 枚举；`IsOriginalPathFallback()` |
| `dlss/dlss_super_sampling_backend.{h,cc}` | **模式参考**：NGX 核心初始化（`Init_with_ProjectID`）、参数图（`AllocateParameters`）、SEH 边界、输出纹理所有权、懒初始化、失败分类、零深度/零 MV 占位 |
| `nvof/nvof_optical_flow_backend.{h,cc}` | **MV 输出复用**：`MotionVectorOutput()`（R16G16_FLOAT，像素单位，当前帧→前一帧，guest 输出分辨率）**直接作为 FG 的 `pMVecs`** |
| `d3d12_presenter.cc` 呈现路径 | 现有效果链、backbuffer 管理、呈现 timeline、view heap、配置 mutex 读取 |
| `runtime/experimental/nvngx_dlssg.dll` | FG 运行时 DLL 已部署，无需重新部署 |

### 修改（最小化）
| 模块 | 修改 |
|---|---|
| `d3d12_presenter.{h,cc}` | 新增 FG 后端成员 + FG 块（仿 SR/NVOF 块）；**新增第二呈现**（FG 输出 → backbuffer → Present）；新增 FG 输出 SRV 到 view heap；Presenter 析构时按序释放（FG → SR → 核心属主） |
| `emulator_window.cc` | 新增 FG 复选框（主开关 on 时绘制，语义同 SR/OF 子开关）+ `dlss_frame_generation_enabled` cvar |
| `guest_output_enhancement.h` | （可选）若 FG 需要独立的帧参数（如 `reset` 标志、`frameId`），可新增 `DlssFgFrameParams`（仿 `DlssSrFrameParams`）。**最小版本可先不加**，用现有参数 |

### 新增
| 模块 | 内容 |
|---|---|
| `src/xenia/ui/fg/fg_frame_generation_backend.{h,cc}` | FG 后端模块（NGX FG 封装，SEH 边界，输出纹理/OutputReal/禁用状态缓冲所有权，懒初始化，能力查询，失败分类，零深度占位） |
| `src/xenia/ui/ngx_core_host.{h,cc}`（或并入 fg/） | **共享 NGX 核心属主**（引用计数，SR/FG 共享，`Shutdown1` 仅在最后一个消费者释放时执行） |
| `src/xenia/ui/testing/fg_backend_test.cc` | FG 后端主机侧测试（配置门控、BackendResult 映射、路径选择、SEH 边界、资源所有权） |
| `src/xenia/ui/CMakeLists.txt` | FG 模块接线 |

---

## 5. FG 与现有 SR 的组合方式（任务 4）

**结论：FG 消费 SR 的输出作为 backbuffer（SR on 时），或消费 guest 输出在显示分辨率下的拷贝（SR off 时）。深度与 MV 始终在 SR 输入分辨率（= guest 输出分辨率）。**

### 分辨率 / 格式契约
| 角色 | 分辨率 | 格式 | 来源 |
|---|---|---|---|
| FG `Width`/`Height`（颜色/backbuffer） | 显示分辨率（swapchain） | `R10G10B10A2_UNORM`（≥3 通道 UNORM，合法） | SR 输出纹理（SR on）/ guest 输出经效果链到 backbuffer（SR off） |
| FG `RenderWidth`/`RenderHeight`（内部/深度/MV） | guest 输出分辨率（SR 输入） | — | 固定 |
| FG `pBackbuffer` | 显示分辨率 | `R10G10B10A2_UNORM` | SR 输出纹理 或 backbuffer 拷贝 |
| FG `pDepth` | guest 输出分辨率 | 零占位（诚实回退） | 复用 SR 零深度 |
| FG `pMVecs` | guest 输出分辨率 | `R16G16_FLOAT`（像素单位，当前→前一帧） | **NVOF `MotionVectorOutput()` 直接复用** |
| FG `pOutputInterpFrame` | 显示分辨率 | `R10G10B10A2_UNORM`（UAV） | FG 后端自有 |
| FG `pOutputRealFrame` | 显示分辨率 | `R10G10B10A2_UNORM`（UAV） | FG 后端自有（SDK 拷贝真实帧 N） |
| FG `pOutputDisableInterpolation` | 4 字节 | 状态缓冲 | FG 后端自有（CPU 读回） |

### 组合矩阵
| SR | NVOF | FG | FG backbuffer 来源 | FG MV 来源 | 行为 |
|---|---|---|---|---|---|
| off | off | off | — | — | 原始路径（现状） |
| on | off | off | — | — | SR 路径（现状） |
| off | on | on | guest 输出经效果链到 backbuffer 的显示分辨率拷贝 | NVOF 输出（guest 分辨率） | FG 2x，SR 关闭 |
| on | on | on | **SR 输出纹理**（显示分辨率） | NVOF 输出（guest 分辨率） | **FG + SR 叠加**（推荐组合） |
| on | off | on | SR 输出纹理 | SR 零 MV 占位 | FG 2x，MV 为零（生成帧质量受限，但可运行） |

### 独立性（AGENTS.md 红线）
- FG 是**独立模块**，正交于 SR 与 NVOF。FG 失败**不强制** SR 失败（SR 回退到自己的零 MV 占位），SR 失败**不强制** FG 失败（FG 回退到原始路径）。`STATIC_VERIFIED`（NVOF/SR 已正交）
- FG 关闭 → 不加载 DLL、不建资源、不做 GPU 工作。`STATIC_VERIFIED`（配置门控）

---

## 6. 最小化集成设计（任务 5）

### 6.1 资源所有权
- **FG 后端自有**：插值输出纹理（`R10G10B10A2`，UAV）、OutputReal 纹理（保留真实帧 N）、禁用插值状态缓冲（4 字节，CPU 读回）、SDK 特性句柄 + 参数图、零深度占位缓冲。
- **FG 后端借用（只读）**：backbuffer（SR 输出纹理 或 backbuffer 拷贝）、深度（零占位）、MV（NVOF 输出）。**永不接管** `guest_output_resource_paint_refs`，**永不销毁**借用资源。
- **共享 NGX 核心属主自有**：NGX 核心初始化状态 + 引用计数。SR 与 FG 都是消费者。

### 6.2 每 guest 帧 N 的调用顺序
1. **（现有）** 呈现真实帧 N：NVOF 块 → SR 块（SelectOutputPath）→ 效果链 → backbuffer 持有 N。
2. **（FG 请求且可用时）** 记录 FG `Evaluate` 到命令列表：输入 backbuffer[NPSR] + 深度[NPSR] + MV[NPSR]；输出 插值帧[UAV] + OutputReal[UAV]（SDK 拷贝 N→OutputReal）+ 禁用状态[UAV]。**执行命令列表是应用责任**（现有呈现路径末尾执行）。
3. **呈现插值帧 N-0.5**：把 FG 插值输出绘制到 backbuffer → `Present`。
4. **呈现真实帧 N**：把 OutputReal（N）绘制到 backbuffer → `Present`。

呈现顺序跨周期：..., (N-1.5), (N-1), (N-0.5), (N), (N+0.5), (N+1), ... —— **时间序正确**，每个生成帧在其对应真实帧之前呈现。

### 6.3 同步与状态转换
- **单命令列表**，一次执行（现有模式）。状态转换：输入到 NPSR（Evaluate 前）、输出到 UAV（Evaluate 前）；Evaluate 后屏障输出到 NPSR/PixelShaderResource（供绘制传递读取）。
- **Fence**：复用现有 `paint_completion_timeline` + `present_completion_timeline`。第二呈现复用同一 timeline（ring 转两次快）。
- **禁用状态读回**（最小版本可省）：4 字节缓冲 CPU 读回，若插值被禁用则跳过第 3 步（只呈现 N 一次）。读回引入同步点，留作 M5-C 优化。

### 6.4 状态切换
- **懒初始化**：首次 FG 请求时建能力查询 + `CreateFeature`（记录到命令列表）。失败则缓存为不可用（不重试）。
- **重建**：尺寸变化（显示分辨率 / guest 分辨率）→ `ReleaseFeature` + 重建。
- **Presenter 析构序**：FG 后端 → SR 后端 → 共享核心属主（`Shutdown1` 在引用计数归零时执行）。NVOF 先于 SR/FG 释放（其注册纹理须先释放，Veyra 已验证）。

### 6.5 回退路径
- **FG 失败**（任意 `BackendResult != kSuccess`）→ 仅呈现真实帧 N（原始路径）。**永不把失败标记为成功**。确定性回退。
- **FG 关闭 / 不可用** → 不建后端、不加载 DLL、不做 GPU 工作（现状原始路径）。
- **设备丢失 / 同步失败 / 输出无效** → 回退 + 失败分类记录。

---

## 7. 逐步测试计划与成功判据（任务 6）

### 主机侧（CPU，无 GPU，本环境可执行）
1. **配置门控**：`frame_generation_enabled=false`（默认）→ `IsFrameGenerationRequested()=false`；主开关 off → FG 不请求。成功判据：FG 不建后端、不加载 DLL。
2. **BackendResult 映射**：stub FG 后端返回各 `BackendResult` → 路径选择正确（kSuccess → 用 FG 输出；其余 → 原始路径）。成功判据：`IsOriginalPathFallback` 全状态正确。
3. **路径选择纯函数**：FG 请求 + stub 成功 → 选 FG；stub 失败 → 回退。成功判据：与 SR 的 `SelectOutputPath` 同构。
4. **SEH 边界**：stub 触发 SEH → 捕获为失败（非崩溃）。成功判据：确定性回退。
5. **资源所有权**：FG 后端析构 → 释放自有纹理/缓冲，不释放借用资源。成功判据：无泄漏、无 double-free。
6. **共享核心属主**：SR + FG 同时初始化 → 核心 Init 一次；SR 释放 → 核心仍活（FG 用）；FG 释放 → 核心 Shutdown1。成功判据：引用计数正确，无提前 Shutdown1。

### 静态 / 编译期（本环境可执行）
7. **整树 `/WX` 编译**：`.\xb build --config release`。成功判据：无警告无错误。
8. **许可证隔离**：SDK 头仅 `fg/*` 翻译单元包含。成功判据：`rg nvsdk` 仅命中 fg/*。
9. **无 NVIDIA import library 链接**：FG 模块不链接 NVIDIA 导入库。成功判据：链接器无 NVIDIA .lib。

### 真实硬件（RTX 4090，**UNVERIFIED**，本环境不可执行）
10. **FG 能力查询**：`FrameGeneration_Available=1`，`MultiFrameCountMax=1`（2x）。成功判据：JSON 证据。
11. **FG 特性创建**：`NGX_D3D12_CREATE_DLSSG` 成功（handle 非空）。成功判据：日志。
12. **FG 评估**：`NGX_D3D12_EVALUATE_DLSSG` 成功（返回 Success）。成功判据：日志 + 生成帧非空。
13. **2x 呈现时序**：呈现顺序 (N-0.5), (N) 正确，无重复帧/丢帧。成功判据：肉眼 + 帧计数。
14. **失败回退**：人为注入失败（无效参数 / 设备丢失）→ 回退原始路径，无崩溃、无卡死。成功判据：确定性回退 + 日志。
15. **SR + FG 叠加**：SR on + NVOF on + FG on → 生成帧质量可接受，无伪影。成功判据：肉眼 + 对比。

---

## 8. 明确结论（任务 7）

**有条件可行（CONDITIONALLY FEASIBLE）。**

- **所有必要前提已确认**：FG SDK API 契约（官方文档 + 310.9.1 头文件）、FG 运行时 DLL 部署、FG 配置子开关、SR/NVOF 可复用模块、RTX 4090 原生 2x 支持。**可执行的实现方案**已给出（§6 最小化集成设计）。
- **存在明确的前置改造 / 待验证事项**（3 个，均非硬性阻断）：
  1. 共享 NGX 核心属主（引用计数）——纯架构改动，无技术难点。
  2. 第二次呈现时序（2x）——呈现路径改动，设计清晰但需谨慎。
  3. 零深度占位 + NVOF 估算 MV——生成帧质量受限，诚实回退非伪造。
- **非不可行**：无硬性限制（RTX 4090 支持 2x，无驱动/硬件阻断）。
- **非证据不足**：关键接口与输入契约已全部证实（SDK-VERIFIED / SDK-DOC-SUPPORTED / STATIC_VERIFIED）。
- **所有硬件相关项 UNVERIFIED**（Hyper-V 无 NVIDIA 驱动），按红线不得宣称已验证。

---

## 附：环境限制与剩余风险
- **环境**：Hyper-V（无 NVIDIA 驱动/GPU），`nvofapi64.dll` 不存在，`nvngx_dlssg.dll` 在 `runtime/experimental/` 但无法加载执行。所有 GPU 项 UNVERIFIED。
- **剩余风险**：
  - **呈现路径改动风险**（最高）：2x 双呈现改 `PaintAndPresentImpl`，须保证不破坏现有 SR/NVOF/原始路径、不破坏呈现 timeline / swapchain ring（3 缓冲）。
  - **共享核心属主竞态**：SR/FG 引用计数须线程安全（UI 线程写配置，paint 线程读）。
  - **零深度 / 估算 MV 质量**：生成帧可能有伪影，需真实硬件量化。
  - **禁用状态读回延迟**：若采用 CPU 读回控制插值帧呈现，引入同步点（延迟）。
  - **许可证**：FG SDK 许可未审计（可整体删除 fg/* 模块，许可隔离）。
  - **ABI / 头差异**：本环境非标准 Windows SDK D3D12 头（缺 `Wait`/`GetDescriptorIncrementSize` 等），SR/NVOF 已适配，FG 须同适配。
- **下一步（最小且安全）**：见 `milestone-5-a-FG_IMPLEMENTATION_PLAN.md`（M5-B：共享核心属主 + FG 后端模块 + 主机侧测试 + 编译期集成；**不改呈现路径**，呈现路径留 M5-C）。
