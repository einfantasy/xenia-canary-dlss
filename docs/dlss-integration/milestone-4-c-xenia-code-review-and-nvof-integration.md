# Milestone 4-C — 本地 Xenia 代码审查与 NVOF 集成点分析

- **状态**：Milestone 4-C（K4.1.3）**完成**——纯只读审查，未修改任何 Xenia 或 NVIDIA 生产代码。
- **范围**：① 从实际代码追踪 D3D12 设备/命令队列的创建、使用与销毁；② 分析 guest output 的格式、尺寸、使用时机与资源可安全访问性；③ 明确资源状态转换、命令提交、Fence 同步、跨队列依赖与资源所有权；④ 识别现有渲染输出与候选 NVOF 模块之间的线程安全、资源复用、生命周期与错误回退问题；⑤ 提出独立接口与最小化集成点，不侵入上游渲染流程。
- **撰写前强制阅读**：`AGENTS.md`、`docs/dlss-integration/architecture-design.md`（K4.1）、`docs/dlss-integration/milestone-3-c-handoff.md`、`docs/dlss-integration/milestone-4-b-veyra-mv-depth-research.md`、`nvidia-nvof-reference/sdk-docs/`。
- **证据标签约定**：`STATIC_VERIFIED`（源码/静态分析层面）、`SDK-VERIFIED`（SDK 头文件/文档/样本层面）、`DESIGN PROPOSAL`（设计提案，未实施）、`UNVERIFIED`（未经真实 GPU 验证）。
- **修改范围核对**：本轮仅本地代码阅读、调用链追踪、资源生命周期分析、依赖关系梳理与集成方案编写；**未**修改上游渲染流程、**未**引入 NVOF 生产代码、**未**改动 Presenter 配置面。工作区仅用户自改的 `docs/dlss-integration/architecture-design.md` 与未落盘的 4-B 文档，均未触碰。
- **停止点**：报告落盘即停，等待用户审核。未经单独批准，不进入 Veyra 仓库研究或实际代码修改。

---

## A. 总结

Xenia 的 D3D12 呈现栈是**单 direct 命令队列 + 提交索引 Fence 时间线 + 3 槽 guest output 邮箱**的清晰结构。guest output 是 `R10G10B10A2_UNORM` 纹理，**进入 paint 路径时恒为 `PIXEL_SHADER_RESOURCE`**，被 SR 后端以 SRV/UAV 借用读取但从不接管所有权。NVOF 的接入点与 SR 完全同构：**借用当前 guest output（只读）+ 后端自持中间纹理/输出 + 把 GPU 命令录制进 paint 命令列表 + 失败时 `command_allocator->Reset()` 重录原始路径**。关键发现是：NVOF 只需"当前帧"guest output，其"前一帧 A"可由 NVOF 自持的 ABGR8 帧池提供，**无需触碰 guest output 邮箱的历史帧**，从而完全非侵入。

- **总体状态**：**PASS WITH FINDINGS**（见 E 节）。

---

## B. 修改文件

**无。** 本轮为纯只读审查，未修改 Xenia 或任何生产代码。工作区仅保留用户自改的 `docs/dlss-integration/architecture-design.md`（未触碰）。

---

## C. 验证证据（代码调用链 + 资源流向 + 集成点 + 可复用机制）

### C.1 guest output 的完整调用链与资源流向

**生产侧（guest 渲染线程，`d3d12_command_processor.cc`）**
`IssueSwap` → `presenter->RefreshGuestOutput(width,height,aspect, refresher)`（`d3d12_command_processor.cc:2279`）。refresher 回调内：读取 guest swap texture（SRV）→ 应用 gamma（PWL/256 表）+ 可选 FXAA → **以 UAV 写入 guest output 纹理**（`kGuestOutputFormat=R10G10B10A2`），写入前 `PIXEL_SHADER_RESOURCE→UNORDERED_ACCESS`、写后迁回。随后 `RefreshGuestOutputImpl`（`d3d12_presenter.cc:~386`）把该纹理登记进邮箱 `guest_output_resources_[mailbox_index]`，并在 `guest_output_resource_refresher_completion_timeline_` 上 `SignalAndAdvance(direct_queue)`。

**消费侧（paint 线程，`PaintAndPresentImpl`，`d3d12_presenter.cc:~463`）**
`ConsumeGuestOutput`（`presenter.cc:596`）取最新 ready 的 guest output 资源（ComPtr，引用计数 +1），格式/尺寸在 `GuestOutputProperties`。随后：
- 为 guest output 建 SRV（`kViewIndexGuestOutput0Srv + paint_ref_index`，view heap）。
- **SR 后端块**（`d3d12_presenter.cc` 中部）：读 `GuestOutputEnhancementConfig`（`guest_output_enhancement_config_mutex_` 保护）→ `IsSuperSamplingRequested() && !1:1` → 惰性 `std::make_unique<DLSSSuperSamplingBackend>` → `SelectOutputPath(...)` → 成功则建后端输出 SRV（`kViewIndexDLSSSrOutputSrv`）并把首个 paint effect 源重定向到它；失败则 `command_allocator->Reset(); command_list->Reset(...)` 重录原始路径。
- **效果链循环**：逐 effect 录制 `ResourceBarrier`（状态迁移）+ `OMSetRenderTargets` + `DrawInstanced`，最终 effect 写 swap chain back buffer，`present` 前 `RENDER_TARGET→PRESENT`。
- 收尾：`command_list->Close()` → `direct_queue->ExecuteCommandLists` → `ui/paint/present` 三条时间线依次 `SignalAndAdvance` → `swap_chain->Present`。

**资源状态契约（STATIC_VERIFIED）**
- guest output 初始/归还态 = `kGuestOutputInternalState = PIXEL_SHADER_RESOURCE`（`d3d12_presenter.h:60`）。
- 格式 `kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM`（`d3d12_presenter.h:58`），`D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`，heap `Default` + `CREATE_NOT_ZEROED`。
- 邮箱 `kGuestOutputMailboxSize = 3`（`presenter.h:360`），双时间线保护：refresher 时间线（写）+ paint 时间线（读）。
- **SR 后端对 guest output 的 4 段 barrier**（`dlss_super_sampling_backend.cc` `TryProcess`）：`PSR→NPSR`（SDK compute 读）→ EvaluateFeature → `NPSR→PSR`（归还）；后端输出纹理 `UAV→PSR`。零占位 `depth_texture_`(R32F)/`motion_vectors_texture_`(R16G16F) 初始化为 NPSR（SDK 以 UAV 绑定，从不写回）——**这就是 M4 的 MV 接线点（头文件 TODO 明示）**。

### C.2 候选 NVOF 调用位置与资源流向

**推荐插入点**：与 SR 后端**同层、同命令列表、同 direct 队列**，在 `PaintAndPresentImpl` 的"SR 后端块"之**前**插入"NVOF 后端块"（或作为其前置步骤）。资源流向：

```
当前 guest output (R10G10B10A2, PSR, 只读 SRV)
   │  ① ABGR8 转换 pass（PS 计算，NVOF 自持）
   ▼
ABGR8 帧池 slot_B (B8G8R8A8, UAV)   ← 本帧写入
ABGR8 帧池 slot_A (B8G8R8A8)        ← 上帧已写入（NVOF 自持，无需邮箱历史帧）
   │  ② NvOFExecuteD3D12(inputFrame=B, referenceFrame=A, in/out fence)
   ▼
flowOut (R16G16_SINT, ceil(w/grid)×ceil(h/grid)) + costOut (R8_UINT)
   │  ③ 稠密化 pass（PS：S10.5/32→全分辨率 float2, cost→confidence, negate）
   ▼
全分辨率 MV (R16G16_FLOAT, NPSR/UAV)  → 供 SR 后端 pInMotionVectors 消费
```

**关键非侵入结论**：
- **帧 A 不需邮箱历史帧**。NVOF 自持 ABGR8 帧池（轮转 4–6，SDK 准则 5），转换 pass 每帧把当前 guest output 写入下一个 ABGR8 槽；"前一帧 A"即上帧已写的槽。因此 NVOF 只借用**当前** guest output（与 SR 完全相同），**不触碰 `guest_output_resource_paint_refs`、不破坏 `kGuestOutputMailboxSize=3` 生命周期**（G4 约束满足）。
- **guest output 状态不变**。转换 pass 以 SRV 读 guest output（保持 `PSR`），写目标是 NVOF 自持的 ABGR8（UAV）。与 SR 后端的 `PSR→NPSR` barrier 顺序相容：转换 pass 先读（PSR）→ SR 再迁 `PSR→NPSR` 读 → 迁回 `PSR`，全在同一命令列表顺序执行，状态一致。
- **无新跨队列 Fence**。全部在 direct 队列的 paint 提交内，复用现有 `paint_completion_timeline`，不引入新跨队列依赖（G4 约束满足）。NVOF 的 in/out `NV_OF_FENCE_POINT` 用 direct 队列上的 `ID3D12Fence`（客户端创建管理，SDK 仅用于同步），与现有 `D3D12GPUCompletionTimeline` 模式同构。

### C.3 候选集成点的 D3D12 接口 / 资源状态 / 同步依赖

| 环节 | 必要 D3D12 接口 | 资源状态要求 | 同步依赖 |
|---|---|---|---|
| ABGR8 转换 pass | `CreateCommittedResource`(ABGR8, `ALLOW_UNORDERED_ACCESS`)、`CreateGraphicsPipelineState`、`CreateUnorderedAccessView`、`ResourceBarrier`、`Dispatch/Draw` | 源=guest output `PSR`(SRV 借读)；目标=ABGR8 `UAV` | 同命令列表顺序；无跨队列 |
| NVOF 会话注册 | `CreateFence`(in/out)、`NvOFRegisterResourceD3D12`(`inputFencePoint`/`outputFencePoint`) | ABGR8 输入帧、flowOut、costOut 均须已分配并注册 | NVOF 在处理前等 in-fence、完成后置 out-fence（`ID3D12CommandQueue::Wait`，无 CPU 等待） |
| NvOFExecuteD3D12 | SDK 内部（录制进命令列表） | 输入帧 `NPSR`、输出 `UAV` | out-fence 由消费方等待 |
| 稠密化 pass | `CreateShaderResourceView`(flowOut SRV)、`CreateUnorderedAccessView`(MV UAV)、`DrawInstanced` | 源=flowOut `PSR/NPSR`；目标=全分辨率 MV `UAV`(NPSR) | 同命令列表顺序 |
| 失败回退 | `command_allocator->Reset(); command_list->Reset(...)` | 未执行的已录制命令被丢弃，guest output 仍在 `PSR` | 原始路径重录 |

**view heap 尺寸约束（STATIC_VERIFIED）**：`kViewCount = kViewIndexDLSSSrOutputSrv + 1 = 9`（`d3d12_presenter.h:220-233`），静态分配。**NVOF 不应向该 heap 追加 SRV**——它自持一个私有 `CBV_SRV_UAV` heap + 私有 root signature（转换 pass + 稠密化 pass），guest output 的既有 SRV（在 paint view heap）通过 `SetGraphicsRootDescriptorTable` 按 GPU handle 绑定即可，无需改 paint heap 尺寸。这是最小侵入设计。

### C.4 现有代码中可复用的机制

| 机制 | 位置 | 复用方式 |
|---|---|---|
| 设备/命令队列/adapter | `D3D12Provider`（`d3d12_provider.{h,cc}`）：单 direct 队列、FL11_0、`GetDevice()`/`GetDirectQueue()`、`CreateCommittedResource` 代理、`LogD3D12DebugMessages()` | NVOF 直接用 `provider.GetDevice()`/`GetDirectQueue()`，不新建设备/队列 |
| Fence 时间线 | `D3D12GPUCompletionTimeline`（`d3d12_gpu_completion_timeline.{h,cc}`）：提交索引 Fence、`SignalAndAdvance`、`AwaitSubmission` | NVOF 的 in/out fence 与其同构；会话单线程持有，无需新锁 |
| guest output 借用 | `ConsumeGuestOutput`（`presenter.cc:596`）+ `guest_output_resources_` 邮箱 | NVOF 只借"当前帧"，不接管 `paint_refs` |
| 后端状态机/SEH | `DLSSSuperSamplingBackend`（`dlss_super_sampling_backend.{h,cc}`）：`kNotInitialized/kAvailable/kUnavailable`、`__try/__except` 包装 SDK 调用、`kUnavailable` 缓存防每帧重试、一次性日志守卫、成员上传缓冲 | NVOF 会话完全照搬此风格（许可隔离：`nvOpticalFlow*`/`LoadLibrary` 仅出现在 `nvof/*`） |
| 失败回退骨架 | `d3d12_presenter.cc` 失败路径 = `command_allocator->Reset()` + 重录原始路径 | NVOF 复用同一回退骨架 |
| 配置门控 | `GuestOutputEnhancementConfig.optical_flow_enabled` + `IsOpticalFlowRequested()`（`guest_output_enhancement.h`，**已存在**） | NVOF 接线直接用既有钩子，不动 Presenter 配置面 |
| 日志 | `XELOGI/W/E/D`（`base/logging.h`） | 复用 |

### C.5 独立 Optical Flow 模块接口草案（`IOpticalFlowModule`，对照 `IDLSSSuperSamplingBackend` 风格）

```cpp
// 许可隔离：nvOpticalFlow*/LoadLibrary 仅出现在 nvof/*。
// 自持：ABGR8 帧池、flowOut/costOut、全分辨率 MV 输出、私有 view heap + root signature、NVOF 会话句柄。
// 借用：当前 guest output（只读 SRV），不接管 guest_output_resource_paint_refs。
class IOpticalFlowModule {
 public:
  // 惰性初始化（master && optical_flow 且非 1:1 时由 Presenter 触发）。
  // 录制 NvOFRegisterResourceD3D12（in/out fence）+ 首帧转换 pass 进 command_list。
  virtual BackendResult TryProcess(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* guest_output,                 // 当前帧，借读
      uint32_t input_width, uint32_t input_height,  // guest 分辨率
      const NV_OF_EXECUTE_INPUT_PARAMS_D3D12& in,   // 帧池槽位映射由模块自管
      const NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12& out,
      NV_OF_FENCE_POINT* out_fence_point) = 0;

  // 模块自持的全分辨率 MV 输出（R16G16_FLOAT，NPSR/UAV）。
  // SR 后端在 NVOF 开启时把 pInMotionVectors 指向它；关闭时用零占位。
  virtual ID3D12Resource* MotionVectorOutput() const { return nullptr; }
  virtual bool HasValidFlow() const { return false; }  // 首帧/失败为 false
  virtual void Shutdown() = 0;
  // 拆毁顺序（Veyra 实测）：unregisterAll → 释放纹理 → NvOFDestroy → FreeLibrary。
};
```

**生命周期**：`kNotInitialized → kAvailable / kUnavailable`（照搬 SR 状态机）。`kUnavailable` 缓存防每帧 SDK 重试。**错误处理**：caps 不符 fail-closed；执行失败 → 确定性回退 `kOriginalPath`，**绝不把失败标记为 `kSuccess`**（红线 2）；`BackendResult` 与 `PaintResult` 正交（复用 F3 契约）。**失败回退**：`command_allocator->Reset()` + 重录原始路径（guest output 仍在 `PSR`，原始链可用）。**默认关闭**：总开关关 → 模块指针 `nullptr` → 不加载 `nvofapi64.dll`、不建资源、不执行 GPU 步骤（结构性默认关闭，红线 1）。

### C.6 MV / 深度 / 光流输出依赖关系与待验证问题

**依赖关系**：
- **MV 来源 = NVOF 估算光流**（非游戏原生 MV，红线 3 须显式标记语义差异）。方向 `current→previous`（与 SR MV 契约一致，Veyra 实证，Xenia 需复验）。
- **MV 消费**：SR 后端 `pInMotionVectors`。当前为零占位 `motion_vectors_texture_`（R16G16F，NPSR）。NVOF 稠密化输出（R16G16_FLOAT，NPSR）**格式/状态/方向与零占位完全一致**，可直接替换绑定——这是唯一接线点。
- **深度**：保留合成零深度（Veyra 实证 SR 忽略深度、FG 几乎无响应，无实测收益）。**不引入深度模型**（方案 A）。
- **帧间关系**：NVOF 需 A(前一帧)+B(当前帧)。B=当前 guest output（借读），A=NVOF 自持 ABGR8 帧池上帧槽。**不依赖 guest output 邮箱历史帧**。
- **格式鸿沟**：guest output `R10G10B10A2` → ABGR8 转换 pass（10bit→8bit 截断损失，待量化）。

**待验证清单（UNVERIFIED，需 RTX 4090）**：
1. `nvofapi64.dll` 加载（驱动自带，Hyper-V 缺，测试机有）。
2. `NvOFGetCaps` 值（网格/宽高范围/hint 支持）与 `NvOFInit` 成功。
3. `NvOFExecuteD3D12` 连续多帧 + 稠密化 shader 运行。
4. `current→previous` 方向在 Xenia GPU/SDK 310.9.1+NVOF 5.0.7 上的匹配（Veyra 实证，需复验）。
5. R10G10B10A2→ABGR8 截断画质损失量化。
6. NVOF 会话拆毁顺序（unregister→释放→destroy→FreeLibrary）段错误防护。
7. fence 与 Xenia 现有队列/时间线耦合的同步正确性（M3 device-removed 0x887A0006 先例警示）。
8. 私有 view heap + 私有 root signature 的 NVOF 内部 pass 与 paint 命令列表共录的正确性（状态迁移顺序、无 view heap 尺寸改动）。
9. SR 后端 `pInMotionVectors` 指向 NVOF 输出 vs 零占位的 A/B 画质对比（M4-E）。
10. NVOF 许可证审计（`LicenseAgreement.pdf`，M4-A 闸门）。

---

## D. 环境限制

- 开发环境为 Hyper-V，**无 NVIDIA 驱动、无真实 GPU**：`nvofapi64.dll` 加载、`NvOFExecuteD3D12` 执行、稠密化 shader 运行、fence 实际行为、SR+NVOF 画质全部 `UNVERIFIED`，直到 RTX 4090 测试机验证。
- 本轮为**纯只读审查**，未运行构建/测试（无代码修改）。所有结论基于源码静态阅读 + SDK 头文件/文档 + Veyra 受控实验文档。
- 测试机：物理 RTX 4090，驱动 617.42，Win 10.0.26100.9549；DLSS SDK 310.9.1、NVOF SDK 5.0.7。

---

## E. 剩余风险

1. **NVOF 许可证**未审计（可能与 DLSS §4e 不同）——M4-A 第一道闸门。
2. **R10G10B10A2→ABGR8 截断损失**：10bit→8bit 是否可接受，需量化论证。
3. **驱动自带 `nvofapi64.dll` 版本漂移**：不同驱动版本 NVOF 行为可能不同。
4. **同步/生命周期**：fence 链路与 Xenia 现有队列/Fence 耦合，任何失败必须回退 `kOriginalPath`（M3 device-removed 0x887A0006 先例）。
5. **私有 view heap 共录**：NVOF 私有 root signature/heap 与 paint 命令列表共录的状态迁移顺序未经真实 GPU 验证。
6. **ABI**：NVOF 5.0.7 与未来驱动自带版本的二进制兼容性未验证。
7. **估算 MV 语义**：NVOF 流 ≠ 原生 MV，若误当原生 MV 消费会引入伪影（红线 3）。
8. **`NvOFRegisterResourceD3D12` 完整签名**：PDF 未逐参数列出（头文件 🔷 交叉核对），M4-B 实现前须对头文件复核。

---

## F. 下一步（最小且安全）

4-C 已完成并等待审核。**不进入**正式实现或上游渲染流程修改。最小安全下一步是 **M4-A 收尾 / M4-B 编译期集成**：在用户批准 C.5 的 `IOpticalFlowModule` 接口草案与 C.3 的集成点后，新建 `src/xenia/ui/nvof/`（`nvof_session.{h,cc}`、`nvof_backend.{h,cc}`、`nvof_densify.hlsl`、`CMakeLists.txt`），照搬 SR 后端的状态机/SEH/回退骨架，许可隔离 `nvOpticalFlow*`/`LoadLibrary` 于 `nvof/*`。默认关闭，行为与 M3 完全一致（无感）。零上游侵入。

---

## G. 各项能力状态清单

| 能力 | 状态 | 备注 |
|---|---|---|
| DLSS SR | 运行时已验证（RTX 4090） | 3×3 上采样；**画质未评估**（C3 悬置）；深度/MV 当前为零占位 |
| NVOF | **未实现**（4-C 完成审查） | 配置钩子 `optical_flow_enabled` 已就位；`IOpticalFlowModule` 接口草案 + 集成点已定，待 M4-B 实现 |
| 帧生成 | 未实现 | `DlssSrFrameParams` 全 0；FG 的 mvecScale 为待验证 diagnostic |
| 实验性神经渲染 | 仅研究 | 无代码；NR 的 MVec/Depth 契约已明确（RG16F + R32F 零） |
| Presenter 集成 | 已接线且关闭（默认关） | 测试配置 `dlss_enabled=true`、`dlss_sr_enabled=true`；回退已验证；NVOF 复用同一回退骨架 |
| 最终画质与性能 | 尚未评估 | M4-E 处理 |

---

## 验收条件核对

- ✅ 已形成 guest output → 候选 NVOF 调用位置的代码调用链与资源流向说明（C.1/C.2）。
- ✅ 已明确候选集成点、必要 D3D12 接口、资源状态要求及同步依赖（C.3）。
- ✅ 已识别现有可复用的设备/命令队列/资源管理/日志机制（C.4）。
- ✅ 已形成独立 Optical Flow 模块的接口草案、生命周期设计、错误处理及失败回退方案（C.5）。
- ✅ 已列出 MV、深度信息及光流输出之间的依赖关系和待验证问题（C.6）。
- ✅ 审查过程未修改 Xenia 上游渲染流程（工作区仅用户自改文档，未触碰）。

---

## H. 关键代码引用速查

| 契约点 | 文件:位置 |
|---|---|
| guest output 格式/内部态 | `xenia-canary/src/xenia/ui/d3d12/d3d12_presenter.h`（`kGuestOutputFormat` / `kGuestOutputInternalState`） |
| guest output 邮箱与双时间线 | `xenia-canary/src/xenia/ui/presenter.h`（`kGuestOutputMailboxSize=3`）、`d3d12_presenter.{h,cc}`（`guest_output_resources_` / `guest_output_resource_refresher_completion_timeline_`） |
| 生产侧 refresher（gamma/FXAA→UAV） | `xenia-canary/src/xenia/gpu/d3d12/d3d12_command_processor.cc:~2279`（`RefreshGuestOutput` 回调） |
| 消费侧 paint 路径 + SR 后端块 | `xenia-canary/src/xenia/ui/d3d12/d3d12_presenter.cc:~463`（`PaintAndPresentImpl`） |
| 邮箱消费 | `xenia-canary/src/xenia/ui/presenter.cc:596`（`ConsumeGuestOutput`） |
| SR 后端 4 段 barrier + 零占位 MV/Depth + TODO 接线点 | `xenia-canary/src/xenia/ui/dlss/dlss_super_sampling_backend.cc`（`TryProcess` / `Initialize`） |
| Fence 时间线 | `xenia-canary/src/xenia/ui/d3d12/d3d12_gpu_completion_timeline.{h,cc}` |
| 设备/队列/adapter | `xenia-canary/src/xenia/ui/d3d12/d3d12_provider.{h,cc}`（单 direct 队列、FL11_0） |
| 配置钩子 `optical_flow_enabled` / `IsOpticalFlowRequested()` | `xenia-canary/src/xenia/ui/guest_output_enhancement.h` |
| NVOF S10.5 定点 / 执行契约 | `nvidia-nvof-reference/sdk-docs/api/core.md` |
| NVOF 初始化 / 资源注册 / 释放顺序 | `nvidia-nvof-reference/sdk-docs/api/initialization.md`、`resources.md`、`dependencies.md` |
| NVOF 线程安全 / 无填充 / 8 位 cost | `nvidia-nvof-reference/sdk-docs/limitations.md` |
| Veyra MV/深度研究 | `docs/dlss-integration/milestone-4-b-veyra-mv-depth-research.md` |
| 架构设计（NVOF 独立模块 C4 / G2 方案 B） | `docs/dlss-integration/architecture-design.md` |
