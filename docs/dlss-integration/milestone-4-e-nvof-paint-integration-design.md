# Milestone 4-E — NVOF 接入上游 paint 路径与 SR 深度/MV 供给设计方案

- **状态**：**DESIGN PROPOSAL**（纯设计，未修改任何代码）。等待用户审核，未经批准不进入实现。
- **范围**：① 设计 NVOF 接入上游 paint 路径（`PaintAndPresentImpl`）的同步模型与命令编排；② 设计 MV 从 NVOF 后端交接给 DLSS SR 后端的机制；③ 确定深度信息策略；④ 明确所有权、生命周期、失败回退与红线遵守。**不修改任何 Xenia / NVIDIA 生产代码。**
- **前置**：4-B（MV/深度研究）、4-C（Xenia 代码审查与集成点）、4-D（NVOF 独立实现，主机侧全绿、真实 GPU UNVERIFIED）。
- **证据标签约定**：`STATIC_VERIFIED`（源码静态分析）、`SDK-VERIFIED`（SDK 头文件/文档）、`DESIGN PROPOSAL`（设计提案，未实施）、`UNVERIFIED`（未经真实 GPU 验证）。
- **停止点**：设计方案输出即停，等待用户审核。未经批准不进入实现，不修改上游渲染流程，不启动下一里程碑。

---

## A. 总结

NVOF 后端（4-D 已实现）产出全分辨率 MV（`R16G16_FLOAT`，current→previous，NPSR/UAV）与置信度，但与 SR 后端（M3 已验证）完全正交——SR 当前用自己的**零占位** `motion_vectors_texture_` / `depth_texture_`。本设计把两者接起来：NVOF 产出 MV，SR 消费 MV。

- **MV 交接（唯一接线点）**：NVOF 的 MV 输出与 SR 的 MV 占位在**格式（R16G16_FLOAT）、状态（NPSR/UAV）、方向（current→previous）、尺度（像素单位，MVScale=1.0）上完全一致**（`STATIC_VERIFIED`），可直接替换绑定。设计通过**扩展 `IDLSSSuperSamplingBackend::TryProcess` 接受一个可选的外部 MV 纹理**（借用、只读、`nullptr`=用内部零占位）实现交接，SR 对 NVOF 零依赖（许可隔离保持：NVOF 留在 `nvof/*`，SR 留在 `dlss/*`）。
- **深度策略**：保留**合成零深度**（`R32_FLOAT` 零占位，`STATIC_VERIFIED`）。4-B 实证 SR 完全忽略深度、FG 几乎无响应（`VEYRA-VERIFIED`），不引入深度模型（方案 A）。SR 的 `depth_texture_` 不动。
- **同步模型（核心，对 4-C 的修正）**：`SDK-VERIFIED`——`nvOFExecuteD3D12` 是**异步**调用，调用时提交到 OF 引擎自己的硬件队列，**不录进调用方命令列表**（推翻 4-C C.2"C.3 录制进命令列表"的假设）。因此 NVOF execute 与调用方 compute 队列是**跨队列**关系，必须用 fence 同步：输入 fence 由 compute 队列在转换 pass 后信号，OF 引擎等待它；输出 fence 由 OF 引擎完成时置位，compute 队列在 densify 前等待它。**本环境 SDK 的 `ID3D12GraphicsCommandList` 无 `Wait()`**（`STATIC_VERIFIED`），GPU 侧等待不可用，M4-D 用 CPU 侧等待。但 CPU 侧等待若在 `TryProcess` 内（`ExecuteCommandLists` 前）会**死锁**（OF 引擎等输入 fence，输入 fence 由尚未执行的命令列表信号）。**设计采用"先执行转换 pass 提交、再提交 NVOF execute、CPU 等待输出 fence、再执行 densify 提交"的拆分提交编排**规避死锁；若目标硬件 SDK 有 `Wait()`，则收敛为单提交 + GPU 侧等待（理想形态）。
- **红线遵守**：默认全关（`master && optical_flow && sr` 全开才执行）；任何失败确定性回退 `kOriginalPath`（`command_allocator->Reset()` + 重录原始路径），绝不把失败标记为成功；不伪造 GPU 执行；NVOF 估算 MV 与原生 MV 语义差异显式标记（红线 3）。

- **总体状态**：**DESIGN PROPOSAL**（设计完成，未实施）。

---

## B. 关键发现：NVOF 执行的异步语义（对 4-C 的修正）

**`SDK-VERIFIED`**（`nvOpticalFlowD3D12.h` `PFNNVOFEXECUTED3D12` 注释）：

> "This is **asynchronous** function call which kicks off computation of optical flow ... and **returns after submitting execute parameters to optical flow engine**."

- `nvOFExecuteD3D12(hOf, inParams, outParams)` 的签名**没有 command list 参数**（`SDK-VERIFIED`）。它在调用时把执行提交到 OF 引擎（Turing+ 硬件光流单元，独立于 graphics/compute 队列的硬件队列），随即返回。
- `NV_OF_FENCE_POINT`（`SDK-VERIFIED`，`nvOpticalFlowD3D12.h`）：`inputFencePoint` 是"GPU 等待其到达或超过 value 后再开始处理"；`outputFencePoint` 是"处理完成后把 fence 置为 value"。
- **结论**：NVOF execute 与调用方命令列表是**跨队列**关系。4-C C.2/C.3 把 `NvOFExecuteD3D12` 归为"SDK 内部（录制进命令列表）"是**误判**，本设计修正之。M4-D 的 CPU 侧等待是对该异步语义的正确响应，但其放置位置（`TryProcess` 内）在接入 paint 路径时会死锁，需重排（见 C 节）。

**对 M4-D 的连带发现**：M4-D 独立模块的 `TryProcess` 里，转换 pass（写 ABGR8 当前帧）与 `nvOFExecuteD3D12`（读 ABGR8 当前帧）在同一命令列表，但 NVOF execute 在调用时即提交到 OF 引擎、等待输入 fence。M4-D 由**测试侧（CPU）**信号输入 fence（假设输入纹理已 GPU-ready）。接入 paint 路径后，ABGR8 当前帧由**转换 pass（GPU）**产出，输入 fence 必须由 **compute 队列**在转换 pass 后信号，而非 CPU。这是同步模型必须改的第二处。

---

## C. 同步模型设计（核心决策）

### C.1 资源流向与队列归属

```
guest output N (R10G10B10A2, PSR, 借读, 由 refresher 在 direct 队列产出, paint 命令列表开始时已 GPU-ready)
   │
   ▼  [compute 队列, paint 命令列表]
   ① 转换 pass：读 guest output N (PSR) → 写 ABGR8 当前帧 (frame_pool_[N%2], UAV)
   ② (首帧) 清零 ABGR8 前帧
   ③ command_list->Signal(input_fence_, N)   ← compute 队列信号输入 fence
   │
   ▼  [OF 引擎硬件队列, 异步]
   ④ nvOFExecuteD3D12：等 input_fence_(N) → 读 ABGR8 当前 N + 前 N-1 → 写 grid flow (R16G16_SINT) + cost (R8_UINT) → 完成时置 output_fence_(V)
   │
   ▼  [compute 队列, paint 命令列表]
   ⑤ command_list->Wait(output_fence_, V)   ← compute 队列等待输出 fence（本 SDK 无 Wait，见 C.3）
   ⑥ densify pass：读 grid flow/cost (PSR) → 写全分辨率 MV (R16G16_FLOAT, UAV) + 置信度 (R8_UNORM, UAV)
   │
   ▼  [compute 队列, paint 命令列表]
   ⑦ SR EvaluateFeature：pInColor=guest output N, pInMotionVectors=⑥的 MV, pInDepth=合成零深度 → 写 SR 输出 (UAV)
   ⑧ 效果链：SR 输出 (PSR) → back buffer → Present
```

### C.2 两条 fence 的契约

| fence | 由谁信号 | 由谁等待 | 语义 |
|---|---|---|---|
| `input_fence_`（值 N） | compute 队列（转换 pass 后 `Signal`） | OF 引擎（execute 前） | ABGR8 当前帧已 GPU-ready，OF 引擎可安全读 |
| `output_fence_`（值 V） | OF 引擎（execute 完成时） | compute 队列（densify 前 `Wait`） | grid flow/cost 已写入，densify 可安全读 |

### C.3 两种实现形态（按 SDK 能力选择，均 `UNVERIFIED` 待 RTX 4090）

**形态 A（理想，单提交 + GPU 侧等待）——需 SDK 有 `ID3D12GraphicsCommandList::Wait`：**
- paint 命令列表一次性录制：`转换 pass → Signal(input_fence_, N) → [清零] → Wait(output_fence_, V) → densify → SR → 效果链`。
- `nvOFExecuteD3D12` 在录制期间提交到 OF 引擎（等输入 fence）。
- 命令列表执行时：转换 pass 信号输入 fence → OF 引擎放行 → 置输出 fence → compute 队列在 `Wait(output_fence_)` 处放行 → densify → SR → 效果链。全程单提交，无 CPU 阻塞。
- **本环境不可用**：`STATIC_VERIFIED` 本 SDK `ID3D12GraphicsCommandList` 无 `Wait()`（M4-D 已证实）。目标硬件若用标准 SDK 则适用，待真实 GPU 确认。

**形态 B（务实，拆分提交 + CPU 侧等待）——本 SDK 可用，M4-D 等待的重排版：**
- 规避死锁的关键：**NVOF execute 必须在转换 pass 已执行之后才提交**（否则 OF 引擎等输入 fence，而输入 fence 由尚未执行的命令列表信号 → 死锁）。
- 编排（paint 线程内，按序）：
  1. 录制并提交**转换提交**：`转换 pass → Signal(input_fence_, N) → [首帧清零]` → `ExecuteCommandLists` → 等其完成（compute 队列完成，输入 fence 已信号）。
  2. `nvOFExecuteD3D12`（提交到 OF 引擎；此时输入 fence 已满足，OF 引擎立即开跑）。
  3. **CPU 侧等待** `output_fence_`（`SetEventOnCompletion` + `WaitForSingleObject`，5s 超时）→ OF 引擎完成，grid flow/cost 就绪。
  4. 录制并提交**主 paint 提交**：`densify → SR → 效果链` → `ExecuteCommandLists` → Present。
- 代价：每帧多一次提交 + 一次 CPU 阻塞（OF 引擎时长）。正确（无竞态、无死锁），非最优。
- **M4-D 现状问题**：M4-D 把 CPU 等待放在 `TryProcess` 内（步骤 4 的 `ExecuteCommandLists` 之前），且 NVOF execute 也在 `TryProcess` 内提交。接入 paint 路径若照搬会死锁。**本设计要求把 NVOF execute 的提交与 CPU 等待挪到转换提交执行之后**（即把 NVOF 块从"纯录制"改成"录制-提交-等待-再录制"的三阶段）。

**设计决策**：默认按**形态 B** 实现（本 SDK 可跑），在代码里留**形态 A 的开关**（运行时检测 `command_list->Wait` 可用性；标准 SDK 上自动收敛为单提交）。两形态均 `UNVERIFIED`，需 RTX 4090 真实 GPU 验证（见 J 节）。

### C.4 与现有 paint 路径的合流点

- NVOF 块插入在 `PaintAndPresentImpl` 的**SR 后端块之前**（4-C C.2 推荐位）。两者同 direct 队列、同 paint 命令列表（形态 A）或同 paint 线程顺序提交（形态 B）。
- guest output 被 NVOF 与 SR **共同借读**（只读）：NVOF 转换 pass 以 PSR 读（保持 `PSR`），SR 后端块再做 `PSR→NPSR` barrier 读、读后迁回 `PSR`。顺序相容（NVOF 先读 PSR，SR 后迁 NPSR）。
- **不新增跨队列 Fence**：全部复用现有 `paint_completion_timeline`；NVOF 的 in/out fence 是模块自持的 `ID3D12Fence`（客户端创建），OF 引擎队列与 compute 队列间的同步由这两条 fence 承担，不引入新的 Xenia 时间线。
- **view heap 尺寸不变**：NVOF 自持私有 `CBV_SRV_UAV` heap + 私有 root signature（转换/densify pass），不向 paint 的 `kViewCount=9` view heap 追加 SRV（4-C C.3 最小侵入约束，`STATIC_VERIFIED`）。

---

## D. MV 交接机制（NVOF → SR）

### D.1 兼容性核对（`STATIC_VERIFIED`，逐项）

| 维度 | SR 占位 `motion_vectors_texture_` | NVOF `MotionVectorOutput()` | 判定 |
|---|---|---|---|
| 格式 | `R16G16_FLOAT` | `R16G16_FLOAT` | 匹配 |
| 状态 | `NON_PIXEL_SHADER_RESOURCE`（SDK 以 UAV 绑定，只读不写） | `UNORDERED_ACCESS`/NPSR（densify 以 UAV 写） | 匹配（均为 UAV 可绑定） |
| 方向 | current→previous（SR MV 契约） | current→previous（4-B `VEYRA-VERIFIED`，Xenia 需复验） | 匹配（待复验） |
| 尺度 | `InMVScaleX/Y=1.0`（像素单位） | 像素单位 | 匹配 |
| 所有权 | SR 后端自持（零占位） | NVOF 后端自持 | **需交接，不可直接写** |

### D.2 交接接口（扩展 SR 后端，保持零依赖）

- **扩展 `IDLSSSuperSamplingBackend::TryProcess`**，新增两个**可选借用输入**（`nullptr`=用内部零占位，行为与现状完全一致）：
  - `ID3D12Resource* external_motion_vectors`（借用、只读、NPSR/UAV、像素单位、current→previous）。非空且有效时，SR 把它绑定为 `pInMotionVectors`，替代内部零占位。
  - `ID3D12Resource* external_depth`（借用、只读、NPSR/UAV）。本设计恒传 `nullptr`（保留合成零深度），接口先行，为将来深度模型留口。
- **SR 后端内部**：当 `external_motion_vectors` 非空时，`eval_params.pInMotionVectors = external_motion_vectors`（否则 `motion_vectors_texture_` 零占位）。**不**改 SR 自持零占位的所有权；外部 MV 是借读，SR 不接管、不销毁。
- **有效性门控**：presenter 仅在 `nvof_backend_->HasValidFlow()`（非首帧、无失败）时传 `nvof_backend_->MotionVectorOutput()`；否则传 `nullptr`（SR 回退零占位）。首帧 MV 无效（无参考帧）→ 零占位，符合红线 3（不用无效 MV 冒充真实运动）。
- **许可隔离保持**：SR 后端（`dlss/*`）只见 `ID3D12Resource*`，不见任何 `NvOF*` 类型；NVOF 后端（`nvof/*`）不见任何 `NVSDK_NGX_*` 类型。两者通过 presenter 的 `IOpticalFlowBackend::MotionVectorOutput()` → `IDLSSSuperSamplingBackend::TryProcess(...)` 交接，**模块间零直接依赖**。

### D.3 为何不选其他交接方式

- **不**让 NVOF 直接写 SR 的 `motion_vectors_texture_`（违反所有权：该纹理是 SR 自持私有成员）。
- **不**引入 Veyra 式 `GuidanceFrame` 中介对象（过重；本场景只有一个消费方 SR，直接传纹理指针即可）。
- **不**改 `SelectOutputPath` 的纯函数签名去传 MV（它是路径选择 + 回退流的纯逻辑，不应承载 GPU 资源交接；MV 交接放在 `TryProcess` 层更清晰）。

---

## E. 深度策略

- **保留合成零深度**（`R32_FLOAT` 零占位，`STATIC_VERIFIED`）。SR 后端 `depth_texture_` 不动，`pInDepth` 恒指向它。
- 依据：4-B `SR_FG_DEPTH_RESPONSE` 受控实验实证 **SR 完全忽略深度、FG 对深度几乎无响应**（`VEYRA-VERIFIED`），不足以支持引入深度模型（方案 A，`DESIGN PROPOSAL`）。
- **不**引入 DAV2 单目深度（方案 B，成本高、FG 无实测收益，低优先级）；**不**引入 guest 真实深度（方案 C，guest 投影矩阵在 guest 内存，成本高、不确定）。
- D.2 的 `external_depth` 接口先行（恒 `nullptr`），为将来深度模型留口，本里程碑不实现。

---

## F. paint 路径接线（`PaintAndPresentImpl`）

插入位置：`d3d12_presenter.cc` 的 **SR 后端块之前**（4-C C.2），同 direct 队列。伪代码（`DESIGN PROPOSAL`）：

```
// 既有：读 GuestOutputEnhancementConfig（guest_output_enhancement_config_mutex_ 保护）
bool sr_requested = config.IsSuperSamplingRequested() && !is_1to1;
bool of_requested = config.IsOpticalFlowRequested() && !is_1to1;

// --- NVOF 块（of_requested 才执行；默认关） ---
ID3D12Resource* mv_for_sr = nullptr;
if (of_requested) {
  // 惰性创建 NVOFOpticalFlowBackend（master && optical_flow）。
  // 形态 B：
  //   1. 录制并提交转换提交（转换 pass + Signal(input_fence_) + [首帧清零]），等其完成。
  //   2. nvOFExecuteD3D12（提交到 OF 引擎）。
  //   3. CPU 等待 output_fence_。
  //   4. （densify 并入下面的主 paint 提交）
  // 形态 A：录制 转换 pass + Signal + Wait + densify 进主 paint 命令列表，提交 NVOF execute。
  BackendResult of_result = nvof_backend_->TryProcess(command_list, guest_output_resource.Get(), frame_index);
  if (of_result == BackendResult::kSuccess && nvof_backend_->HasValidFlow()) {
    mv_for_sr = nvof_backend_->MotionVectorOutput();   // 有效 MV，交给 SR
  } else {
    // NVOF 失败：确定性回退。SR 仍可用零占位 MV 跑（NVOF 不是 SR 的硬依赖）。
    // 或整帧回退原始路径（视配置语义，见 H 节）。
    mv_for_sr = nullptr;
  }
}

// --- SR 块（既有，扩展 TryProcess 传 mv_for_sr / external_depth=nullptr） ---
if (sr_requested) {
  backend_result = SelectOutputPath(config, dlss_sr_backend_.get(), command_list,
      guest_output_resource.Get(), out_w, out_h, guest_output_properties,
      frame_params, mv_for_sr, /*external_depth=*/nullptr, &fallback_flow);
  ...
}
```

- **NVOF 与 SR 正交**：`of_requested` 与 `sr_requested` 独立门控。NVOF 失败不强制 SR 失败（SR 可退回零占位 MV）；SR 失败回退原始路径（既有逻辑）。
- **默认全关**：`master_enabled=false` → `of_requested=false` 且 `sr_requested=false` → 不加载 `nvofapi64.dll`、不建 NVOF 资源、不执行 GPU 步骤（结构性默认关，红线 1）。
- **frame_index 来源**：NVOF 需要单调递增的帧号（区分首帧、轮转帧池槽）。presenter 持一个 `uint64_t nvof_frame_counter_`，每次成功 paint +1（场景切换 / `in_reset` 时归零，触发 NVOF 首帧语义）。

---

## G. 所有权与生命周期

| 资源 | 所有者 | 借用方 | 备注 |
|---|---|---|---|
| guest output (R10G10B10A2) | Xenia 邮箱 `guest_output_resources_` | NVOF（转换 pass，PSR 读）、SR（NPSR 读） | 两模块都只借读，**不接管** `guest_output_resource_paint_refs` |
| ABGR8 帧池 (2×B8G8R8A8) | NVOF 后端 | —（NVOF 内部） | 轮转 2 槽（M4-D；4-B 建议 4-6 池，见 J） |
| grid flow / cost | NVOF 后端 | —（NVOF 内部） | densify 读 |
| 全分辨率 MV (R16G16_FLOAT) | NVOF 后端 | **SR（借读，pInMotionVectors）** | D 节交接点 |
| 置信度 (R8_UNORM) | NVOF 后端 | —（本里程碑仅验证用） | 将来 FG/NR 用 |
| input/output fence | NVOF 后端 | OF 引擎 / compute 队列 | C 节同步 |
| 私有 view heap + root signature | NVOF 后端 | —（NVOF 内部 pass） | 不污染 paint view heap |
| SR 输出 (R10G10B10A2) | SR 后端 | 效果链（PSR 读） | 既有 |
| SR 零占位 depth / MV | SR 后端 | —（SR 内部） | `external_*` 为 null 时用 |

**生命周期顺序（拆毁，`STATIC_VERIFIED`，4-B/4-D 已验证顺序）**：presenter 先 `nvof_backend_->Shutdown()`（`UnregisterResources` → 释放 NVOF 纹理 → `DestroySession` → `FreeLibrary`）→ 再 `dlss_sr_backend_->Shutdown()`。NVOF 已注册的纹理**必须**在 `nvOFDestroy` + `FreeLibrary` 之前释放（否则段错误，Veyra 实测）。

---

## H. 失败回退

- **NVOF 失败**（`kUnavailable`/`kInitFailed`/`kInputContractFailed`/`kSyncConditionFailed`/`kExecutionFailed`/`kOutputInvalid`）：
  - `HasValidFlow()` 为 false → `mv_for_sr=nullptr` → **SR 退回零占位 MV**（NVOF 非 SR 硬依赖），SR 照常跑。这是**确定性回退**，绝不把 NVOF 失败标记为成功。
  - 可选更保守语义：NVOF 失败则整帧回退 `kOriginalPath`（`command_allocator->Reset()` + 重录原始路径）。**设计决策**：默认"SR 退回零占位 MV"（NVOF 失败不拖累 SR 画质基线），配置项 `nvof_fail_fallback_to_original` 可选更保守语义。
  - `kUnavailable`（运行时缺失 / capability 失败）缓存，后续帧不重试 SDK（照搬 SR 状态机）。
- **SR 失败**：既有逻辑，`SelectOutputPath` 返回非 `kSuccess` → `command_allocator->Reset()` + 重录原始路径（`kOriginalPath`）。
- **设备丢失 / 同步超时**：CPU 等待 `output_fence_` 5s 超时 → `kSyncConditionFailed` → NVOF 失败路径（SR 退回零占位）。设备丢失走 Xenia 既有 device-removed 处理（M3 0x887A0006 先例）。
- **绝不**：把失败标记为 `kSuccess`；用 Mock 伪装真实 GPU 执行；用零占位 MV 冒充 NVOF 估算 MV（红线 2/3）。

---

## I. 红线遵守

| 红线 | 本设计如何遵守 |
|---|---|
| 1. 默认全关 | `master && optical_flow && sr` 全开才执行；关 → 不加载 DLL、不建资源、不执行 GPU 步骤（结构性默认关） |
| 2. 确定性回退 `kOriginalPath` | 任何失败 → SR 退回零占位 MV 或整帧回退原始路径；`command_allocator->Reset()` 重录 |
| 3. 不伪造 GPU / MV | NVOF 估算 MV 显式标记为非原生 MV（红线 3）；首帧 / 失败用零占位，不冒充；不 Mock |
| 4. 未真实验证不宣称完成 | 本设计所有执行项 `UNVERIFIED`；真实 GPU 在 RTX 4090 验证前不宣称 NVOF 执行 / MV 有效性 / 画质完成 |

---

## J. 待验证清单（`UNVERIFIED`，需 RTX 4090）

1. **NVOF execute 异步语义实锤**：确认 `nvOFExecuteD3D12` 确实提交到独立 OF 引擎队列（非调用方命令列表）；确认形态 A（GPU 侧 `Wait`）在目标 SDK 是否可用。
2. **输入 fence 由 compute 队列信号**：转换 pass 后 `command_list->Signal(input_fence_, N)`，OF 引擎正确等待（无竞态读未就绪 ABGR8）。
3. **输出 fence 同步**：形态 A 的 `command_list->Wait(output_fence_, V)` 或形态 B 的 CPU 等待，densify 正确读到已写入的 grid flow/cost。
4. **方向复验**：current→previous 在 Xenia RTX 4090 + SDK 310.9.1/NVOF 5.0.7 上匹配（4-B `VEYRA-VERIFIED`，Xenia 需复验）。
5. **MV 交接 A/B 画质**：SR 用 NVOF MV vs 零占位 MV 的 A/B 对比（M4-E 核心验收）。
6. **R10G10B10A2→ABGR8 截断损失**：10bit→8bit 量化对 SR 画质影响量化。
7. **帧池槽数**：M4-D 用 2 槽轮转；4-B 准则 5 建议 4-6 槽饱和硬件。2 槽是否足够（首帧 / 快速场景切换下）待验证。
8. **拆毁段错误防护**：`UnregisterResources` → 释放纹理 → `DestroySession` → `FreeLibrary` 顺序在 Xenia 设备/驱动下无段错误。
9. **fence 与 Xenia 队列/时间线耦合**：in/out fence 与现有 `paint_completion_timeline` / refresher 时间线无死锁、无竞态。
10. **NVOF 许可证**：`LicenseAgreement.pdf` 未审计（M4-A 闸门）。

---

## K. 各项能力状态清单

- **DLSS SR**：运行时已验证（M3-B，RTX 4090）。本设计扩展其 `TryProcess` 接受外部 MV/深度（`DESIGN PROPOSAL`，未实施）。
- **NVOF**：编译期已集成（4-D）；执行 `UNVERIFIED`。本设计给接入 paint 路径的同步模型与 MV 交接（`DESIGN PROPOSAL`，未实施）。
- **帧生成**：未实现（本设计不触碰）。
- **实验性神经渲染**：仅研究（本设计不触碰）。
- **Presenter 集成**：本设计给出接线方案（NVOF 块在 SR 块前），**未实施**（待审核 + 真实 GPU）。
- **最终画质与性能**：尚未评估（M4-E 实现后 A/B 对比）。

---

## L. 下一步（最小且安全）

本设计**输出即停**，等待用户审核。审核通过后，最小安全实现步骤：
1. 扩展 `IDLSSSuperSamplingBackend::TryProcess`（+`external_motion_vectors`/`external_depth`，`nullptr`=零占位），SR 后端内部按有效性门控绑定。
2. 改 NVOF 后端 `TryProcess` 的 fence 编排（形态 B：转换提交→NVOF execute→CPU 等待→主 paint 提交；留形态 A 开关）。
3. 在 `PaintAndPresentImpl` 的 SR 块前插入 NVOF 块（惰性创建、frame_index、`mv_for_sr` 交接、失败回退）。
4. 默认全关，行为与 M3 完全一致（无感）。零上游侵入。
5. 真实 GPU 验证（J 节 1-9）→ M4-E A/B 画质对比。

在此之前**不**修改上游渲染流程、**不**集成 SR/FG、**不**启动下一里程碑。
