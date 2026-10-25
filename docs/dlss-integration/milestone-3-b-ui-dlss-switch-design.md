# Milestone 3-B：DLSS UI 开关设计与实施报告（SR 质量预设）

- **文档性质**：DESIGN + IMPLEMENTATION。本文档合并原 `ui-dlss-switch-design.md`（Phase 1：主开关 + SR 子开关设计）、`milestone-3-b-sr-quality-ui-design.md`（Phase 2：质量预设设计）与 `milestone-3-b-sr-quality-ui-implementation.md`（Phase 2 实施报告）为单一文档；三个源文档已删除。
- **仓库状态**：分支 `dev/dlss5-integration-sr`。
- **设计目标**：将 `GuestOutputEnhancementConfig`（主开关 + SR 子开关）与 DLSS SR 质量预设暴露到 UI 层，遵循 FSR/CAS 的 cvar → 对话框 → Presenter 存取器链路模式。UI 层不引入任何 `nvsdk_*` 头文件（许可证隔离）。
- **总体状态**：**实施完成 / PASS WITH FINDINGS**。Phase 1（主开关 + SR 子复选框）与 Phase 2（质量滑块）已全部实施并编译通过，主机侧测试全绿。质量变更后的后端重建、滑块实际渲染与 cvar 持久化在 RTX 4090 实机前标记 `UNVERIFIED`。

---

## A. 总结

M3-B 已交付真实 `DLSSSuperSamplingBackend` 与 Presenter 接线：`GuestOutputEnhancementConfig` 主开关 + SR 子开关全部默认 OFF，`IsSuperSamplingRequested()` 有效请求 = `master && sub`，Presenter 在 `IsSuperSamplingRequested()` 为真时惰性创建后端、失败时确定性回退原始路径。本文档为 DLSS 设计并实施一套平行的、独立的 UI 开关，参照 FSR/CAS 的 UI 开关链路（ImGui 对话框 → cvar → `GetGuestOutputPaintConfigFromUIThread` → 效果链构建）。

**关键差异**：FSR/CAS 是预创建 PSO，配置变更只影响效果链构建，无回退概念；DLSS 是 SDK `Evaluate` GPU 命令，开关变更 = 惰性后端创建，质量变更 = 后端重建，运行时缺失时静默回退。因此不能盲目复制 FSR/CAS 的 RadioButton 互斥模式，DLSS 主开关必须是独立的复选框（放在与 FSR/CAS 同级的 `Resampling and sharpening` 树节点内），其 SR 子复选框仅在主开关启用后显示。

**Phase 1（主开关 + SR 子开关）**：新增 cvar `dlss_enabled`（主开关）+ `dlss_sr_enabled`（SR 子开关）；在 "Post-processing" 对话框的 `Resampling and sharpening` 树节点内、与 FSR/CAS `RadioButton` 同级，新增 "NVIDIA DLSS" 主复选框 `master_enabled`，主复选框启用后才显示 SR 子复选框 `super_sampling_enabled`（主关闭时子复选框不绘制，仅保留其值）。新增 `GetGuestOutputEnhancementConfigForCvars()`，扩展 `ApplyDisplayConfigForCvars()` 调用 `SetGuestOutputEnhancementConfigFromUIThread`。仅触及 `emulator_window.cc/.h`，零后端改动。

**Phase 2（质量预设）**：`GuestOutputEnhancementConfig` 新增 `perf_quality` 字段与 `DlssSrPerfQuality` 枚举（Xenia 侧纯整数，UI 层零 `nvsdk_*` 依赖）；新增字符串 cvar `dlss_sr_perf_quality`（默认 `balanced`）与 `Super Resolution` 复选框之后的 `SliderInt` 滑块（仅 D3D12 && master && SR 开启时绘制）；后端构造函数增加 `perf_quality` 参数、`Initialize()` 入口越界输入契约校验、用其填充 `create_params.Feature.InPerfQualityValue`（替换硬编码 Balanced）；Presenter 惰性创建处传入配置质量、持"已用质量"哨兵、质量不匹配时 `reset()` 触发同帧重建。默认 `balanced` 与旧硬编码一致，首次启用 SR 不触发重建。

## 1. Phase 1：主开关 + SR 子开关（设计）

### 1.1 FSR/CAS UI 链路（模板，已核实）

- **对话框入口**：`emulator_window.cc` "Post-processing" 对话框内 `ImGui::TreeEx("Resampling and sharpening", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)`。
- **效果选择**：`RadioButton` 三选一——`None / Bilinear`、`AMD FidelityFX Contrast Adaptive Sharpening (CAS)`、`AMD FidelityFX Super Resolution 1.0 (FSR)`，写入 `new_presenter_config.SetEffect(...)`。**互斥单选**，因为效果链是单管线（`GetGuestOutputPaintFlow` 按 `GetEffect()` 分支）。
- **cvar 定义**（`emulator_window.cc`）：`postprocess_scaling_and_sharpening`（string，映射 Effect 枚举）、`postprocess_ffx_cas_additional_sharpness`（double）、`postprocess_ffx_fsr_sharpness_reduction`（double）。
- **cvar 保存**：对话框关闭时 `OVERRIDE_string(...)` / `OVERRIDE_double(...)` 持久化。
- **cvar 加载**：`GetGuestOutputPaintConfigForCvars()` 从 cvar 重建 `GuestOutputPaintConfig`。
- **写入 Presenter**：`ApplyDisplayConfigForCvars()` 调用 `presenter->SetGuestOutputPaintConfigFromUIThread(GetGuestOutputPaintConfigForCvars())`。
- **Presenter 读取**（`presenter.h`）：`GetGuestOutputPaintConfigFromUIThread()` 返回 `const GuestOutputPaintConfig&`。

### 1.2 M3-B 接线证据（已存在，本设计复用）

- **配置结构体**（`guest_output_enhancement.h`）：`GuestOutputEnhancementConfig` 含 `master_enabled`（主开关，默认 `false`）、`super_sampling_enabled`（SR 子开关，默认 `false`）、`optical_flow_enabled`、`frame_generation_enabled`、`experimental_neural_rendering_enabled`（后三者 M3 不启用，保持 `false`）。有效请求 `IsSuperSamplingRequested()` = `master_enabled && super_sampling_enabled`。注释明确：主开关 `false` 时不请求后端、不加载 DLL、不创建 GPU 资源、不执行额外 GPU 步骤。
- **Presenter 存取器**（`d3d12_presenter.h` / `d3d12_presenter.cc`）：`SetGuestOutputEnhancementConfigFromUIThread(...)` 在 `guest_output_enhancement_config_mutex_` 保护下写入；`GetGuestOutputEnhancementConfigFromUIThread()` 返回 `const GuestOutputEnhancementConfig&`。paint 线程在 `PaintAndPresentImpl` 内读取。
- **惰性后端创建**（`d3d12_presenter.cc` PaintAndPresentImpl 内）：`enhancement_config.IsSuperSamplingRequested()` 为真时，若 `dlss_sr_backend_` 为空则创建 `std::make_unique<DLSSSuperSamplingBackend>(...)`，然后 `SelectOutputPath(enhancement_config, dlss_sr_backend_.get(), command_list, ...)` 选择输出路径；成功则 paint 输入 = `dlss_sr_backend_->OutputResource()`，失败走 `BuildOriginalPathFallbackFlow` 原始路径。
- **运行时目录**（`d3d12_presenter.cc`）：`GetDLSSRuntimeDirectories()` 返回 `std::vector<std::wstring>`，供后端 `PathList` 加载 `nvngxruntime.dll`。

### 1.3 设计决策与 FSR/CAS 的关键差异

| 维度 | FSR/CAS（模板） | DLSS（本设计） |
|---|---|---|
| 效果选择 UI | `RadioButton` 互斥单选（单管线） | **独立复选框**，与 FSR/CAS 同级放在 `Resampling and sharpening` 树节点内（DLSS 是平行增强路径，不与 FSR/CAS 互斥）；SR 子复选框仅在主复选框启用后显示 |
| 配置变更影响 | 仅效果链构建（PSO 预创建） | 开关 ON → **惰性后端创建**（首帧 `CreateFeature` GPU 命令）；质量变更 → **后端重建** |
| 回退概念 | 无（PSO 总是可用） | **确定性回退**：运行时缺失/初始化失败/输入无效 → `BuildOriginalPathFallbackFlow` 原始链（`effect_count=1`, `kBilinear`）+ `CommandList::Reset`+重录 |
| 可用性 | 总是可用（纯 Xenia 内建） | 需要运行时 DLL + GPU；缺失时静默回退，不弹窗报错 |
| 主开关语义 | 无（Effect 枚举直接控制） | `master_enabled` 为总闸：`false` 时不加载 DLL、不创建 GPU 资源 |
| 子开关 | 无 | SR / NVOF / FG / 实验性神经渲染四独立子开关，全部默认 OFF，有效 = `master && sub` |

**设计注记（同级放置带来的两点语义）**：

1. **显示/隐藏而非禁用**：主复选框 `master_enabled` 关闭时，SR 子复选框**不绘制**（仅保留其值），而非禁用。由于有效请求 `IsSuperSamplingRequested()` = `master && sub`，子开关即使保留旧值也不会触发后端创建。主开关重新启用后，子复选框以先前状态重现。这与 `master && sub` 语义一致，无需在关闭主开关时重置子开关。
2. **重采样正交性**：DLSS SR 与 FSR/CAS 都是重采样/超采样方法，同级放置意味着用户可同时启用。按架构独立性原则，二者是正交模块，本设计保持正交（不做互斥约束）；同时启用会产生双重重采样，画质影响留给实机评估，不在设计阶段做取舍。

**Phase 1 UI 布局**（"Post-processing" 对话框内，`Resampling and sharpening` 树节点内，与 FSR/CAS 同级）：

```
▾ Resampling and sharpening          (DefaultOpen)
    ○ None / Bilinear                (RadioButton，不变)
    ○ AMD FidelityFX ... CAS         (RadioButton，不变)
    ○ AMD FidelityFX ... FSR 1.0     (RadioButton，不变)
    [✓] NVIDIA DLSS                  (master_enabled，与 FSR/CAS 同级)
        [ ] Super Resolution         (super_sampling_enabled，仅主复选框启用后显示)
      (NVOF / Frame Generation / Neural Rendering 子开关 Phase 2+ 再增)
```

## 2. Phase 2：质量预设 InPerfQualityValue（设计）

### 2.1 现状调查：现有 DLSS / Super Resolution UI 设计

**对话框与控件链路**（全部为已核实的工作树代码）：

| 环节 | 位置 | 内容 |
|---|---|---|
| 对话框入口 | `emulator_window.cc` `DisplayConfigDialog::OnDraw` | ImGui 窗口 "Post-processing" |
| 树节点 | `emulator_window.cc` | `ImGui::TreeNodeEx("Resampling and sharpening", Framed \| DefaultOpen)` |
| 效果单选 | `emulator_window.cc` | `RadioButton`：None/Bilinear、CAS、FSR、**NVIDIA DLSS（仅 D3D12 presenter 绘制）** |
| master 同步 | `emulator_window.cc` | 选中 DLSS → `master_enabled = true` 且强制 paint 效果为 bilinear（避免 FSR/CAS 双重重采样）；选其他项 → master 关闭 |
| SR 子开关 | `emulator_window.cc` | **仅当 `d3d12_presenter && master_enabled` 时绘制** `ImGui::Checkbox("Super Resolution", &new_dlss_sr)` |
| 写入 Presenter | `emulator_window.cc` | `presenter->SetGuestOutputPaintConfigFromUIThread(...)` / `d3d12_presenter->SetGuestOutputEnhancementConfigFromUIThread(new_enhancement_config)` |
| 持久化 | `emulator_window.cc` | 仅当值实际变化时 `OVERRIDE_bool(dlss_enabled, ...)` 与 `OVERRIDE_bool(dlss_sr_enabled, ...)`（仅 master 开启时） |
| cvar 定义 | `emulator_window.cc` | `dlss_enabled`、`dlss_sr_enabled` 均 `DEFINE_bool`，默认 `false`，归 "Display" 组 |
| cvar 加载 | `emulator_window.cc` `GetGuestOutputEnhancementConfigForCvars()` | bool 填充 `GuestOutputEnhancementConfig`；NVOF/FG/实验神经渲染子开关保持默认 OFF |
| cvar 应用 | `emulator_window.cc` `ApplyDisplayConfigForCvars()` | 启动时经 `dynamic_cast<D3D12Presenter*>` 写入 Presenter 增强配置 |
| Presenter 存取器 | `d3d12_presenter.h/.cc` | `Set/GetGuestOutputEnhancementConfigFromUIThread` 在 `guest_output_enhancement_config_mutex_` 保护下读写；paint 线程每帧读取 |

**后端现状（质量硬编码）**：

- 后端构造（工作树未提交版）：`DLSSSuperSamplingBackend(device, runtime_dirs, config_path)`——**无质量参数**。
- 质量硬编码：`create_params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced;`，初始化成功日志 `PerfQuality=Balanced`。
- 惰性创建点：`d3d12_presenter.cc` PaintAndPresentImpl 内 `if (!dlss_sr_backend_) { ... }`，仅当 `IsSuperSamplingRequested()` 且非 1:1（guest 输出尺寸 ≠ 交换链尺寸）时到达。
- 生命周期：`Shutdown()` 执行 `ReleaseFeature()` + SDK `Shutdown1` + 状态复位 `kNotInitialized`；析构调用 Shutdown。因此 Presenter 侧 `dlss_sr_backend_.reset()` 即完整释放 SDK 会话，下一帧按新配置惰性重建——**这就是质量变更的重建机制**（与 1:1/尺寸变更导致的重建同一通路）。

**既有 UI 中"数值选择"控件范式**（新滑块参照）：FSR/CAS 的数值控件采用固定范式——(1) `ImGui::TextUnformatted` 说明控件含义与方向；(2) `ImGui::SliderFloat("##HiddenId", &v, min, max, label, ImGuiSliderFlags_NoInput)`，label 为格式化后的当前值文本；(3) `ImGui::SameLine()` + `ImGui::Button("Reset##...")` 一键复位。新滑块逐条复用该范式。

### 2.2 InPerfQualityValue 的 SDK 定义（权威来源）

**枚举定义**（`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx_defs.h:251-260`）：

```c
typedef enum NVSDK_NGX_PerfQuality_Value
{
    NVSDK_NGX_PerfQuality_Value_MaxPerf,          // 0
    NVSDK_NGX_PerfQuality_Value_Balanced,         // 1
    NVSDK_NGX_PerfQuality_Value_MaxQuality,       // 2
    // Extended PerfQuality modes
    NVSDK_NGX_PerfQuality_Value_UltraPerformance, // 3
    NVSDK_NGX_PerfQuality_Value_UltraQuality,     // 4
    NVSDK_NGX_PerfQuality_Value_DLAA,             // 5
} NVSDK_NGX_PerfQuality_Value;
```

本滑块采用的值与对应数值：

| 档位 | 枚举 | 数值 |
|---|---|---|
| MaxPerf | `NVSDK_NGX_PerfQuality_Value_MaxPerf` | **0** |
| Balanced | `NVSDK_NGX_PerfQuality_Value_Balanced` | **1** |
| MaxQuality | `NVSDK_NGX_PerfQuality_Value_MaxQuality` | **2** |
| UltraPerformance | `NVSDK_NGX_PerfQuality_Value_UltraPerformance` | **3** |
| UltraQuality | `NVSDK_NGX_PerfQuality_Value_UltraQuality` | **4** |

DLAA=5 存在但**不在滑块内**（见 §2.3 与 §3.6 设计决策 D6）。

**参数流（创建期参数）**：

- `NVSDK_NGX_Feature_Create_Params`（`nvsdk_ngx_params.h`）：`InWidth/InHeight/InTargetWidth/InTargetHeight` + `/*** OPTIONAL ***/ NVSDK_NGX_PerfQuality_Value InPerfQualityValue;`。
- `NVSDK_NGX_DLSS_Create_Params`（`nvsdk_ngx_params.h`）= `Feature` + `InFeatureCreateFlags` + `InEnableOutputSubrects`。
- D3D12 创建辅助 `NGX_D3D12_CREATE_DLSS_EXT`（`nvsdk_ngx_helpers_d3d.h`）内部执行 `NVSDK_NGX_Parameter_SetI(pInParams, NVSDK_NGX_Parameter_PerfQualityValue, pInDlssCreateParams->Feature.InPerfQualityValue)`；参数名常量 `"PerfQualityValue"`（`nvsdk_ngx_defs.h`）。
- 后端经 `CallCreateDlss` → `NGX_D3D12_CREATE_DLSS_EXT` 走同一通路，`create_params` 由 `Initialize()` 填充。
- **语义**：质量是**创建期参数**，不是逐帧参数。SDK 文档明确：影响 DLSS 的设置变更须 "release the current feature and go back to step 3"（重新创建）。无"改参不重建"的文档化通路。

**官方文档对可选模式数的确认**（`DLSS_Programming_Guide_Release.pdf`）：

- 物理页 20-21（§3.2.1 DLSS Execution Modes）：六个 PerfQualityValues；**"DLAA 指输入=输出尺寸（1.0 缩放比），应在游戏中以独立 UI 选项暴露"**；"只展示已启用的模式，完全隐藏其余模式"。
- 物理页 59（§5.2.8）：**"The PerfQualityValue is currently defined in the NGX DLSS header as having five possible values. All five PerfQualityValue values should be checked and if they are enabled, they should be selectable in the Game UI (and hidden if disabled)."** 同页示例代码注释给出数值映射 `[0 MaxPerformance, 1 Balance, 2 MaxQuality, 3 UltraPerformance, 4 UltraQuality]`——与本设计采用的档位完全一致。
- RTX UI Developer Guidelines：官方 UI 模式顺序 `Off, Auto, Quality, Balanced, Performance, Ultra-Performance, DLAA`，Auto 是面向用户的分辨率映射而非 SDK 模式。本设计采用枚举名作为滑块标签（对模拟器语境更精确、与 SDK 日志一致），不实现 Auto（Auto 需要逐分辨率查询 Optimal Settings 的完整通路，超出本里程碑范围）。

### 2.3 新滑块设计（复用现有设计）

**控件布局**（"Resampling and sharpening" 树节点内）：

```
Resampling and sharpening                  (Framed | DefaultOpen，不变)
  (o) None / Bilinear                      (RadioButton，不变)
  (o) AMD FidelityFX CAS                   (RadioButton，不变)
  (o) AMD FidelityFX Super Resolution 1.0  (RadioButton，不变)
  (o) NVIDIA DLSS                          (RadioButton -> master_enabled)
      [ ] Super Resolution                 (Checkbox -> super_sampling_enabled)
      DLSS SR quality preset (lower is faster, higher is higher quality):
      [======*======] Balanced             [Reset]   <- SliderInt 0..3
```

- **可见性链**：D3D12 presenter 存在 && master 开 && SR 子开关开，三者同时满足才绘制。SR 取消勾选后滑块消失，**值保留**（与 SR 复选框在 master 关闭时的保留语义一致）；重新勾选后滑块以保留值重现。
- **D3D11 presenter**：不绘制（dynamic_cast 守卫），与 SR 复选框行为一致。

**控件选型**：离散命名档位，`ImGui::SliderInt`，`ImGuiSliderFlags_NoInput`，隐藏 ID `"##DLSSSrPerfQuality"`，当前档位名作为 value label。描述行说明"低档更快、高档更高质量"，并提示变更将重建 DLSS 会话。`SameLine` + `Reset` 按钮复位到默认档 Balanced。备选方案 `ImGui::Combo` 不采用（任务明确要求"滑块"且现有 FSR/CAS 数值控件均为 Slider 范式）。

**配置字段（设计决策 D1）**：新增字段 `DlssSrPerfQuality perf_quality = DlssSrPerfQuality::kBalanced;`。**对前序 Phase 2 设计表的修正**：前序 Phase 2 表曾提议字段类型直接用 `NVSDK_NGX_PerfQuality_Value`，该方案要求 `guest_output_enhancement.h` 引入 SDK 头文件，违反许可证隔离约定。本设计改为 **Xenia 侧具名枚举**：

```cpp
// Xenia-side mirror of NVSDK_NGX_PerfQuality_Value (DLSS SDK 310.9.1,
// nvsdk_ngx_defs.h:251-260). UI/config layer never includes nvsdk_* headers;
// the dlss/* backend maps this value to the SDK enum at feature creation.
enum class DlssSrPerfQuality : uint8_t {
  kMaxPerf = 0, kBalanced = 1, kMaxQuality = 2, kUltraPerformance = 3,
};
```

后端在 `Initialize()` 内做 `static_cast<NVSDK_NGX_PerfQuality_Value>(perf_quality)` 并校验范围（越界 → `BackendResult::kInputContractFailed` + 回退，不伪造、不钳位静默通过）。

**cvar 与持久化（设计决策 D2）**：新增 `DEFINE_string(dlss_sr_perf_quality, "balanced", ...)`（紧随 `dlss_sr_enabled` 之后）。字符串型 cvar 与 `postprocess_scaling_and_sharpening` 的命名值范式一致，配置文件人类可读。新增映射函数 `GetDlssSrPerfQualityForCvarValue(const std::string&)` 与反向 `GetCvarValueForDlssSrPerfQuality(...)`。`GetGuestOutputEnhancementConfigForCvars()` 读取新 cvar 填充 `perf_quality`。关闭对话框持久化：仅当 `master && super_sampling_enabled` 且值变化时 `OVERRIDE_string(dlss_sr_perf_quality, ...)`。**默认值 `balanced` = 当前硬编码值**，用户首次启用 SR 时不触发后端重建，行为与 M3-B 已验证状态完全一致；只有显式拖动滑块才重建。

**后端与 Presenter 接线（设计决策 D3 / D4）**：

- 后端构造函数扩为 `(device, runtime_dirs, config_path, perf_quality)`（`perf_quality` 为 Xenia 侧枚举），`Initialize()` 用它填充 `create_params.Feature.InPerfQualityValue`，替代硬编码；初始化成功日志输出实际档位名。
- **D3（重建判定的归属）**：Presenter 持有"已用质量"哨兵成员。惰性创建处：(1) 若 `dlss_sr_backend_` 为空 → 用配置中的质量创建并记录；(2) 若非空但配置质量 != 已用质量 → `dlss_sr_backend_.reset()`（析构 → `Shutdown()`）→ 同帧/下一帧以新质量惰性创建并记录新值；(3) 质量值来自每帧在 `guest_output_enhancement_config_mutex_` 下读出的 `enhancement_config`，无需额外加锁通路。不修改 `IDLSSSuperSamplingBackend` 接口。
- **D4（重建时机）**：在每帧创建判定处做（而非对话框关闭时）。判定成本为一次整数比较；重建本身是 GPU 命令，下一帧生效，与现有惰性创建语义一致。重建失败走既有确定性回退，绝不标记成功。
- **1:1 绕过**：guest 输出尺寸 = 交换链尺寸时后端不创建，滑块无实际效果（与现状一致）。

**设计决策 D6（排除 DLAA）**：DLAA=5 表示输入=输出尺寸（1.0 缩放），在模拟器场景下无对应操作（Xenia 的 guest 输出尺寸由游戏决定，不存在"以 1:1 送入 DLSS"的用户选项），且任务明确只列非 DLAA 档位；SDK 文档亦将 DLAA 单列为独立 UI 概念。滑块不暴露 DLAA。

## 3. 实施报告（Phase 1 + Phase 2）

按设计完成 DLSS SR 质量预设的 UI 全链路。修改文件如下：

| 文件 | 修改内容 |
|---|---|
| `src/xenia/ui/guest_output_enhancement.h` | 新增 `DlssSrPerfQuality` 枚举（kMaxPerf=0 … kUltraPerformance=3，无 DLAA）；`GuestOutputEnhancementConfig` 新增 `perf_quality` 字段（默认 kBalanced）。UI 层不引入 `nvsdk_*` |
| `src/xenia/app/emulator_window.h` | 声明静态映射函数 `GetCvarValueForDlssSrPerfQuality` / `GetDlssSrPerfQualityForCvarValue` |
| `src/xenia/app/emulator_window.cc` | 新增 `dlss_sr_perf_quality` 字符串 cvar（默认 `balanced`）；文件局部 `GetDlssSrPerfQualityName` 标签函数；对话框 SR 复选框后新增 `SliderInt` 0..3 + 描述行 + Reset 按钮（仅 D3D12 && master && SR 开时绘制，值保留语义同 SR 复选框）；`GetGuestOutputEnhancementConfigForCvars()` 读取新 cvar；两个 cvar↔枚举映射函数；关闭对话框时按 `master && SR && 值变化` 守卫 `OVERRIDE_string` |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h` | 构造函数增加 `ui::DlssSrPerfQuality perf_quality` 参数；新增 `created_perf_quality_` 成员 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc` | 匿名命名空间 `PerfQualityName` 日志名函数；构造函数接收并存储质量；`Initialize()` 入口对越界质量做输入契约校验（缓存 `kUnavailable` + `kInputContractFailed`，不钳位、不伪造）；`create_params.Feature.InPerfQualityValue` 由质量映射填充（替换硬编码 Balanced）；成功日志输出实际档位名 |
| `src/xenia/ui/d3d12/d3d12_presenter.cc` | 惰性创建处传入配置质量；质量不匹配时 `dlss_sr_backend_.reset()` 触发重建并记录日志 |

## C. 验证证据

- **构建**：`.\xb build --config release` → `Success!`（exit 0）。全部编译/链接步骤通过，含 `emulator_window.cc.obj`、`xenia-ui.lib`、`xenia-ui-d3d12.lib`、`xenia-dlss.lib`、`xenia_canary.exe`。
- **主机侧测试**：`xenia-ui-tests.exe` → **All tests passed**。`GuestOutputEnhancementConfig` 新增字段默认值不变，既有 `SelectOutputPath` / 回退逻辑用例全绿。
- 未执行 GPU 命令、未加载任何 NVIDIA DLL、未在实机运行。

## D. 环境限制

开发环境运行于 Hyper-V，无法安装 NVIDIA 驱动或执行真实 GPU 渲染。滑块的实际渲染效果、cvar 持久化往返、质量变更触发的后端重建（ReleaseFeature+Shutdown1+重 Init+CreateFeature 同帧序列）、各档位 CreateFeature 成功/回退，在 RTX 4090 实机验证前一律标记 **UNVERIFIED**。

## E. 剩余风险

1. **重建同帧安全性**：质量变更帧内先 `Shutdown1`（旧会话）再 `Init_with_ProjectID`+`CreateFeature`（新会话）。SDK 文档未明确禁止同帧 Shutdown→Init，理论上安全，但需 RTX 4090 实机确认无驱动侧异常。
2. **UltraPerformance/UltraQuality 可用性**：未查询 Optimal Settings，固定暴露档位；若某档被 SDK 判定不可用，CreateFeature 失败走确定性回退（原始路径+日志），UI 暂无灰显。
3. **cvar 非法值**：加载侧回退 `balanced`（不崩溃、不伪造），但 `xenia-ui-tests` 未链接 `xenia-app`，映射函数往返测试缺失。
4. **许可证隔离**：`guest_output_enhancement.h` 无 `nvsdk_*` 依赖；后端映射处是唯一 int→枚举转换点。`dlss/*` 外 SDK 依赖为零。
5. **1:1 绕过**：guest 输出=交换链尺寸时后端不创建，滑块无效果且无提示（与现状一致）。
6. **主开关语义边界**：`master_enabled = false` 时后端指针必须为 `nullptr`（不加载 DLL、不创建资源）。已创建的后端在主开关 OFF 后是否释放需实机确认。

## F. 下一步

1. 将 `xenia-ui-tests` 的 CMake `LINKS` 增加 `xenia-app`，补 cvar 映射函数往返（合法值 + 非法值回退）与 `GetGuestOutputEnhancementConfigForCvars()` 默认值断言。
2. RTX 4090 实机验证：滑块渲染与可见性链、各档位 CreateFeature 成功/回退、质量变更重建的日志序列与同帧 Shutdown→Init 安全性。

## G. 各项能力状态清单

- **DLSS SR**：[编译期已集成]（质量 UI 已实施并编译通过；运行时与画质 UNVERIFIED）
- **NVOF**：[未实现]
- **帧生成**：[未实现]
- **实验性神经渲染**：[未实现]
- **Presenter 集成**：[已接线且关闭]（主开关默认 OFF，后端未创建；质量哨兵默认=从未创建）
- **最终画质与性能**：[尚未评估]
