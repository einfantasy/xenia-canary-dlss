# M3-B SR 修复方案 — 回退 Provider 持有 `IDXGIAdapter1`，改由后端按 LUID 自行枚举

- **状态**：修复设计稿（DESIGN），**待用户批准后实施**。本阶段未改任何代码。
- **修复对象**：`milestone-3-b-sr-fix-adapter-retain.md` 所引入的回归——**Provider 持有 `IDXGIAdapter1` 导致 NVIDIA 驱动对后续 `CreateCommittedResource`（512 MB 共享内存缓冲）返回 `E_INVALIDARG`（0x80070057）**，使模拟器无法启动（见 `milestone-3-b-sr-fix-verification-report.md` 交叉验证）。
- **选定方案**：**方案 A（完整修复，推荐）**——回退 Provider 的 adapter 持有（恢复 `c6543c66` 行为），同时让 DLSS 后端**自包含地按 LUID 枚举 adapter** 做能力检查，不再依赖 Provider 持有。方案 B（最小验证，后端传 `nullptr`）作为可选的快速诊断路径。
- **总体状态**：**PASS（设计可行）**。方案 A 同时满足两个约束：①Provider 必须释放 adapter（否则共享内存 `E_INVALIDARG`）；②后端仍需一个真实 adapter 做能力检查（否则 DLSS SR 无法启用）。用 `CreateDXGIFactory1` + `EnumAdapters1` + `GetDesc1`（读 `DXGI_ADAPTER_DESC1.AdapterLuid`）匹配绕开原 `device->QueryInterface(IDXGIDevice1)` 失败路径，两者兼得。

---

## A. 总结

上一轮把「Provider 持有 `IDXGIAdapter1`」当作修复，实为 bug：NVIDIA 驱动在设备创建后若仍持有创建设备所用的 adapter，会拒绝后续 `CreateCommittedResource`（`E_INVALIDARG`）。交叉验证两份日志（同机、同驱动、共享内存分配代码逐字节一致，唯一前置行为性差异即 adapter 持有）已定论：`c6543c66`（释放 adapter）共享内存成功，`9848b09`（持有 adapter）共享内存失败。

因此必须**回退 Provider 持有**。但回退后，后端能力检查仍需要一个 adapter（原 `device_->QueryInterface(IDXGIDevice1)` 恒失败，拿不到）。方案 A 让后端自行枚举：`ID3D12Device3::GetAdapterLuid()` 拿到设备 LUID → `CreateDXGIFactory1` + `EnumAdapters1` 枚举所有 adapter → `GetDesc1` 读 `AdapterLuid` 匹配出物理 adapter → 用它做 `GetFeatureRequirements` → 释放。后端自包含，Provider 不再持有任何 adapter。

---

## B. 逐文件改动（实施时照此执行）

> 以下行号基于 `9848b09`（当前 HEAD，含 adapter 持有）。所有「删除」均为回退 `milestone-3-b-sr-fix-adapter-retain.md` 所加内容。

### 共同部分（方案 A 与 B 都做）：回退 Provider 持有

#### B1. `d3d12_provider.h`（删成员 + getter）

- getter 区（第 53 行）删：
  ```cpp
  -  IDXGIAdapter1* GetAdapter() const { return adapter_; }
  ```
- 成员区（第 201 行）删：
  ```cpp
  -  IDXGIAdapter1* adapter_ = nullptr;
  ```

#### B2. `d3d12_provider.cc`（成功路径恢复释放 + 析构删释放块）

- 成功路径（第 335–338 行）恢复 `adapter->Release();`：
  ```cpp
  -  // Retain the adapter (the DLSS backend borrows it for the capability
  -  // check). It is released in the destructor. The failure paths above still
  -  // release `adapter` directly.
  -  adapter_ = adapter;
  +  adapter->Release();
  ```
- 析构函数（第 91–93 行）删释放块：
  ```cpp
  -  if (adapter_ != nullptr) {
  -    adapter_->Release();
  -  }
  ```
- 失败路径的 `adapter->Release()`（第 302 / 331 行）**保持不变**。

#### B3. `d3d12_shared_memory.cc`（诊断日志保留，不动）

- 上一轮加的诊断日志（分配 HRESULT + `GetDeviceRemovedReason` + PIX 挂载状态，tiled / non-tiled 两分支）**保留不动**，无害且有诊断价值。本修复不触碰此文件。

---

### 方案 A 独有部分：后端按 LUID 自行枚举 adapter

#### B4. `d3d12_presenter.cc`（调用点去掉 `GetAdapter()`）

- 第 816 行：
  ```cpp
  // 改前
  dlss_sr_backend_ =
      std::make_unique<xe::ui::dlss::DLSSSuperSamplingBackend>(
          device, provider_.GetAdapter(), GetDLSSRuntimeDirectories());
  // 改后
  dlss_sr_backend_ =
      std::make_unique<xe::ui::dlss::DLSSSuperSamplingBackend>(
          device, GetDLSSRuntimeDirectories());
  ```

#### B5. `dlss_super_sampling_backend.h`（构造器减参 + 删成员）

- 前向声明区 `struct IDXGIAdapter1;` **保留**（Initialize 内枚举仍用此类型）。
- 构造器签名（约第 85 行）：
  ```cpp
  // 改前
  DLSSSuperSamplingBackend(ID3D12Device* device,
                           IDXGIAdapter1* adapter,
                           std::vector<std::wstring> runtime_dirs);
  // 改后
  DLSSSuperSamplingBackend(ID3D12Device* device,
                           std::vector<std::wstring> runtime_dirs);
  ```
- 成员区删 `adapter_` 及其注释（约第 130–132 行）。
- 更新构造器注释：说明 adapter 不再由 provider 传入，改由后端按 LUID 自行枚举。
- `init_early_failure_logged_` 成员**保留**（仍用于一次性日志）。

#### B6. `dlss_super_sampling_backend.cc`（构造器减参 + 替换空判为 LUID 枚举）

- 构造器初始化列表去掉 `adapter_(adapter)`：
  ```cpp
  // 改前
  DLSSSuperSamplingBackend::DLSSSuperSamplingBackend(
      ID3D12Device* device, IDXGIAdapter1* adapter,
      std::vector<std::wstring> runtime_dirs)
      : device_(device),
        adapter_(adapter),
        runtime_dirs_(std::move(runtime_dirs)) {}
  // 改后
  DLSSSuperSamplingBackend::DLSSSuperSamplingBackend(
      ID3D12Device* device, std::vector<std::wstring> runtime_dirs)
      : device_(device), runtime_dirs_(std::move(runtime_dirs)) {}
  ```
- `Initialize()` 第 1 步「能力检查」，把现有的 `if (!adapter_)` 空判分支**整段替换**为 LUID 枚举：
  ```cpp
  // 改前（删除）
  // 1. Capability check (CPU side, before any command is recorded). The adapter
  //    is the one the provider used to create device_; it is borrowed (owned by
  //    the provider, released in its destructor), never released here.
  if (!adapter_) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: no DXGI adapter was provided by the provider; cannot "
             "run the DLSS capability check. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }

  // 改后（新增，实际实现复用上方诊断块已解析的 luid_available / adapter_luid，
  // 不再二次 QueryInterface，避免与诊断块同作用域重名 device3）。
  // 1. Capability check (CPU side, before any command is recorded). The provider
  //    releases the adapter it used to create device_ (retaining it makes the
  //    NVIDIA driver reject later CreateCommittedResource calls with E_INVALIDARG).
  //    So the backend enumerates adapters itself and matches by LUID to find the
  //    physical adapter device_ sits on. The matched adapter is used only for the
  //    capability check, then released. Reuses the LUID resolved by the
  //    diagnostic QI above (no second QueryInterface).
  if (!luid_available) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: device does not expose ID3D12Device3 (QueryInterface "
             "failed); cannot resolve the adapter LUID. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: CreateDXGIFactory1 failed; cannot enumerate adapters for "
             "the capability check. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> matched_adapter;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enum_hr = factory->EnumAdapters1(i, adapter.GetAddressOf());
    if (enum_hr == DXGI_ERROR_NOT_FOUND || FAILED(enum_hr)) {
      break;  // end of enumeration (or unexpected error)
    }
    // Match by the adapter LUID embedded in its description. (GetDesc1 is used
    // rather than IDXGIAdapter1::GetLuid because this toolchain's dxgi.h trims
    // the latter; GetDesc1 is present in every SDK and carries the same LUID.)
    DXGI_ADAPTER_DESC1 desc1{};
    if (SUCCEEDED(adapter->GetDesc1(&desc1)) &&
        desc1.AdapterLuid.HighPart == adapter_luid.HighPart &&
        desc1.AdapterLuid.LowPart == adapter_luid.LowPart) {
      matched_adapter = adapter;
      break;
    }
  }
  if (!matched_adapter) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: no DXGI adapter with LUID {:04X}:{:04X} was found; cannot "
             "run the DLSS capability check. Returning kInitFailed.",
             adapter_luid.HighPart, adapter_luid.LowPart);
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  XELOGD("DLSS SR: matched DXGI adapter LUID {:04X}:{:04X} for the capability "
         "check.",
         adapter_luid.HighPart, adapter_luid.LowPart);
  ```
- 能力检查调用改用 `matched_adapter.Get()`：
  ```cpp
  NVSDK_NGX_Result requirement_result = CallGetFeatureRequirements(
  -     adapter_, &discovery, &requirement, seh_code);
  +     matched_adapter.Get(), &discovery, &requirement, seh_code);
  ```
- 能力检查完成后 `matched_adapter.Reset();`，确保不再持有。`CreateFeature` / `InitWithProjectId` 只用 `device_`，不用 adapter，故释放安全。（已实现）
- 上一轮加的 `ID3D12Device3` LUID 诊断 QI **保留不动**（只喂日志行，与本次修复正交）。
- `Shutdown()` / `ReleaseFeature()` 的 SDK 结果日志 **保留不动**（无害）。

---

### 方案 B 独有部分：后端传 `nullptr`（最小验证）

#### B7. `d3d12_presenter.cc`（调用点改传 `nullptr`）

- 第 816 行：
  ```cpp
  // 改前
  device, provider_.GetAdapter(), GetDLSSRuntimeDirectories());
  // 改后
  device, nullptr, GetDLSSRuntimeDirectories());
  ```
- DLSS 后端代码 **完全不动**（保留 `adapter_` 成员与构造参数）。`Initialize()` 命中既有 `if (!adapter_)` 分支 → `kInitFailed` → 本帧确定性回退 paint 路径。
- 方案 B 总共只改 **5 行**（B1/B2 的 provider 4 行 + B7 的 presenter 1 行）。
- 目的仅是确认「回退 adapter 持有后共享内存恢复」。DLSS SR 保持关闭（每帧回退），不是最终态。

---

## C. 所有权与生命周期契约

- **方案 A**：Provider **不再持有**任何 adapter（创建设备后立即 `Release`，回到 `c6543c66` 行为）。后端在 `Initialize()` 内自行 `CreateDXGIFactory1` + `EnumAdapters1`，得到的 `ComPtr<IDXGIAdapter1>` 是后端自有的临时引用，能力检查用完即释放，不跨函数持有。Provider 与后端的 adapter 所有权彻底解耦。
- **Provider 析构顺序不变**：`adapter_` 成员已删，析构只释放 `device_` / `direct_queue_` / `dxgi_factory_` / `graphics_analysis_`。
- 后端仍不接管 `guest_output_resource_paint_refs`、不拥有 guest 输出——隔离原则不变。

---

## D. 错误处理 / 边界

- `device_->QueryInterface(ID3D12Device3)` 失败（极罕见）→ `kInitFailed` + 一次性日志。
- `CreateDXGIFactory1` 失败 → `kInitFailed` + 一次性日志。
- `EnumAdapters1` 未匹配到设备 LUID（理论上不该，设备必在某 adapter 上）→ `kInitFailed` + 一次性日志，记录目标 LUID 便于诊断。
- 所有枚举得到的 adapter 均为 `ComPtr` 临时引用，作用域结束自动 `Release`，无泄漏。
- `GetFeatureRequirements` 仍走既有 SEH 边界，崩溃等同失败回退，不变。
- 能力检查失败仍缓存 `kUnavailable`（既有行为），不变。
- **一次性日志**：`init_early_failure_logged_` 保证上述永久失败每帧重试时只记一次，不刷屏。

---

## E. 许可证隔离（不变）

- 方案 A 后端新增 `CreateDXGIFactory1` / `EnumAdapters1` / `GetDesc1` 均为纯 Windows/DirectX API，**非 NVIDIA SDK**。`dlss/*` 模块仍独占所有 `nvsdk_*` 依赖。Provider / Presenter / 配置**不**新增任何 SDK include。隔离边界完好。
- 若 §4e 最终裁定不能分发，删掉 `dlss/*` 模块即可——Provider/Presenter 的回退改动对无 SDK 构建无害。

---

## F. 验证计划

1. **编译**：单文件 `cl` 配方（网络盘 ninja 全量受限），确认构造器签名、`EnumAdapters1` 枚举、`ComPtr<IDXGIFactory1>` 均编译通过（exit 0 + 仅上游告警 = 成功）。
2. **主机侧**：`--build-tests` 跑 `guest_output_enhancement_test`，确认无回归。
3. **RTX 4090 实机**（`UNVERIFIED` → 验证）：
   - **共享内存恢复**：启动日志不再出现 `Shared memory: Failed to create the 512 MB buffer (HRESULT=0x80070057)`，模拟器正常加载游戏（对照 `c6543c66`）。
   - **方案 A**：开 SR 开关，日志应出现 `matched DXGI adapter LUID` → `capability check passed`（而非 `0x80004002` QI 失败、也非 `no DXGI adapter was provided`）。`GetFeatureRequirements` 返回 `Supported`；`Init` / `CreateFeature` / `Evaluate` 无 `FAIL_*`。
   - **方案 B**：开 SR 开关，日志应出现 `no DXGI adapter was provided`（一次性）→ `kInitFailed` → 回退 paint 路径；共享内存仍恢复。
   - 关闭 SR 时零 DLL 加载、零后端调用（默认关闭语义不变）。
   - 回退路径：模拟失败仍确定性回退原始路径。
4. **交叉验证**：对照 `milestone-3-b-sr-fix-verification-report.md`，确认共享内存 `E_INVALIDARG` 行消失、模拟器可启动、（方案 A）能力检查通过。

---

## G. 影响面 / 爆炸半径

- **方案 A**：5 个文件——`d3d12_provider.h/.cc`（删成员/getter/析构块，成功路径恢复释放）、`dlss_super_sampling_backend.h/.cc`（构造器减参、删成员、`Initialize` 改 LUID 枚举）、`d3d12_presenter.cc`（调用点去参）。不碰 SDK 调用序列、不碰资源状态屏障、不碰邮箱/Fence/paint-refs、不碰回退逻辑。
- **方案 B**：仅 5 行（provider 4 + presenter 1），DLSS 后端零改动。
- 两方案都不碰 `d3d12_shared_memory.cc`（诊断日志保留）。

---

## H. A–G 交付报告（按 AGENTS.md §7）

**A. 总结**：修复设计稿。定论「Provider 持有 `IDXGIAdapter1` 是共享内存 `E_INVALIDARG` 的根因」。选定**方案 A**——回退 Provider 持有 + 后端按 LUID 自行枚举 adapter，同时满足「Provider 必须释放 adapter」与「后端需真实 adapter 做能力检查」。方案 B（传 `nullptr`）为可选快速诊断。**PASS（设计可行）**，待批准后实施。

**B. 修改文件**：（本设计稿未改代码；实施时按下表）

| 文件 | 修改内容 | 修改原因 |
|---|---|---|
| `src/xenia/ui/d3d12/d3d12_provider.h` | 删 `adapter_` 成员、删 `GetAdapter()` getter | 回退持有（根因） |
| `src/xenia/ui/d3d12/d3d12_provider.cc` | 成功路径 `adapter_=adapter`→`adapter->Release()`；析构删释放块 | 恢复 `c6543c66` 释放行为 |
| `src/xenia/ui/d3d12/d3d12_presenter.cc` | 后端构造点去掉 `provider_.GetAdapter()`（方案 A）或改 `nullptr`（方案 B） | 后端不再依赖 provider 持有 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h` | 构造器减参、删 `adapter_` 成员（方案 A） | 后端自包含 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc` | 构造器减参、`Initialize` 改 LUID 枚举 + `matched_adapter.Get()` 传入 `GetFeatureRequirements`（方案 A） | 自行获取真实 adapter 做能力检查 |

**C. 验证证据**：设计稿未编译/未运行。实施后按 F 节执行。交叉验证基线：`xenia_sr-fix.log`（`9848b09`，持有，FAIL）vs `xenia_sr-fix-2.log`（`c6543c66`，释放，WORKS）。

**D. 环境限制**：Hyper-V 无 NVIDIA 驱动/真实 GPU；网络盘 ninja 全量构建受限。实机验证项 `UNVERIFIED`。

**E. 剩余风险**：方案 A 的 LUID 枚举在测试机上是否成功匹配物理 adapter 尚未经真实环境验证（API 标准，预期可工作）；`E_INVALIDARG` 与 adapter 持有的因果关系虽经两份日志强证，但未在 NVIDIA 官方文档找到明文（属驱动行为观察）；许可证 §4e 仍 `LICENSE REVIEW REQUIRED`。

**F. 下一步（最小且安全）**：用户批准方案 A 或 B → 按 B 节实施 → 单文件编译 + 主机侧测试 → RTX 4090 实机复验共享内存恢复（方案 A 加验能力检查）→ 更新 `milestone-3-b-sr-fix-verification-report.md`（追加 A2 节）。批准前不动代码。

**G. 各项能力状态清单**：
- **DLSS SR**：接口骨架已实现（M2）；真实后端已编译期集成（M3-B）；**运行时未验证**——当前因 adapter 持有导致共享内存失败无法启动；方案 A 修复后预期恢复，待实机复验。
- **NVOF**：未实现（独立模块，非 SR 依赖）。
- **帧生成**：未实现（独立模块）。
- **实验性神经渲染**：仅研究（社区 `nvngx_dlssnr.dll` 310.8.3 HashMismatch，无 Xenia 契约）。
- **Presenter 集成**：已接线且关闭（M3-B 已接入 `PaintAndPresentImpl`，默认关闭时零后端调用；回退已验证——`kInitFailed` 确定性回退原始路径）。
- **最终画质与性能**：尚未评估（待 RTX 4090 实机）。
