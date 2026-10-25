# M3-C HAND OFF — DLSS SR 开发经验总结与 Milestone 4（NVOF）实施方案

- **状态**：Milestone 3（DLSS SR）**完成**——RTX 4090 测试机运行时已验证（画质未评估）。本文为交接文档，**本轮未修改任何代码**。
- **范围**：① 归纳 `4046a47..15265e0`（14 提交、23 文件、+3361/−34，全部为 M3 工作）的开发经验；② 依据 `architecture-design.md`（K4.1）与 `nvidia-dlss-reference/04-nvof-official-reference.md`，为 Milestone 4（NVOF 光流）做功能拆解与分步实施设计。
- **撰写前强制阅读**：`AGENTS.md`、`docs/dlss-integration/architecture-design.md`、`nvidia-dlss-reference/04-nvof-official-reference.md`。
- **证据标签约定**：`STATIC_VERIFIED`（源码/静态分析层面）、`SDK-VERIFIED`（SDK 头文件/样本/文档层面）、`DESIGN PROPOSAL`（设计提案，未实施）、`UNVERIFIED`（未经真实环境验证）、`RUNTIME_VERIFIED_ON_RTX4090`（测试机运行时验证）。

---

## A. 总结

- **当前里程碑**：M3（DLSS SR）收尾交接。主分支 `dev/dlss5-integration` HEAD 为 `15265e0`，工作区干净；`4046a47`（M2 tip）为其祖先提交，diff 即 M3 全部增量。
- **M3 成果**：DLSS SR 后端独立模块（默认关闭、惰性创建、确定性回退）、Presenter 接线、UI 质量预设、15 个主机侧测试用例（45 断言全绿）。在 RTX 4090 测试机上完成 1280×720 → 3840×2160 的 3×3 超采样，用户确认工作正常。
- **交接对象**：Milestone 4 —— NVOF 光流模块（独立模块，红线第 1 条：默认全部关闭，独立启用）。本文第 F 节给出 M4-A～M4-E 五步拆解，每步含修改范围、验收条件与停止点（K4.4：每步完成后停止，等待用户审查）。
- **总体状态**：**PASS WITH FINDINGS**。Findings：画质评估未做（C3 调查悬置：零深度/零 MV/后色调映射输入/曝光），NVOF 许可证审计未做。

---

## B. 修改文件（`4046a47..15265e0`，23 文件）

### B.1 新增：`src/xenia/ui/dlss/`（后端核心，许可隔离区）

| 文件 | 内容 | 原因 |
|---|---|---|
| `dlss_super_sampling_backend.h/.cc` | 状态机（kNotInitialized/kAvailable/kUnavailable）、SEH 包装、`NGXResourceAllocCallback` 共享内存回调、输出纹理（ALLOW_UAV/UAV）、零填充深度 R32F + MV R16G16F 占位、按 LUID 自枚举 adapter 做能力检查、`TryProcess` 向调用方命令列表记录 4 个 barrier + `EvaluateFeature` | DLSS SR 的独立模块实现；`nvsdk_*` 仅存在于 `dlss/*`（许可隔离红线） |
| `CMakeLists.txt` | 后端编译单元 | 纳入构建 |

### B.2 修改：Presenter 接线与增强配置

| 文件 | 内容 | 原因 |
|---|---|---|
| `ui/d3d12/d3d12_presenter.{h,cc}` | `PaintAndPresentImpl` 中读 `GuestOutputEnhancementConfig`（互斥锁保护）→ `IsSuperSamplingRequested() && !1:1` → 惰性创建后端 → `SelectOutputPath` → 成功时在 `kViewIndexDLSSSrOutputSrv` 建 SRV 并重定向首 paint effect 源；失败时 `command_allocator->Reset()` 重录原始路径。析构在时间线 await 后 `dlss_sr_backend_.reset()`。`BeginSubmission` 记录 device-removed 原因 HRESULT | 回退路径必须无缝（红线 2）；device-removed HRESULT 是 R6 修复的关键诊断手段 |
| `ui/guest_output_enhancement.{h,cc}` | `GuestOutputEnhancementConfig`：master_enabled、super_sampling_enabled、**optical_flow_enabled（M4 钩子，`IsOpticalFlowRequested()` 已存在）**、perf_quality、frame_generation_enabled、experimental_neural_rendering_enabled；`DlssSrFrameParams {jitter_x, jitter_y, in_reset}` 当前全 0 | 主开关控制一切子功能（红线 1）；M4 的配置面已预留 |
| `ui/CMakeLists.txt`、`ui/d3d12/CMakeLists.txt` | 目标接线 | 构建 |
| `app/emulator_window.{h,cc}` | UI 开关与 FSR/CAS 互斥单选 | 独立启用的入口 |
| `gpu/d3d12/d3d12_command_processor.cc`、`gpu/d3d12/d3d12_shared_memory.cc` | 共享内存/提交路径适配 | 支撑 `NGXResourceAllocCallback` |
| `ui/testing/guest_output_enhancement_test.cc` | 15 用例 / 45 断言 | 主机侧回归保护 |

### B.3 新增文档（9 份，均在本目录）

`milestone-3-a-audit.md`、`milestone-3-b-sr-debug-report.md`、`milestone-3-b-sr-fix-adapter-retain.md`、`milestone-3-b-sr-fix-luid-enum.md`、`milestone-3-b-sr-fix-upload-buffer-lifetime.md`、`milestone-3-b-sr-fix-verification-report.md`、`milestone-3-b-sr-quality-ui-design.md`、`milestone-3-b-sr-quality-ui-implementation.md`、`ui-dlss-switch-design.md`。

---

## C. 验证证据

- **构建**：`.\xb build --config release` 通过（`15265e0`）。测试构建 `--build-tests --target xenia-ui-tests`。
- **主机侧测试**：`guest_output_enhancement_test` 15 用例 / 45 断言全绿（STATIC_VERIFIED）。
- **静态检查**：所有 `nvsdk_*` 调用逐一对照 SDK 官方头文件与 Veyra 参考（`STATIC_VERIFIED` + `SDK-VERIFIED`）。
- **运行时**（RUNTIME_VERIFIED_ON_RTX4090，构建 80e875e，测试机驱动 617.42 / Win 10.0.26100.9549 / SDK 310.9.1，安装于 `E:\Emulator\xenia_canary_windows\Emulators\Xenia DLSS`）：
  - LUID `0000:1233D` 匹配、能力检查通过；
  - DLTSS 创建成功：输入 1280×720 → 输出 3840×2160（Balanced）；
  - 日志出现 “first frame evaluated”、“backend activated”；
  - `d8b4c2c`（纹理状态）与 `59d4fa9`（上传缓冲生命周期）之后，用户确认模拟器稳定工作。
- **A/B 交叉验证**（adapter 持有回归，见 D.1）：`c6543c6`（释放 adapter）与 `9848b09`（持有 adapter）两份日志，同机、同驱动、共享内存分配代码**逐字节一致**，唯一行为性差异即 adapter 持有 → 定论驱动拒绝 `CreateCommittedResource`。

---

## D. M3 开发经验总结

### D.1 缺陷时间线（按提交顺序）

**① adapter 枚举：R1 结构 bug → R2 回归 → R6 终修**（`1dabfbb`/`9848b09`/`34f4803`）
- **R1**：后端以 `ID3D12Device::QueryInterface(IDXGIDevice1)` 取 adapter 恒失败（0x80004002）→ kInitFailed。确定性回退路径本身工作完美（红线 2 第一次实战检验）。
- **R2 回归**（`9848b09`）：改为 Provider 持有 `IDXGIAdapter1` 供后端借用 → NVIDIA 驱动在设备创建后仍持有创建设备所用 adapter 时，**拒绝后续 512 MB `CreateCommittedResource`（`E_INVALIDARG` 0x80070057）**。此为未文档化的驱动行为，第 4–5 轮通过 `GetDeviceRemovedReason`（S_OK）+ PIX 附加 + 精确 HRESULT 定位。
- **R6 终修**（`34f4803`）：Provider 释放 adapter；后端**自包含枚举**——`ID3D12Device3::GetAdapterLuid` → `CreateDXGIFactory1` → `EnumAdapters1` → `GetDesc1` 读 `AdapterLuid` 匹配 → 能力检查后释放。A/B 交叉验证定论（见 C）。
- **教训**：COM 查询失败 ≠ “换个方式拿同一对象”就能解决；**持有/释放时序是驱动契约的一部分**，未文档化行为必须靠 A/B 实证，不能靠推理。

**② 上传缓冲生命周期**（`59d4fa9`）
- 本地 ComPtr 上传缓冲在 GPU 执行首帧 `CopyTextureRegion` 前被释放 → device removed **0x887A0006**。修复：成员化 `upload_buffer_`，Shutdown 时 Reset。Veyra 参考同样使用成员上传缓冲。
- **教训**：D3D12 命令录制与执行异步，**CPU 侧资源生命周期必须覆盖 GPU 消费窗口**；“首帧才炸”是典型特征。

**③ 深度/MV 纹理资源状态**（`d8b4c2c`）
- 零填充的深度 R32F / MV R16G16F 停留在 `PIXEL_SHADER_RESOURCE`，而 SDK 的 DLAA compute 把它们绑为 **UAV** → GPU fault → device loss → PM4 “Failed in backend” 噪声。修复：零初始化后过渡到 `NON_PIXEL_SHADER_RESOURCE`；因 SDK 从不写回，无需每帧 barrier。
- **教训**：占位纹理（honest fallback）也是真实 GPU 资源，**状态必须匹配 SDK 的绑定意图**；“PM4 报错”这类无指向性噪声要用资源状态审计来破。

**④ 质量 UI**（`15265e0`）
- `DlssSrPerfQuality` 最终只保留 **4 档**（kMaxPerf=0 / kBalanced=1 / kMaxQuality=2 / kUltraPerformance=3），UltraQuality 因当前运行时不支持而移除。SliderInt 0..3，cvar `dlss_sr_perf_quality`（字符串，默认 "balanced"）。质量变更走哨兵 `dlss_sr_backend_perf_quality_=0xFF` + 后端 Reset/惰性重建。
- **教训**：UI 暴露的枚举必须与运行时真实支持的集合**逐值对齐**，宁缺毋滥。

### D.2 流程方法论（可复用于 M4）

1. **设计稿 → 用户批准 → 实施 → 测试机验证** 闭环；仅加诊断日志的轮次无需批准（纯观测）。
2. **A/B 交叉验证**：共享代码逐字节一致，只改一个行为变量，用日志定因。
3. **记录精确 HRESULT**（而非笼统“失败”），是跨轮诊断的公共语言。
4. **SDK 调用逐一对照官方头文件 + Veyra 参考**——Veyra 是 M3 全部三处结构性偏差（adapter 枚举、上传缓冲、纹理状态）的参照系。
5. **一次性日志守卫**（one-shot guard）防每帧刷屏；`kUnavailable` 缓存防每帧 SDK 重试。
6. **惰性创建置于主开关之后** = 结构性默认关闭（红线 1 不靠“记得关”，靠结构保证）。
7. **许可隔离**：`nvsdk_*` 仅出现在 `dlss/*`，全程未破。
8. 编译成功 ≠ 运行时通过，历史构建 ≠ 当前构建（红线 3/4 的构建侧对应物）。

### D.3 运行时验证状态与遗留

- DLSS SR = **运行时已验证**（3×3 上采样，Balanced）。
- **C3 调查悬置（画质未评估）**：当前输入是后色调映射帧、深度全零、MV 全零、曝光链未核——“能跑”与“画质对”是两回事。此项并入 M4-E（见 F.5）。

---

## E. 代码现状与 M4 钩子（HEAD = `15265e0`）

- `dlss_super_sampling_backend.{h,cc}`：头文件 TODO 明确写着 “Replace with … **NVOF-derived motion vectors**”——**这就是 M4 的接线点**。当前 `motion_vectors_texture_` 为全零占位（honest fallback，未伪装成真实 MV，红线 3 合规）。
- `d3d12_presenter.cc`：失败路径 = `command_allocator->Reset()` + 重录原始路径，M4 的 NVOF 后端复用同一回退骨架。
- `GuestOutputEnhancementConfig`：`optical_flow_enabled` 字段与 `IsOpticalFlowRequested()` **已存在**，M4 不需要动 Presenter 的配置面。
- `DlssSrFrameParams {jitter_x, jitter_y, in_reset}` 全 0：jitter 属帧生成范畴，M4 不碰。
- 配置：`runtime\config\ngx-local.json`；运行时目录 = `exe\runtime\experimental`（相对路径，AGENTS.md §2）。
- 测试基线：15 用例 / 45 断言全绿；M4 新增用例不得破坏现有断言。

---

## F. Milestone 4（NVOF）功能拆解与详细方案

> 依据：`architecture-design.md` K4.1（NVOF 作为独立模块）、`04-nvof-official-reference.md`（SDK 5.0.7 官方参考）、Veyra `NvOfSession` 契约（只读参考）。
> **M4 总约束**：NVOF flow ≠ DLSS 原生 MV，M4 只做“用 NVOF 输出稠密化后喂给 SR 后端”的接线，不宣称 NVOF 具备原生 MV 语义。

### F.0 SDK/运行时事实基线（SDK-VERIFIED）

- **SDK**：`C:\SDKs\NVIDIA\Optical_Flow_SDK_5.0.7`。头文件 `NvOFInterface\nvOpticalFlowD3D12.h` / `nvOpticalFlowCommon.h`；公共库 `Common\NvOFBase`；D3D12 样本 `AppOFD3D12`。**`LicenseAgreement.pdf` 未审计，条款可能与 DLSS §4e 不同**（M4-A 首要事项）。
- **运行时**：`nvofapi64.dll` 为**驱动自带**（System32）——开发机（Hyper-V）缺失，测试机存在。加载方式按 Veyra：`LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`。
- **API 生命周期**：load → `NvOFAPICreateInstanceD3D12`（5.0）→ `nvCreateOpticalFlowD3D12(device)` → `NvOFGetCaps`（grid、min/max 尺寸、hint 支持）→ `NvOFInit`（w/h、outGridSize 1/2/4、mode=OPTICALFLOW、perfLevel SLOW=5/MEDIUM=10/FAST=20、enableExternalHints、enableOutputCost、predDirection、enableGlobalFlow、inputBufferFormat NV12/ABGR8/GRAYSCALE8）→ `NvOFRegisterResourceD3D12`（fence 点 in/out 对 → `NvOFGPUBufferHandle`）→ 每帧 `NvOFExecuteD3D12`（inputFrame、referenceFrame、fencePoints；出 outputBuffer、outputCostBuffer、bwd、out fencePoint）→ `NvOFUnregisterResourceD3D12` → `NvOFDestroy` → `FreeLibrary`。
- **输出格式**：flow 为 **int16 S10.5 SHORT2**，网格尺寸 `ceil(w/grid)×ceil(h/grid)`；cost 为 R8_UINT 同网格。
- **Veyra 契约**：inputA=上一帧、inputB=当前帧，均 **B8G8R8A8_UNORM（ABGR8）**；flowOut R16G16_SINT、costOut R8_UINT **必需**（置信门控）；disableTemporalHints=true；predDirection FORWARD；fence 同步（调用方在输入 GPU 就绪时 signal inFence，NVOF signal outFence，消费方 `ID3D12CommandQueue::Wait`，无 CPU 等待）；**拆毁顺序 unregisterAll() → 释放纹理 → shutdown**（在 destroy+FreeLibrary 之后释放已注册资源会段错误——Veyra 实测）；注册前做 GetDesc 契约校验，caps 不符 fail-closed。
- **稠密化**（Veyra `NvofDensify.hlsl`）：网格 SHORT2 /32 → 全分辨率 float2（最近邻复制）；cost→confidence = (255−cost)/255；可选边界/亮度/场景切换校验；negate 开关。
- **方向性**：NVOF 向量指向 **current→previous**，与 DLSS SR MV 契约（current→previous, RG16）一致——这是 M4-C 接线成立的前提。
- **格式鸿沟**：Xenia 终帧为 **R10G10B10A2_UNORM**，非 NVOF 原生输入 → 需要 **ABGR8 转换 pass**。
- **线程安全**：NVOF 非线程安全 → 会话单线程持有。

### F.1 M4-A：只读审计 + 设计（不改代码）

- **修改范围**：仅新增 `docs/dlss-integration/milestone-4-a-nvof-audit.md`。
- **内容**：
  1. 审计 `LicenseAgreement.pdf`（对照 DLSS §4e，结论写入文档；不通过则 M4 停止）；
  2. 核对 API 表面（上表逐条对头文件复核，标记 5.0.7 与文档不一致处）；
  3. 运行时可用性判定（System32 `nvofapi64.dll` 的加载失败分类）；
  4. R10G10B10A2 → ABGR8 转换 pass 设计（PS 计算、复用 Presenter 命令列表、资源状态）；
  5. 帧池设计（4–6 帧滚动，ABGR8 双输入 + flow/cost 输出）；
  6. grid 选择（1/2/4 的画质/性能权衡，默认值）；
  7. perfLevel / cost / fence 设计；
  8. `IOpticalFlowModule` 接口草案（对照 `dlss_super_sampling_backend` 的状态机风格）；
  9. MV 注入点设计（对接 E 节的 TODO 钩子）。
- **验收条件**：文档含上述 9 项结论，全部标注证据标签；用户批准。
- **停止点**：设计稿交付即停，等待批准（K4.4）。

### F.2 M4-B：NVOF 后端实现（编译期集成、默认关闭）

- **修改范围**：新增 `src/xenia/ui/nvof/`（`nvof_session.{h,cc}`、`nvof_backend.{h,cc}`、`nvof_densify.hlsl`、`CMakeLists.txt`）；修改 `ui/CMakeLists.txt`、`guest_output_enhancement.{h,cc}`（仅接线已存在的 `optical_flow_enabled`）。**许可隔离：`nvOpticalFlow*` 与 `LoadLibrary` 只出现在 `nvof/*`**。
- **实现要点**：
  1. 会话生命周期按 F.0 的 API 流程，拆毁顺序 unregisterAll → 释放纹理 → destroy → FreeLibrary；
  2. 输入帧池（4–6 帧）+ ABGR8 转换 pass（消费 R10G10B10A2 终帧，输出 B8G8R8A8）；
  3. 输出稠密化 shader（/32 定点、最近邻、confidence 门控、negate）；
  4. fence 同步：输入就绪 signal inFence → `NvOFExecuteD3D12` → outFence → 消费方 `Wait`，无 CPU 等待；
  5. 失败处理：caps 不符 fail-closed；执行失败 → 确定性回退 kOriginalPath，**绝不把失败标记为 kSuccess**（红线 2）；`kUnavailable` 缓存防每帧重试（M3 经验 D.2.6）；
  6. 主机侧测试：`guest_output_enhancement_test` 新增 NVOF 用例（配置默认关、开关语义、回退路径），不破坏现有 45 断言。
- **验收条件**：`.\xb build --config release` + 测试全绿；静态确认许可隔离；默认配置下行为与 M3 完全一致（无感）。
- **停止点**：编译期集成完成即停（本地无驱动，执行类全部 UNVERIFIED），等待用户审查后进 M4-C。

### F.3 M4-C：接线 SR 后端（替换零填充 MV）

- **修改范围**：`dlss_super_sampling_backend.{h,cc}`（把零填充 `motion_vectors_texture_` 换成 NVOF 稠密 MV 的 SRV/绑定）、`d3d12_presenter.cc`（按 `IsOpticalFlowRequested()` 驱动 NVOF 会话）。**前置条件：SR 与 NVOF 各自已独立验证**；若 NVOF 未过 M4-D，SR 必须保持零 MV 回退。
- **验收条件**：NVOF 关 = 行为与 M3 逐字节一致；NVOF 开 = SR 输入为稠密 MV；任一失败 = 确定性回退并记录失败分类。
- **停止点**：接线完成即停，进 M4-D。

### F.4 M4-D：RTX 4090 真实硬件验证

- **修改范围**：仅诊断日志（免批准轮次，M3 经验 D.2.1）。
- **验证项**：`nvofapi64.dll` 加载、`NvOFGetCaps` 值、`NvOFInit` 成功、`NvOFExecuteD3D12` 连续多帧、拆毁顺序（无段错误）、无 device loss。
- **验收条件**：测试机日志逐条对上；用户确认。
- **停止点**：验证报告交付即停。

### F.5 M4-E：画质与性能评估（兼 C3 调查）

- **内容**：SR 开/关真实 MV 的 A/B 对比（兼做悬置的 C3 画质调查：深度、色调映射、曝光）；perfLevel SLOW/MEDIUM/FAST 与 grid 1/2/4 的预设扫描；帧时间、VRAM 占用。
- **验收条件**：给出可复测的评估数据与结论；更新 J 节状态清单。
- **停止点**：评估报告交付即停。

---

## G. 环境限制

- 开发环境为 Hyper-V，**无 NVIDIA 驱动、无真实 GPU**：NVOF 加载、`NvOFExecuteD3D12` 执行、稠密化 shader 运行、fence 实际行为、DLSS 画质，全部 `UNVERIFIED`，直到 RTX 4090 测试机验证。
- 本地可执行：源码检查、SDK 头文件/文档/样本分析、架构设计、编译期集成、输入验证、基于 Mock 的主机侧测试、静态分析。
- 测试机：物理 RTX 4090（0x10DE:0x2684），驱动 617.42，Win 10.0.26100.9549；DLSS SDK 310.9.1（`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1`）、NVOF SDK 5.0.7（`C:\SDKs\NVIDIA\Optical_Flow_SDK_5.0.7`）。

## H. 剩余风险

1. **NVOF 许可证**：`LicenseAgreement.pdf` 未审计，条款可能不同于 DLSS §4e——M4-A 第一道闸门。
2. **R10G10B10A2 → ABGR8 转换的画质损失**：10bit→8bit 截断是否可接受，M4-A 需给出量化论证。
3. **驱动自带 `nvofapi64.dll` 的版本漂移**：不同驱动版本的 NVOF 行为可能不同，需按驱动版本记录验证结果。
4. **同步**：fence 链路与 Xenia 现有队列/Fence 依赖的耦合，任何失败必须回到 kOriginalPath（M3 的 device-removed 0x887A0006 先例警示：生命周期与 fence 错误都会炸设备）。
5. **C3 画质悬置**：SR 当前画质未评估，M4-E 一并处理。
6. **ABI**：NVOF 5.0.7 与未来驱动自带版本的二进制兼容性未验证。

## I. 下一步（最小且安全）

执行 **M4-A**：只读审计 + 设计稿 `milestone-4-a-nvof-audit.md`，交付后停止，等待用户批准。零代码风险。

## J. 各项能力状态清单

| 能力 | 状态 | 备注 |
|---|---|---|
| DLSS SR | **运行时已验证** | RTX 4090，3×3 上采样；**画质未评估**（C3 悬置） |
| NVOF | 未实现 | M4-B 起实施；配置钩子已就位 |
| 帧生成 | 未实现 | `DlssSrFrameParams` 全 0，属后续里程碑 |
| 实验性神经渲染 | 仅研究 | 无代码 |
| Presenter 集成 | 已接线且开启 | 测试配置 `dlss_enabled=true`、`dlss_sr_enabled=true`；回退已验证 |
| 最终画质与性能 | 尚未评估 | M4-E 处理 |

---

## 附录 A：`4046a47..15265e0` 提交清单（14）

```
15265e0 ui: DLSS SR quality preset UI and backend config/robustness (M3-B)
6129fea ui: log D3D12 device-removed reason HRESULT in BeginSubmission (M3-B)
59d4fa9 ui: fix DLSS SR first-frame device loss from upload-buffer lifetime (M3-B)
d8b4c2c ui: fix DLSS SR device loss from depth/MV texture state (M3-B)
80e875e Merge verify/dlss5-integration-sr-fix: DLSS SR adapter fix (M3-B)
34f4803 ui: revert adapter retention; enumerate adapter by LUID in DLSS backend (M3-B)
9848b09 temp commit
1dabfbb ui: log DLSS early Initialize failures (M3-B)
c6543c6 ui: log DLSS input-contract failures (M3-B)
437cb2a ui: fix DLSS log format specifiers to fmt style (M3-B)
cc77abb ui: add DLSS backend/presenter logging (M3-B)
5169e95 ui: add DLSS UI switch, FSR/CAS-exclusive radio (M3-B Phase 1+2)
e8d7585 docs(dlss): add DLSS UI switch design (M3-B Phase 1)
4dda197 ui: add DLSS SR backend and presenter wiring (M3-B)
```
