# Milestone 0.1 — 开发基线修正与关键审计结论复核

> 本文档是 **Milestone 0.1 — 修正开发基线与复核关键审计结论** 的审计记录，承接 `milestone-0-audit.md`（Milestone 0 前置审计）。
>
> **本阶段性质**：受限文档修正 + 只读源码复核。允许的唯一改动是修正主工作仓库内 `AGENTS.md` 的过时路径映射；未修改任何 C++ 源码、头文件、测试或构建脚本；未执行构建；未安装依赖；未加载任何 NVIDIA DLL；未执行真实 GPU / NVOF / DLSS 渲染。
>
> **本阶段结论：`PASS WITH FINDINGS`**。已复核并修正的关键事实均有源码或 SDK 文档证据支撑；未执行的编译验证与 GPU 验证一律标记为待验证，不得视为已通过。

| 项目 | 值 |
| --- | --- |
| 总体状态 | `PASS WITH FINDINGS` |
| 里程碑 | `Milestone 0.1 — Baseline Fix & Re-verification` |
| 审计日期 | 2026-10-20 |
| 审计方式 | 受限文档修正 + 只读源码 / SDK 复核 |
| 仓库分支 | `dev/dlss5-integration` |
| HEAD 提交 | `c28af1125d7a50c1cdfc4ba0c7e6c1df5b00f295` |
| 本阶段唯一允许改动 | `AGENTS.md` 路径映射修正 |
| 前置报告 | [`milestone-0-audit.md`](milestone-0-audit.md) |

---

## 1. 审计摘要与总体状态

**总体状态：`PASS WITH FINDINGS`**

Milestone 0.1 完成三件事：(1) 修正 `AGENTS.md` 的过时路径映射；(2) 以 310.9.1 头文件 + 310.6.0 官方文档为权威源，复核 DLSS SR 输入契约；(3) 只读复核 Presenter 结果语义与 `effect_count = 0` 回退行为，并修正 Milestone 0 报告中的一处记录错误。

**关键发现（Findings）**：

1. **`AGENTS.md` 路径已修正**：`\\EIN-9900k\DLSS5_Research\...` → `\\ein-9900k\Code\DLSS5_Research\...`，四个目录均已核实存在。
2. **DLSS SR 输入契约复核**：运动矢量（MV）是**条件必需**而非所有模式必需；静态场景可用零矢量 + `InReset` 回退运行（质量退化）。不得把"缺少 MV"等同于"所有 SR 模式绝对不可用"，也不得反向断言某模式必然产出可接受输出。
3. **`NVSDK_NGX_PerfQuality_Value` 无 `Off` 值**（310.9.1 头文件核实）：仅有 `MaxPerf` / `Balanced` / `MaxQuality` / `UltraPerformance` / `UltraQuality` / `DLAA`。功能关闭应通过配置开关实现，而非依赖枚举 `Off`。
4. **`effect_count = 0` 记录错误修正**：Milestone 0 报告称"`effect_count = 0` 时 guest 输出直接显示"，实际是 **guest 输出不绘制、后缓冲清黑 + 绘制 UI**。原始路径是 `effect_count ≥ 1` 且最终效果为 `kBilinear`。DLSS 后端若返回 `effect_count = 0` 作为"回退"，会丢失 guest 输出，必须改为构造 `effect_count ≥ 1` 的原始效果链。
5. **DLSS 输入状态要求**：D3D12 输入须 `NON_PIXEL_SHADER_RESOURCE`，输出须 UAV + `ALLOW_UNORDERED_ACCESS`。现有 guest 输出为 `PIXEL_SHADER_RESOURCE` 且已具备 `ALLOW_UNORDERED_ACCESS`，插入前需一次资源状态迁移。
6. **许可证**：本地研发允许（§1a）；将自有实现提交到 MIT Xenia 仓库、随二进制分发 SDK DLL 等，均保留 `LICENSE REVIEW REQUIRED`，不作最终法律裁定。

**仍待验证（不得视为已通过）**：编译验证（未构建）、Mock 主机侧测试（未做）、真实 GPU / NVOF / DLSS 执行（Hyper-V 无驱动）、许可证分发边界（需法律 / 合规确认）。

---

## 2. 审计环境、仓库分支和提交基线

**仓库位置（已确认）**：
- 主工作仓库：`\\ein-9900k\Code\DLSS5_Research\xenia-canary`
- 研究根目录：`\\ein-9900k\Code\DLSS5_Research\`，含 `xenia-canary`、`nvidia-dlss-reference`、`runtime`、`Veyra-NRVideo`、`AGENTS.md`。

**Git 基线（已确认）**：
- 分支：`dev/dlss5-integration`
- HEAD：`c28af1125d7a50c1cdfc4ba0c7e6c1df5b00f295`
- 审计开始前 `git status --short --branch` 显示 `## dev/dlss5-integration` + `?? docs/dlss-integration/`；`git diff --stat` 为空（无已跟踪文件修改）。
- 审计前工作区干净；本阶段唯一改动为 `AGENTS.md` 路径修正（见第 3 节）。

**开发环境限制**：运行于 Hyper-V，无法安装 NVIDIA 驱动，无法执行真实 GPU / NVOF / DLSS 渲染。本地可执行：源码检查、SDK 分析、架构设计、编译期集成、输入验证、基于 Mock 的主机侧测试、静态分析。本地不可执行：真实 GPU 初始化、NVOF 执行、DLSS 质量评估。所有硬件相关项在远程验证前必须标记为 `UNVERIFIED`。

---

## 3. AGENTS.md 路径修正（本阶段唯一改动）

**过时路径 → 实际路径映射（已核实存在）**：

| AGENTS.md 原映射（过时） | 实际确认路径 | 核实 |
| --- | --- | --- |
| `\\EIN-9900k\DLSS5_Research\xenia-canary` | `\\ein-9900k\Code\DLSS5_Research\xenia-canary` | 存在 |
| `\\EIN-9900k\DLSS5_Research\nvidia-dlss-reference` | `\\ein-9900k\Code\DLSS5_Research\nvidia-dlss-reference` | 存在 |
| `\\EIN-9900k\DLSS5_Research\runtime` | `\\ein-9900k\Code\DLSS5_Research\runtime` | 存在 |
| `\\EIN-9900k\DLSS5_Research\Veyra-NRVideo` | `\\ein-9900k\Code\DLSS5_Research\Veyra-NRVideo` | 存在 |

**修正范围**：仅 `AGENTS.md` 中对应的过时 UNC 路径前缀 `\\EIN-9900k\DLSS5_Research\` → `\\ein-9900k\Code\DLSS5_Research\`。保留所有安全规则、功能边界、报告格式及其他无关内容。未修改 Veyra-NRVideo 中任何文件。

**未确认项**：无（四个目录均已核实存在）。

---

## 4. DLSS SR 输入契约复核（310.9.1 头文件 + 310.6.0 官方文档）

**权威源**：
- 头文件（权威，310.9.1）：`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx_helpers_d3d.h`（D3D12 结构 `NVSDK_NGX_D3D12_DLSS_Eval_Params` L225–261）、`nvsdk_ngx_defs.h`（`NVSDK_NGX_PerfQuality_Value` L251–260）。
- 官方文档（310.6.0，滞后于 310.9.1，头文件与文档冲突处以头文件为准）：`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\doc\DLSS_Programming_Guide_Release.pdf`（页脚 31 March 2026）。

**输入契约（区分 API 必需 / 条件必需 / 可选）**：

| 输入 | 契约 | 性质 |
| --- | --- | --- |
| 颜色（`pInColor`） | 任何 API 支持的缓冲格式；LDR 必须为感知线性（类 sRGB），值域 0.0–1.0，`IsHDR=0`；若为线性空间则 `IsHDR=1`（HDR 模式） | API 必需 |
| 输出（`pInOutput`） | 任何 API 支持的缓冲格式 | API 必需 |
| 抖动（jitter） | 渲染像素空间 [-0.5, +0.5]，与 MV 同一坐标系；Total Phases = 基相位 × (Target/Render)²，各模式有最小相位数 | API 必需（逐帧更新） |
| 运动矢量（`pInMotionVectors`） | RG32_FLOAT 或 RG16_FLOAT；当前帧→前一帧；[0,0] 左上角；`InMVScaleX/Y=1.0` 无需缩放，绝不为 0.0f | **条件必需**（场景运动时；静态场景可用零矢量 + `InReset` 回退，质量退化） |
| 深度（`pInDepth`） | 任何单通道或深度-模板格式（R32_FLOAT / D32_FLOAT / D24S8） | 可选 |
| 曝光（`pInExposureTexture`） | 仅 Preset J/K；Preset L 自动曝光；1×1 纹理，首通道 R16F | 条件可选 |
| `InReset` | 场景完全变化（切换关卡等）时置 1 | 条件 |
| D3D12 状态 | 输入 `NON_PIXEL_SHADER_RESOURCE`；输出 UAV + `ALLOW_UNORDERED_ACCESS`；evaluate 后缓冲回到已知状态 | API 必需 |

**`NVSDK_NGX_PerfQuality_Value`（310.9.1，`nvsdk_ngx_defs.h:251-260`）取值**：`MaxPerf`、`Balanced`、`MaxQuality`、`UltraPerformance`、`UltraQuality`、`DLAA`。**无 `Off` 值**。关闭 DLSS 须通过配置开关，不依赖枚举。

**结论**：
- **API 调用所需条件**（颜色 / 输出 / 抖动 + 状态）Xenia 当前可满足或可主机侧生成。
- **高质量画面所需条件**（真实 MV、深度、HDR / 曝光）Xenia 当前**缺失**：`src/xenia/gpu` 运动矢量零命中，深度无公开访问器。
- **最小上游依赖**：真实 MV 需 guest 渲染 / 着色器翻译层注入，或由 NVOF 从相邻帧计算（NVOF 输出可作为 SR 的 MV 来源）；深度需上游资源访问器。
- **不得**把"缺少运动矢量"直接等同于"所有 SR 模式绝对无法运行"，也不得反向推断某模式必然能产生可接受输出。静态场景 + 零矢量回退的运行时可行性仍属 `GPU_UNVERIFIED`。

**头文件引用修正（记录，不改写旧报告）**：Milestone 0 报告第 4.1 节将 `NVSDK_NGX_D3D12_DLSS_Eval_Params` 标注为 `nvsdk_ngx_helpers_d3d.h:37-69`，该区间实为 **D3D11** 结构体（`pInDepth` L37、`pInMotionVectors` L38、jitter L39-40、`InReset` L43、`InPreExposure` L55、`pInMotionVectors3D` L62、`pInDepthHighRes` L65、`pInMotionVectorsReflections` L69）。**D3D12** 结构体实际位于 **L225–261**（`pInDepth` L228、`pInMotionVectors` L229、jitter L230-231、`InReset` L234、`InMVScaleX/Y` L235-236、`pInExposureTexture` L238、`InPreExposure` L246、`pInMotionVectors3D` L253、`pInDepthHighRes` L256、`pInMotionVectorsReflections` L260）。两结构体字段并行且名称一致，字段结论不变，仅行号引用修正。

---

## 5. Presenter 结果语义复核

只读检查现有 `PaintResult`、`GuestOutputPaintFlow` 及调用者，回答五项问题：

**1. `PaintResult` 是否只表达呈现结果？**
是。`PaintResult`（`src/xenia/ui/presenter.h:340`）取值：`kPresented`、`kPresentedSuboptimal`、`kNotPresented`、`kNotPresentedConnectionOutdated`、`kGpuLostExternally`、`kGpuLostResponsible`。它只表达"是否成功呈现到交换链"，**不表达后端（DLSS / NVOF）处理结果**。

**2. 后端处理结果是否需要与呈现结果分离？**
需要。`PaintResult` 是呈现层语义，DLSS / NVOF 后端处理结果（成功 / 失败分类）是处理层语义，二者正交。需引入独立的**后端结果类型**（如 `kUseBackend` / `kOriginalPath` / 失败分类），不得复用 `PaintResult`。

**3. 原始路径回退应由哪个调用层负责？**
由**后端管理器 / paint-flow 构造层**负责。`GetGuestOutputPaintFlow`（`src/xenia/ui/presenter.cc:651`）构建效果链；若后端不可用 / 失败，该层应构造 `effect_count ≥ 1` 且最终效果为 `kBilinear` 的**原始效果链**（而非 `effect_count = 0`）。

**4. `effect_count = 0` 的回退行为是否确实保留原始输出？**
**否（修正 Milestone 0 报告第 3 节 L94 的记录错误）**。`effect_count == 0` 时，guest 输出**不绘制**，后缓冲清为 `kBackBufferClearColor = {0,0,0,1}`（黑）+ 绘制 UI（`src/xenia/ui/d3d12/d3d12_presenter.cc:497`、`:530`、`:535`、`:671`、`:929`、`:1037`）。原始路径是 `effect_count ≥ 1` 且最终效果 `kBilinear`（`src/xenia/ui/presenter.cc:927`、`:930`）。因此 DLSS 后端**不能**以 `effect_count = 0` 作为"安全回退"——会丢失 guest 输出；必须构造 `effect_count ≥ 1` 的原始效果链。

**5. 新增配置应放在 Presenter、后端管理器还是独立功能配置结构？**
建议放在**独立的功能配置结构**，由**后端管理器**持有并消费，Presenter 仅在构造 `GuestOutputPaintFlow` 时读取其开关。理由：
- `GuestOutputPaintConfig`（`presenter.h:196`）是 UI 预设（effect 类型、CAS 锐度、FSR 通道 / 锐度、dither、overscan），是**用户可见的显示偏好**，不宜混入 SDK 后端开关。
- 后端开关（SR / NVOF / 帧生成 / 神经渲染 + 诊断状态）是**运行时能力门控**，与 UI 预设正交。
- 独立结构使"主开关关闭 → 所有子功能关闭 → 不执行新增处理"的语义清晰，且不与 UI 预设耦合。

---

## 6. 许可证边界复核

依据本机 `C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\LICENSE.txt`（v. 2024-03-14 + RTX Supplement）。只检查已有许可证文件，不作最终法律裁定。

| 活动 | 相关条款 | 结论 |
| --- | --- | --- |
| 阅读 SDK 文档 | §1a（L10，安装 / 使用） | `SUPPORTED BY LICENSE TEXT` |
| 使用头文件进行本地开发 | §1a（L10） | `SUPPORTED BY LICENSE TEXT` |
| 链接 SDK 库 / 调用 SDK（本地构建） | §1a / §1c（L10-12），受 §2a（L15）约束 | `SUPPORTED BY LICENSE TEXT`（条件） |
| 将自有实现提交到公开 MIT Xenia 仓库 | §4e（L27，不得使 SDK 受开源许可约束）+ §2c（L17，SDK 须以至少同等保护条款分发）+ §2b（L16，派生源须含 NVIDIA 声明） | `LICENSE REVIEW REQUIRED` |
| 将 SDK DLL 随 Xenia 二进制分发 | §1c + §2a-2d（L12, 15-18）+ §4b（L24，不得独立分发 SDK）；§4e/§2c 与 MIT 冲突 | `LICENSE REVIEW REQUIRED` |
| 不分发 DLL，仅分发自有实现 | §1c 不触发；§1a"使用"仍适用；§4e 风险较低但需确认 | `LICENSE REVIEW REQUIRED` |
| 公开 / 商业发布前通知 | Supplement §4（L77，`developer.nvidia.com/sw-notification`） | `SUPPORTED BY LICENSE TEXT`（义务存在） |

**待解决事项（进入后续开发前必须明确）**：
- §4e（开源兼容性）：嵌入 SDK 目标代码到开源 Xenia 是否构成违禁——`LICENSE REVIEW REQUIRED`。
- 公开 / 商业发布前 NVIDIA 通知——`UNVERIFIED`（操作未做）。
- 本阶段不作最终法律裁定，不宣称"所有 DLSS 实现代码必然不能开源"，也不宣称"不附带 DLL 就一定可公开分发"。

---

## 7. 验证证据及其等级

**本阶段证据等级**：
- `STATIC_VERIFIED`：全部第 3–6 节结论（AGENTS.md 路径、DLSS SR 输入契约、PerfQuality 枚举、Presenter 结果语义、effect_count 修正、许可证条款定位）。
- `COMPILE_VERIFIED`：**未达到**（本阶段未构建）。
- `MOCK_VERIFIED`：**未达到**（本阶段未做 Mock 主机侧测试）。
- `GPU_UNVERIFIED`：所有真实 GPU 行为（DLSS init / evaluate、NVOF 执行、图像输出）。
- `RUNTIME_UNVERIFIED`：真实目标环境上的运行时行为。

**关键源码证据（可追溯）**：

| 结论 | 证据位置 |
| --- | --- |
| `PaintResult` | `src/xenia/ui/presenter.h:340` |
| `GuestOutputPaintConfig` | `src/xenia/ui/presenter.h:196` |
| `GetGuestOutputPaintFlow` | `src/xenia/ui/presenter.cc:651` |
| `PaintAndPresentImpl` | `src/xenia/ui/d3d12/d3d12_presenter.cc:463` |
| `kBackBufferClearColor` | `src/xenia/ui/d3d12/d3d12_presenter.cc:497` |
| `effect_count = 0` 回退 | `src/xenia/ui/d3d12/d3d12_presenter.cc:530/535/671` |
| 后缓冲清黑 | `src/xenia/ui/d3d12/d3d12_presenter.cc:929/1037` |
| 原始路径最终效果 `kBilinear` | `src/xenia/ui/presenter.cc:927/930` |
| guest 输出格式 | `src/xenia/ui/d3d12/d3d12_presenter.h:58-59`（`R10G10B10A2_UNORM`） |
| guest 输出内部状态 | `src/xenia/ui/d3d12/d3d12_presenter.h:60-61`（`PIXEL_SHADER_RESOURCE`） |
| guest 输出允许 UAV | `src/xenia/ui/d3d12/d3d12_presenter.cc:422` |
| D3D12 DLSS Eval 结构 | `nvsdk_ngx_helpers_d3d.h:225-261` |
| `PerfQuality` 枚举 | `nvsdk_ngx_defs.h:251-260` |

**证据区分**：
- **已核实事实**：上表所有源码 / 头文件 / SDK 版本 / DLL 版本 / 许可证条款。
- **合理推断**：后端结果类型与 UI 预设分离、回退由后端管理器负责。
- **UNKNOWN**：深度缓冲资源状态与分辨率；曝光可否由寄存器推导。
- **待复核**：许可证分发边界（第 6 节）。

---

## 8. 环境限制与未验证事项

**Hyper-V 环境无法验证**：真实 GPU 初始化、DLSS SR `Evaluate` 执行、NVOF 光流执行、DLSS 输出画质与帧生成性能。

**未验证事项清单**：
- 所有真实 GPU 行为：`GPU_UNVERIFIED`。
- 真实目标环境运行时行为：`RUNTIME_UNVERIFIED`。
- 编译通过：未验证（本阶段未构建），**不得**标记为 `COMPILE_VERIFIED`。
- Mock 主机侧逻辑：未验证（本阶段未做 Mock 测试）。
- 公开发布前 NVIDIA 通知：`UNVERIFIED`（操作未做）。

---

## 9. 剩余风险

| 风险 | 说明 | 等级 |
| --- | --- | --- |
| SDK API / ABI 兼容 | 文档 310.6.0 vs 头文件 310.9.1，以头文件为准，需核对 ABI 稳定性 | 中 |
| 运行时 DLL 版本 / 加载 | SR/RR/FG DLL = 310.9.1.0、NR = 310.8.3.0、lite FG = 310.9.16.0，与 SDK 头文件一致但未加载验证 | 中 |
| D3D12 资源状态 / 格式 | guest 输出 `PIXEL_SHADER_RESOURCE` 与 DLSS 输入要求 `NON_PIXEL_SHADER_RESOURCE` 不一致，需一次状态迁移 | 中 |
| GPU 队列 / Fence / 跨阶段同步 | refresher 与 paint 时间线依赖，不得破坏现有 Fence | 高 |
| 游戏 MV / 深度可用性 | MV 缺失（条件必需，硬阻塞高质量 SR）；深度需访问器 | 高 |
| 帧生成呈现机制 | 交错生成 / 渲染帧、Reflex+HAGS 在模拟器中可行性未定 | 高 |
| 许可证 / 分发 | 嵌入 SDK 目标代码入开源 Xenia 并重分发触发 `LICENSE REVIEW REQUIRED` | 高 |
| 失败回退 / 原始路径兼容 | 新路径关闭 / 失败必须无缝回到原始效果链，`effect_count = 0` 不可用作回退 | 高 |

---

## 10. 各项能力状态清单

> 每项只填写**当前证据支持的最高状态**。无法确认处注明 `UNKNOWN`，不猜测。本阶段**未**升级任何状态。

| 能力 | 当前状态 | 说明 |
| --- | --- | --- |
| DLSS SR | **未实现** | 输入契约已复核，无集成代码 |
| NVOF | **未实现** | API 契约已核实，`nvofapi64.dll` 缺失 |
| 帧生成 | **未实现** | 运动矢量缺失（硬阻塞），帧生成呈现机制未定 |
| 实验性神经渲染 | **未实现** | RR 需 RT G-Buffer，对 Xenia 非 RT 架构不适用 |
| Presenter 集成 | **尚未开始** | 结果语义已复核，插入点已识别，无代码 |
| 最终画质与性能 | **尚未评估** | 需目标硬件验证 |

**状态定义**：未实现 / 尚未开始 / 尚未评估：本阶段仅完成文档修正与静态复核。编译期已集成 / 运行时已验证 / 执行已验证 / 输出已验证 / 回退已验证 / 后端输出已验证 / 本地评估 / 目标硬件验证：均需后续阶段以新证据达成，本阶段**不得**预先标记。

---

## 附录：修改范围与 Git 验证

- **本阶段允许**：修正 `AGENTS.md` 路径映射。
- **本阶段禁止**：修改 C++ 源码 / 头文件 / 测试 / 构建脚本、修改 SDK / 运行时 / Veyra-NRVideo、执行构建、安装依赖、加载 NVIDIA DLL。
- **Git 验证**：本阶段仅修改 `AGENTS.md`（路径前缀修正）。`git status --short --branch` 应仅显示 `## dev/dlss5-integration`、`?? docs/dlss-integration/`（未跟踪的审计报告目录）与 ` M AGENTS.md`（路径修正），无其他差异。
- **Git 验证补充更正**：经 Milestone 0.2 复核核验，`AGENTS.md` 实际位于研究根目录 `\\ein-9900k\Code\DLSS5_Research\AGENTS.md`，**不在** `xenia-canary` 仓库内（`git -C xenia-canary ls-files AGENTS.md` 无匹配，`--error-unmatch` 报错）。因此 `xenia-canary` 仓库的 `git status --short --branch` 实际仅显示 `## dev/dlss5-integration` 与 `?? docs/dlss-integration/`，**不显示** ` M AGENTS.md`。上行所列 ` M AGENTS.md` 系对仓库边界的误记，特此更正；`AGENTS.md` 的路径修正本身仍然有效，只是不属于该仓库的 Git 变更。
