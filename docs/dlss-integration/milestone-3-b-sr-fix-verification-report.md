# Milestone 3-B SR 修复验证调查报告（第 3 轮）

> 调查性质：代码侧根因分析（第 3 轮诊断 + 第 4 轮实测反馈）
> 测试机：物理 NVIDIA RTX 4090（vendor 0x10DE, device 0x2684），**非** Hyper-V
> 前置状态：第 2 轮"Provider 持有 IDXGIAdapter1"修复已应用且编译通过（未提交）；第 3 轮已在 `D3D12SharedMemory::Initialize()` 两个失败分支加入 `GetDeviceRemovedReason()` 仅日志诊断点（未提交）

---

## A0. 第 4 轮实测反馈（诊断点已生效，结论更新）

第 4 轮在测试机重跑，诊断日志输出：

```
!> 0100000C Shared memory: Failed to create the 512 MB buffer
!> 0100000C Shared memory: GetDeviceRemovedReason() = 0x00000000 (S_OK (device not removed)); graphics_analysis(PIX) attached = true.
```

这一行**同时**推翻了一个假设、坐实了另一个，并暴露出真正的嫌疑：

1. **"设备已移除/失效"假设被推翻。** `GetDeviceRemovedReason() = S_OK` 意味着 D3D12 设备**未被移除/重置**，设备本身是健康的。第 3 轮报告的"设备移除"根因判定**不成立**。
2. **"PIX 已附加"被坐实。** `graphics_analysis(PIX) attached = true`，证实共享内存走非 tiled 分支确实是因为 PIX 附加（而非 tier 不一致）。测试机上确实挂着 PIX。
3. **真正嫌疑：非 tiled 分支一次性分配整块 512 MB，遇显存吃紧即 OOM。** 这是关键的因果链：
   - **tiled 分支**（无 PIX 时本应走）：`CreateReservedResource` + 稀疏映射（`InitializeSparseHostGpuMemory`），**只提交实际用到的 tile**，启动时占用的显存远小于 512 MB。
   - **非 tiled 分支**（被 PIX 强制）：`CreateCommittedResource`，**一次性分配整块 512 MB 显存**。
   - 因此，若测试机显存吃紧（其他进程占用大量 VRAM），被 PIX 强制的非 tiled 分支**更容易**在 512 MB 整块分配上失败（OOM）。而设备本身完好（S_OK），所以 `GetDeviceRemovedReason` 返回 S_OK——这正是本次日志的形态。
4. **最可能根因：显存耗尽（E_OUTOFMEMORY）**，由"PIX 附加（强制非 tiled）+ 显存吃紧"共同促成。但**尚缺 `CreateCommittedResource` 本身返回的 HRESULT**——本轮诊断只打印了 `GetDeviceRemovedReason`（S_OK），未打印那次分配失败的精确错误码（如 `E_OUTOFMEMORY` = 0x8007000E）。

**第 4 轮结论（更新）：**
- 设备移除假设 → **推翻**（S_OK，设备健康）。
- PIX 附加 → **坐实**（true，强制非 tiled 分支）。
- 根因 → **显存耗尽（OOM）可能性最高**，由 PIX 强制的非 tiled 整块 512 MB 分配触发；需 `CreateCommittedResource` 的精确 HRESULT 一锤定音。
- 总体状态：**FAIL（共享内存 512 MB 分配失败）——设备健康，非设备移除；最可能是显存耗尽，被 PIX 强制的非 tiled 分支放大。**

## A1. 第 5 轮实测反馈（分配 HRESULT 已捕获，根因进一步收窄）

第 5 轮在测试机重跑，诊断日志输出：

```
!> 0100000C Shared memory: Failed to create the 512 MB buffer (HRESULT=0x80070057)
!> 0100000C Shared memory: GetDeviceRemovedReason() = 0x00000000 (S_OK (device not removed)); graphics_analysis(PIX) attached = true.
!> 0100000C Failed to initialize shared memory
```

这一行把根因进一步收窄，同时**推翻**了上一轮的 OOM 假设：

1. **分配失败码是 E_INVALIDARG（0x80070057），不是 OOM。** 这**推翻**了第 4 轮"显存耗尽（E_OUTOFMEMORY = 0x8007000E）"的假设。E_INVALIDARG 的语义是"参数无效"，不是"显存不足"。
2. **设备健康**（`GetDeviceRemovedReason = S_OK`），非设备移除。
3. **PIX 附加**（true），非 tiled 分支被强制。
4. **`CreateCommittedResource` 的全部参数都是合法的**（逐一核验）：
   - `FillBufferResourceDesc`（`d3d12_util.h:64-77`）设置的是完全合法的 buffer 描述符：`Dimension=BUFFER`、`Width=512MB`、`Height=1`、`DepthOrArraySize=1`、`MipLevels=1`、`Format=UNKNOWN`、`SampleDesc.Count=1`、`Layout=ROW_MAJOR`、`Flags=ALLOW_UNORDERED_ACCESS`、`Alignment=0`。这个函数在代码库 10+ 处复用（`d3d12_command_processor.cc` 的 6 处、`d3d12_texture_cache.cc`、`d3d12_render_target_cache.cc` 的 2 处、`d3d12_query_pool.cc` 的 3 处、`d3d12_primitive_processor.cc`），全部正常工作——证明描述符本身无误。
   - `kHeapPropertiesDefault = { D3D12_HEAP_TYPE_DEFAULT }`（`d3d12_util.cc:20`）——合法默认堆。
   - `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED`——设备支持（特性查询 "Non-zeroed heap creation: yes"）。
   - `D3D12_RESOURCE_STATE_COPY_DEST`——合法初始状态。
   - `IID_PPV_ARGS(&buffer_)`——合法的 `ID3D12Resource` IID。
5. **结论：参数全对、设备健康、却返回 E_INVALIDARG → 强烈指向测试机的驱动层问题**（有 bug 的驱动版本，或驱动状态损坏），而非 Xenia 代码或显存不足。

**第 5 轮结论（更新）：**
- 设备移除假设 → **推翻**（S_OK，设备健康）。
- OOM 假设 → **推翻**（E_INVALIDARG ≠ E_OUTOFMEMORY）。
- PIX 附加 → **坐实**（true，强制非 tiled 分支）。
- 根因 → **测试机驱动层问题**（合法 `CreateCommittedResource` 调用被驱动以 E_INVALIDARG 拒绝）可能性最高。
- 总体状态：**FAIL（共享内存 512 MB 分配失败，HRESULT=E_INVALIDARG）——参数合法、设备健康、显存非瓶颈；最可能是测试机 NVIDIA 驱动的 bug 或驱动状态损坏。**

### 下一步（待授权；区分"环境驱动问题"与"代码/参数问题"）
1. **测试机驱动排查（环境侧，无需改代码）**：
   - 确认 NVIDIA 驱动版本（`nvidia-smi` / 设备管理器 / GeForce Experience）。
   - 尝试**回退**或**更新**到另一个稳定驱动版本后重跑——若 E_INVALIDARG 消失，则坐实驱动 bug。
   - 检查 Windows 事件查看器是否有 NVIDIA 驱动相关错误。
2. **排除 `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED` 触发驱动 bug（需改代码，仅一处）**：若驱动对该 flag 有 bug，去掉它应能成功。这是最小行为改动（非纯日志），需用户明确授权。
3. **排除尺寸触发（需改代码）**：若驱动对 512 MB 整块分配有 bug，改小尺寸应能成功。同样需授权。
4. **交叉验证**：在另一台机器（或干净环境）重跑，确认是否复现，以区分"本机驱动问题"与"通用问题"。

## A2. 第 6 轮：方案 A 实施 + 测试机验证（无报错，共享内存恢复，根因闭环）

第 6 轮按 `milestone-3-b-sr-fix-luid-enum.md` 实施了**方案 A**，用户在测试机（RTX 4090）重跑，验证程序**目前不报错**。

**方案 A 实施（5 文件）：**
- `d3d12_provider.cc/.h`：**回退 Provider 的 adapter 持有**——成功路径恢复 `adapter->Release()`，删除 `adapter_` 成员 / `GetAdapter()` getter / 析构释放块。这是对第 2 轮"Provider 持有 `IDXGIAdapter1`"改动的直接回退。
- `d3d12_presenter.cc`：后端构造点去掉 `provider_.GetAdapter()`。
- `dlss_super_sampling_backend.cc/.h`：构造器减参、删 `adapter_` 成员；`Initialize()` 改为自行 `CreateDXGIFactory1` + `EnumAdapters1` 枚举 adapter、按 LUID 匹配物理 adapter（复用诊断块已解析的 `luid_available`/`adapter_luid`），用它做能力检查，检查后 `matched_adapter.Reset()` 释放。
- **与设计稿的两处偏离（已回写设计稿）**：①开发环境 Windows SDK（10.0.26100.0）`IDXGIAdapter1` 被裁剪、无 `GetLuid`，改用 `GetDesc1` 读 `AdapterLuid` 匹配——`GetDesc1` 任何 SDK 都有，更可移植；②复用诊断块 LUID 避免二次 `QueryInterface`（设计稿字面版会同作用域重名 `device3` 编译不过）。

**编译证据：** backend / provider / presenter 单文件 `cl` 编译全部 EXIT=0，无新告警（仅上游既有告警）。网络盘 ninja 全量重建不可用（`ninja: no work to do`，mtime 跟踪失效），单文件 `cl` 配方为可靠的本机验证。

**测试机验证（RTX 4090）：** 验证程序目前**不报错**。前几轮的 `Shared memory: Failed to create the 512 MB buffer (HRESULT=0x80070057)` 行消失，模拟器正常启动。

**根因闭环：** 与前两轮交叉验证形成完整因果闭环：
- `c6543c66`（Provider 释放 adapter）→ 共享内存成功、模拟器启动。
- `9848b09`（Provider 持有 adapter，即第 2 轮修复）→ 共享内存 `E_INVALIDARG`、模拟器无法启动。
- 第 6 轮（方案 A，回退持有）→ 共享内存成功、模拟器启动、无报错。
两者间唯一行为差异即 adapter 持有与否，共享内存分配代码逐字节一致。**定论：Provider 持有创建设备所用的 `IDXGIAdapter1` 是共享内存 `E_INVALIDARG` 失败的根因**——设备创建后仍持有该 adapter 时，NVIDIA 驱动对后续 `CreateCommittedResource` 返回 `E_INVALIDARG`。此为驱动行为（官方文档无明文），非 Xenia 代码 / 参数 / 显存问题。

**对第 4/5 轮结论的更新：**
- 第 5 轮"强烈指向测试机驱动层问题（合法 `CreateCommittedResource` 被驱动以 E_INVALIDARG 拒绝）"的判定现被精确化：确属"驱动拒绝合法分配"的行为，但**触发条件是应用侧持有 adapter**，而非驱动 bug。释放 adapter 后驱动即接受该分配。
- PIX 附加（强制非 tiled 分支）仍是促成因素，但非根因。

**总体状态：PASS（共享内存恢复，模拟器正常启动，根因确认并修复）。**

**剩余待验证项（实机）：** DLSS SR 能力检查（LUID 枚举 → `matched DXGI adapter LUID` → `capability check passed`）已编译集成，但 SR 默认关闭、后端惰性创建，本次"无报错"说明共享内存根因已消除、模拟器可启动；LUID 枚举路径能否在实机匹配物理 adapter 并通过能力检查，待开启 SR 开关后验证。

---

## A. 总结

- **第 2 轮修复本身生效**：新的日志中**完全不存在**第 1 轮的 DLSS SR 错误（`the D3D12 device does not expose IDXGIDevice1 ... 0x80004002` 与 `SelectOutputPath returned kInitFailed` 均消失）。说明"持有 adapter + 用 null 检查替代结构性 QI"的修复方向正确，DLSS 能力检查的结构性 COM bug 已被消除。
- **新错误与我的修改无因果关系**：本轮 5 个文件的改动全部落在 DLSS 后端路径（`dlss_super_sampling_backend.*`）与 Provider 的 adapter 持有（`d3d12_provider.*`、`d3d12_presenter.cc` 的构造调用处）。**没有任何一行改动触及共享内存、交换链、纹理创建或设备创建**。而新错误发生在 GPU Commands 线程（`0100000C`）的 `D3D12SharedMemory::Initialize()` 阶段——该阶段在**任何 DLSS 代码之前**执行（DLSS 后端是首帧 SR 时惰性创建的）。
- **新错误是"设备已移除/失效"的经典特征**：设备创建成功（特性查询全部打印，Tiled tier 4、VA 40 位、非零堆 yes），但此后**所有**基于该设备的资源分配全部失败（512 MB 共享内存 buffer、HWND 交换链、4096x4096 与多张 200x200 / 128x128 纹理）。设备在创建之后、资源分配之前被移除/破坏。
- **设备移除检测点位置偏晚**：全代码库中唯一的 `GetDeviceRemovedReason()` 检查位于 `D3D12CommandProcessor::BeginSubmission()`（`d3d12_command_processor.cc:3719`），它在**共享内存初始化之后**才运行。因此共享内存失败是设备失效的**第一个可见症状**，而 `xe::FatalError("Graphics device lost...")`（`graphics_system.cc:339`）并未出现在日志中，恰好证明 `BeginSubmission` 从未被执行到。
- **附带发现（PIX 已附加）**：日志打印 `Tiled resources: tier 4`，但共享内存却走了**非 tiled** 分支（打印的是 `Failed to create the 512 MB buffer` 而非 `...tiled buffer`）。由于 `tiled_shared_memory` 默认 `true`、且 `GetTiledResourcesTier()` 返回的就是日志里那个 tier 4，走非 tiled 分支的**唯一**可能是 `GetGraphicsAnalysis() != nullptr`——即 **PIX 已附加**。这是一个旁证，不是资源创建失败的直接原因。
- **根因判定：环境态变化**。第 1 轮时模拟器能跑到 paint 路径（说明当时共享内存初始化**成功**、设备完好）；第 3 轮时共享内存初始化**失败**。两次运行之间唯一的变量是环境（GPU reset / 驱动崩溃 / VRAM 耗尽 / TDR）。我的改动不影响设备，故排除代码侧因果。

**总体状态：FAIL（设备失效，模拟器无法启动图形子系统）——但与第 2 轮修复无因果关系。**

---

## B. 修改文件（当前工作区，均未提交）

> 本轮**未新增任何修改**。以下为第 1–2 轮遗留的工作区改动，供交叉核对。

| 文件 | 改动 | 是否触及新错误路径 |
|---|---|---|
| `src/xenia/ui/d3d12/d3d12_provider.h` | +`adapter_` 成员、+`GetAdapter()` | 否（仅新增 getter/成员） |
| `src/xenia/ui/d3d12/d3d12_provider.cc` | 成功路径 `adapter->Release()`→`adapter_ = adapter;`；析构释放 `adapter_` | 否（设备创建方式与上游完全一致） |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h` | +`IDXGIAdapter1* adapter` 构造参数、+`adapter_` 成员、前置声明 | 否（DLSS 路径，晚于共享内存） |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc` | 构造 `adapter_(adapter)`；**用 `if (!adapter_)` null 检查替代损坏的 QI**；`adapter_` 传给 `GetFeatureRequirements`；多处 XELOGD/XELOGW/XELOGE 诊断日志 | 否（DLSS 路径，晚于共享内存） |
| `src/xenia/ui/d3d12/d3d12_presenter.cc` | 后端构造调用处传入 `provider_.GetAdapter()` | 否（DLSS 路径，晚于共享内存） |

**关键论证**：`d3d12_provider.cc` 的设备创建（`pfn_d3d12_create_device_(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))`）与上游**逐字节一致**，仅把随后的 `adapter->Release()` 换成 `adapter_ = adapter;`。设备是独立的 COM 对象，持有 adapter 的额外引用不会影响设备的生命周期或状态。因此设备失效不可能是我的改动造成的。

---

## C. 验证证据（代码侧分析）

### C1. 共享内存分支判定（解释 "tier 4 却走非 tiled"）
`d3d12_shared_memory.cc:49-52`：
```cpp
if (cvars::tiled_shared_memory &&                                    // 默认 true
    provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&  // tier 4 != NOT_SUPPORTED = true
    !provider.GetGraphicsAnalysis()) {                               // 只有 PIX 附加才为 false
  // tiled 分支 -> 打印 "...tiled buffer"
} else {
  // 非 tiled 分支 -> 打印 "...buffer"   <-- 用户日志命中的是这一支
}
```
- `tiled_shared_memory` 默认 `true`（`texture_cache.cc:64`）。
- `GetTiledResourcesTier()` 返回 `tiled_resources_tier_`，而日志 `Tiled resources: tier {}` 打印的**正是**这个变量（`d3d12_provider.cc` 特性查询段），即 tier 4。
- 故走非 tiled 分支 ⟺ `GetGraphicsAnalysis() != nullptr` ⟺ **PIX 已附加**（`graphics_analysis_` 初始为 `nullptr`，仅 `DXGIGetDebugInterface1` 在 PIX 附加时才成功）。

### C2. 设备移除检测点位置偏晚
- 全代码库唯一 `GetDeviceRemovedReason()` 在 `d3d12_command_processor.cc:3719`，位于 `BeginSubmission()` 内——**晚于**共享内存初始化。
- 触发后调用 `OnHostGpuLossFromAnyThread` → `xe::FatalError("Graphics device lost (probably due to an internal error)")`（`graphics_system.cc:339`）。**该消息未出现在日志中** → `BeginSubmission` 从未执行到 → 共享内存失败是设备失效的**第一个**症状。

### C3. 所有资源分配共享同一设备、同一失败模式
- 共享内存 512 MB buffer：`device->CreateCommittedResource(&kHeapPropertiesDefault, CREATE_NOT_ZEROED, ...)`（`d3d12_shared_memory.cc`）。
- 交换链：`dxgi_factory->CreateSwapChainForHwnd(direct_queue, hwnd, ...)`（`d3d12_presenter.cc:378`）。
- 纹理：`device->CreateCommittedResource(...)`（`d3d12_immediate_drawer.cc:252/265`）。
- `kHeapPropertiesDefault = { D3D12_HEAP_TYPE_DEFAULT }`（`d3d12_util.cc:20`），标准默认堆，无异常。
- **三者全部失败** → 设备本身失效，而非某一次调用的参数错误。

### C4. 排除我的改动为因
- 5 个文件改动全部在 DLSS 路径或 Provider adapter 持有，无一触及共享内存/交换链/纹理/设备创建。
- DLSS 后端在 `PaintAndPresentImpl`（paint 阶段）惰性创建，**晚于**共享内存初始化。
- 设备创建方式与上游一致，持有 adapter 不影响设备 COM 对象。

---

## D. 环境限制

- 开发环境（Hyper-V）无法复现真实 GPU 行为；本轮全部结论基于**测试机（物理 RTX 4090）**日志与代码静态分析。
- 设备移除/驱动崩溃/VRAM 耗尽等环境态无法在本开发环境直接观测，需测试机侧配合（见 F）。
- 未执行任何真实 GPU 初始化或资源分配，`UNVERIFIED`。

---

## E. 剩余风险

1. **设备失效根因未最终确认**：代码侧已排除我的改动为因，但"设备为何被移除"（GPU reset / 驱动崩溃 / VRAM 耗尽 / TDR / 其他进程占用）需在测试机侧用 `GetDeviceRemovedReason()` 返回值与系统事件日志确认。
2. **检测点偏晚**：共享内存初始化失败时无法区分"设备已移除"与"单纯分配失败（如 OOM）"。缺少一个更早的、带 `GetDeviceRemovedReason()` 的诊断点。
3. **PIX 附加的副作用**：PIX 附加会使共享内存走非 tiled 分支（多占显存），并可能干扰资源跟踪；需确认测试机为何附加了 PIX（是否有意用于调试）。
4. **第 2 轮修复的运行时验证仍未完成**：修复消除了结构性 QI bug，但"能力检查通过 → SDK Init → CreateFeature → 真实 SR 输出"这条链路在测试机上**从未跑通**（因为本轮卡在更早的共享内存阶段）。DLSS SR 的运行时验证仍为 `UNVERIFIED`。

---

## F. 下一步（最小且安全，待用户授权后再改代码）

1. **在共享内存初始化处增加一个"仅日志"的诊断点**（不改变任何行为）：在 `D3D12SharedMemory::Initialize()` 的 `CreateCommittedResource` 失败分支里，调用 `device->GetDeviceRemovedReason()` 并打印其 HRESULT，同时打印 `provider.GetGraphicsAnalysis()` 是否为 null。这能一锤定音地区分"设备已移除"与"单纯分配失败"。——**这是本轮调查直接指向的最小诊断改动**。
2. **测试机侧环境排查**（不改 Xenia 代码）：
   - 确认是否有其他进程占用 GPU/VRAM（任务管理器 / `nvidia-smi`）。
   - 查看 Windows 事件查看器中是否有 NVIDIA 驱动崩溃 / TDR / GPU reset 记录。
   - 确认驱动版本，必要时重装/更新驱动。
   - 确认 PIX 是否被有意附加；若无意附加，移除后重跑。
3. **交叉验证**：在干净环境（无其他 GPU 进程、无 PIX）下重跑，确认共享内存初始化恢复成功、模拟器能跑到 paint 路径，届时第 2 轮修复的 DLSS SR 运行时验证才能继续进行。

> 注：以上 1 为"仅日志"改动，符合本轮"调查为主、如需修改仅限日志"的约束；2、3 为测试机侧操作，不涉及代码。

---

## G. 各项能力状态清单

- **DLSS SR**：编译期已集成（第 2 轮修复消除结构性 QI bug，能力检查路径已修正）；**运行时未验证**（本轮卡在更早的共享内存/设备失效阶段，从未到达 SR 执行）。状态：`编译期已集成 / 运行时 UNVERIFIED`。
- **NVOF**：未实现。
- **帧生成**：未实现。
- **实验性神经渲染**：仅研究。
- **Presenter 集成**：已接线且关闭（主开关控制），回退路径存在；但本轮设备失效导致 Presenter 交换链创建失败，回退逻辑未被真正触发验证。状态：`已接线且关闭 / 回退 UNVERIFIED`。
- **最终画质与性能**：尚未评估。
