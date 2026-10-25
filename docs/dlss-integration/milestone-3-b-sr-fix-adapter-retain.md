# M3-B SR 修复方案 — Provider 持有 `IDXGIAdapter1` 传给后端构造器

- **状态**：修复设计稿（DESIGN），**待用户批准后实施**。本阶段未改任何代码。
- **修复对象**：M3-B 能力检查的结构性 COM 缺陷（见 `milestone-3-b-sr-debug-report.md`）——`DLSSSuperSamplingBackend::Initialize()` 错误地对 `ID3D12Device` QI `IDXGIDevice1`，恒返 `0x80004002`，能力检查从未真正执行。
- **选定方案**：报告「G. 剩余风险 / 建议修复」中的方案 1——**Provider 保留创建设备所用的 `IDXGIAdapter1`**，传给后端构造器。理由：Provider 创建设备时本就持有该适配器，只是 `d3d12_provider.cc:332` 用完后 `Release` 丢弃；保留它是改动最小、最贴合现有所有权的修法。
- **总体状态**：**PASS（设计可行）**。方案与现有所有权/许可证隔离原则兼容，爆炸半径可控，不碰 SDK 调用序列与资源状态屏障。

---

## A. 总结

`ID3D12Device` 拿不到 `IDXGIAdapter`（无「设备→适配器」直连 API）。但 `D3D12Provider::Initialize()` 用 `D3D12CreateDevice(adapter, ...)` 创建设备时，`adapter` 就是那个 `IDXGIAdapter1`——修复只需 **保留它、加 getter、传给后端**，后端用它做能力检查，删掉错误 QI。

---

## B. 逐文件改动（实施时照此执行）

### B1. `d3d12_provider.h`（保留适配器 + getter）

- 成员区（`dxgi_factory_` / `device_` 旁，约 198-199 行）加：
  ```cpp
  IDXGIAdapter1* adapter_ = nullptr;
  ```
- getter 区（`GetDXGIFactory()` / `GetDevice()` 旁，约 47-52 行）加：
  ```cpp
  IDXGIAdapter1* GetAdapter() const { return adapter_; }
  ```
- 头文件已 include `<dxgi1_6.h>`（提供 `IDXGIAdapter1`），**无需新增 include**。

### B2. `d3d12_provider.cc`（成功路径保留而非释放 + 析构释放）

- 成功路径（约 332 行）：`adapter->Release();` → `adapter_ = adapter;`
  - 失败路径（302 / 328 行的 `adapter->Release()`）**保持不变**——它们只在该次初始化失败时释放，与成功路径互斥。
- 析构函数（约 89-93 行，`device_->Release()` / `dxgi_factory_->Release()` 旁）加：
  ```cpp
  if (adapter_ != nullptr) {
    adapter_->Release();
  }
  ```
  注意：析构里**不要**再对裸变量 `adapter` `Release`，统一走 `adapter_` 成员，避免双释放。

### B3. `dlss_super_sampling_backend.h`（构造器加参 + 持有借用指针）

- 前向声明区（`struct ID3D12Device;` 旁）加：
  ```cpp
  struct IDXGIAdapter1;
  ```
- 构造器签名（约 85 行）：
  ```cpp
  // 改前
  DLSSSuperSamplingBackend(ID3D12Device* device,
                           std::vector<std::wstring> runtime_dirs);
  // 改后
  DLSSSuperSamplingBackend(ID3D12Device* device,
                           IDXGIAdapter1* adapter,
                           std::vector<std::wstring> runtime_dirs);
  ```
- 成员区（`device_` 旁）加：
  ```cpp
  ID3D12Device* device_;
  // Borrowed: the provider owns this and releases it in its destructor.
  // The backend only reads it during Initialize(); it never Releases it.
  IDXGIAdapter1* adapter_;
  ```

### B4. `dlss_super_sampling_backend.cc`（替换坏 QI + 构造器赋值 + 空指针防御）

- 构造器初始化列表：`device_(device), adapter_(adapter), runtime_dirs_(...)`。
- `Initialize()` 第 1 步「能力检查」，把这段**整段删除**：
  ```cpp
  Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
  const HRESULT query_hr = device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
  if (FAILED(query_hr)) { ... return kInitFailed; }
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  const HRESULT adapter_hr = dxgi_device->GetAdapter(adapter.GetAddressOf());
  if (FAILED(adapter_hr)) { ... return kInitFailed; }
  ```
  替换为：
  ```cpp
  if (!adapter_) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: no DXGI adapter was provided by the provider; "
             "cannot run the DLSS capability check. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  ```
  随后 `CallGetFeatureRequirements(adapter_, &discovery, &requirement, seh_code);`（把原来的 `adapter.Get()` 换成成员 `adapter_`）。`IDXGIAdapter1*` 隐式转基类 `IDXGIAdapter*`，签名吻合。
- 上一轮我加的 `ID3D12Device3` LUID 诊断 QI：**保留不动**（它只喂日志行，与本次修复正交，仍可用于记录设备所属适配器 LUID）。

### B5. `d3d12_presenter.cc`（调用点传参）

- 约 816 行：
  ```cpp
  // 改前
  std::make_unique<xe::ui::dlss::DLSSSuperSamplingBackend>(
      device, GetDLSSRuntimeDirectories());
  // 改后
  std::make_unique<xe::ui::dlss::DLSSSuperSamplingBackend>(
      device, provider_.GetAdapter(), GetDLSSRuntimeDirectories());
  ```
  `provider_` 在 Presenter 内已是成员（`provider_.GetDevice()` 已在 151/421/470/544 行多处使用）。

---

## C. 所有权与生命周期契约

- **Provider 拥有** `adapter_`，在其析构中 `Release`。
- **后端只借用**（裸指针），**绝不** `Release`。后端仅在 `Initialize()`（即 `TryProcess` 期间、应用运行中）读它；`Shutdown()` / 析构**不**用它。
- 需保证**后端先于 Provider 析构**：后端是 Presenter 的成员（`dlss_sr_backend_`），Presenter 是逐窗口对象，Provider 是顶层对象（窗口/应用搭建时创建、关机时销毁），Provider 生命周期严格长于 Presenter → 安全。此为既有假设，沿用即可。
- 后端**不得**接管 `guest_output_resource_paint_refs`、不拥有 guest 输出——本次修复不触碰这些，隔离原则不变。

---

## D. 错误处理 / 边界

- `adapter_ == nullptr`（理论上不该发生，Provider 成功路径必然持有）→ 防御性 `kInitFailed` + 一次性日志，不崩。
- `GetFeatureRequirements` 仍走既有 SEH 边界（`CallGetFeatureRequirements` 已包 `__try/__except`），崩溃等同失败回退，不变。
- 能力检查失败仍缓存 `kUnavailable`（既有行为），不变。
- **语义修正（可选，建议一并做）**：把「适配器缺失 / 能力检查失败」这类**永久性**初始化失败缓存为 `kUnavailable`，避免每帧重试。当前 QI 缺陷修复后，真实「不支持」会走 `kUnavailable`；但 `adapter_==nullptr` 这条防御路径仍留 `kNotInitialized` 会每帧重试——建议一并改为 `kUnavailable`。

---

## E. 许可证隔离（不变）

- 所有 SDK 依赖仍只在 `dlss/*` 模块；Provider / Presenter / 配置**不**新增任何 `nvsdk_*` include。Provider 只是多持有一个 DXGI 适配器（纯 Windows/DirectX 对象，非 NVIDIA SDK），Presenter 只是多传一个指针。隔离边界完好。
- 若 §4e 最终裁定不能分发，删掉 `dlss/*` 模块即可——Provider/Presenter 的改动（持有适配器 + 传参）对无 SDK 的构建无害（`adapter_` 仅被后端消费，后端删除后 Provider 持有适配器也无副作用，可再择机移除）。

---

## F. 验证计划

1. **编译**：本地磁盘路径 `.\xb build --config release`（网络盘 ninja 受限），确认 `IDXGIAdapter1*` 转 `IDXGIAdapter*`、构造器签名、getter 均编译通过。
2. **主机侧**：`--build-tests` 跑 `guest_output_enhancement_test`，确认无回归。
3. **RTX 4090 实机**（`UNVERIFIED` → 验证）：
   - 开 SR 开关，日志应出现 `capability check passed`（而非 `0x80004002` QI 失败）。
   - `GetFeatureRequirements` 返回 `Supported`；`Init` / `CreateFeature` / `Evaluate` 无 `FAIL_*`。
   - 关闭 SR 时零 DLL 加载、零后端调用（默认关闭语义不变）。
   - 回退路径：模拟失败仍确定性回退原始 `kBilinear`。
4. **交叉验证**：对照 `milestone-3-b-sr-debug-report.md`，确认 QI 失败行消失、能力检查通过、LUID 诊断行仍显示正确适配器。

---

## G. 影响面 / 爆炸半径

- 5 个文件：`d3d12_provider.h/.cc`（+成员、+getter、成功路径保留、析构释放）、`dlss_super_sampling_backend.h/.cc`（构造器加参、持有、替换 QI、空指针防御）、`d3d12_presenter.cc`（调用点传参）。
- 不碰 SDK 调用序列、不碰资源状态屏障、不碰邮箱/Fence/paint-refs、不碰回退逻辑。

---

## H. A–G 交付报告（按 AGENTS.md §7）

**A. 总结**：修复设计稿。选定「Provider 持有 `IDXGIAdapter1`」方案，与现有所有权/许可证隔离原则兼容，爆炸半径 5 文件，不碰 SDK 序列与资源屏障。**PASS（设计可行）**，待批准后实施。

**B. 修改文件**：（本设计稿未改代码；实施时按下表）
| 文件 | 修改内容 | 修改原因 |
|---|---|---|
| `src/xenia/ui/d3d12/d3d12_provider.h` | +`adapter_` 成员、+`GetAdapter()` getter | Provider 保留创建设备所用适配器 |
| `src/xenia/ui/d3d12/d3d12_provider.cc` | 成功路径 `adapter->Release()`→`adapter_=adapter`；析构释放 `adapter_` | 成功时持有，失败时仍释放，析构统一释放 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h` | 构造器加 `IDXGIAdapter1*` 参、+`adapter_` 借用成员、+前向声明 | 后端获得真实适配器 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc` | 删除坏 QI、空指针防御、`adapter_` 传入 `GetFeatureRequirements`、构造器赋值 | 修复结构性 COM 缺陷 |
| `src/xenia/ui/d3d12/d3d12_presenter.cc` | 后端构造点传 `provider_.GetAdapter()` | 接线 |

**C. 验证证据**：设计稿未编译/未运行。实施后按 F 节执行。

**D. 环境限制**：Hyper-V 无 NVIDIA 驱动/真实 GPU；网络盘 ninja 全量构建受限。实机验证项 `UNVERIFIED`。

**E. 剩余风险**：`adapter_==nullptr` 防御路径的语义修正（建议改 `kUnavailable`）；实机 `GetFeatureRequirements`/`Init`/`Evaluate` 仍 `UNVERIFIED`；许可证 §4e 仍 `LICENSE REVIEW REQUIRED`。

**F. 下一步（最小且安全）**：用户批准本方案 → 按 B 节实施 5 文件改动 → 本地磁盘全量构建 + 主机侧测试 → RTX 4090 实机复验能力检查与回退。批准前不动代码。

**G. 各项能力状态清单**：
- **DLSS SR**：接口骨架已实现（M2）；真实后端已编译期集成（M3-B）；**运行时未验证**——能力检查因 QI 缺陷从未真正执行（本方案修复后待实机复验）。
- **NVOF**：未实现（独立模块，非 SR 依赖）。
- **帧生成**：未实现（独立模块）。
- **实验性神经渲染**：仅研究（社区 `nvngx_dlssnr.dll` 310.8.3 HashMismatch，无 Xenia 契约）。
- **Presenter 集成**：已接线且关闭（M3-B 已接入 `PaintAndPresentImpl`，默认关闭时零后端调用；回退已验证——`kInitFailed` 确定性回退原始路径）。
- **最终画质与性能**：尚未评估（待 RTX 4090 实机）。
