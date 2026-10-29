# Milestone 6-A：实验性神经渲染（NR / DLSS 5）可行性报告

- 日期：2026-10-28
- 阶段：Milestone 6-A（严格只读审计，未修改任何生产代码）
- 设计依据：`architecture-design.md` K4.3
- 配套文档：`milestone-6-a-NR_IMPLEMENTATION_PLAN.md`

---

## 1. 执行摘要与最终可行性结论

**最终结论：有条件可行。**

技术路径已具备完整的静态实现证据（E2）：Veyra-NRVideo 的 NR（NVIDIA DLSS 神经渲染，内部称 "DLSSNR / Feature 18"）调用链已被完整追踪至实际调用点；其全部输入契约在 Xenia 侧均有现成来源或零占位符；目标硬件 RTX 4090 可使用的运行时（社区 ShortFuse 310.9.16）已在本地部署并通过 Veyra 的实机验证记录。

但 6-B 准入前存在三项明确的前置事项（均为"有条件"成分，非硬性阻断）：

1. **许可与运行时身份**：4090 上可用的 `nvngx_dlssnr.dll` 是**未签名的社区 ShortFuse 310.9.16-cuda13.4.3 构建**（NVIDIA 官方签名原版 310.8.0.0 仅限 RTX 50）。其调用方身份伪装机制（IAT 修补 `GetModuleFileNameW`）必须在 Xenia 中如实披露并经用户批准；分发层面受决策 6 约束（本地研究允许，公开分发禁止）。
2. **Parity 编解码契约**：Feature 18 的输入/输出空间是 Veyra 逆向自真实游戏 addon 的 "RenoDX-equivalent" 编解码（线性 BT.709 → sRGB 编码 RGBA8 proxy，解码为 original+proxy+neural 三路合成回线性 FP16）。该数学在 Veyra 中以 CPU 双精度黄金参考完整实现（GPL-3.0 代码），Xenia 需**净室重实现**数学（公式不受版权约束），不得复制代码。
3. **真实 GPU 验证**：开发环境（Hyper-V）无 NVIDIA 驱动，所有 NR 执行、画质、显存、延迟项在 4090 实机验证前均为 `UNVERIFIED`。

**明确不属于**：不存在"官方 SDK 神经渲染 API"可供 Xenia 直接调用——本地 DLSS SDK 310.9.1 头文件家族（dlss / dlssg / dlssd）中没有任何神经渲染接口（`dlssd` 经核实为 **DLSS Denoise + Ray Reconstruction**，非神经渲染）。NR 只能通过私有运行时 `nvngx_dlssnr.dll` 的保留 Feature ID 18 到达。

---

## 2. 审计环境、Git 基线与依赖版本

### 2.1 Xenia Canary 基线

- 分支：`dev/dlss5-integration`；HEAD：`757925f`（"ui: drop the 1:1 enable gate for SR/NVOF/FG; add DLAA and UltraQuality quality tiers to DLSS SR with a Balanced fallback"，领先 origin 1 个提交，未推送）
- 工作区状态：`docs/dlss-integration/architecture-design.md` 有未提交修改（用户工作，本次审计未触碰）；另有本里程碑两份新交付物（未跟踪）。审计启动时存在的其余 7 个未提交修改已由用户在审计期间以 `757925f` 提交。
- 基线注意：`757925f` 移除了 SR/NVOF/FG 的 1:1 启用门控（后端现在接收真实请求的输入/输出尺寸并自行判定），SR 质量档位扩展为六档（新增 UltraQuality/DLAA，初始化失败时下一帧以 Balanced 重试）。NR 的 1:1 语义不受影响，但 NR 输入源在 SR 启用时变为 SR 输出纹理（见第 5 节矩阵）。
- NR 相关现状：`src/xenia/ui/dlss/` 下仅有 `sr`、`nvof`、`fg`、`ngx` 四个子模块，**无 NR 模块**；NR 仅有配置钩子（`guest_output_enhancement.h:184` 的 `experimental_neural_rendering_enabled = false` 与 `:199-200` 的 `IsExperimentalRequested()`）。

### 2.2 Veyra-NRVideo 基线

- 目录：`V:\DLSS5_Research\Veyra-NRVideo\`（非 Git 检出，按文件内容定版）
- 内容版本：2.1.1（2026-10-27 发布说明在 README.md 中）；许可证 **GPL-3.0**
- 审计范围：`src/`、`include/`、`tools/nr_probe`、`docs/`（NR 相关 40 篇文档仅作旁证）
- 排除项：AMD Lmxxf 后端内部实现（仅审计其接口边界）、Intel XESS 路径、采集卡/导出路径、UI/QML 层。Playbook 文档本体不在 Veyra 树内（外部知识库），以代码注释与 CPU 参考实现为契约证据。

### 2.3 SDK 与运行时版本

| 项 | 版本 | 位置 | 证据等级 |
|---|---|---|---|
| DLSS SDK 头文件 | 310.9.1 | `C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\` | E2 |
| NVOF SDK | 5.0.7 | `C:\SDKs\NVIDIA\Optical_Flow_SDK_5.0.7` | E2 |
| SR 运行时 | `nvngx_dlss.dll` 310.7.0.0（NVIDIA 签名） | `runtime\experimental\` | E2（manifest） |
| FG 运行时 | `nvngx_dlssg.dll` 310.9.1.0（NVIDIA 签名）+ `dlssg-lite` 310.9.16.0 | `runtime\experimental\` | E2（manifest） |
| **NR 运行时（4090 可用）** | `nvngx_dlssnr.dll` **310.9.16.0**，**NotSigned**，ShortFuse compat-fp16-310.9.16-cuda13.4.3 | `runtime\experimental\nr-ampere\` | E2（manifest + 适配器代码） |
| **NR 运行时（仅 RTX 50）** | `nvngx_dlssnr.dll` **310.8.0.0**，NVIDIA 签名 Valid | `runtime\experimental\nr-original\` | E2（manifest） |
| Transfusion PTX 内核 | v1.4.5.3（sm86 优化，无许可授予） | `runtime\experimental\dlssg-kernels\` | E2（manifest） |

运行时分类以 `runtime\experimental\release-runtime-manifest.json`（schema 3, package 2.1.0）为权威记录。

---

## 3. Veyra-NRVideo 实际调用链与关键源码证据

> 证据等级标注：E2 = 静态实现证据（源码/头文件/调用点），E1 = 文档或注释证据。所有结论均可按下表路径复核。

### 3.1 入口与运行时加载

- **适配器**：`src/ngx/DlssNrRuntimeAdapter.cpp`
  - 导出表（`kExportNames[]`，约 :15）：`NVSDK_NGX_D3D12_Init_Ext`、`NVSDK_NGX_D3D12_CreateFeature`、`NVSDK_NGX_D3D12_EvaluateFeature`、`NVSDK_NGX_D3D12_ReleaseFeature`、`NVSDK_NGX_D3D12_Shutdown1`（后两者非公共 SDK 导出，属私有运行时）。
  - `load()`（约 :396-470）：DLL 名为 `nvngx_dlssnr.dll`，**仅允许**从 `runtime_local\nvidia\` 或 `runtime\experimental\`（含 `nr-community`/`nr-ampere`/`nr-original` 子目录）加载，否则拒绝。加载前强制 `processNvapi()`（防止运行时自身 FreeLibrary 触发驱动 detach）。
  - `CallSnippetInitExt/CreateFeature/EvaluateFeature/ReleaseFeature`：全部用 SEH `__try/__except` 包裹（私有运行时可能崩溃），异常时返回 `NVSDK_NGX_Result_FAIL_PlatformError` 并记录异常码。
- **保留 Feature ID**：`include/veyra/ngx/DlssNrRuntimeAdapter.h:50` — `static constexpr NVSDK_NGX_Feature kFeatureId = static_cast<NVSDK_NGX_Feature>(18);`。公共 SDK 头文件中无 Feature 18 的枚举定义（E2：`rg "Transfusion|DLSSNR" C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include` 零命中）。
- **调用方身份伪装（Playbook 8.3，E1+E2）**：签名 snippet 通过 `GetModuleFileNameW` 查询调用模块名，要求看到字面量 `L"nvngx.dll"`。Veyra 仅在 snippet 自己的 IAT 槽位上安装 `SnippetGetModuleFileNameW` 垫片（非全局 Kernel32 hook），`installCallerCompatibility()` 找到 `GetModuleFileNameW` 导入槽后原子交换，卸载时还原。
- **Ampere 架构改写（E2）**：`installAmpereCompatibility()` 对 snippet 导入表中的 `GetProcAddress` 安装作用域改写（`AmpereGetArch`），仅对选定的 RTX30 适配器把 NVAPI 架构号 0x170 改写为 0x1B0。**RTX 4090（Ada，0x190）不需要改写**（`NrArchitecturePolicy.h::nrRuntimeNeedsAmpereRewrite` 对 310.9.16 返回 false）。

### 3.2 参数契约

- **`include/veyra/ngx/DlssNrParameters.h`**（全文件，E2，"Playbook 8.5/8.6 集中化"）：
  - 创建期：`DLSSNR.Width/Height`、`DLSSNR.InputWidth/InputHeight`、`DLSSNR.OutputWidth/OutputHeight`、`DLSSNR.Output.Width/Height`、`DLSSNR.Upscaling`、`DLSSNR.Scale`、`DLSSNR.ScalingRatio`、`DLSSNRComputeScalingRatioCallback`、`DLSSNR.Hint.Render.Preset`、`PerfQualityValue`、`CreationNodeMask`、`VisibilityNodeMask`。
  - 求值资源：`DLSSNR.Color`、`DLSSNR.Output`、`DLSSNR.MVec`、`DLSSNR.Depth`（各带 Subrect 四元组）。
  - 求值控制：`DLSSNR.MVecScaleX/Y`、`DLSSNR.DepthInverted`、`DLSS.Indicator.Invert.X/Y.Axis`、`DLSSNR.Enabled`、`DLSSNR.Reset`、`DLSSNR.Style`、`DLSSNR.Intensity`、`DLSSNR.LocalToneStrength`、`DLSSNR.LocalStructureStrength`、`DLSSNR.SkinStructureStrength`、`DLSSNR.UseAutoMask`、`DLSSNR.UICorrection`。
- **创建调用**：`src/pipeline/EnhanceGraph.cpp::createNrFeature`（:1576-1607，E2）——1:1 模式（InputWidth==OutputWidth、`Upscaling=0`、`Scale=1.0`、`ScalingRatio=1.0`）、`PerfQualityValue=1`、两个 NodeMask 均为 1、`Hint.Render.Preset` 由 `nrRenderPreset()` 决定（SF 运行时：performance→2，否则→1；其他运行时→0）。创建在独立命令列表上执行，`submitAndSignal(0) && waitIdle()` 后才算成功；失败置 `FailedBackend::Nr` 并返回 false（**整图构建失败**）。

### 3.3 每帧求值路径（单图层产品路径）

- **`src/pipeline/EnhanceGraph.cpp`**（:2930-3010 区域，E2）：
  1. 在**新命令列表**上记录（snippet 的单图层约束）。
  2. 资源绑定：`Color = layer->proxy()`（R8G8B8A8_UNORM，parity 编码后）、`Output = layer->neural()`（R8G8B8A8_UNORM）、**`MVec = haveFlow && nrUsesFlow() ? layer->motion() : layer->zeroMotion()`**、**`Depth = layer->zeroDepth()`（恒为零深度）**。
  3. 控制参数：`MVecScaleX/Y = 1.0`（原始像素单位，不做归一化）、`DepthInverted = 1`、`IndicatorInvert = 0/0`、`Enabled = 1`、`Reset = reset || inputRevision != 1`（上游变更即重置历史）、Style/Intensity/Tone/Structure/Skin/AutoMask/UICorrection 来自 `NrSettings`。
  4. 求值：`snippetEvaluateFeature(nlist, handle, parameters, er, es)`；失败时记录诊断（stage="NR Evaluate"、ngx 结果码、SEH 码）、置 `FailedBackend::Nr`、**return false**（整帧增强中止，无部分输出）。
  5. 成功后：UAV 屏障 + 状态迁移，执行 **parity decode**（`decPass_`，输出到 `finalRgba_` R16G16B16A16_FLOAT），`historyValid = true`、`inputRevision = 1`。
- **测试钩子**：`VEYRA_TEST_REJECT_NR_STYLE2`、`VEYRA_TEST_NR_RUNTIME_FAILURE`（环境变量注入失败，仅测试用）。
- **失败语义**：NR 失败 = 整帧 enhance 失败（诚实回退，无部分提交）。设备丢失走 `GetDeviceRemovedReason()` 分支（`EngineController.cpp:845`）。

### 3.4 输入来源（引导帧契约）

- **`include/veyra/core/GuidanceFrame.h`**（E2）：NR、SR、FG 共用的统一引导契约，方向 **CurrentPrevious**。
  - 运动矢量：`R16G16_FLOAT`，**source-pixel units**（像素单位）。
  - 深度：`R32_FLOAT`（NR 使用 inverted 约定）。
  - 提供者：`Zero`（诚实回退：全零运动/深度）、`Nvof`（NVIDIA Optical Flow）、`Dav2`（Depth Anything V2 经 DirectML）、`NvofDav2`。
  - 单图层 NR 产品路径实际绑定 `zeroDepth` 恒成立；运动为 NVOF 输出或零。
- **Parity 编码**：`src/pipeline/EnhanceGraph.cpp:1736`（`encPass_` 加载 `ParityEncode.dxil`）；工作纹理 R16G16B16A16_FLOAT → proxy R8G8B8A8_UNORM（:1945-1946）。
- **Parity 编解码契约**：`include/veyra/parity/RenoDxParityCodec.h` + `src/parity/RenoDxParityCodec.cpp`（E2，"RenoDX-equivalent parity codec, CPU reference (Playbook section 9)"，纯 double 精度黄金参考）：
  - sRGB transfer（Playbook 9.2）；soft-clip 肩点常数 5.7780（**来自对真实 addon codec 的观察**，即逆向自实际使用 DLSSNR 的游戏）。
  - `encodeProxy`：线性 BT.709 RGB（Original）→ sRGB 编码后的 proxy 线性值 → RGBA8。
  - `parityDecode`：Original + Proxy + Neural → Final（线性 FP16）；Neural 输出是残差语义，非直接替换。
  - 矩阵（Playbook 9.3）：RGB↔LMS、LMS↔Lab（cbrt 变体）、BT.709↔AP1，全部在源码中给出。

### 3.5 实例、历史与栈叠

- **`include/veyra/pipeline/NrInstance.h`**（E2）：一个 NR 图层 = 独立纹理集（input/proxy/neural/finalRgba/residual/outputFull/motion）+ 独立 `NrTemporalPass` 历史 + 独立 feature handle 与参数块；`inputRevision` 变更即丢弃历史（Magpie 契约）；`handle_`/`parameters_` 由 graph 设置（`setHandle`/`setParameters`）。
- **栈叠**：最多 8 个并发 handle 在同一 snippet 会话上创建/求值/释放（`tools/nr_probe/main.cpp` 验证，SAOG0721/Magpie GPL-3.0 多 pass 结构）。
- **运行时选择**：`include/veyra/engine/EnhancementSettings.h:98` — `enum class NrRuntime { Original, Community, Ampere, NvidiaOriginal, LmxxfAmd }`；当前打包：Ampere（SF 310.9.16，RTX20-50）、NvidiaOriginal（310.8.0.0，RTX50）、LmxxfAmd（AMD）。**RTX 4090 仅适用 Ampere/SF 变体。**

### 3.6 实际运行代码 vs 实验分支

- 实际运行（产品路径）：单图层 NR（Feature 18）、parity 编解码、零深度、NVOF/零运动、SF 310.9.16 运行时、IAT 垫片、SEH 包裹。
- 实验/条件路径：多图层栈叠（`nrAutoPool`，有严格准入条件）、AMD Lmxxf 变体、Ampere 架构改写、NR 强度保护/输出稳定化（`NrHoldPass`，默认关闭且"缺失不得使 graph 失败"）。
- 占位/计划：无。所有 NR 路径都有真实实现与失败处理。

---

## 4. 官方 SDK 契约与 RTX 4090 兼容性证据

1. **本地 DLSS SDK 310.9.1 无神经渲染接口**（E2）：头文件家族为 `nvsdk_ngx.h`（核心）、`*_defs_dlssd.h`（DLSS **Denoise** + **RayReconstruction**，见 :17-54 的 `NVSDK_NGX_DLSS_Denoise_Mode`、`NVSDK_NGX_RayReconstruction_Hint_Render_Preset`）、`*_defs_dlssg.h`（帧生成）。`rg "Transfusion|DLSSNR|NeuralRendering"` 在 `include\` 下零命中。**DLSSNR 不是官方 SDK 功能。**
2. **NR 的唯一到达路径**是私有运行时 `nvngx_dlssnr.dll` 的保留 Feature ID 18（E2，第 3.1 节）。
3. **RTX 4090 兼容性**：
   - SF 310.9.16 构建自带 SM89 支持，不需要架构改写（`NrArchitecturePolicy.h`，E2）；`preferSfNr` 显式包含 "RTX 40"。
   - Veyra 的 manifest 记录该构建 "2026-10-17 local test authorization"（E1）。
   - 本地开发环境无 NVIDIA 驱动（Hyper-V），4090 上的任何执行均为 `UNVERIFIED`（E0，待 6-B 后在测试机验证）。
4. **ABI 稳定性**：私有运行时，无官方 ABI 承诺；`Shutdown1`、`Init_Ext` 等导出名随版本可能变化。必须按 fileVersion 310.9.16.0 + SHA256 锁定版本（manifest 已记录）。

---

## 5. 输入契约矩阵

| 输入或资源 | SDK（Feature 18）要求 | Veyra 来源 | Xenia 来源 | 缺口与验证方式 |
|---|---|---|---|---|
| 当前帧颜色 | parity 编码后的 `DLSSNR.Color`（R8G8B8A8_UNORM，sRGB 编码空间） | `ParityEncode.dxil` 从工作 FP16 纹理编码（E2） | SR 启用时为 SR 输出纹理，否则为 guest 输出 R10G10B10A2_UNORM（SR 已验证）→ **新增** parity 编码 compute pass | 净室重实现编解码数学（Veyra CPU 参考为黄金标准）；4090 上比对输出 |
| 深度信息 | `DLSSNR.Depth`（R32_FLOAT，`DepthInverted=1`），单图层路径恒为零深度 | `nrZeroDepth_` 共享常量纹理（E2） | Xbox 360 guest 深度不可作为 D3D12 纹理；FG 已有零深度占位符模式（`IFrameGenerationBackend::ZeroDepthPlaceholder`） | **无缺口**：复用零占位符契约；`UNVERIFIED`（真实 GPU） |
| 运动信息 | `DLSSNR.MVec`（R16G16_FLOAT，CurrentPrevious，**像素单位**，`MVecScale=1.0`） | NVOF 输出或零（`GuidanceFrame.h`，E2） | NVOF 后端已集成（Milestone 4）：R16G16_FLOAT、像素单位、current→previous（`guest_output_enhancement.h` FG 接口文档）——**契约直接匹配** | NVOF 关闭时走零占位符（诚实回退）；NVOF 执行本身 `UNVERIFIED`（4090） |
| 历史帧/历史状态 | SDK 内部历史，`DLSSNR.Reset=1` 重置 | `inputRevision` 契约 + `NrTemporalPass`（E2） | 重启用/场景切换时置 Reset；无需 Xenia 侧维护帧缓存 | 单测（host-side）覆盖 reset 时序；真实 GPU `UNVERIFIED` |
| 相机及场景参数 | Feature 18 **不要求**相机矩阵（仅 FG 需要） | —（E2：无相机参数） | 无需提供，不得伪造 | **无缺口** |
| 输出目标 | `DLSSNR.Output`（R8G8B8A8_UNORM，parity 编码空间，1:1 分辨率） | `neural_` 纹理，随后 parity decode 回线性 FP16（E2） | **新增**输出纹理 + decode/composite pass → 接入 Presenter 效果链（`effect_count >= 1`，首效果 kBilinear 回退不变） | 4090 上输出正确性验证（E4/E5 待做） |
| 同步与帧序 | 同一命令列表记录，无跨队列；创建需 submit+waitIdle | `ring_.acquireNext` 单队列环（E2） | Xenia 现有 D3D12 队列/Fence 模式（SR/FG 已验证的契约） | 状态迁移单测 + 真实 GPU `UNVERIFIED` |

**结论：所有输入在 Xenia 侧均有真实来源或诚实零占位符，无需伪造任何数据。** 这是"有条件可行"而非"不可行"的决定性依据。

---

## 6. Xenia 现有能力与缺口

**已有（E2，本仓库代码）：**
- 旁挂配置结构 `GuestOutputEnhancementConfig`（NR 子开关 `experimental_neural_rendering_enabled` 默认 false，`IsExperimentalRequested()` 已就位）；
- 九状态 `BackendResult` 独立枚举（决策 4，语义已定稿）；
- 每后端独立接口的模式（`IDLSSSuperSamplingBackend`、`IFrameGenerationBackend`）及"借用输入、自持输出、绝不接管 `guest_output_resource_paint_refs`"的所有权契约；
- 原始路径回退 `BuildOriginalPathFallbackFlow`（effect_count=1、kBilinear，决策 5）；
- NVOF 运动矢量来源（Milestone 4）；零深度/零运动占位符模式（FG 已实现）；
- 运行时部署目录与 manifest 制度（`runtime\experimental\`）。

**缺口（6-B 需新增）：**
1. NR 后端接口及实现（`src/xenia/ui/dlss/nr/` 新目录）；
2. 私有运行时加载器 + 调用方身份垫片 + SEH 包裹（4090 不需要 Ampere 改写，但加载器必须保留版本校验）；
3. Parity 编解码 compute pass（净室重实现，含 CPU 黄金参考单测）；
4. NR 输出接入 Presenter 效果链的路径选择逻辑；
5. 版本锁定与 manifest 条目（fileVersion 310.9.16.0 + SHA256）。

**明确不得改变**：SR/NVOF/FG 现有路径、默认配置行为、原始输出路径语义。

---

## 7. 外部依赖与许可风险

| 依赖 | 性质 | 许可状态 | 风险 |
|---|---|---|---|
| `nvngx_dlssnr.dll` 310.9.16.0（nr-ampere） | 社区 ShortFuse 构建，**未签名** | manifest："no licence granted"（PTX 同族）；用户 2026-10-17 本地测试授权 | **P0**：非官方运行时，无许可授予，无 ABI 承诺。决策 6 允许本地研究、禁止分发；§4e 开源兼容性 `LICENSE REVIEW REQUIRED` 维持不变 |
| `nvngx_dlssnr.dll` 310.8.0.0（nr-original） | NVIDIA 签名原版，RTX 50 专用 | "Preserved local NVIDIA-signed original" | 4090 不可用；仅作为契约旁证 |
| Parity 编解码数学 | Veyra GPL-3.0 代码，数学公式不受版权约束 | GPL-3.0（代码） | **P1**：Xenia（BSD）不得复制代码，必须净室重实现；以 Veyra CPU 参考为黄金标准做数值比对 |
| Playbook（契约文档） | 外部知识库，不在 Veyra 树内 | — | **P1**：关键章节（8.3/8.5/8.6/9.x）已由代码注释与 CPU 参考实现覆盖，残余缺口在子矩形语义与边缘行为，需 6-B 中以主机探针工具实机核对 |
| IAT 垫片（GetModuleFileNameW） | 私有兼容性机制，作用域限于 snippet 导入表 | — | **P0 决策点**：必须在 Xenia 中如实实现并披露；禁止升级为全局 hook |
| SEH 包裹 | 私有运行时崩溃隔离 | — | 必须保留，否则一次 snippet 崩溃即杀死模拟器进程 |
| Magpie（SAOG0721）多 pass 结构参考 | GPL-3.0，仅结构参考 | GPL-3.0 | 仅借鉴结构，不复制代码 |

**分发红线（决策 6 已批准，维持）**：不向公开 Xenia 仓库提交受影响的集成代码、不分发 NVIDIA/社区 DLL。6-B 全部工作限定本地研究。

---

## 8. 阻断项与风险分级

**P0（6-B 准入必须处理）：**
- P0-1：许可与运行时身份决策——4090 仅能使用未签名社区 SF 310.9.16 运行时 + IAT 垫片；需用户明确批准在 Xenia 本地研究中使用该组合（分发仍禁止）。
- P0-2：Parity 编解码契约确认——以 Veyra CPU 参考（`RenoDxParityCodec`）为黄金标准，Xenia 净室重实现并通过数值比对；未通过前不得接通真实求值。
- P0-3：真实 GPU 验证——所有执行/画质/显存项在 4090 上 `UNVERIFIED`；6-B 验收必须包含测试机验证计划。

**P1（6-B 实施中解决，不阻断准入）：**
- P1-1：运行时 ABI 稳定性（私有、未签名）——版本锁定 + 导出缺失即 `kUnavailable` 的确定性失败。
- P1-2：IAT 垫片在 Xenia 进程上下文的重放验证（单 DLL 单会话所有权）。
- P1-3：显存开销（276MB DLL 映射 + 运行时内部模型驻留，具体数值未知）——6-B 需在 4090 上实测并记录。
- P1-4：Playbook 残余缺口（子矩形、边缘参数）——以主机探针工具在 4090 上核对。

**无隐藏阻断**：不存在"当前条件下无法合理解决的硬性限制"——输入契约全部可满足，回退路径有既有模式，失败语义有先例。

---

## 9. 最小集成方案概述

1:1 神经增强路径，插入在既有增强链之后、呈现之前（详细设计见实现计划）：

```
guest 输出 (R10G10B10A2)
  → [SR 可选] → 工作纹理
  → parity encode (compute, R8G8B8A8 proxy)
  → DLSSNR Evaluate (Feature 18, Color=proxy, MVec=NVOF/零, Depth=零, Output=neural)
  → parity decode/composite (compute, 回线性)
  → 输出纹理 (1:1) → Presenter 效果链 (kBilinear 呈现)
  任何失败 → 回退既有输出路径 (effect_count >= 1, 首效果 kBilinear)
```

- 独立模块 `src/xenia/ui/dlss/nr/`，独立接口、独立配置（默认关）、独立 `BackendResult` 语义（复用九状态枚举）。
- 借用输入（guest 输出 / NVOF 运动矢量 / 零深度占位符），自持输出纹理与运行时句柄，绝不接管 `guest_output_resource_paint_refs`。
- 创建期 submit+waitIdle；求值期单命令列表；失败即放弃本帧 NR 输出并使用既有有效路径（与 Veyra 的诚实失败语义一致）。

---

## 10. 测试环境限制及未验证事项

- 开发环境（Hyper-V）无 NVIDIA 驱动：**真实 GPU 初始化、Feature 18 创建/求值、画质、显存、延迟全部 `UNVERIFIED`**。
- 本地可执行（6-B 内完成）：host-side 单测（配置默认关闭、路径选择、回退、parity 数学数值比对、状态机）、编译期集成、静态分析。
- 测试机（RTX 4090）待执行：运行时加载与版本核对、单帧求值、连续帧历史、reset 时序、NVOF+NR 联调、回退路径、显存/延迟测量、与 SR/FG 组合回归。
- 未验证事项清单（全部显式标注）：Feature 18 在 4090 上的可执行性（E0）、parity 数学与 Veyra 黄金参考的数值一致性（E3 待做）、运行时崩溃隔离有效性（E3 待做）、输出画质（E5 待做）。

---

## 11. 明确的 6-B 准入结论

**结论：有条件可行，准入条件明确，可由用户批准后启动 6-B。**

准入条件（全部满足即可启动）：
1. 用户批准 P0-1（4090 上使用未签名社区 SF 310.9.16 运行时 + IAT 垫片，本地研究限定）。
2. 用户批准实现计划的范围（第 9 节最小方案，不含多图层栈叠、AMD 变体、强度保护）。
3. 实现计划中"必须由用户批准的决策点"逐项确认（见 `milestone-6-a-NR_IMPLEMENTATION_PLAN.md` 第 9 节）。

若用户拒绝 P0-1，则本里程碑结论降级为**不可行（当前许可条件下）**，6-B 不启动，NR 子开关维持默认关闭且无后端代码。
