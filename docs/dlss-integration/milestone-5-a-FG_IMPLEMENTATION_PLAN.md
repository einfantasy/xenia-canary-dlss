# Milestone 5-A：DLSS 帧生成（FG）实施计划

- **文档性质**：IMPLEMENTATION PLAN / 实施计划（**只读**，未实施）。基于 `milestone-5-a-FG_FEASIBILITY_REPORT.md` 的可行性结论（**有条件可行**）给出 M5-B / M5-C 的细化实施方案。
- **对应任务**：`architecture-design.md` K4.2.2 Milestone 5-B、K4.2.3 Milestone 5-C。
- **仓库状态**：分支 `dev/dlss5-integration`，HEAD `67ccea8`。
- **范围遵守**：M5-B 仅做编译期集成 + 主机侧测试，**不改呈现路径**（呈现路径留 M5-C）；默认关闭；失败确定性回退；禁止 Mock/伪装真实 GPU 执行（红线 3）；所有硬件相关项 UNVERIFIED（Hyper-V 无 NVIDIA 驱动）。

---

## M5-B：FG 接入基础设施（编译期集成，不改呈现路径）

**目标**：建立 FG 模块及其与 Xenia 渲染链路的**连接骨架**（接口、配置、核心属主、主机侧测试），**不接通真实 FG 流程**（呈现路径不动），**不改原始呈现路径**。

### B1. 共享 NGX 核心属主（前置条件 1，最高优先）

- **新增** `src/xenia/ui/ngx_core_host.{h,cc}`：引用计数的进程级/设备级单例。
  - `Acquire(device)` → 若核心未初始化则 `NVSDK_NGX_D3D12_Init_with_ProjectID(device, ...)`（SEH 边界），引用计数 +1，返回核心属主。
  - `Release()` → 引用计数 -1；**归零时**才 `NVSDK_NGX_D3D12_Shutdown1(device)`（SEH 边界）。
  - `AllocateParameters()` / `DestroyParameters()` 通过核心属主（反序拆除保证，仿 Veyra `NgxCoreHost`）。
  - 线程安全：mutex 保护引用计数（UI 线程写配置，paint 线程读）。
- **改造** SR 后端：`DLSSSuperSamplingBackend` 改为通过核心属主 `Acquire/Release`（不再自行 `Init_with_ProjectID` / `Shutdown1`）。**行为等价**（SR 单独用时核心仍 Init 一次、Shutdown1 一次），但为 FG 共享铺路。
- **验收**：SR 单独初始化 → 核心 Init 一次、Shutdown1 一次（与现状等价）。主机侧测试引用计数。

### B2. FG 后端模块（接口 + 骨架）

- **新增** `src/xenia/ui/fg/fg_frame_generation_backend.{h,cc}`：
  - 实现 `IFrameGenerationBackend` 接口（仿 `IDLSSSuperSamplingBackend` / `IOpticalFlowBackend`）。
  - `IsAvailable()` / `IsUnavailable()` / `TryProcess(command_list, backbuffer, depth, mvecs, frame_params)` → 返回 `BackendResult`。
  - 自有：插值输出纹理（`R10G10B10A2`，UAV）、OutputReal 纹理、禁用状态缓冲（4 字节）、SDK 特性句柄 + 参数图、零深度占位。
  - 借用（只读）：backbuffer、深度、MV。**永不接管** `guest_output_resource_paint_refs`。
  - 懒初始化：首次 `TryProcess` 建能力查询 + `CreateFeature`（记录到命令列表）。失败缓存为不可用。
  - SEH 边界包裹全部 SDK 入口（`CreateFeature` / `EvaluateFeature` / `ReleaseFeature`）。
  - **M5-B 阶段 `TryProcess` 返回 `kNotRequested` 或 `kUnavailable`（骨架，不真调 SDK）**，或用一个可配置结果的 stub（主机侧测试用）。**真实 SDK 调用留 M5-C。**
- **新增** `IFrameGenerationBackend` 接口（在 `guest_output_enhancement.h` 或 `fg_frame_generation_backend.h`）：
  ```cpp
  class IFrameGenerationBackend {
   public:
    virtual ~IFrameGenerationBackend() = default;
    virtual bool IsAvailable() const = 0;
    virtual bool IsUnavailable() const = 0;
    // 处理一帧：记录 FG Evaluate 到命令列表，返回 BackendResult。
    virtual BackendResult TryProcess(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* backbuffer,    // 显示分辨率，R10G10B10A2，只读借用
        ID3D12Resource* depth,         // guest 分辨率，零占位，只读借用
        ID3D12Resource* mvecs,         // guest 分辨率，R16G16_FLOAT，只读借用
        const DlssFgFrameParams& frame_params) = 0;
    // 自有输出纹理（Presenter 用它建 SRV）。
    virtual ID3D12Resource* OutputResource() const { return nullptr; }
    virtual ID3D12Resource* OutputRealResource() const { return nullptr; }
    virtual uint32_t OutputWidth() const { return 0; }
    virtual uint32_t OutputHeight() const { return 0; }
    // 释放自有资源。
    virtual void Shutdown() = 0;
  };
  ```
- **新增** `DlssFgFrameParams`（仿 `DlssSrFrameParams`）：`reset`（场景变化）、`frameId`（单调递增真实帧 id）、`depthInverted`、`mvecScaleX/Y`。M5-B 可先只定义，M5-C 才用。
- **验收**：接口可编译；stub 后端返回可配置 `BackendResult`；主机侧测试路径选择。

### B3. 配置接线（已有，确认）

- `GuestOutputEnhancementConfig.frame_generation_enabled`（默认 false）+ `IsFrameGenerationRequested()` **已存在**（M2）。M5-B 不改配置结构。
- **确认** `IsFrameGenerationRequested()` 在 Presenter 中**尚未被使用**（现状仅 SR/OF 请求）。M5-B 不接 Presenter，留 M5-C。

### B4. 主机侧测试

- **新增** `src/xenia/ui/testing/fg_backend_test.cc`：
  1. 配置门控：`frame_generation_enabled=false` → `IsFrameGenerationRequested()=false`。
  2. `BackendResult` 映射：stub FG 返回各结果 → `IsOriginalPathFallback` 全状态正确。
  3. 路径选择纯函数（FG 请求 + stub 成功/失败）。
  4. SEH 边界：stub 触发 SEH → 捕获为失败。
  5. 资源所有权：析构释放自有、不释放借用。
  6. 共享核心属主：SR + FG 同时 Acquire → 核心 Init 一次；SR Release → 核心仍活；FG Release → Shutdown1。
- **验收**：全部主机侧测试通过（无 GPU）。

### B5. 编译期集成

- **新增** `src/xenia/ui/fg/CMakeLists.txt` + 改 `src/xenia/ui/CMakeLists.txt`（FG 模块接线）。
- **整树 `/WX` 编译**：`.\xb build --config release`。成功判据：无警告无错误。
- **许可证隔离**：SDK 头仅 `fg/*` 翻译单元包含（`rg nvsdk` 仅命中 fg/*）。
- **无 NVIDIA import library 链接**：FG 模块不链接 NVIDIA 导入库。

### B6. M5-B 验收条件
- 工程可构建（`.\xb build --config release` → Success）。
- 共享核心属主 + FG 后端骨架 + 主机侧测试全部通过。
- **不改呈现路径**（`PaintAndPresentImpl` 不动，无第二呈现，无 FG 输出 SRV 到 view heap）。
- **不改原始呈现路径**（FG 关闭时行为与现状完全一致）。
- **不接通真实 FG 流程**（`TryProcess` 是 stub，不真调 SDK）。

---

## M5-C：FG 调用与安全回退（接通真实 FG 流程）

**前置**：M5-B 审核通过。

**目标**：接通真实 FG 流程，确保故障时安全恢复。

### C1. 真实 SDK 调用

- **实现** `FGFrameGenerationBackend::Initialize`：通过共享核心属主 `AllocateParameters` + `NGX_D3D12_CREATE_DLSSG`（记录到命令列表）+ 能力查询（`FrameGeneration_Available` / `MultiFrameCountMax`）。
- **实现** `FGFrameGenerationBackend::TryProcess`：`NGX_D3D12_EVALUATE_DLSSG`（记录到命令列表），输入 backbuffer[NPSR] + 深度[NPSR] + MV[NPSR]，输出 插值帧[UAV] + OutputReal[UAV] + 禁用状态[UAV]。
- **实现** `FGFrameGenerationBackend::Shutdown`：`ReleaseFeature` + 核心属主 `Release`。
- **验收**：真实 SDK 调用符合目标 SDK 输入与时序契约。

### C2. 资源传递（与渲染后端）

- **Presenter 新增 FG 块**（仿 SR/NVOF 块，在 SR 块之后）：
  - `fg_requested = IsFrameGenerationRequested() && !is_one_to_one`（或 FG 不要求非 1:1，视设计）。
  - 懒创建 FG 后端。
  - `TryProcess(command_list, backbuffer, depth, mvecs, frame_params)`。
  - 成功 → 创建 FG 输出 SRV 到 view heap（`kViewIndexDLSSFgOutputSrv`）。
  - 失败 → 命令列表 Reset + 回退原始路径（仅呈现真实帧）。
- **MV / 深度来源**：MV = NVOF `MotionVectorOutput()`（或 SR 零 MV 占位）；深度 = 零占位。
- **backbuffer 来源**：SR on → SR 输出纹理；SR off → guest 输出经效果链到 backbuffer 的显示分辨率拷贝。
- **验收**：资源所有权与同步符合已审核设计。

### C3. 第二次呈现（2x 呈现时序，前置条件 2）

- **修改** `PaintAndPresentImpl`：在现有呈现真实帧 N 之后（或 FG Evaluate 之后），新增一次「FG 插值输出 → backbuffer → Present」的绘制传递（呈现 N-0.5），再呈现真实帧 N（OutputReal → backbuffer → Present）。
- **呈现顺序**：..., (N-0.5), (N), ... —— 时间序正确。
- **呈现 timeline**：复用现有 `paint_completion_timeline` + `present_completion_timeline`（第二呈现复用同一 timeline）。
- **swapchain**：3 缓冲 ring 吸收 2x 呈现速率（每个 guest 帧 Present 两次）。
- **禁用状态读回**（可选优化）：4 字节 CPU 读回，若插值被禁用则跳过 N-0.5 呈现（只呈现 N 一次）。最小版本可**始终呈现两帧**。
- **验收**：FG 关闭 / 不可用 → 仅呈现真实帧（原始路径，行为与现状一致）。

### C4. 生成帧与原始帧关联

- **FrameKind**：区分「正常帧」（真实帧 N）与「生成帧」（N-0.5）。仿 Veyra `FrameKind::Generated`。
- **frameId**：单调递增真实帧 id（`DlssFgFrameParams.frameId`）。
- **reset**：场景变化时置 `reset=true`（重置 FG 内部历史）。
- **验收**：基础测试可区分正常帧、生成帧和回退帧。

### C5. FG 开关与运行时能力检查（UI）

- **新增** `emulator_window.cc`：FG 复选框（主开关 on 时绘制，语义同 SR/OF 子开关）+ `dlss_frame_generation_enabled` cvar。
- **运行时能力检查**：FG 后端 `Initialize` 查询 `FrameGeneration_Available` / `MultiFrameCountMax`，不可用 → 确定性 `kUnavailable`（不重试）。
- **验收**：仿现有 DLSS UI 开关设计；FG 开关独立控制。

### C6. 异常处理与安全回退

- **SDK 初始化失败** → `kInitFailed`（或 `kUnavailable`）→ 回退原始路径。
- **资源无效**（null / 尺寸不匹配）→ `kInputContractFailed` → 回退。
- **设备丢失** → `kSyncConditionFailed` / `kGpuLost*` → 回退 + GPU 丢失处理。
- **状态不兼容** → `kSyncConditionFailed` → 回退。
- **回退保证**：FG 失败 → 仅呈现真实帧 N（原始路径）。**永不把失败标记为成功**。
- **防过期资源 / 重复呈现 / 破坏帧同步**：回退后不继续使用过期资源、不重复呈现、不破坏帧同步。
- **验收**：FG 关闭 / 不可用 → 安全回退；FG 失败不破坏原有游戏渲染。

### C7. 日志 / 诊断

- **运行状态**：FG 激活 / 关闭 / 不可用（原因分类）。
- **错误原因**：`BackendResult` + NGX 错误码 + SEH 码。
- **回退事件**：每次回退记录原因 + 帧 id。
- **验收**：诊断信息完整，可区分正常帧、生成帧、回退帧。

### C8. M5-C 验收条件
- 调用符合目标 SDK 的输入和时序契约。
- FG 关闭 / 不可用 → 安全回退（原始路径）。
- FG 失败不破坏原有游戏渲染。
- 基础测试可区分正常帧、生成帧和回退帧。
- 整树 `/WX` 编译通过。
- 真实硬件（RTX 4090）UNVERIFIED（Hyper-V 无 GPU）。

---

## 构建 / 测试命令

- **基础构建**：`.\xb build --config release`
- **测试目标**：`.\xb build --config release --build-tests --target xenia-app`（按实际 xb 脚本支持参数调整）
- **主机侧测试**：`.\xenia-ui-tests.exe`（Catch2）
- **许可证隔离检查**：`rg nvsdk src/xenia/ui/fg/` 仅命中 fg/*；`rg nvsdk src/xenia/ui/` 不应命中 fg/* 以外
- **无 NVIDIA import library**：链接器输出无 NVIDIA .lib

---

## 范围 / 红线遵守

- **默认关闭**：FG 子开关默认 false，主开关控制所有子功能。
 **失败回退**：任意 FG 失败 → 原始路径（仅呈现真实帧），确定性，永不标记成功。
- **禁止 Mock/伪装**：不引入 Veyra `FgCompatibilitySession`（AdaMfgUnlock / AmpereMfgUnlock / DlssgTransfusion 补丁/伪装机制）。RTX 4090 原生 2x，无需解锁。
- **硬件验证**：所有 GPU 项 UNVERIFIED（Hyper-V 无 NVIDIA 驱动），静态检查 / Mock 测试不表述为真实 GPU 已通过。
- **独立性**：FG 正交于 SR / NVOF，互不强制。
- **资源所有权**：FG 永不接管 `guest_output_resource_paint_refs`，永不破坏 Xenia 资源生命周期 / 队列同步 / Fence 依赖。

---

## 停止点

- **M5-B 完成后停止**：编译期集成 + 主机侧测试，不改呈现路径，不接通真实 FG。
- **M5-C 需 M5-B 审核通过后另批**：接通真实 FG + 第二次呈现 + UI 开关 + 安全回退。
- **真实硬件验证（RTX 4090）需人工参与**：FG 能力 / 创建 / 评估 / 2x 呈现 / 失败回退 / SR+FG 叠加。
