# Milestone 3.1：DLSS SR Render Preset 枚举（`NVSDK_NGX_DLSS_Hint_Render_Preset`）用法调查报告

- **文档性质**：INVESTIGATION / 调查报告（**修正版**，替代同文件上一轮版本）。本轮**未修改任何代码**，仅只读分析（官方手册精读、SDK 头文件核对、当前源码核对）+ 报告输出。未编译、未运行、未加载任何 DLL、未执行任何 GPU 操作。
- **修正说明**：上一轮报告把重心放在参数宏（`NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_*`，按模式选字符串）的接线可行性上。该部分已实现、不再重复。本轮**聚焦枚举类型 `NVSDK_NGX_DLSS_Hint_Render_Preset` 本身**：取值、语义、功能约束（曝光、动态分辨率、VRAM、OTA），以及与当前集成的交互。
- **仓库状态**：分支 `dev/dlss5-integration-sr-merge`，HEAD `fac5037`（ui: pass DLSS SR quality preset per-frame; backend re-creates feature on change）。工作树干净（仅本报告文档未跟踪）。
- **证据标签约定**：`STATIC_VERIFIED`（当前源码已核实）｜`SDK-DOC-SUPPORTED`（SDK 文档支持）｜`SDK-VERIFIED`（SDK 头文件支持）｜`DESIGN PROPOSAL`（设计建议，未实施）｜`UNVERIFIED`（未验证）｜`RUNTIME_VERIFIED_ON_RTX4090`（仅在真实 RTX 4090 证据存在时使用）。

---

## A. 总结

- `NVSDK_NGX_DLSS_Hint_Render_Preset` 是 DLSS SR 的 **create-time UI hint 枚举**，表示「选择哪个 DLSS 模型变体」。它在 `CreateFeature` 之前经 `NVSDK_NGX_Parameter_SetUI(参数块, <按模式的参数宏>, 枚举值)` 设置，**对整个 feature 生命周期固定**，不随缩放比变化。`SDK-DOC-SUPPORTED`（§3.12、§3.2.2）
- 310.9.1 头文件中的可用取值：**Default(0)、J(10)、K(11)、L(12)、M(13)**。`E/F` 弃用、`G/H/I/N/O` 保留（设置后静默回退默认）、`A-D` 已移除。`SDK-VERIFIED`
- **关键 ABI 发现**：用户引用的官方手册（310.6.0）§3.12 代码块**没有显式数值**，按 C 语言隐式编号会得到 J=6、K=7、L=8——这是**错的**。头文件中 A-D（数值 1-4）虽已移除但**保留了数值槽位**，因此 J=10、K=11、L=12、M=13。若按 PDF 隐式编号传 6/7/8，SDK 会解读为 F（弃用）/G（保留）/H（保留），全部「回退默认行为」，等于没设。头文件是数值的权威来源。`SDK-VERIFIED`
- **与当前集成的核心交互**：当前后端不传曝光纹理、使用 `AutoExposure` 创建标志 + `InPreExposure=1.0` / `InExposureScale=1.0`（标量）。§3.9 规定曝光**纹理**输入仅 Preset J/K 支持、Preset L 恒用 AutoExposure——当前后端的曝光配置与 J/K/L（及推断的 M）**全部兼容**，没有任何 preset 因曝光配置被排除。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`
- 当前后端**未设置该枚举**，运行在 Default 行为上（310.9.1 下按模式解析为 K/M/L）。后端未调用 `NVSDK_NGX_UpdateFeature`，无 OTA 拉取，Default 模型锁定在部署的 310.9.1.0 运行时 DLL 内。`STATIC_VERIFIED`
- **总体状态**：`PASS`。

---

## 1. 枚举定义：官方手册（310.6.0）与头文件（310.9.1）对照

### 1.1 用户引用的官方手册版本（PDF 310.6.0，§3.12，无显式数值）

```c
typedef enum NVSDK_NGX_DLSS_Hint_Render_Preset {
    NVSDK_NGX_DLSS_Hint_Render_Preset_Default,  // default behavior, weights may or may not be updated after OTA
    NVSDK_NGX_DLSS_Hint_Render_Preset_E,        // Deprecated
    NVSDK_NGX_DLSS_Hint_Render_Preset_F,        // Deprecated
    NVSDK_NGX_DLSS_Hint_Render_Preset_G,        // Reserved – please do not use
    NVSDK_NGX_DLSS_Hint_Render_Preset_H_Reserved,
    NVSDK_NGX_DLSS_Hint_Render_Preset_I_Reserved,
    NVSDK_NGX_DLSS_Hint_Render_Preset_J,
    NVSDK_NGX_DLSS_Hint_Render_Preset_K,
    NVSDK_NGX_DLSS_Hint_Render_Preset_L,
} NVSDK_NGX_DLSS_Hint_Render_Preset;
```

### 1.2 310.9.1 头文件版本（`nvsdk_ngx_defs.h:70-88`，权威）

```c
typedef enum NVSDK_NGX_DLSS_Hint_Render_Preset {
    NVSDK_NGX_DLSS_Hint_Render_Preset_Default = 0,   // default behavior, may or may not change after OTA
    // A-D removed (数值 1-4 保留槽位), use preset J or K
    NVSDK_NGX_DLSS_Hint_Render_Preset_E = 5,          // Deprecated
    NVSDK_NGX_DLSS_Hint_Render_Preset_F = 6,          // Deprecated
    NVSDK_NGX_DLSS_Hint_Render_Preset_G = 7,          // Do not use, reverts to default behavior
    NVSDK_NGX_DLSS_Hint_Render_Preset_H_Reserved = 8, // Do not use, reverts to default behavior
    NVSDK_NGX_DLSS_Hint_Render_Preset_I_Reserved = 9, // Do not use, reverts to default behavior
    NVSDK_NGX_DLSS_Hint_Render_Preset_J = 10,         // 类似 K；ghosting 略少、flicker 略多；K 通常优于 J
    NVSDK_NGX_DLSS_Hint_Render_Preset_K = 11,         // DLAA/Balanced/Quality 默认；transformer based；最佳画质、更高性能开销
    NVSDK_NGX_DLSS_Hint_Render_Preset_L = 12,         // Ultra Perf 默认
    NVSDK_NGX_DLSS_Hint_Render_Preset_M = 13,         // Perf 模式默认
    NVSDK_NGX_DLSS_Hint_Render_Preset_N = 14,         // Do not use, reverts to default behavior
    NVSDK_NGX_DLSS_Hint_Render_Preset_O = 15,         // Do not use, reverts to default behavior
} NVSDK_NGX_DLSS_Hint_Render_Preset;
```

### 1.3 数值差异（关键 ABI 点）

| 名称 | PDF 310.6.0 隐式编号 | 头文件 310.9.1 显式数值 |
|---|---|---|
| Default | 0 | **0** |
| A-D（已移除） | （无） | **1-4（槽位保留）** |
| E | 1 | **5** |
| F | 2 | **6** |
| G | 3 | **7** |
| H_Reserved | 4 | **8** |
| I_Reserved | 5 | **9** |
| J | 6 | **10** |
| K | 7 | **11** |
| L | 8 | **12** |
| M | （PDF 无） | **13** |
| N / O | （PDF 无） | **14 / 15** |

- 按 PDF 隐式编号传 J=6/K=7/L=8，SDK 会解读为 F/G/H（全部弃用或保留）→「reverts to default behavior」，**静默无效**，不会报错。这是最隐蔽的集成错误。`SDK-VERIFIED`
- 代码必须引用头文件枚举（`nvsdk_ngx_defs.h`），数值由编译器给出；Xenia 侧配置层若用纯整数承载（许可隔离），**必须用 0/10/11/12/13**，不得用 0-8 顺序编号。`DESIGN PROPOSAL`

---

## 2. 各取值的语义与状态（官方手册 + 头文件双源）

| 数值 | 名称 | 状态 | 官方语义 |
|---|---|---|---|
| 0 | `Default` | 可用 | 默认行为；**OTA 更新后 weights 可能改变**。按当前算法版本与质量模式解析为 K/M/L |
| 1-4 | A-D | **已移除**（310.4.0 起） | 头文件注释：use preset J or K。传这些数值的行为文档未明示（推断回退默认，`UNVERIFIED`） |
| 5 | `E` | **弃用** | 310.6.0 重新加回并标 deprecated（仅为保留数值槽位与 VRAM 表）。不应使用 |
| 6 | `F` | **弃用** | 唯一支持 bias buffer（§3.15）的 preset，但 bias buffer 本身「should not be used」。不应使用 |
| 7 | `G` | 保留 | 3.1.13（2023）加入，现已「reverts to default behavior」。不应使用 |
| 8 | `H_Reserved` | 保留 | 回退默认行为 |
| 9 | `I_Reserved` | 保留 | 回退默认行为 |
| 10 | `J` | 可用 | transformer based（310.1.0 起为新 Transformer Model）。ghosting 略少于 K，flicker 略多；**K 通常优于 J**。支持曝光纹理输入（§3.9） |
| 11 | `K` | 可用 | **DLAA/Balanced/Quality 模式的默认 preset**。transformer based。官方描述「Best image quality preset at a higher performance cost」（但比 L 便宜）。支持曝光纹理输入（§3.9） |
| 12 | `L` | 可用 | **UltraPerformance 模式的默认 preset**。比 J/K 更锐利、更稳定、ghosting 更少，性能开销更高；**RTX 40 系及以后最优**。恒用 AutoExposure（§3.9） |
| 13 | `M` | 可用 | **Performance 模式的默认 preset**。画质接近 L，速度接近 J/K；**RTX 40 系及以后最优**。曝光行为文档未明示（`UNVERIFIED`，推断同 L） |
| 14 | `N` | 保留（仅头文件） | 回退默认行为 |
| 15 | `O` | 保留（仅头文件） | 回退默认行为 |

**历史沿革**（官方手册 Revision History，p.5-6）：2.5.1（2023-01）加入 preset selection 章节；3.1.13（2023-05）加 DLAA 模式 + `G` 枚举；310.1.0（2025-01）J 改为新 Transformer Model（非默认）；310.2.0（2025-02）加 `K`；310.3.0（2025-05）弃用 A-E；310.4.0（2025-07）移除 A-E；310.5.0（2025-11）加 `L`/`M` 并更新 Performance/UltraPerformance 的默认 preset；310.6.0（2026-02）重新加回 E/F（标 deprecated）并补 VRAM/执行时间表。`SDK-DOC-SUPPORTED`

---

## 3. 用法规则（官方手册）

### 3.1 设置时机与生命周期

- **create-time**：`NVSDK_NGX_Parameter_SetUI(parameter_block, <参数宏>, 枚举值)` 必须在 `CreateFeature` 之前调用。`SDK-DOC-SUPPORTED`（§3.12）+ `SDK-VERIFIED`（`NGX_D3D12_CREATE_DLSS_EXT` 先 SetUI 后 CreateFeature 的模式）
- **整个 feature 生命周期固定**：「The Preset chosen for the init-time input size will persist across the lifetime of the feature (presets will not vary even if scaling ratio varies)」——即使动态分辨率改变缩放比，preset 也不变。改 preset 必须 ReleaseFeature + 重建。`SDK-DOC-SUPPORTED`（§3.2.2）
- 按模式各设一次：每个 PerfQuality 模式有独立参数宏（DLAA/Quality/Balanced/Performance/UltraPerformance/UltraQuality），可各自选不同 preset。`SDK-DOC-SUPPORTED`（§3.12）——此部分已实现，不重复展开。
- `DLSS.Hint.Render.Preset.DLAA` 仅在输入尺寸 == 输出尺寸时生效。`SDK-DOC-SUPPORTED`（§3.12）

### 3.2 曝光约束（与枚举强相关，§3.9）

- 官方原文：「**Only supported by Presets J and K. Preset L always uses AutoExposure.**」
- 含义：曝光**纹理**输入（`pInExposureTexture`，1x1 纹理）只有 J/K 接受；L 恒用 SDK 内部 AutoExposure；M 未明示（310.5.0 与 L 同版加入，推断同 L，`UNVERIFIED`）。
- **当前后端曝光配置**（`STATIC_VERIFIED`）：创建时设 `NVSDK_NGX_DLSS_Feature_Flags_AutoExposure`（bit 6）；evaluate 时 `InPreExposure = 1.0f`、`InExposureScale = 1.0f`（CPU 标量）；**不传** `InExposureTexture`（全文件无引用）。
- 结论：当前配置与 J/K/L（及推断的 M）**全部兼容**——J/K 下 AutoExposure 标志使其忽略曝光纹理，L 本就恒用 AutoExposure。没有任何 preset 因曝光配置被排除。`DESIGN PROPOSAL`

### 3.3 动态分辨率约束（§3.2.2.1，L/M 专用）

- 官方原文：L/M 在 Feature Creation 时按**最大维度**静态分配内部缓冲；执行成本随该最大分配缩放，而非当前帧输入分辨率；**若某帧输入分辨率超过创建时维度，触发昂贵的内部缓冲重分配，造成严重帧时间尖峰**。
- 官方建议：动态分辨率场景下「always initialize the DLSS feature using the maximum target resolution of your dynamic range」。
- **当前后端行为**（`STATIC_VERIFIED`）：输入/输出尺寸变化时 ReleaseFeature + 重建（创建维度恒等于当前帧尺寸），`InRenderSubrectDimensions` 恒等于全输入尺寸（无动态分辨率）。因此**永远不会出现输入超过创建维度的情况**，L/M 的静态分配特性对当前集成无害。`STATIC_VERIFIED` + `DESIGN PROPOSAL`

### 3.4 VRAM 与执行时间（官方表，p.5-7）

VRAM（输出分辨率 1920x1080 / 2560x1440 / 3840x2160，RTX 50/40 系）：

| Preset | 1080p | 1440p | 2160p |
|---|---|---|---|
| E, F | 58.99 MB | 95.22 MB | 197.91 MB |
| J, K | 85.77 MB | 143.54 MB | 307.37 MB |
| L | 114.41 MB | 200.94 MB | 448.18 MB |
| M | 116.32 MB | 204.39 MB | 456.02 MB |

执行时间（16-bit Performance Mode，输入为输出像素的 1/4；RTX 4090 行）：

| Preset | 1080p | 1440p | 2160p |
|---|---|---|---|
| E, F | 0.17 ms | 0.25 ms | 0.53 ms |
| J, K | 0.38 ms | 0.54 ms | 1.06 ms |
| L | 0.59 ms | 0.85 ms | 1.67 ms |
| M | 0.41 ms | 0.64 ms | 1.39 ms |

- 4090 上 L 比 J/K 慢约 55%，M 介于 J/K 与 L 之间（比 L 快约 17%）；VRAM 上 L/M 比 J/K 多约 40%（2160p 下 J/K 307 MB vs L/M 448-456 MB）。对 24 GB 的 4090 无压力。`SDK-DOC-SUPPORTED`
- 注：表中执行时间为「command line utility（无 3D renderer）」的粗略估计，实际集成数值不同。`SDK-DOC-SUPPORTED`

### 3.5 OTA 行为（§2.4 + §3.12.1）

- OTA 需应用**主动 opt-in**：调用 `NVSDK_NGX_UpdateFeature(ApplicationId, FeatureID)`（须在独立线程；更新在下一次 `Init` 时生效）。
- 显式选定 preset 的应用：OTA 后**保留**该模型。使用 Default 的应用：OTA 后默认模型**可能改变**。
- **当前后端**：仅调用 `NVSDK_NGX_D3D12_Init_with_ProjectID`，**从未调用 `NVSDK_NGX_UpdateFeature`**（`STATIC_VERIFIED`）→ 无 OTA 拉取；Default 模型锁定在部署的 310.9.1.0 运行时 DLL 内，不会自行漂移。运行时 DLL 版本升级（换 nvngx_dlss.dll）时才可能改变 Default 的解析结果。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`

---

## 4. 与当前集成的交互与选型建议

### 4.1 当前后端的 preset 现状

- 当前后端**未设置** `NVSDK_NGX_DLSS_Hint_Render_Preset`（全文件 grep 无命中），运行在 Default 行为上。`STATIC_VERIFIED`
- Default 在 310.9.1 下按模式解析（文档默认）：Balanced/MaxQuality → **K**；MaxPerf → **M**；UltraPerformance → **L**。因此当前已验证的运行时基线（M3-C：1280×720 → 3840×2160 Balanced，用户确认正常）**实际跑在 K 上**。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`
- 后端曝光配置（AutoExposure 标志 + 标量 PreExposure/ExposureScale，无曝光纹理）与 K 兼容（K 支持曝光输入，AutoExposure 标志使其用内部自动曝光）。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`
- 后端尺寸/质量变更即重建 feature 的模式，天然满足 L/M 的「创建维度 >= 任何输入维度」要求。`STATIC_VERIFIED`

### 4.2 选型建议（`DESIGN PROPOSAL`，未实施）

| 场景 | 建议取值 | 理由 |
|---|---|---|
| 保持现状（零行为变化） | **Default (0)**，即完全不 Set | 与当前已验证基线完全一致；无 OTA 拉取，Default 锁定 310.9.1.0 |
| 显式固定模型（防未来运行时升级漂移） | 按模式设 **K(11) / M(13) / L(12)** | 与文档默认一致，但跨 OTA/运行时升级固定；显式选定即受 §3.12.1「保留该模型」保护 |
| UltraPerformance 模式追求画质 | **L (12)** | 文档默认；4090 上比 J/K 慢约 55%（2160p 1.06 → 1.67 ms），VRAM +140 MB |
| Performance 模式追求画质 | **M (13)** | 文档默认；比 L 快约 17%，画质接近 L |
| 诊断对比（ghosting vs flicker） | **J (10)** | K 的替代品，ghosting 略少、flicker 略多；不作为默认 |
| 禁止 | A-D(1-4)、E(5)、F(6)、G/H/I(7-9)、N/O(14-15) | 已移除/弃用/保留；G/H/I/N/O 静默回退默认（无报错），A-D 行为文档未明示 |

- 数值面提醒（再次强调）：Xenia 侧配置若用纯整数承载枚举（许可隔离），合法集合为 **{0, 10, 11, 12, 13}**；越界（尤其 1-9、14-15）应作为输入契约失败处理（`kInputContractFailed`，确定性回退，绝不静默钳制），与 `perf_quality_` 的越界处理同构。`DESIGN PROPOSAL`
- 零深度/零 MV 占位输入（当前后端的「honest fallback」）与 preset 选择正交：任何 preset 都要求正确的 depth/MV 才能发挥设计画质，但都接受占位输入而不崩溃。preset 不改变这一约束。`DESIGN PROPOSAL`

### 4.3 风险与未决项

- **M 的曝光行为**文档未明示（§3.9 只说 J/K/L）。若 M 既不支持曝光纹理也不自动 AutoExposure，在 HDR 输入下可能有画质风险——当前后端输入为 LDR（guest 输出经 gamma ramp，R10G10B10A2_UNORM），且用 AutoExposure 标志，风险极低。`UNVERIFIED`
- **A-D 数值的行为**文档未明示（移除后传 1-4 是报错还是回退默认）。`UNVERIFIED`
- **PDF 隐式编号误导**（J=6/K=7/L=8）：任何按手册代码块手抄数值的集成都会静默失效。以头文件显式数值为唯一权威。`SDK-VERIFIED`
- Preset 语义随 SDK 修订可变（官方明示）；未来升级运行时 DLL 须重读本节。
- 所有枚举取值结论在 Hyper-V 下为 `UNVERIFIED`；唯一已验证基线是 Default（=K for Balanced）在 RTX 4090 上工作正常（M3-C）。`RUNTIME_VERIFIED_ON_RTX4090`（仅 Default/K 路径，画质未评估）

---

## B. 修改文件

| 文件 | 修改内容 | 原因 |
|---|---|---|
| `docs/dlss-integration/milestone-3-1-dlss-sr-render-preset-investigation.md` | 重写（本报告，修正版；替代上一轮同文件版本） | 用户指出关注点错误：参数宏接线已实现，报告应聚焦枚举 `NVSDK_NGX_DLSS_Hint_Render_Preset` 本身 |

**未修改任何代码文件。**

## C. 验证证据

- **C.1 官方手册提取**（`DLSS_Programming_Guide_Release.pdf`，310.6.0，84 页，pypdf）：§3.12/3.12.1（p.28-29）、§3.9 曝光（p.25，「Only supported by Presets J and K. Preset L always uses AutoExposure.」）、§3.2.2/3.2.2.1 动态分辨率（p.11-13，L/M 静态分配与重分配尖峰）、§2.4 OTA（p.5，`NVSDK_NGX_UpdateFeature`）、§3.14 VRAM 机制（p.30）、执行时间/VRAM 表（p.5-7，含 RTX 4090 行）、Revision History（p.5-6，preset 沿革 2.5.1 → 310.6.0）、§3.15 bias buffer（p.30，仅 F 支持且不应使用）。
- **C.2 头文件核对**（`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\`）：`nvsdk_ngx_defs.h:70-88`（枚举显式数值 0/5-15）、:251-260（PerfQuality）、:286-300（Feature_Flags，AutoExposure=bit 6）、:848-853（参数宏，含 UltraQuality）；`nvsdk_ngx_params.h:85-86`（SetUI 签名：`unsigned int InValue`）；`nvsdk_ngx_helpers_d3d.h:275-294`（create 流程：先 SetUI 后 CreateFeature）。
- **C.3 源码核对**：`dlss_super_sampling_backend.cc` —— :774（AutoExposure 创建标志）、:908-919（eval 参数：无 `InExposureTexture`，`InPreExposure=1.0f`、`InExposureScale=1.0f`、`InRenderSubrectDimensions` 恒为全输入尺寸）、:131-139/:552-596（`Init_with_ProjectID`，全文件无 `UpdateFeature` 调用）；全文件 grep 无 `Hint_Render`/`Render_Preset`。`guest_output_enhancement.h`（`DlssSrPerfQuality` 0-3 定义）。
- **C.4 工作树状态**：`git status --short` 仅本报告文档未跟踪；HEAD `fac5037`。

## D. 环境限制

- Hyper-V 无 NVIDIA 驱动/GPU：未执行真实初始化/`Evaluate`/preset 对比。所有枚举取值结论 `UNVERIFIED`。
- 未加载 DLL、未构建（无代码修改）。

## E. 剩余风险

- PDF 隐式编号 vs 头文件显式数值的 ABI 陷阱（§1.3）：手抄数值会静默失效。
- M 的曝光行为、A-D 移除值的行为均未明示（`UNVERIFIED`）。
- Preset 语义随 SDK 修订漂移（官方明示）；Default 解析结果随运行时 DLL 版本变化（当前无 OTA 拉取，受控）。
- 无许可证新增面（枚举为既有头文件内容）。

## F. 下一步（最小且安全）

- 若用户批准集成：后端增加 create-time 枚举字段，**默认 Default(0) 即完全不 Set**（零行为变化）；合法集合 {0,10,11,12,13}，越界 → `kInputContractFailed`；接线点为 Initialize 中回调 Set 之后、`CallCreateDlss` 之前（一次 `SetUI`）；初始化日志追加枚举名称；主机侧测试断言默认路径不 Set。实施需明确授权。
- 实机对比（J/K/L/M 的画质与帧时间，4090 上官方预估 J/K 0.38-1.06 ms、L 0.59-1.67 ms、M 0.41-1.39 ms）推迟至 RTX 4090 验证阶段。
- M4（NVOF）按 `milestone-3-c-handoff.md` §F 推进，不受本报告影响。

## G. 各项能力状态清单

- **DLSS SR**：编译期已集成（RTX 4090 运行时已验证 Balanced/Default=K 路径，画质未评估）。Render preset 枚举：本轮完成用法调查，**未集成**（接线点、合法数值集合、越界处理均已明确，实施待授权）。
- **NVOF**：未实现（M4 已规划）。
- **帧生成**：未实现。
- **实验性神经渲染**：仅研究。
- **Presenter 集成**：已接线且关闭（主开关默认 OFF），回退已验证。
- **最终画质与性能**：本地评估（运行时功能已验证，画质未评估）。
