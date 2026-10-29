# Milestone 6-A：实验性神经渲染（NR / DLSS 5）实现计划

- 日期：2026-10-28
- 依据：`milestone-6-a-NR_FEASIBILITY_REPORT.md`（结论：有条件可行）与 `architecture-design.md` K4.3.3
- 状态：**待用户批准**。本计划描述 6-B 的实施范围，批准前不产生任何代码变更。
- 范围限定：最小化单图层 NR 调用路径（Feature 18，1:1，零深度，NVOF/零运动）。**不含**多图层栈叠、AMD Lmxxf 变体、Ampere 架构改写、NR 强度保护/输出稳定化、Veyra 的 `nrAutoPool` 自适应层。

---

## 1. 分阶段任务与依赖顺序

| 阶段 | 任务 | 依赖 | 产出 |
|---|---|---|---|
| B0 | 实施前复核（K4.3.3 规定动作） | — | 复核记录（HEAD/工作区/SDK/6-A 一致性/构建基线） |
| B1 | 最小化接口与配置 | B0 | `INeuralRenderingBackend` 接口、`NrFrameParams`、NR 配置扩展、路径选择函数、host-side 单测 |
| B2 | 运行时加载器与适配器 | B1 | `nr_runtime_adapter`（DLL 加载、版本锁定、IAT 垫片、SEH 包裹）、`nvngx_dlssnr_backend` 的 Init/Create/Release/Shutdown 真实接通 |
| B3 | Parity 编解码 | B1（与 B2 并行可） | parity 数学库（CPU 参考 + GPU compute pass）、数值比对单测 |
| B4 | 每帧求值接通 | B2 + B3 | `TryProcess` 真实求值、输出验证、Presenter 接入、回退接通 |
| B5 | 安全回退与状态恢复完善 | B4 | 全部故障分类覆盖、连续失败禁用、设备丢失契约、日志限流 |
| B6 | 测试机验证与报告 | B5（4090） | GPU 验证记录、实施报告、已知问题、依赖清单更新 |

依赖图：B0 → B1 → {B2, B3} → B4 → B5 → B6。B2 与 B3 互不依赖，可并行。

---

## 2. 目标文件与具体修改范围

**新增（全部在 `src/xenia/ui/dlss/nr/` 下，除注明外）：**
- `nr/neural_rendering_backend.h` / `.cc` — `INeuralRenderingBackend` 实现（自持输出纹理、零深度/零运动占位符、运行时句柄、参数块）。
- `nr/nr_runtime_adapter.h` / `.cc` — 私有运行时加载与调用（版本锁定、IAT 垫片、SEH 包裹、导出解析）。
- `nr/nr_parity.h` / `.cc` — parity 编解码数学（CPU 双精度参考 + 供 compute pass 使用的常量/系数）。
- `nr/nr_parity_encode.hlsl` / `nr_parity_decode.hlsl` — compute 着色器（净室重实现，以 Veyra CPU 参考为黄金标准）。
- `nr/CMakeLists.txt` — 模块构建（沿用 `sr/`、`fg/` 的 CMake 模式）。
- `testing/neural_rendering_backend_test.cc` — host-side 单测（stub 后端 + 路径选择 + parity 数值比对）。

**修改（最小化，逐文件列出）：**
- `src/xenia/ui/guest_output_enhancement.h` — 新增 `INeuralRenderingBackend` 接口声明与 `NrFrameParams` 结构（旁挂，不改动现有类型）；**不改动** `BackendResult`、`GuestOutputEnhancementConfig` 既有字段语义。
- `src/xenia/ui/d3d12/d3d12_presenter.h` / `.cc` — 新增 NR 后端成员与生命周期钩子、NR 输出接入效果链的路径选择调用（与 SR/FG 接线模式一致）。
- `src/xenia/ui/dlss/CMakeLists.txt` — 挂载 `nr` 子目录。
- `src/xenia/app/emulator_window.cc` — NR 子开关的配置读取/设置（沿用现有增强配置传递模式）。
- `src/xenia/ui/testing/guest_output_enhancement_test.cc` — 追加 NR 默认关闭与路径选择断言。
- `src/xenia/ui/d3d12/d3d12_presenter.cc` 的原始路径回退逻辑 — **不改动**（`BuildOriginalPathFallbackFlow` 保持不变）。

**明确不改**：`src/xenia/ui/dlss/sr/*`、`src/xenia/ui/dlss/nvof/*`、`src/xenia/ui/dlss/fg/*`、`src/xenia/ui/dlss/ngx/*`（除 `ngx_core_host` 若需共享导出解析的纯增量扩展）、`guest_output_resource_paint_refs` 所有权链、Presenter 的 Fence/队列同步骨架。

---

## 3. 接口、资源与状态设计

### 3.1 接口（`INeuralRenderingBackend`，旁挂于 `guest_output_enhancement.h`）

- 复用既有九状态 `BackendResult`（决策 4，语义不变）。
- `IsAvailable()` / `IsUnavailable()` — 运行时存在性与能力。
- `TryProcess(command_list, guest_output_or_work_texture, motion_vectors_or_null, frame_params)` — 记录 encode + Evaluate + decode 到调用方命令列表；返回九状态之一。
- `OutputResource()` / `OutputWidth()` / `OutputHeight()` — 后端自持输出纹理（1:1 分辨率）与尺寸。
- `ZeroDepthPlaceholder()` / `ZeroMotionVectorPlaceholder()` — 与 FG 相同的占位符契约。
- `Shutdown()` — 释放后端资源、ReleaseFeature、Shutdown1、卸载 DLL（反向顺序）。
- 所有权契约（与 SR/FG 一致）：**借用** guest 输出与运动矢量（只读）；**自持**输出纹理、零占位符、feature handle、参数块、运行时 DLL 句柄。绝不接管 `guest_output_resource_paint_refs`。

### 3.2 每帧参数（`NrFrameParams`）

- `in_reset`（bool）：重启用/场景切换时置 true → `DLSSNR.Reset = 1`。
- `frame_id`（uint64）：单调递增，诊断用。
- `render_width` / `render_height`（uint32）：NR 工作分辨率（1:1）——SR 启用时为 SR 输出分辨率，否则为 guest 输出分辨率（`757925f` 已移除 1:1 门控，后端按真实请求尺寸自行判定，NR 的 InputWidth==OutputWidth 语义不变）。
- 样式参数（Style/Intensity/Tone/Structure/Skin/AutoMask/UICorrection）：B1 阶段仅暴露 `Style` 与 `Intensity`，默认值与 Veyra 产品路径一致（style=0, intensity=1.0）；其余参数在 B1 保持内部常量，不进入配置层。

### 3.3 资源配置

- 现有 `GuestOutputEnhancementConfig` 的 `experimental_neural_rendering_enabled` 与 `IsExperimentalRequested()` **直接复用**，不新增字段。
- 若需运行时变体选择（nr-ampere vs nr-original），B1 不暴露配置：4090 固定 nr-ampere（由 B0 的硬件判定决定），避免配置膨胀。

### 3.4 资源与生命周期

- 后端创建顺序：DLL 加载 + 版本核对 → IAT 垫片安装 → Init_Ext → 零占位符纹理 → CreateFeature（独立命令列表 + submit + waitIdle）→ 就绪。
- 每帧：命令列表记录 encode → Evaluate → decode → 输出提交。
- 销毁顺序（严格反向）：ReleaseFeature → Shutdown1 → IAT 垫片还原 → FreeLibrary。
- 单 DLL 单会话：进程内仅一个适配器会话（Veyra 契约），Presenter 保证单实例。

---

## 4. 初始化、调用、同步、输出提交与销毁流程

1. **初始化**：B0 判定硬件（4090 → nr-ampere 路径）→ 加载器按 manifest SHA256 + fileVersion 310.9.16.0 锁定 DLL → 五个导出全部解析，缺一即 `kUnavailable` → Init_Ext（SEH 包裹）→ CreateFeature（1:1 参数集，`Upscaling=0`、`Scale=1.0`、`PerfQualityValue=1`、NodeMasks=1、preset 按 Veyra SF 规则）→ submit + waitIdle → 成功则 `kSuccess` 语义进入就绪态。
2. **每帧调用**：输入校验（非空、尺寸匹配、状态正确）→ 零占位符或 NVOF 运动矢量绑定 → `DepthInverted=1`、`MVecScale=1.0`、`Enabled=1`、`Reset` 按 `in_reset` → encode pass → Evaluate（SEH 包裹）→ decode pass → 输出验证（纹理存在、尺寸正确、状态可用）→ 返回 `kSuccess`。
3. **同步**：单队列记录（与 SR/FG 相同契约）；创建期 waitIdle；求值期不引入额外 Fence 等待（调用方按既有命令列表完成 Fence 等待）。状态迁移：输入 NPSR，输出 UAV→NPSR，屏障顺序与 Veyra 单图层路径一致。
4. **输出提交**：仅当 `TryProcess` 返回 `kSuccess` 时，Presenter 将 NR 输出纹理纳入效果链（kBilinear 呈现）；否则使用既有有效输出路径。
5. **销毁**：严格反向顺序；设备丢失时遵循 Xenia 既有设备恢复契约（不盲目继续提交）。

---

## 5. 故障分类与安全回退设计

故障分类（全部映射到九状态 `BackendResult`，无新增枚举）：

| 故障 | 状态 | 回退行为 |
|---|---|---|
| NR 未启用 / 主开关关 | `kNotRequested` | 原始路径，无 DLL 加载 |
| DLL 缺失 / 版本不匹配 / 导出缺失 | `kUnavailable` | 原始路径；后端进入不可重试态 |
| Init_Ext / CreateFeature 失败 / SEH 异常 | `kInitFailed` | 原始路径；后端进入不可重试态；DLL 反向卸载 |
| 输入尺寸/格式/状态不匹配 | `kInputContractFailed` | 本帧原始路径；后端保持就绪（下帧可重试） |
| 状态迁移失败 / 队列超时 | `kSyncConditionFailed` | 本帧原始路径；后端保持就绪 |
| Evaluate 失败 / SEH 异常 | `kExecutionFailed` | 本帧原始路径；连续 N 次（N=3）后禁用 NR 进入不可重试态 |
| 输出缺失/无效 | `kOutputInvalid` | 本帧原始路径；连续 N 次后禁用 |
| 设备丢失 | `kInitFailed`（语义：后端不可恢复） | 遵循 Xenia 设备恢复契约；NR 禁用 |
| 历史状态不一致 | `kInputContractFailed` | 置 `in_reset=true` 重启用；原始路径本帧 |

回退硬约束：
- 回退路径**恒为** `effect_count >= 1` 且首效果 `kBilinear`（`BuildOriginalPathFallbackFlow`，决策 5）；**严禁** `effect_count = 0`。
- NR 失败不得破坏原始输入或既有有效输出（借用语义保证）。
- 连续失败禁用后，仅当用户显式重新启用 NR 子开关时重置（不自动重试，避免每帧重复失败日志）。
- 故障日志限流：同一错误类型每帧至多一条，禁用后不再输出。
- 不得因单帧失败导致永久卡死、资源泄漏或持续使用损坏的后端状态（销毁顺序 + 不可重试态保证）。

---

## 6. 测试计划

### 6.1 单元测试（host-side，本地可执行）
- 配置默认关闭：`experimental_neural_rendering_enabled == false`，`IsExperimentalRequested() == false`，NR 后端不创建、不加载 DLL。
- 路径选择：`kNotRequested` / `kSuccess` / 各失败状态 → 回退 flow 恒为 `effect_count=1`、`effects[0]=kBilinear`。
- Parity 数值比对：CPU 参考实现与 Veyra `RenoDxParityCodec` 黄金参考在代表性输入集（线性 BT.709 全范围、肩点附近、负值钳位、alpha 透传）上逐值比对（double 精度，容差 0）。
- 状态机：连续失败计数、不可重试态、重启用重置、日志限流。
- 占位符契约：零深度/零运动占位符尺寸与格式。

### 6.2 集成测试（host-side，stub 后端）
- stub 后端返回各 `BackendResult` → Presenter 路径选择正确。
- 资源所有权：后端 Shutdown 后无悬空引用（ASan/泄漏检测）。
- 设备丢失契约：模拟设备移除 → NR 禁用、呈现路径有效。

### 6.3 回归测试
- 现有 SR/NVOF/FG 单测全量通过（不改动其代码，但运行以确认无副作用）。
- 原始路径回退回归：NR 关闭时 Presenter 行为与 6-B 前逐字节一致（效果链、Fence 时序）。

### 6.4 GPU 测试（4090，测试机）
- 运行时加载：版本核对、导出解析、IAT 垫片安装/还原、Init_Ext、CreateFeature。
- 单帧求值：零运动/零深度 → 输出非空、尺寸正确、无 NaN/Inf。
- 连续帧：历史稳定（无闪烁）、`in_reset` 时序正确。
- NVOF+NR 联调：真实运动矢量输入，对比零运动输出的差异合理性。
- 回退路径：注入 Evaluate 失败（Veyra 环境变量模式或 stub）→ 回退有效、无卡死。
- 显存/延迟：DLL 映射 + 模型驻留显存、单帧 GPU 延迟（encode+Evaluate+decode）。
- 画质：与原始路径的 A/B 对比（人工评估 + 无伪影检查）。
- 崩溃隔离：SEH 包裹有效性（注入非法调用验证进程存活）。
- 每项测试标注：前置条件、步骤、预期、通过标准、无法执行时的报告要求。

---

## 7. 各任务验收标准

- **B0**：HEAD/工作区/SDK 版本与 6-A 报告一致；构建基线（release + tests）记录在案；P0 阻断项解除确认。
- **B1**：接口与配置合入，host-side 单测全绿，默认关闭行为不变（现有测试不回归）。
- **B2**：DLL 加载/版本锁定/垫片/SEH 在 4090 上通过加载探针；导出缺失 → `kUnavailable`；卸载无泄漏。
- **B3**：parity CPU 参考与 Veyra 黄金参考数值一致（容差 0）；compute pass 在 4090 上输出与 CPU 参考逐值一致（FP 容差内）。
- **B4**：单帧求值在 4090 上成功，输出通过验证，Presenter 接入生效，回退接通。
- **B5**：全部故障分类覆盖，连续失败禁用、设备丢失契约、日志限流验证通过。
- **B6**：GPU 测试全项执行，实施报告 + 已知问题 + 依赖清单更新合入。

---

## 8. 回滚方案

- B1-B5 全部新增代码在 `src/xenia/ui/dlss/nr/` 独立目录 + 旁挂修改。回滚 = 删除 `nr/` 目录 + 还原 `guest_output_enhancement.h`、`d3d12_presenter.h/.cc`、`CMakeLists.txt`、`emulator_window.cc`、测试文件的旁挂修改。
- 回滚后 NR 子开关不存在，Presenter 行为回到 6-B 前状态（原始路径 + SR/NVOF/FG 不变）。
- 每次合入前保存独立提交（不 squash），便于按阶段回滚。
- 运行时 DLL 与 manifest 条目独立于代码回滚（删除 `nr/ampere` 部署目录即可完全移除运行时依赖）。

---

## 9. 必须由用户批准的决策点

1. **P0-1 运行时身份**：批准在 Xenia 本地研究中使用未签名社区 ShortFuse 310.9.16 运行时 + IAT 垫片（`GetModuleFileNameW` 伪装为 `nvngx.dll`）+ SEH 包裹。分发仍禁止（决策 6）。**拒绝则 6-B 不启动。**
2. **Parity 净室重实现**：批准以 Veyra CPU 参考为黄金标准、Xenia 净室重实现编解码数学（不复制 GPL 代码）。
3. **范围确认**：批准最小化单图层方案（不含栈叠/AMD/Ampere 改写/强度保护）。
4. **连续失败禁用阈值**：N=3 帧后禁用 NR（可调整，需确认）。
5. **样式参数暴露范围**：B1 仅暴露 Style/Intensity，其余参数内部常量（避免配置膨胀）。
6. **本地研究限定**：6-B 全部工作不进入公开 Xenia 仓库、不分发 DLL（决策 6 维持）。

---

## 10. 与 6-A 可行性报告的对应关系

- P0-1（许可/运行时）→ 决策点 1。
- P0-2（parity 契约）→ 决策点 2 + B3 验收标准。
- P0-3（真实 GPU）→ B6 + 6.4 节。
- P1-1（ABI 稳定性）→ B2 版本锁定。
- P1-2（IAT 重放）→ B2 加载探针。
- P1-3（显存）→ 6.4 显存测量。
- P1-4（Playbook 残余）→ 6.4 子矩形/边缘参数核对（如需要）。
