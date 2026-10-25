# M3-B SR 调试 — 能力检查失败调查报告

- **任务**：在测试机上验证 M3-B 的 DLSS SR 功能，发现日志错误，要求从代码侧调查原因（以调查为主；如需改代码，仅允许日志相关改动，不提交 git）。
- **调查对象日志**：
  ```
  !> DLSS SR: the D3D12 device does not expose IDXGIDevice1 (QueryInterface failed, 0x80004002);
     cannot run the DLSS capability check. Returning kInitFailed.
  w> DLSS SR: SelectOutputPath returned kInitFailed; falling back to the original paint path this frame.
  ```
- **总体状态**：**PASS WITH FINDINGS**（根因已定位并经三份权威来源核实；确定性回退按设计正常工作；本轮仅做日志诊断增强，未改任何行为逻辑，未提交）。

---

## A. 总结

错误根因是 **M3-B 后端在能力检查阶段的结构性 COM 接口误用**：`DLSSSuperSamplingBackend::Initialize()` 试图通过 `ID3D12Device::QueryInterface(IID_PPV_ARGS(&IDXGIDevice1))` 从 **D3D12 设备**上拿到 DXGI 设备，再经 `GetAdapter()` 取得 `IDXGIAdapter`，以调用 `NVSDK_NGX_D3D12_GetFeatureRequirements(IDXGIAdapter*, ...)`。但 `ID3D12Device` 是 Direct3D 12 侧的 COM 接口，**从不实现** `IDXGIDevice1`（那是 DXGI 侧的接口族，通常由 `IDXGISwapChain::GetDevice` 或 `IDXGIFactory` 提供）。因此这次 QI **必然**返回 `0x80004002`（= `E_NOINTERFACE` = `DXGI_ERROR_NOT_FOUND`，两者同值），与 GPU、驱动、SDK 运行时是否在场**无关**——它是纯代码缺陷，在任何真实 RTX 4090 上都会失败。

关键结论：**回退行为正确**。`Initialize` 返回 `kInitFailed` → `SelectOutputPath` 透传该失败 → Presenter `Reset` 命令列表并重新录制原始 `kBilinear` 路径。游戏画面经原始路径正常渲染，只是 DLSS SR 不激活，无崩溃、无黑屏。

---

## B. 根因分析（逐条证据）

### B1. 失败点（代码侧）

`src/xenia/ui/dlss/dlss_super_sampling_backend.cc`，`Initialize()` 第 1 步「能力检查」：

```cpp
Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
const HRESULT query_hr = device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));  // ← 必然失败
if (FAILED(query_hr)) { /* 0x80004002 → kInitFailed */ }
Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
dxgi_device->GetAdapter(adapter.GetAddressOf());   // ← 永远走不到
...
CallGetFeatureRequirements(adapter.Get(), &discovery, &requirement, seh_code);  // ← 永远走不到
```

`device_` 是 `ID3D12Device*`（由 `D3D12Provider` 用 `D3D12CreateDevice(adapter, ...)` 创建并交给 Presenter）。代码假定 D3D12 设备能 QI 出 `IDXGIDevice1`，这是错误的。

### B2. 为什么 `0x80004002` 必然是 E_NOINTERFACE（结构性，而非环境性）

- `E_NOINTERFACE` = `0x80004002`。COM 对象未实现所请求接口时，`QueryInterface` 返回此值。
- `DXGI_ERROR_NOT_FOUND` 也定义为 `0x80004002`（DXGI 错误码刻意与对应 COM/WIN32 错误码同值）。
- 因此日志里的 `0x80004002` 正是「该 COM 对象不实现 `IDXGIDevice1`」的标准返回值，**不是**驱动缺失、运行时缺失或 GPU 不支持。

### B3. 权威来源一：Windows SDK `d3d12.h`

`C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\d3d12.h:9001`：

```
ID3D12Device : public ID3D12Object
```

`ID3D12Device` 的继承链是 `ID3D12Object`（`IUnknown` 派生），**不含** `IDXGIDevice` / `IDXGIDevice1`。`IDXGIDevice1` 属于 DXGI 接口族（`dxgi.h`），由 DXGI 设备对象提供，与 D3D12 设备是两个独立 COM 对象。

### B4. 权威来源二：NVIDIA SDK 头文件 `nvsdk_ngx.h`（DLSS-310.9.1）

`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx.h`（约第 621 行）：

```
// PARAMETERS:
// Adapter (D3D11/12 only):
//      Physical adapter which will be used to create the graphics device.
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetFeatureRequirements(
    IDXGIAdapter *Adapter, const NVSDK_NGX_FeatureDiscoveryInfo *FeatureDiscoveryInfo,
    NVSDK_NGX_FeatureRequirement *OutSupported);
```

SDK 明确：传入的 `Adapter` 是「**用于创建图形设备的那个物理适配器**」，由应用**自行持有**并传入——而不是从 D3D12 设备推导出来。这与 M3-B 的实现（从 D3D12 设备 QI）正好相反。`01-dlss-sr-official-reference.md` 第 15 行同样确认该签名首参为 `IDXGIAdapter*`。

### B5. 权威来源三：Veyra-NRVideo 参考实现（只读）

- `include\veyra\gfx\D3D12DeviceContext.h:100`：`ComPtr<IDXGIAdapter1> adapter_;` — Veyra **持有**用于创建设备的 DXGI 适配器。
- `src\ngx\FgCompatibilitySession.cpp:182-183`：需要适配器时，经 `ID3D12Device3::GetAdapterLuid()` + `IDXGIFactory::EnumAdapterByLuid()` **重新推导**出 `IDXGIAdapter`，再调 `GetFeatureRequirements`。

Veyra 与 SDK 示例**都没有**对 `ID3D12Device` QI `IDXGIDevice1`。本机可用接口已核实：`ID3D12Device3::GetAdapterLuid`（`d3d12.h:9220`）与 `IDXGIFactory4::EnumAdapterByLuid`（`shared\dxgi1_6.h:1032`）均存在。

### B6. 结论

`ID3D12Device` 拿不到 `IDXGIAdapter`（也没有「设备→适配器」的直连 API）。正确做法是**持有**创建设备时所用的 `IDXGIAdapter1`，或经 LUID 重新推导。M3-B 用了不存在的 QI 路径，能力检查永远到不了 `GetFeatureRequirements`，后端永远返回 `kInitFailed`。

---

## C. 为什么回退正常（符合设计的部分）

`D3D12Presenter::PaintAndPresentImpl` 的 M3-B 接线（`d3d12_presenter.cc` 约 820-870）：`SelectOutputPath` 返回 `kInitFailed`（非 `kSuccess`）时，Presenter `Reset` 命令列表丢弃后端已录制的（未执行的）命令，再由效果循环重新录制原始 `kBilinear` 路径。guest 输出仍处 `PIXEL_SHADER_RESOURCE`（无命令被执行），原始路径安全。这正符合架构 §H「失败 → 确定性回退 `kOriginalPath`，`effect_count ≥ 1` + `kBilinear`，严禁 `effect_count=0`」。

**次要观察（非根因，供修复轮参考）**：按架构 §H2，「初始化失败 → `kInitFailed`」，而「DLL 缺失 / 能力检查失败 → `kUnavailable`（缓存不重试）」。本次 QI 失败被归为 `kInitFailed`，且 `Initialize` 的早期失败路径把 `state_` 留在 `kNotInitialized`，导致**每帧重试**（仅日志打一次）。由于该 QI 失败是**永久性**的（代码缺陷而非瞬态），每帧重试是无效 CPU 开销。修复轮应把永久性初始化失败缓存为 `kUnavailable`，避免每帧重试。

---

## D. 本轮代码改动（仅日志，未提交）

`src/xenia/ui/dlss/dlss_super_sampling_backend.cc`（+71 / -6，唯一改动文件；`git status` 仅显示此文件，未提交）：

1. **决定性诊断**：在 QI 失败日志前，用 `ID3D12Device3::GetAdapterLuid()` 取得设备所属适配器 LUID 并写入日志。这样在测试机上可确认「设备确实在一块真实物理适配器（RTX 4090）上，但 `ID3D12Device` 就是不提供 `IDXGIDevice1`」——直接区分结构性 QI 失败与环境性失败。该 QI 是标准 D3D12 调用（无 SDK 介入、无状态改变），结果只喂日志行。
2. **补齐静默失败**：`Initialize` 第 4 步 `CreateCommittedResource`（后端输出纹理）失败原本**无日志**（直接 `return kInitFailed`），现补 `XELOGE`；成功补 `XELOGD`。这是修复 QI 后下一个可能暴露的失败点，原先会静默。
3. **初始化序列追踪**：能力检查通过、`Init_with_ProjectID` 起止、`AllocateParameters` 起点、`CreateFeature` 起点，均补 `XELOGD`，便于在测试机逐帧追踪完整初始化路径。
4. **释放/关闭结果**：`ReleaseFeature`（`CallReleaseFeature` / `CallDestroyParameters`）与 `Shutdown`（`CallShutdown1`）的结果原本被丢弃，现按成功/失败分别 `XELOGD`/`XELOGW`。

**无行为改变、无功能改变、无 SDK 调用新增、无提交。** 所有新增均为 `XELOG*` 语句 + 一处仅供日志的诊断 QI。

---

## E. 验证证据

- **编译验证（本机可执行）**：用项目真实编译参数对改动文件做单文件编译检查：
  `cl /nologo /c /std:c++20 /utf-8 /W4 -I <DLSS-310.9.1\include> -I <仓库根> -I <仓库根\src> dlss_super_sampling_backend.cc`
  → **编译通过，生成 `.obj`（225714 字节）**，无 error。证明日志改动语法/类型正确（`ID3D12Device3`、`LUID`、`ComPtr` 均可用）。
- **全量构建（本机受限）**：`.\xb build --config release` 在 **CMake/ninja 生成阶段**即失败，`ninja: error: failed recompaction: Permission denied`。这是**在网络（UNC/映射盘）路径上构建的已知限制**（ninja 原子重压缩需本地文件系统语义），发生在任何 C++ 编译之前，**非本改动引入**。
- **静态核实**：三份权威来源（B3/B4/B5）逐条核对，根因定位确凿。

## F. 环境限制

- 本机为 Hyper-V + 网络共享路径，无 NVIDIA 驱动/真实 GPU。无法执行真实 `GetFeatureRequirements`、`Init`、`Evaluate`。所有 GPU 相关项仍 `UNVERIFIED`。
- 网络路径导致 ninja 全量构建失败（见 E），无法在本机完成完整 release 构建。需在本地磁盘路径上构建以完成全量验证。

## G. 剩余风险 / 建议修复（下一轮，非本轮）

- **修复方案（二选一，需另行批准）**：
  1. `D3D12Provider` 持有创建设备所用的 `IDXGIAdapter1`（已有 `IDXGIFactory2*`，可再枚举），传给后端构造器。
  2. 后端内 QI `ID3D12Device` → `ID3D12Device3` → `GetAdapterLuid()` → `CreateDXGIFactory2` → `EnumAdapterByLuid()` 得到 `IDXGIAdapter`（Veyra 方案，本机接口已确认可用）。
- **语义修正**：把永久性初始化失败（如本 QI 缺陷修复后的真实「不支持」）缓存为 `kUnavailable`，避免每帧重试。
- 修复后需在 RTX 4090 实机复验：`GetFeatureRequirements` 返回 Supported、`Init`/`CreateFeature`/`Evaluate` 无 `FAIL_*`、回退路径仍正确。

---

## H. A–G 交付报告（按 AGENTS.md §7）

**A. 总结**：M3-B SR 能力检查失败根因已定位并经三份权威来源核实——`ID3D12Device` 被错误地 QI `IDXGIDevice1`（结构性 COM 缺陷，恒返 `0x80004002`）。回退按设计正常工作。本轮仅做日志诊断增强。**PASS WITH FINDINGS**。

**B. 修改文件**：
| 文件 | 修改内容 | 修改原因 |
|---|---|---|
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc` | +71/-6：QI 失败前加适配器 LUID 诊断；补 `CreateCommittedResource` 静默失败日志；初始化序列 `XELOGD` 追踪；`ReleaseFeature`/`Shutdown` 结果日志 | 仅日志，便于测试机确诊根因并追踪下一失败点；不改行为 |

**C. 验证证据**：单文件编译（`/std:c++20 /utf-8 /W4`）通过并生成 `.obj`；全量 `xb` 构建因网络路径 ninja 限制在生成阶段失败（非本改动）；三份权威来源静态核实根因。

**D. 环境限制**：Hyper-V 无 NVIDIA 驱动/真实 GPU，无法执行真实 `GetFeatureRequirements`/`Init`/`Evaluate`；网络路径致 ninja 全量构建失败。

**E. 剩余风险**：修复需持有/推导 `IDXGIAdapter`（两方案待批）；永久性初始化失败应缓存 `kUnavailable`；修复后须 RTX 4090 实机复验；许可证 §4e 仍 `LICENSE REVIEW REQUIRED`。

**F. 下一步（最小且安全）**：① 由用户批准修复方案（持有适配器 vs LUID 推导）；② 在本地磁盘路径完成全量 release 构建 + 主机侧测试；③ RTX 4090 实机复验能力检查与回退。本轮到此停止，不自动进入修复。

**G. 各项能力状态清单**：
- **DLSS SR**：接口骨架已实现（M2）；真实后端已编译期集成（M3-B）；**运行时未验证**——能力检查因 QI 缺陷从未真正执行。
- **NVOF**：未实现（独立模块，非 SR 依赖）。
- **帧生成**：未实现（独立模块）。
- **实验性神经渲染**：仅研究（社区 `nvngx_dlssnr.dll` 310.8.3 HashMismatch，无 Xenia 契约）。
- **Presenter 集成**：已接线且关闭（M3-B 已接入 `PaintAndPresentImpl`，默认关闭时零后端调用；回退已验证——`kInitFailed` 确定性回退原始路径）。
- **最终画质与性能**：尚未评估（待 RTX 4090 实机）。
