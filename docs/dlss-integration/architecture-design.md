# Milestone 1 — RTX 4090 优先的 NVIDIA 图形增强架构设计

> 状态：架构设计稿（DESIGN），**用户已正式批准**（2026-10-20，决策 1–7，见 §L3）。
> 本阶段**仅做只读分析、证据核查、架构设计、文档编写**。未实现任何代码、未构建、未加载 DLL、未执行 GPU 操作、未创建 Mock。
> 证据标签约定：`STATIC_VERIFIED`（当前源码已核实）｜`SDK-DOC-SUPPORTED`（SDK 文档/头文件支持）｜`DESIGN PROPOSAL`（本设计建议）｜`UNVERIFIED`（未验证）｜`RUNTIME_VERIFIED_ON_RTX4090`（仅在真实 RTX 4090 证据存在时使用——本阶段**无任何**项可用）。

---

## A. 总结与设计状态

### A1. 设计结论速览

- **首要目标硬件**：NVIDIA GeForce RTX 4090（Ada，`AD102`）。`STATIC_VERIFIED`（硬件型号由测试机约定给定）。
- **首个可运行版本**：DLSS SR（`SuperSampling`，特征枚举 =1）作为唯一主线后端，采用**方案 A**（直接复用现有 guest 输出纹理 + 一次资源状态迁移）。`SDK-DOC-SUPPORTED` + `DESIGN PROPOSAL`。
- **默认关闭语义**：通过**结构设计**保证（非仅文档约定）。总开关关闭 → 不加载 DLL、不初始化后端、不创建后端资源、不执行额外 GPU 步骤、不改变现有资源同步与 Presenter 生命周期。`DESIGN PROPOSAL`。
- **唯一回退路径**：原始效果链 `effect_count ≥ 1` 且最终效果 `kBilinear`。**严禁**使用 `effect_count = 0` 作为回退（会丢失 guest 输出并清屏为黑）。`STATIC_VERIFIED`。
- **后端结果契约**：新增独立枚举，与 `PaintResult` 正交。后端成功 ≠ 最终呈现成功。`DESIGN PROPOSAL`。
- **NVOF**：独立模块设计，**不**假定它是唯一运动矢量来源。`DESIGN PROPOSAL`。
- **帧生成 / Ray Reconstruction / 实验性神经渲染**：仅定义扩展边界与隔离策略，**不**成为首个 SR 版本的强制依赖。`DESIGN PROPOSAL`。

### A2. 本阶段证据等级总览

| 标签 | 本阶段使用情况 |
|---|---|
| `STATIC_VERIFIED` | 全部源码结论（Presenter 流程、guest 输出资源、状态、UAV、效果链门控、邮箱/Fence/paint-refs 契约、回退路径）。已对当前源码逐条复核。 |
| `SDK-DOC-SUPPORTED` | SDK 头文件与官方文档的输入契约、生命周期、格式、颜色空间、错误码、许可条款。已核对 310.9.1 头文件与 310.6.0/310.7.0/v08 文档。 |
| `DESIGN PROPOSAL` | 模块边界、配置结构、后端结果契约、回退策略、架构方案选择。 |
| `UNVERIFIED` | 一切真实 GPU 执行、NVOF 执行、DLSS `Evaluate`、输出画质、延迟、帧时间、显存占用、状态迁移在真实 GPU 上的正确性。 |
| `RUNTIME_VERIFIED_ON_RTX4090` | **本阶段无任何项**。真实 RTX 4090 测试机未接入（当前为 Hyper-V，无 NVIDIA 驱动/GPU）。 |

### A3. 设计状态声明

- 本阶段**未**执行构建，**未**加载 NVIDIA DLL，**未**执行 DLSS `Evaluate` 或 NVOF 执行，**未**做性能测试。
- 本阶段**未**将 SDK 文件存在等同于运行时可用。
- 本阶段**未**将静态审计等同于编译通过或运行时验证。
- 本阶段**未**将运动矢量缺失推导为所有 DLSS SR 模式绝对不可用。
- 本阶段**未**将“DLSS 5”项目目标名称理解为当前 SDK 已提供全部对应功能。

---

## B. 当前源码架构及证据

> 本节所有行号基于当前 HEAD `8a7afe3c`（`dev/dlss5-integration` 分支，干净工作树）复核。旧审计报告行号仅作检索起点，已按当前源码重新确认。

### B1. guest output 进入 Presenter 的完整链路

GPU 线程 → Presenter 的入口是 `D3D12CommandProcessor::IssueSwap`：

- `IssueSwap`（`src/xenia/gpu/d3d12/d3d12_command_processor.cc:2250`）在客机 swap 时被调用。`STATIC_VERIFIED`
- 调用 `texture_cache_->RequestSwapTexture(swap_texture_srv_desc, frontbuffer_format)`（:2270）获取客机最终 resolved 输出纹理 `swap_texture_resource`（客机原生分辨率）。`STATIC_VERIFIED`
- 调用 `presenter->RefreshGuestOutput(w, h, aspect, callback)`（:2279）。回调内对 guest 输出资源执行 **FXAA + gamma ramp** compute。`STATIC_VERIFIED`
- 回调内：`context.resource_uav_capable()` 取得 guest 输出资源（初始状态 `kGuestOutputInternalState`），`PushTransitionBarrier(..., D3D12_RESOURCE_STATE_UNORDERED_ACCESS)` 迁移为 UAV，创建 UAV 写入 gamma 结果，再迁回 `kGuestOutputInternalState`。`STATIC_VERIFIED`（`d3d12_command_processor.cc:2425-2450` 附近）

**契约（`d3d12_presenter.h:77-79`）**：guest 输出回调初始状态为 `kGuestOutputInternalState`，回调**必须**在使用后迁回该状态。`STATIC_VERIFIED`

### B2. guest 输出资源定义（`D3D12GuestOutputRefreshContext`）

| 项 | 值 | 位置 | 标签 |
|---|---|---|---|
| `kGuestOutputFormat` | `R10G10B10A2_UNORM` | `d3d12_presenter.h:58-59` | `STATIC_VERIFIED` |
| `kGuestOutputInternalState` | `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` | `d3d12_presenter.h:60-61` | `STATIC_VERIFIED` |
| `kGuestOutputIntermediateFormat` | `R10G10B10A2_UNORM` | `d3d12_presenter.h:63-64` | `STATIC_VERIFIED` |
| `kSwapChainFormat` | `B8G8R8A8_UNORM` | `d3d12_presenter.h:67` | `STATIC_VERIFIED` |
| UAV 能力 | 创建时 `Flags = ALLOW_UNORDERED_ACCESS`，`Layout = UNKNOWN` | `d3d12_presenter.cc:420-427` | `STATIC_VERIFIED` |
| 尺寸 | `frontbuffer_width × frontbuffer_height`，MipLevels=1，SampleCount=1 | `d3d12_presenter.cc:413-418` | `STATIC_VERIFIED` |

### B3. 效果链定义与门控

- `GuestOutputPaintConfig`（`presenter.h:~196`）是 **UI 呈现预设**（`kBilinear` / `kCas` / `kFsr`），与 SDK 后端门控**正交**（审计 0.1 建议）。字段：`allow_overscan_cutoff_`、`effect_`（默认 `kBilinear`）、`cas_additional_sharpness_`、`fsr_max_upsampling_passes_`（默认/上限 4）、`fsr_sharpness_reduction_`、`dither_`。`STATIC_VERIFIED`
- `kFsrMaxUpscalingPassesMax = 4`（`presenter.h:219`）。`STATIC_VERIFIED`
- `kMaxGuestOutputPaintEffects = kFsrMaxUpscalingPassesMax + 2 = 6`（`presenter.h:436-437`）。`STATIC_VERIFIED`
- `GuestOutputPaintEffect`（`presenter.h:~389`）：`kBilinear, kBilinearDither, kCasSharpen, kCasSharpenDither, kCasResample, kCasResampleDither, kFsrEasu, kFsrRcas, kFsrRcasDither`。`STATIC_VERIFIED`
  - `CanGuestOutputPaintEffectBeIntermediate()`（`presenter.h:403`）：所有 `*Dither` 变体 + `kBilinear` 为 `false`。
  - `CanGuestOutputPaintEffectBeFinal()`（`presenter.h:421`）：仅 `kFsrEasu` 为 `false`。
- `GuestOutputPaintFlow`（`presenter.h:~439`）：`properties`（`IsActive()` 要求全非零）、`effect_count`（**0 = 不显示 guest 输出**）、`effects`、`effect_output_sizes`、`output_x/y`、`letterbox_clear_rectangle_count`、`letterbox_clear_rectangles`。`STATIC_VERIFIED`

### B4. `GetGuestOutputPaintFlow`（效果链构造，`presenter.cc:~651`）

- `const` 方法：`(properties, host_rt_w, host_rt_h, max_rt_w, max_rt_h, config)`。`STATIC_VERIFIED`
- 初始化 `flow.letterbox_clear_rectangle_count = 1`（全屏清矩形，即“画黑”默认）。
- **早退（`effect_count` 保持 0）**条件：`!properties.IsActive() || !host_rt_w || !host_rt_h || !surface_width_in_paint_connection_ || !surface_height_in_paint_connection_` → **黑屏**。`STATIC_VERIFIED`
- 若 `config.GetEffect() == kFsr` 且需上采样：输出若干 `kFsrEasu`（逐 pass 倍增尺寸，至 `fsr_max_upsampling_passes`）+ 最终 `kFsrRcas`。若 `kCas`：一个 `kCasSharpen`/`kCasResample`。
- **若仍需 bilinear 拉伸**：追加 `kBilinear`（尺寸钳制）。因此**原始路径恒为 `effect_count ≥ 1` 且以 `kBilinear` 结尾**（`kBilinear` 配置下即单个 `kBilinear`）。`STATIC_VERIFIED`
- 效果构造后 `assert_not_zero(flow.effect_count)`。`STATIC_VERIFIED`
- **dither 仅施加于最终效果**（switch 最后效果，映射到 `*Dither` 变体；`kBilinearDither` 在 1:1 8bpc 拷贝时跳过）。
- **仅当 guest 输出 inactive/invalid 时返回 `effect_count = 0`（黑）**——这正是审计标记的“错误的后端回退”用法。确认：`effect_count = 0` 会清空后缓冲为黑、丢弃 guest 图像。`STATIC_VERIFIED`

### B5. `ConsumeGuestOutput`（邮箱消费，`presenter.cc:~596`）

- 签名：`(uint32_t& mailbox_index_or_max_if_inactive_out, GuestOutputProperties* properties_out, GuestOutputPaintConfig* paint_config_out) -> std::unique_lock<std::mutex>`。`STATIC_VERIFIED`
- 在 `guest_output_paint_config_mutex_` 下拷贝 paint config（UI 写、guest 读）。
- **邮箱**：`kGuestOutputMailboxSize = 3`（`presenter.h:360`）无锁 SPSC。`guest_output_mailbox_acquired_and_ready_` 为打包原子（acquired 在 bits 0:1，ready 在 bits 2:3）。生产者（guest refresh）发布 `writable`，消费者获取 `ready`。CAS 循环 `memory_order_acq_rel`。返回 `guest_output_mailbox_consumer_mutex_` 锁；inactive 时返回 `UINT32_MAX` 索引。`STATIC_VERIFIED`

### B6. `PaintAndPresentImpl`（呈现路径，`d3d12_presenter.cc:~463`）

- `current_paint_submission = paint_completion_timeline->GetUpcomingSubmission()`。等待 `command_allocator_count` 个 pending 提交，选空闲 command allocator。`STATIC_VERIFIED`
- 取得后缓冲索引 + RTV；`kBackBufferClearColor = {0,0,0,1}`（黑）。`back_buffer_clear_needed = true`。`STATIC_VERIFIED`
- `ConsumeGuestOutput` → guest 输出资源（ComPtr）。**若资源非空**：
  - `GetGuestOutputPaintFlow(...)`。
  - **回退检查**：若 `effect_count > 0` 但**最终管线为空** → `effect_count = 0`（:530）；若**任一中间管线为空** → `effect_count = 0`（:535）。（这是**既有的** `effect_count = 0` 回退。）`STATIC_VERIFIED`
  - 若 `effect_count > 0`：查找/创建 **paint ref**（匹配纹理，或空位，或经 `paint_completion_timeline->AwaitSubmissionAndUpdateCompleted` 替换最早者）；创建 SRV（格式 `kGuestOutputFormat`）入 view heap。
  - 确保中间纹理按所需尺寸存在（创建 `ALLOW_RENDER_TARGET`，状态 `PIXEL_SHADER_RESOURCE`，建 SRV+RTV）。**创建失败 → `effect_count = 0, continue`**（:671）。`STATIC_VERIFIED`
  - **效果循环**（`for i in 0..effect_count`）：最终效果画到后缓冲（首次使用屏障 PRESENT→RENDER_TARGET，`OMSetRenderTargets`）；中间效果画到中间纹理（首次使用屏障 SRV→RTV，`DiscardResource`）。源 view = 前一中间纹理 SRV 或 guest 输出 SRV。`DrawInstanced(4,1,0,0)`。最终效果若有 letterbox 矩形则清。每个非最终效果后，对刚写入的中间纹理做 RTV→SRV 屏障。`STATIC_VERIFIED`
- 效果后：若 `back_buffer_clear_needed` 仍为 `true`（即 `effect_count` 为 0 → 未画 guest 输出）→ 清整个后缓冲为黑。随后 RTV→PRESENT 屏障、Close、ExecuteCommandLists、signal ui + paint timeline、`Present(0, DXGI_PRESENT_RESTART | tearing)`。`STATIC_VERIFIED`
- 映射 `DXGI_ERROR_DEVICE_REMOVED → kGpuLostExternally`，`DXGI_ERROR_DEVICE_RESET → kGpuLostResponsible`，其余 `kPresented/kNotPresented`。`STATIC_VERIFIED`

### B7. `InitializeSurfaceIndependent`（资源创建，`d3d12_presenter.cc`）

- 每效果创建 **root signature**（kSource 描述符表 + kRectangle 顶点常量 + kEffectConstants 像素常量；仅 bilinear/EASU 有静态 bilinear sampler）与 **PSO**（中间 RTV = `kGuestOutputIntermediateFormat`，最终 RTV = `kSwapChainFormat`）。共 `kCount` 效果。`STATIC_VERIFIED`
- 创建 paint + present 完成 timeline、3 个 paint command allocator + 1 个 command list、RTV heap（`kRTVCount`）、CBV/SRV/UAV heap（`kViewCount`）、refresher + ui timeline。`STATIC_VERIFIED`
- 从 IDXGIFactory5 查询 `dxgi_supports_tearing_`（tearing / VRR）。`STATIC_VERIFIED`

### B8. `RefreshGuestOutputImpl`（`d3d12_presenter.cc:~386`）

- 尺寸变化时：`guest_output_resource_refresher_completion_timeline_->AwaitSubmissionAndUpdateCompleted(...)` 后重置 ref。`STATIC_VERIFIED`
- 创建 guest 输出纹理：`frontbuffer_width×height`、`R10G10B10A2_UNORM`、MipLevels=1、SampleCount=1、`Layout=UNKNOWN`、**`Flags=ALLOW_UNORDERED_ACCESS`**、初始状态 `kGuestOutputInternalState`。`STATIC_VERIFIED`
- 调 `refresher(context)`，随后记录 `guest_output_resource_ref.first = refresher_timeline->GetUpcomingSubmission()` 并 `SignalAndAdvance(direct_queue)`。`STATIC_VERIFIED`

### B9. 关键常量与所有权（`d3d12_presenter.h`）

- `kGuestOutputMailboxSize = 3`。`kMaxGuestOutputPaintEffects = 6`。`STATIC_VERIFIED`
- `PaintContext`：`kSwapChainBufferCount = 3`。RTVIndex：3 个后缓冲后 `kGuestOutputIntermediate0`(=3)，`kRTVCount = kRTVIndexGuestOutputIntermediate0 + kGuestOutputMailboxSize - 1 = 5`。ViewIndex：`kViewIndexGuestOutput0Srv`（3 个 guest 输出 SRV）后 `kViewIndexGuestOutputIntermediate0Srv`(=3)，`kViewCount = 3 + 6 - 1 = 8`。`STATIC_VERIFIED`
- **Timeline**：`paint_completion_timeline`（呈现前 signal）、`present_completion_timeline`（呈现后），成员 `guest_output_resource_refresher_completion_timeline_`、`ui_completion_timeline_`。`STATIC_VERIFIED`
- **`guest_output_resource_paint_refs`** = `std::array<std::pair<uint64_t, ComPtr<ID3D12Resource>>, kGuestOutputMailboxSize>`：paint 提交索引 + 纹理引用（可为空）。“最旧的引用（如需）被替换，等待最后一次 paint 使用完成。”**审计结论：不得接管。**`STATIC_VERIFIED`
- **`guest_output_resources_`** = 同型数组（mailbox 索引 → refresher 提交索引 + 纹理）。受 refresher timeline（写）与 paint timeline（读）双重保护。`STATIC_VERIFIED`
- `guest_output_intermediate_textures` = `std::array<ComPtr<ID3D12Resource>, kMaxGuestOutputPaintEffects - 1>`（=5）。空闲状态 = `PIXEL_SHADER_RESOURCE`。`AwaitSwapChainUsageCompletion()` 等待 present + paint timeline。`STATIC_VERIFIED`

### B10. 线程边界与锁序（`presenter.h` 私有）

- `PaintMode`：`kNone` / `kUIThreadOnRequest` / `kGuestOutputThreadImmediately`。无 UI drawer 且无隐式 vsync 且 `host_present_from_non_ui_thread` cvar 时，guest 输出可**直接呈现**（绕过 UI）。`STATIC_VERIFIED`
- `paint_mode_mutex_`（guest 输出线程呈现时持有）、`guest_output_paint_config_mutex_`、`guest_output_mailbox_consumer_mutex_`。**锁序**：guest 线程消费时**先持** consumer mutex **再** paint_mode mutex_；UI 线程反之。`STATIC_VERIFIED`
- `PaintResult` 枚举（`presenter.h:~340`）：`kPresented, kPresentedSuboptimal, kNotPresented, kNotPresentedConnectionOutdated, kGpuLostExternally, kGpuLostResponsible`。**仅表达呈现结果，不表达后端处理**——审计 0.1 确认需独立后端结果类型。`STATIC_VERIFIED`

### B11. 对 10 问的源码级回答（任务 A）

1. **配置如何创建、传递、消费、更新？** UI 线程经 `guest_output_paint_config_mutex_` 写 `GuestOutputPaintConfig`（`presenter.cc:~596` 消费）；guest 输出线程读。`STATIC_VERIFIED`
2. **guest 输出如何从模拟器进入 Presenter？** 客机 swap → `IssueSwap` → `RequestSwapTexture` → `RefreshGuestOutput` 回调写 guest 输出资源（FXAA+gamma）。`STATIC_VERIFIED`
3. **效果链如何选择、创建、执行？** `GetGuestOutputPaintFlow`（`presenter.cc:651`）依 `config.GetEffect()` 构造链；`PaintAndPresentImpl`（`d3d12_presenter.cc:463`）按链执行。`STATIC_VERIFIED`
4. **UI 线程与 guest 输出线程分别如何调用 Presenter？** UI 线程经 `kUIThreadOnRequest`；guest 线程经 `kGuestOutputThreadImmediately`（满足条件时）。`STATIC_VERIFIED`
5. **哪些操作必须遵守特定线程/队列约束？** 资源状态转换须在拥有该资源状态的队列上下文；paint 在 direct queue；mailbox 消费锁序固定。`STATIC_VERIFIED`
6. **谁拥有 guest 输出、中间纹理、swapchain？** guest 输出与中间纹理由 `D3D12Presenter` 拥有（`guest_output_resources_` / `guest_output_intermediate_textures`）；swapchain 由 DXGI。`STATIC_VERIFIED`
7. **哪些资源可由新后端安全创建，哪些只能借用？** 新后端可**创建自己的输出/伴生纹理**（需 UAV）；**只能借用** guest 输出资源（读，SRV），不得接管 `guest_output_resource_paint_refs`。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
8. **邮箱/Fence/引用/生命周期的不可破坏约束？** `kGuestOutputMailboxSize=3` 生命周期、`paint_completion_timeline` 等待、`guest_output_resource_paint_refs` 所有权、refresher/paint 双 timeline 保护。`STATIC_VERIFIED`
9. **新后端失败时，在哪一层决定回退最合理？** 在 `PaintAndPresentImpl` 构造效果链**之前**决定：后端失败 → 构造原始效果链（`effect_count ≥ 1` + `kBilinear`）。`DESIGN PROPOSAL`
10. **哪些职责属通用 Presenter，哪些仅 D3D12？** 邮箱、配置、`PaintResult`、效果链抽象属通用 Presenter；资源状态转换、timeline、UAV、格式属 D3D12 后端。`STATIC_VERIFIED` + `DESIGN PROPOSAL`

---

## C. RTX 4090 功能兼容性矩阵

> 每项功能**独立**核查四个能力轴：① GPU 硬件 ② 驱动/OS ③ 当前安装的 SDK/运行时 ④ Xenia 集成可行性。**不得**将四者混为一个“支持”结论。
> 硬件状态取值：`RTX4090_SUPPORTED` / `RTX4090_SUPPORTED_WITH_CONDITIONS` / `RTX4090_UNSUPPORTED` / `SDK_OR_DRIVER_UNVERIFIED` / `XENIA_INTEGRATION_UNVERIFIED`。运行状态单独取 `RUNTIME_UNVERIFIED` / `RUNTIME_VERIFIED_ON_RTX4090`。

### C1. 兼容性矩阵

| 功能 | RTX4090 硬件支持证据 | SDK / 驱动条件 | Xenia 集成难度 | 优先级 | 当前验证状态 |
|---|---|---|---|---|---|
| **DLSS Super Resolution** | 所有 RTX（含 4090）支持 SR。`SDK-DOC-SUPPORTED`（RTX UI 指南 p.5：SR 可用所有 RTX GPU） | SDK 310.9.1 头文件 + `nvngx_dlss.dll` 310.9.1.0（官方签名，`experimental=false`）。`SDK-VERIFIED`（运行时 manifest） | 中——有最终帧+深度；缺 MV/jitter/曝光（可生成/估算）。`STATIC_VERIFIED` + `UNVERIFIED` | **P0** | `RUNTIME_UNVERIFIED`（Hyper-V 无 GPU） |
| **NVIDIA Optical Flow** | Turing+ 支持；4090（Ada）支持。`SDK-DOC-SUPPORTED`（NVOF 编程指南） | 需 `nvofapi64.dll`（**驱动安装提供，本环境缺失**）。`SDK-VERIFIED`（环境审计） | 中——有最终帧序列；需转 NV12/ABGR8 格式。`STATIC_VERIFIED` + `UNVERIFIED` | P1 | `RUNTIME_UNVERIFIED`（缺 `nvofapi64.dll`） |
| **DLSS Frame Generation** | RTX 40 系列及以上支持 2x；4090 支持 2x。`SDK-DOC-SUPPORTED`（FG 指南 + RTX UI 指南 p.4） | 需 Reflex + HAGS；driver 520+；`nvngx_dlssg.dll` 310.9.1.0（官方，`experimental=true`）。`SDK-VERIFIED` | 高——缺相机矩阵×5、MV、UI/HUD 分离、呈现时序改造。`STATIC_VERIFIED` + `UNVERIFIED` | P2 | `RUNTIME_UNVERIFIED`（缺相机矩阵） |
| **Ray Reconstruction** | 所有 RTX（含 4090）支持，需 580+ 驱动 + DLSS 4.5+。`SDK-DOC-SUPPORTED`（RR 指南） | `nvngx_dlssd.dll` 310.9.1.0（官方，`experimental=true`）。`SDK-VERIFIED` | 极低/不适用——需全部 RT G-Buffer 信号，Xenia 非 RT 渲染**全无**。`STATIC_VERIFIED` + `UNVERIFIED` | 暂不纳入主线（研究项） | `RUNTIME_UNVERIFIED`（缺 RT 信号） |
| **实验性神经渲染（NR）** | 具体能力**待定义**；本地 NR 运行时为**社区构建**（见下）。`UNVERIFIED` | `nvngx_dlssnr.dll` 310.8.3.0 = 社区 Lecram 构建，**Authenticode HashMismatch**，`experimental=true`；另有 310.8.0.0 为“用户批准的 NVIDIA 原始 RTX50 实验运行时”（Valid）。`SDK-VERIFIED`（运行时 manifest） | 未定义——无 Xenia 侧契约。`UNVERIFIED` | 暂不纳入主线（研究项） | `RUNTIME_UNVERIFIED`（非官方签名运行时） |

### C2. 关键区分与证据说明

**DLSS SR（P0）**
- 硬件：`SDK-DOC-SUPPORTED`（RTX UI 指南明确 SR 可用所有 RTX GPU）。
- SDK/驱动：310.9.1 头文件 `NVSDK_NGX_PerfQuality_Value`（`nvsdk_ngx_defs.h:251-260`）提供 `MaxPerf, Balanced, MaxQuality, UltraPerformance, UltraQuality, DLAA`。**注意：枚举中无 `Off`**——“关闭”由 Xenia 侧配置控制，而非 SDK 枚举。`SDK-VERIFIED`
- 输入契约：输入色彩必需（LDR 感知线性，`IsHDR=0`）、深度可选、MV 条件必需、jitter 逐帧必需、曝光条件。`SDK-DOC-SUPPORTED`（SR 指南 §3/§4）。
- Xenia 现状：色彩（最终帧）可用、深度可用、显示分辨率可用、帧时序可用；**缺 MV（条件必需）、jitter（可生成）、曝光（可估算）**。`STATIC_VERIFIED` + `UNVERIFIED`
- 运行状态：`RUNTIME_UNVERIFIED`（Hyper-V 无 GPU，无法执行 `Evaluate`）。

**NVOF（P1，独立模块）**
- 硬件：Turing+，4090 支持。`SDK-DOC-SUPPORTED`
- SDK/驱动：**需 `nvofapi64.dll`（驱动安装提供，本环境无）**。`SDK-VERIFIED`（环境审计）。输入格式 `NV12`/`ABGR8`/`GRAYSCALE8`，输出 `int16` S10.5 `SHORT2`。
- 关键：硬件支持 ≠ 驱动运行时存在 ≠ Xenia 可用性，三者为**独立验证项**。本环境三者均未验证。`UNVERIFIED`
- 不得假定 DLSS SR 的硬件支持自动证明 NVOF 运行时依赖可用。`DESIGN PROPOSAL`

**帧生成（P2）**
- 硬件：RTX 40+ 支持 2x；4090 支持 2x。`SDK-DOC-SUPPORTED`
- 依赖：**Reflex + HAGS**（必须）、相机矩阵×5、相机位置向量、深度、MV、UI/HUD 分离、呈现时序改造。**几乎全部缺失**。`STATIC_VERIFIED` + `UNVERIFIED`
- 不能阻塞首个 SR 版本，不能因未来可能需要而提前改造无关渲染路径。`DESIGN PROPOSAL`

**Ray Reconstruction（暂不纳入主线）**
- 硬件：所有 RTX，需 580+ 驱动。`SDK-DOC-SUPPORTED`
- 依赖：RT 专用 G-Buffer 信号（albedo/法线/粗糙度/命中距离/射线方向/世界→视图/视图→裁剪矩阵），Xenia 非 RT 渲染**全无**。`STATIC_VERIFIED` + `UNVERIFIED`
- RR 启用时**覆盖** DLSS-SR（SR 不再执行），二者互斥。`SDK-DOC-SUPPORTED`（RR 指南 p.13）
- 对 Xenia 基本不适用，仅作独立未来研究项。`DESIGN PROPOSAL`

**实验性神经渲染（NR，暂不纳入主线）**
- 关键发现：本地 NR 运行时 `nvngx_dlssnr.dll` 310.8.3.0 为**社区 Lecram 构建**，Authenticode **HashMismatch**（非 NVIDIA 官方签名），`experimental=true`。另一份 310.8.0.0 为“用户批准的 NVIDIA 原始 RTX50 实验运行时”（Valid）。`SDK-VERIFIED`（运行时 manifest `release-runtime-manifest.json`）
- 含义：NR 非官方已验证能力，且本地运行时非官方签名。不得将“4090 支持某 NVIDIA 技术”推断为 Xenia 已具备集成所需数据。`DESIGN PROPOSAL` + `UNVERIFIED`
- 仅作为独立研究项，不成为主线强制依赖。`DESIGN PROPOSAL`

### C3. 首个可运行版本范围

- **主线**：DLSS SR（方案 A）。最小、可靠、可回退。`DESIGN PROPOSAL`
- **独立模块**：NVOF 作为独立 MV 来源设计，**不**假定唯一。`DESIGN PROPOSAL`
- **暂缓**：帧生成、Ray Reconstruction、实验性神经渲染——不阻塞首个 SR 版本。`DESIGN PROPOSAL`
- 不得将“DLSS 5”目标名理解为当前 SDK 已提供全部对应功能。`DESIGN PROPOSAL`

---

## D. 推荐架构、备选方案及选择理由

### D1. 后端选择与协调层：两种方案比较

**方案 1：直接后端接口（无独立协调器）**
- `D3D12Presenter` 直接持有**可选**的 `IDLSSSuperSamplingBackend*`（惰性创建，默认 `nullptr`）。
- 单一方法 `TryProcess(guest_output, out_params) -> BackendResult`。
- 回退逻辑内联在 `PaintAndPresentImpl` 的呈现路径中：后端失败 → 构造原始效果链。
- 优点：无额外管理器、无全局单例、无重复状态；默认关闭**结构性**成立（`nullptr` 后端 = 零开销）；测试简单（直接注入后端/空指针）。
- 缺点：协调逻辑（能力检查、输入契约检查、路径选择、结果归类、回退决策）若全内联会使 Presenter 变重；未来多后端（SR+NVOF+FG）时内联逻辑膨胀。
- 测试难度：低——直接对后端接口与 Presenter 的回退分支单测。

**方案 2：独立协调器层**
- 新增 `GuestOutputEnhancementCoordinator`，拥有后端、做能力/容量检查、路径选择、结果归类、回退决策。
- `D3D12Presenter` 仅问协调器“处理还是原始”。
- 优点：协调逻辑集中、可独立测试；多后端时职责清晰。
- 缺点：引入一个管理层级；若后端只有 SR 一个，协调器成为**不必要的中间人**；有重复状态与全局单例风险（违反 AGENTS.md “不要为了模块化引入不必要管理器”）。
- 测试难度：中——需 mock 协调器与后端两层。

### D2. 推荐：方案 1（直接后端接口）+ 轻量纯函数路径选择

- **核心**：`D3D12Presenter` 持有可选 `IDLSSSuperSamplingBackend*`，惰性创建，默认 `nullptr`。`DESIGN PROPOSAL`
- **协调逻辑**：不引入重量级管理器，而是将“后端是否被请求 + 输入契约检查 + 路径选择 + 结果归类 + 回退决策”表达为**一个小型、纯、可测试的路径选择组件**（无状态或仅读配置），而非一个持有资源的管理器。`DESIGN PROPOSAL`
- **理由**：
  1. 默认关闭语义**结构性**成立——`nullptr` 后端时零开销，不加载 DLL、不创建资源、不执行 GPU 步骤。`DESIGN PROPOSAL`
  2. 避免全局单例与重复状态。`DESIGN PROPOSAL`
  3. 单后端（SR）时方案 2 的协调器是过度设计。`DESIGN PROPOSAL`
  4. 当 NVOF + FG 加入后，路径选择组件可**自然演进**为协调器（无需重写 Presenter）。`DESIGN PROPOSAL`

### D3. 模块边界（对应任务 C1–C5）

- **C1 Presenter 与现有效果链**：保持不变。`GuestOutputPaintConfig`（UI 预设）与 SDK 后端门控**正交**。主开关关闭时，`GetGuestOutputPaintFlow` 行为**完全不变**。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
- **C2 后端选择与协调层**：直接后端接口 + 轻量路径选择组件（D2）。`DESIGN PROPOSAL`
- **C3 DLSS SR 后端**：独立接口 `IDLSSSuperSamplingBackend`（见 E）。`DESIGN PROPOSAL`
- **C4 NVOF 模块**：独立接口 `IOpticalFlowModule`（见 E）。`DESIGN PROPOSAL`
- **C5 帧生成与实验性神经渲染**：仅定义扩展边界与隔离策略，**不**成为 SR 启动依赖。`DESIGN PROPOSAL`

---

## E. 模块职责、数据流与 Mermaid 流程图

### E1. 模块职责

**IDLSSSuperSamplingBackend（C3，独立接口）**
- 职责：SDK 初始化与可用性检查、输入契约验证、输入资源准备、后端执行、输出有效性与结果状态报告、清理自身拥有的资源。
- **不得**擅自接管现有 guest 输出资源所有权（仅读，SRV）。`DESIGN PROPOSAL` + `STATIC_VERIFIED`
- 拥有自己的输出纹理与（可选）伴生 MV/深度纹理。`DESIGN PROPOSAL`

**IOpticalFlowModule（C4，独立接口）**
- 职责：运动矢量生成及其生命周期。
- 与 DLSS SR 的关系：NVOF 输出 MV **可**作为 SR 的 MV 输入，但 SR **不**假定 NVOF 是唯一 MV 来源（SR 可无 MV 运行，质量受限；也可由未来自研/上游注入 MV）。`DESIGN PROPOSAL`
- 失败行为：NVOF 失败 → SR 回退为无 MV 模式或原始路径（取决于配置），**不**导致 SR 整体失败。`DESIGN PROPOSAL`
- 三个独立验证项：硬件支持（4090 Turing+）、驱动提供的运行时（`nvofapi64.dll`，本环境缺）、Xenia 实际可用性（帧序列 + 格式转换）。`UNVERIFIED` ×3

**轻量路径选择组件（C2）**
- 无状态或仅读配置；输入：总开关、SR 子开关、后端能力、输入契约、资源状态。输出：后端结果 + 最终输出路径（后端 / 原始）。`DESIGN PROPOSAL`

### E2. 数据流与回退（Mermaid）

```mermaid
flowchart TB
  subgraph "guest 输出线程"
    ISSUE["IssueSwap<br/>d3d12_command_processor.cc:2250"] --> REFRESH["RefreshGuestOutput 回调<br/>FXAA + gamma → guest_output_resource<br/>R10G10B10A2, 初始 PIXEL_SHADER_RESOURCE"]
  end

  subgraph "paint 线程 / PaintAndPresentImpl (d3d12_presenter.cc:463)"
    CONSUME["ConsumeGuestOutput<br/>presenter.cc:596<br/>取 guest 输出资源 (SRV, 借用)"] --> MASTER{总开关?<br/>SR 子开关?}
    MASTER -- "关 / 后端 nullptr" --> ORIG1["原始路径<br/>GetGuestOutputPaintFlow<br/>effect_count≥1, 以 kBilinear 结尾"]
    MASTER -- "开 + 后端已初始化" --> CONTRACT{输入契约满足?<br/>资源状态可迁移?}
    CONTRACT -- "否" --> ORIG2["原始路径<br/>(回退, 记录失败分类)"]
    CONTRACT -- "是" --> TRANS["guest 输出状态迁移<br/>PIXEL_SHADER_RESOURCE → NON_PIXEL_SHADER_RESOURCE"]
    TRANS --> EVAL["DLSS SR 后端 Execute<br/>输入: 色(SRV)/depth/MV/jitter/曝光<br/>输出: 重建色 UAV"]
    EVAL --> VALID{输出有效?<br/>结果==成功?}
    VALID -- "否 / 执行失败 / 输出无效" --> ORIG3["原始路径<br/>(回退, 记录失败分类)"]
    VALID -- "是" --> BUILD["构造效果链<br/>后端输出 → 中间纹理 → 后缓冲<br/>effect_count≥1, 以 kBilinear 结尾"]
  end

  ORIG1 --> PRESENT["PaintAndPresentImpl 呈现<br/>signal paint timeline → Present()"]
  ORIG2 --> PRESENT
  ORIG3 --> PRESENT
  BUILD --> PRESENT
  PRESENT --> RESULT["PaintResult<br/>(仅表达呈现结果)"]
  EVAL -.-> BRESULT["BackendResult<br/>(独立: 是否请求/成功/失败分类/回退)"]
  VALID -.-> BRESULT
```

### E3. 后端结果契约（独立于 PaintResult）

- `BackendResult` 与 `PaintResult` **正交**：后端成功 ≠ 最终呈现成功。`DESIGN PROPOSAL`
- `PaintResult` 保持现状（仅表达呈现结果）。`STATIC_VERIFIED`
- 新增 `BackendResult` 见 F 节。`DESIGN PROPOSAL`

---

## F. 配置结构与后端处理结果契约

### F1. 配置放置方式比较（任务 D1）

| 放置方式 | 优点 | 缺点 | 适用 |
|---|---|---|---|
| 1. `GuestOutputPaintConfig` 内部 | 单一配置对象 | 污染 UI 呈现预设；SDK 参数生命周期/线程边界/每帧参数与 UI 预设不同；易让配置对象拥有不该有的 GPU 资源 | 不推荐 |
| 2. 与现有配置关联的旁挂结构 | 不污染 UI 预设；可复用现有 mutex 模式；无 GPU 资源 | 需明确与 `GuestOutputPaintConfig` 的关联方式 | **推荐（呈现级门控）** |
| 3. 独立 D3D12 后端配置或管理模块 | SDK 参数自包含；生命周期清晰；无 GPU 资源 | 多一个配置对象 | **推荐（SDK 后端参数）** |

**推荐（分裂）**：
- **总开关 + 子功能开关 + 呈现级门控** → 旁挂结构 `GuestOutputEnhancementConfig`（方式 2），经与 `guest_output_paint_config_mutex_` **相同**的 mutex 模式读写，线程安全，**不含 GPU 资源**。`DESIGN PROPOSAL`
- **SDK 后端参数**（SR `PerfQuality` 模式、NVOF 参数等）→ 由后端模块自持（方式 3），初始化时设置，**不**进入每帧配置结构。`DESIGN PROPOSAL`
- **每帧参数**（jitter 偏移、`InReset` 标志）→ 作为后端每帧调用的**参数**传入，**不**存入持久配置对象。`DESIGN PROPOSAL`

### F2. 默认关闭语义（结构保证，非仅文档约定）

必须满足（由结构设计保证，未来由测试验证）：
- 总开关关闭 → `GuestOutputEnhancementConfig.master_enabled_ == false` → 路径选择组件直接返回“原始路径”，**不**触达后端。`DESIGN PROPOSAL`
- 总开关关闭 → 后端指针 `nullptr`（惰性创建未触发）→ **不初始化 DLSS/NVOF**、**不加载 DLL**、**不创建后端资源**、**不执行额外 GPU 步骤**、**不改变现有资源同步与 Presenter 生命周期**。`DESIGN PROPOSAL` + `STATIC_VERIFIED`（`nullptr` 路径 = 现有路径）
- **保证机制**：`nullptr` 后端是默认态；后端仅在 `master_enabled_ && sr_enabled_` 且能力检查通过时惰性创建。关闭时零对象、零 GPU 调用。`DESIGN PROPOSAL`

### F3. 后端处理结果契约（任务 D3）

新增独立枚举（状态名可调整），明确四个独立问题：
1. 后端是否被请求。
2. 后端是否成功产生可用输出。
3. 最终使用哪条输出路径。
4. Presenter 是否成功呈现（→ `PaintResult`，正交）。

| 状态 | 含义 |
|---|---|
| `kNotRequested` | 后端未被请求（总开关关，或 SR 子开关关） |
| `kSuccess` | 后端成功产生可用输出 |
| `kUnavailable` | 后端不可用（DLL 缺失、能力检查失败） |
| `kInitFailed` | 后端初始化失败 |
| `kInputContractFailed` | 输入契约不满足（格式/尺寸/MV/深度/jitter） |
| `kSyncConditionFailed` | 资源状态或同步条件不满足（状态迁移失败、Fence 超时） |
| `kExecutionFailed` | 后端执行失败 |
| `kOutputInvalid` | 输出验证失败 |
| `kOriginalPathFallback` | 已选择原始路径回退 |

**约束**：
- 后端未实际成功执行时**不得**返回 `kSuccess`。`DESIGN PROPOSAL`
- `kSuccess` 与最终呈现成功**不合并**——呈现成功由 `PaintResult` 表达。`DESIGN PROPOSAL`
- `PaintResult` 与 `BackendResult` **独立**。`STATIC_VERIFIED` + `DESIGN PROPOSAL`

---

## G. 资源格式、状态转换、Fence 与生命周期设计

### G1. guest 输出资源（输入侧）

| 项 | 设计 | 标签 |
|---|---|---|
| 格式 | `R10G10B10A2_UNORM`（`kGuestOutputFormat`，`d3d12_presenter.h:58`） | `STATIC_VERIFIED` |
| 颜色空间 | 经 tone-mapping 后为感知线性 LDR（`IsHDR=0`） | `STATIC_VERIFIED` + `SDK-DOC-SUPPORTED` |
| 进入后端前状态 | `PIXEL_SHADER_RESOURCE`（`kGuestOutputInternalState`） | `STATIC_VERIFIED` |
| 后端执行期间状态 | `NON_PIXEL_SHADER_RESOURCE`（SRV 读） | `SDK-DOC-SUPPORTED` + `DESIGN PROPOSAL` |
| 退出后端后状态 | 迁回 `PIXEL_SHADER_RESOURCE` | `DESIGN PROPOSAL` |
| 借用方式 | 后端仅**读**（SRV），**不接管** `guest_output_resource_paint_refs` | `STATIC_VERIFIED` + `DESIGN PROPOSAL` |

### G2. 方案比较（任务 E）

**方案 A：直接复用现有 guest 输出纹理 + 一次状态迁移**（审计 0.2 推荐，本阶段重新核实）
- 格式 `R10G10B10A2_UNORM` 属 DLSS 接受输入（SR 指南 §3.3），**无需格式转换**。`SDK-DOC-SUPPORTED`
- 仅需一次 `PIXEL_SHADER_RESOURCE` → `NON_PIXEL_SHADER_RESOURCE` 状态迁移（现有代码已具备屏障迁移能力，`d3d12_presenter.cc:935-941`）。`STATIC_VERIFIED`
- 输出：后端创建**自己的**输出纹理（UAV，`ALLOW_UNORDERED_ACCESS`），尺寸=目标分辨率，格式 `R10G10B10A2_UNORM`。`DESIGN PROPOSAL`
- 资源所有者：输入 = `D3D12Presenter`（借用）；输出 = SR 后端。`DESIGN PROPOSAL`
- 进入后端前状态：`PIXEL_SHADER_RESOURCE`；执行期间：`NON_PIXEL_SHADER_RESOURCE`（输入）/ `UNORDERED_ACCESS`（输出）；退出后：输入迁回 `PIXEL_SHADER_RESOURCE`，输出保留供效果链读。`DESIGN PROPOSAL`
- 队列与 Fence 依赖：在 direct queue 的 paint 提交内执行；复用现有 `paint_completion_timeline`，**不新增**跨队列 Fence。`DESIGN PROPOSAL`
- 资源何时可安全复用：paint timeline 等待完成（`AwaitSubmissionAndUpdateCompleted`）后。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
- 失败时如何保证原始路径仍可执行：状态迁移**在**原始效果链之前失败 → 直接走原始路径（guest 输出仍为 `PIXEL_SHADER_RESOURCE`，原始链可用）。`DESIGN PROPOSAL`
- 额外显存/带宽/延迟：SR `Evaluate` 本身有 GPU 开销，**无额外格式转换带宽**。无数据，不编造具体数字。`DESIGN PROPOSAL`
- 需真实 RTX 4090 验证：`Evaluate` 执行、输出画质、状态迁移在真实 GPU 上的正确性、帧时间、显存占用。`UNVERIFIED`
- 推荐程度：**推荐**——最小改动，复用现有生命周期，不破坏原始路径。`DESIGN PROPOSAL`

**方案 B：独立中间纹理做格式/颜色空间转换**
- guest 输出已为感知线性 LDR，格式/颜色空间**均满足** DLSS 输入要求，**无需**转换。仅在选 HDR 模式（`IsHDR=1`，需线性空间）或生产伴生 MV/深度纹理时才需独立中间纹理。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`
- 每多一张中间纹理增加一份显存与一次带宽读写。无数据，不编造数字。
- 推荐程度：**不推荐**作为 SR 必需步骤；仅在 HDR 模式或伴生 MV/深度需求时引入。`DESIGN PROPOSAL`

**方案 C：调整上游资源生产方式**
- 仅在需要真实 MV/深度/曝光时才适用。需修改上游渲染（guest 渲染/着色器翻译层注入 MV）、深度访问器或曝光 1×1 纹理。`STATIC_VERIFIED`
- 改动范围大、同步风险高（新增纹理生产需与 paint timeline/Fence 对齐）。
- 推荐程度：**暂不推荐**——仅在 MV/深度需求明确且方案 A 无法满足时引入。`DESIGN PROPOSAL`

### G3. 方案对比汇总

| 方案 | 技术可行性 | 改动范围 | 同步风险 | 推荐 |
|---|---|---|---|---|
| A 直接复用 guest 输出 + 一次状态迁移 | `UNVERIFIED`（格式/状态兼容已 `STATIC_VERIFIED`；`Evaluate` 执行 `UNVERIFIED`） | 小（Presenter 层） | 低 | **推荐** |
| B 独立中间纹理（HDR/MV/深度） | `UNVERIFIED`（HDR）/ 不必需 | 中 | 中 | 仅 HDR/MV/深度时 |
| C 调整资源生产（上游渲染） | `UNVERIFIED` | 大（上游渲染） | 高 | 暂不推荐 |

### G4. 关键同步与生命周期约束（不得破坏）

- **不得**擅自接管 `guest_output_resource_paint_refs`。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
- **不得**破坏 `kGuestOutputMailboxSize = 3` 对应的生命周期。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
- **不得**假设单次资源状态转换就足以保证跨队列同步安全。本设计复用现有 paint timeline，不引入新跨队列依赖。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
- **不得**让后端失败导致 `effect_count = 0` 黑屏回退。回退恒为 `effect_count ≥ 1` + `kBilinear`。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
- **不得**因为新后端需要纹理就擅自重构整个上游渲染器。方案 A 不改上游。`STATIC_VERIFIED` + `DESIGN PROPOSAL`
- 设备丢失：遵守现有 `DXGI_ERROR_DEVICE_REMOVED → kGpuLostExternally` / `DXGI_ERROR_DEVICE_RESET → kGpuLostResponsible` 契约。后端失败**不得**覆盖设备丢失状态。`STATIC_VERIFIED` + `DESIGN PROPOSAL`

---

## H. 失败分类与原始路径回退策略

### H1. 原始路径的源码定位

- 原始路径 = `effect_count ≥ 1` 且最终效果 `kBilinear`。`STATIC_VERIFIED`
- **严禁**使用 `effect_count = 0` 作为回退——它会清空后缓冲为黑、丢失 guest 输出（`d3d12_presenter.cc:530/535/671` 的既有 `effect_count = 0` 是管线创建失败的**内部**处理，**不是**后端回退路径）。`STATIC_VERIFIED` + `DESIGN PROPOSAL`

### H2. 失败分类（与 BackendResult 对应）

| 失败 | 分类 | 回退 | BackendResult |
|---|---|---|---|
| 总开关关 / SR 子开关关 | 未请求 | 原始路径 | `kNotRequested` |
| 后端未初始化 | 未请求 | 原始路径 | `kNotRequested` |
| DLL 缺失 / 能力检查失败 | 不可用 | 原始路径 | `kUnavailable` |
| 初始化失败 | 初始化失败 | 原始路径 | `kInitFailed` |
| 输入格式/尺寸/MV/深度/jitter 不满足 | 输入契约不满足 | 原始路径 | `kInputContractFailed` |
| 状态迁移失败 / Fence 超时 | 同步条件不满足 | 原始路径 | `kSyncConditionFailed` |
| `Evaluate` 执行失败 | 执行失败 | 原始路径 | `kExecutionFailed` |
| 输出无效 | 输出验证失败 | 原始路径 | `kOutputInvalid` |
| 以上任一 → 走原始路径 | 已回退 | 原始路径 | `kOriginalPathFallback` |
| 设备丢失 | 设备丢失 | 现有设备丢失契约 | **不**被后端错误覆盖 |

### H3. 回退决策（单一、明确）

1. 主开关关闭 → 现有呈现行为。
2. 后端未初始化 → 原始路径。
3. 输入资源不满足契约 → 原始路径。
4. 资源转换或同步条件失败 → 原始路径。
5. 后端执行失败 → 原始路径。
6. 后端输出无效 → 原始路径。
7. 设备丢失 → 遵守现有设备丢失处理契约。
8. 后端失败**不得**被记录为成功（`BackendResult != kSuccess`）。
9. 后端处理结果与 `PaintResult` 保持独立。
10. 回退逻辑**不得**重复执行导致资源生命周期错误的 GPU 操作（状态迁移失败后**不**再对同一资源做额外迁移，直接走原始路径）。

**核心保证**：原始路径依据当前源码明确定位（`effect_count ≥ 1` + `kBilinear`），**不**将 `effect_count = 0` 当回退路径。`STATIC_VERIFIED` + `DESIGN PROPOSAL`

---

## I. 测试和验收策略（只设计，不执行）

### I1. 主机侧单元测试（任务 G1）

测试框架与测试目录**以当前仓库实际情况为准**（`tests/` 目录、`xb build --build-tests`），**不得**臆造现有测试设施。`DESIGN PROPOSAL`

覆盖：
1. 总开关默认关闭。
2. 总开关关闭时不初始化后端（后端指针 `nullptr`）。
3. 子功能开关相互独立（SR 关 / NVOF 关 / FG 关 互不影响）。
4. 后端不可用时选择原始路径。
5. 后端初始化失败时选择原始路径。
6. 输入契约失败时选择原始路径。
7. 资源状态准备失败时选择原始路径。
8. 后端执行失败时选择原始路径。
9. 无效输出不能被报告为成功（`BackendResult != kSuccess`）。
10. 原始路径使用 `effect_count = 1` 和 `kBilinear`。
11. `effect_count = 0` 不得用于后端失败回退。
12. 后端处理结果与 `PaintResult` 独立。
13. 不支持的配置不会启动后端。
14. 设备丢失状态不会被普通后端错误覆盖。

### I2. 静态检查与编译验证（任务 G2，未来）

- 接口与类型定义：核对 `IDLSSSuperSamplingBackend` / `IOpticalFlowModule` / `BackendResult` 头文件一致性。
- 编译期 SDK 集成：`xb build --config release` 验证头文件可编译、链接符号存在（**源码级**）。
- 资源状态转换调用：静态检查状态迁移屏障的正确性与配平。
- 错误处理与日志：核对所有失败路径有分类与日志。
- 默认关闭门控：静态检查 `master_enabled_ == false` 时零后端调用。
- 不必要的 DLL 加载/初始化避免：核对关闭路径不触达 `LoadLibrary`。

### I3. RTX 4090 实机验收（任务 G3，独立矩阵）

**只有实际在 RTX 4090 上完成的验证才能标记 `RUNTIME_VERIFIED_ON_RTX4090`。不得用 Mock、静态分析或编译成功代替。**

| 验收项 | 内容 | 状态 |
|---|---|---|
| 环境记录 | 实际 GPU、驱动、Windows、SDK 版本记录 | `UNVERIFIED`（需实机） |
| 后端初始化 | `GetFeatureRequirements` → `Init` → `CreateFeature`（无 GPU 错误） | `UNVERIFIED` |
| DLSS SR 实际执行 | 逐帧 `EvaluateFeature`（色/depth/MV/jitter/曝光） | `UNVERIFIED` |
| 输入/输出纹理有效性 | 分辨率/颜色正确；无色彩带（线性输入测试） | `UNVERIFIED` |
| 原始路径 vs SR 输出对比 | 画质对比 | `UNVERIFIED` |
| 不同 SR 模式输出检查 | Quality/Balanced/Performance/Ultra-Performance/DLAA | `UNVERIFIED` |
| 运动矢量与深度输入条件 | MV/depth 提供与否的质量差异 | `UNVERIFIED` |
| 画质、延迟、帧时间、显存占用 | 实测数据 | `UNVERIFIED` |
| 初始化失败/执行失败/设备丢失行为 | 回退正确性 | `UNVERIFIED` |

### I4. 验证层次声明

- 静态分析 ✅（本阶段已完成）。
- 编译 ✅（**未**在本阶段执行，属未来）。
- 单元测试 ✅（**未**在本阶段执行，属未来）。
- 真实 GPU/光流/图像 ❌ 需 RTX 4090 测试机（`UNVERIFIED`）。

---

## J. 许可证与发布边界

> 本阶段**不作**最终法律裁定。区分五类活动，列出仍需 NVIDIA/法律专业人士确认的事项及它们分别阻塞哪些发布行为。

### J1. 五类活动区分

| 活动 | 状态 | 阻塞项 |
|---|---|---|
| 本地研究与开发 | 允许（DLSS `LICENSE.txt` §1a：安装使用） | 无 |
| 本地链接 SDK | 允许（§1：以目标代码形式嵌入应用软件） | 无（本地） |
| 将自有集成代码提交到公开 Xenia 仓库 | `LICENSE REVIEW REQUIRED` | §4e 开源兼容性 |
| 分发包含 SDK DLL 的二进制包 | `LICENSE REVIEW REQUIRED` + 须先通知 NVIDIA（Supplement §4） | §4e + §4b + Supplement §4 |
| 不附带 SDK DLL 的实现分发 | `LICENSE REVIEW REQUIRED` | §4e（嵌入目标代码是否构成开源约束） |

### J2. 关键冲突点（仍需确认）

- **§4e**：不得使 SDK 受开源软件许可约束。Xenia 为公共开源（MIT/类似）。将 NVIDIA SDK 目标代码嵌入并随开源仓库分发，**可能**使 SDK 受 Xenia 开源许可约束。**证据不足以给出结论。** `LICENSE REVIEW REQUIRED`
- **§4b**：不得将 SDK 作为独立产品分发/再授权。`OFFICIAL`
- **§2b**：分发派生源码须含声明 “This software contains source code provided by NVIDIA Corporation.” `OFFICIAL`
- **Supplement §4**：商业发布前须通知 NVIDIA（`developer.nvidia.com/sw-notification`）。`UNVERIFIED`（操作未做）
- **Supplement §1**：应用须与 NVIDIA GPU 硬件互操作；DLSS/NGX/RTXDV 仅授权用于含 NVIDIA GPU 的系统。`OFFICIAL`

### J3. 本阶段约束

- **不得**擅自将 SDK DLL 复制进仓库。
- **不得**因架构设计通过就视为许可证问题已解决。
- 架构设计**可以**继续（本地研发被允许），但发布行为被阻塞至许可澄清。

> **用户决策 6（已批准）**：在许可证问题得到适当澄清之前，不向公开 Xenia 仓库提交受相关许可问题影响的集成代码，不分发 NVIDIA SDK DLL；允许在现有许可条件允许的范围内继续本地研究。§J1–J2 的 `LICENSE REVIEW REQUIRED` 标记保持不变，本决策不构成法律裁定。

---

## K. 后续里程碑计划（只设计，不执行）

### K1. Milestone 1：架构与接口设计（本阶段）
- 任务：完成架构文档并等待用户审核。**已完成**，用户已正式批准（决策 1–7，见 §L3）。
- 停止点：文档完成即停止，**不**自动进入 Milestone 2。

### K2. Milestone 2：最小接口骨架与主机侧测试（架构已批准；启动待用户明确指示）
- 可能包括：最小后端接口（`IDLSSSuperSamplingBackend`）、默认关闭配置（`GuestOutputEnhancementConfig`）、独立处理结果契约（`BackendResult`）、原始路径回退测试、必要的编译与静态验证。
- **不**在此阶段宣称真实 DLSS 已可用。
- 修改范围：仅 Presenter/后端管理层接口与静态检查，不触及上游渲染。
- 验收条件：构建通过（届时方可标 `COMPILE_VERIFIED`）；主开关默认关闭；单测验证回退到原始效果链（`effect_count ≥ 1` + `kBilinear`），`effect_count = 0` 不用于回退；零 GPU 依赖、零 DLL 加载。
- 停止点：完成本步后停止。

### K3. Milestone 3：DLSS SR 后端接入（接口骨架与资源契约稳定后）
- 独立处理：SDK 版本兼容、DLL 加载策略、D3D12 资源状态与同步、失败回退、RTX 4090 实机验证。
- 修改范围：`D3D12Presenter` 后端接入 + 路径选择组件。
- 验收条件：RTX 4090 实机 `Evaluate` 成功、输出有效、回退正确。
- 停止点：SR 版本稳定后停止。

### K4. 后续独立阶段
- NVOF（独立 MV 模块）。
- 帧生成（需相机矩阵/MV/UI 分离/时序改造）。
- 实验性神经渲染及其他功能。
- 这些模块**不**成为首个 DLSS SR 版本的强制依赖。
- 每个阶段必须列出修改范围、验收条件和停止点。不得因制定计划而提前执行。

---

## L. 未决问题、设计假设和需要用户批准的决策

### L1. 未决问题（`UNVERIFIED` / `LICENSE REVIEW REQUIRED`）

| 项 | 状态 | 说明 |
|---|---|---|
| 许可 §4e（开源兼容性） | `LICENSE REVIEW REQUIRED` | 嵌入目标代码到开源应用是否构成违禁，需 NVIDIA/法律确认 |
| 公开发布前 NVIDIA 通知 | `UNVERIFIED` | Supplement §4 操作未做 |
| DLSS `Evaluate` 执行 | `UNVERIFIED` | 需 RTX 4090 实机 |
| 输出画质 | `UNVERIFIED` | 需 RTX 4090 实机 |
| NVOF 执行 | `UNVERIFIED` | 需 `nvofapi64.dll` + GPU |
| 帧生成性能/画质 | `UNVERIFIED` | 需相机矩阵 + GPU |
| 实验性神经渲染 | `UNVERIFIED` | 非官方签名运行时，无 Xenia 契约 |
| 状态迁移屏障正确性 | `UNVERIFIED` | 需单测 + 真实 GPU |
| MV 缺失对 SR 质量影响 | `UNVERIFIED` | 需 RTX 4090 实机对比 |
| 头文件 310.9.1 vs 文档 310.6.0 ABI 稳定性 | `UNVERIFIED` | 以头文件为权威，需核对 |

### L2. 设计假设（需在后续验证）

- 假设 guest 输出 `R10G10B10A2_UNORM` 可直接作为 DLSS SR 输入（格式/颜色空间兼容）——已 `STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`，但 `Evaluate` 执行 `UNVERIFIED`。
- 假设一次 `PIXEL_SHADER_RESOURCE → NON_PIXEL_SHADER_RESOURCE` 状态迁移足以满足 SR 输入——已 `STATIC_VERIFIED`（现有屏障能力），但真实 GPU 正确性 `UNVERIFIED`。
- 假设 SR 后端可仅读 guest 输出（SRV）而不接管 `guest_output_resource_paint_refs`——已 `STATIC_VERIFIED`（paint-refs 所有权），`DESIGN PROPOSAL`。
- 假设 `NVSDK_NGX_PerfQuality_Value` 无 `Off`，关闭由 Xenia 侧控制——已 `SDK-VERIFIED`。
- 假设 NVOF 可作为独立 MV 来源而非唯一来源——`DESIGN PROPOSAL`，`UNVERIFIED`。

### L3. 用户批准状态（2026-10-20 正式决策）

1. **架构方案选择**：推荐方案 1（直接后端接口 + 轻量路径选择）。**✅ 已批准（决策 1）**。
2. **首个可运行版本范围**：仅 DLSS SR（方案 A）。NVOF、Frame Generation、Ray Reconstruction、实验性神经渲染暂缓，均不得成为 SR 的强制依赖。**✅ 已批准（决策 2）**。
3. **配置分裂**：总开关和子开关在旁挂结构独立管理、SDK 专用参数由后端自持、每帧参数通过调用参数传递。实现必须符合当前源码的配置传递、线程和生命周期约束。**✅ 已批准（决策 3）**。
4. **后端结果契约**：九状态独立枚举与 `PaintResult` 正交。**✅ 已批准（决策 4）**。实现前核对本文档中的完整状态定义，不得擅自改变其语义。
5. **原始路径回退**：依据当前源码，回退使用 `effect_count = 1` 且 `effects[0] = kBilinear`。**✅ 已批准（决策 5）**。严禁将 `effect_count = 0` 用作后端失败回退。
6. **许可证阻塞**：在许可证问题得到适当澄清之前，不向公开 Xenia 仓库提交受相关许可问题影响的集成代码，不分发 NVIDIA SDK DLL；允许在现有许可条件允许的范围内继续本地研究。**✅ 已批准（决策 6）**。§4e 等 `LICENSE REVIEW REQUIRED` 标记保持不变。
7. **实验性神经渲染**：当前来源存在 HashMismatch 的社区神经渲染运行时仅作为研究材料，不纳入主线，不复制到主线运行时目录，不将其报告为可信或已验证的组件。**✅ 已批准（决策 7）**。
8. **Milestone 2 启动**：架构已批准；Milestone 2 的实际启动**仍需用户明确指示**，本阶段不自动开始。

---

## 附：证据索引

- 源码：`src/xenia/ui/presenter.h`（`GuestOutputPaintConfig` ~196、`GuestOutputPaintFlow` ~439、`GuestOutputPaintEffect` ~389、`PaintResult` ~340、`kFsrMaxUpscalingPassesMax` 219、`kMaxGuestOutputPaintEffects` 436、`kGuestOutputMailboxSize` 360、`ConsumeGuestOutput` 596、`GetGuestOutputPaintFlow` 651）、`src/xenia/ui/d3d12/d3d12_presenter.h`（`kGuestOutputFormat` 58、`kGuestOutputInternalState` 60、refresher 契约 77-79、paint refs、timeline）、`src/xenia/ui/d3d12/d3d12_presenter.cc`（`RefreshGuestOutputImpl` 386、`PaintAndPresentImpl` 463、`effect_count=0` 530/535/671、状态迁移 935-941）、`src/xenia/gpu/d3d12/d3d12_command_processor.cc`（`IssueSwap` 2250、`RequestSwapTexture` 2270、`RefreshGuestOutput` 回调 2279+）。
- SDK：`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx_defs.h`（`NVSDK_NGX_PerfQuality_Value` 251-260）、`nvsdk_ngx_helpers_d3d.h`、`nvsdk_ngx_params.h`；`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\doc\DLSS_Programming_Guide_Release.pdf`（310.6.0）、`DLSS-FG Programming Guide.pdf`（310.7.0）、`DLSS-RR Integration Guide.pdf`（v08）；`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\LICENSE.txt`。
- NVOF：`C:\SDKs\NVIDIA\Optical_Flow_SDK_5.0.7\Doc\NVOFA_Programming_Guide.pdf`、`NVOFA_FRUC.pdf`、`NVOFA_Tracker.pdf`。
- 运行时：`\\ein-9900k\Code\DLSS5_Research\runtime\experimental\release-runtime-manifest.json`（SR 310.9.1.0 官方、FG 310.9.1.0 官方、RR 310.9.1.0 官方、NR 310.8.3.0 社区 Lecram HashMismatch、NR 310.8.0.0 用户批准 NVIDIA 原始 Valid）。
- 参考：`\\ein-9900k\Code\DLSS5_Research\nvidia-dlss-reference\`（00-09 系列）、`\\ein-9900k\Code\DLSS5_Research\Veyra-NRVideo\`（只读架构参考）。
- 审计：`docs/dlss-integration/milestone-0-audit.md`、`milestone-0.1-baseline-review.md`、`milestone-0.2-feasibility-review.md`。
