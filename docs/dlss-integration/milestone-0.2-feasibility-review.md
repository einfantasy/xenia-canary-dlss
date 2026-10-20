# Milestone 0.2 — 许可证与输入格式可行性审查

> 本文档是 **Milestone 0.2 — License & Input Format Feasibility Review** 的审查记录，承接 `milestone-0-audit.md`（Milestone 0）与 `milestone-0.1-baseline-review.md`（Milestone 0.1）。
>
> **本阶段性质**：只读可行性审查。仅研究、核对证据并形成文档；不实现任何功能，不修改 C++ 源码 / 头文件 / 项目配置 / 构建脚本，不构建项目，不加载 NVIDIA DLL，不进入 Milestone 1。
>
> **本阶段结论：`PASS WITH FINDINGS`**。许可证与 DLSS 输入格式均存在明确证据支撑的可行路径，同时保留若干必须解决的 `LICENSE REVIEW REQUIRED` 与 `GPU_UNVERIFIED` 事项。不得把 SDK 文件存在等同于运行时可用，不得把静态审计等同于编译通过或运行时验证。

| 项目 | 值 |
| --- | --- |
| 总体状态 | `PASS WITH FINDINGS` |
| 里程碑 | `Milestone 0.2 — License & Input Format Feasibility Review` |
| 审查日期 | 2026-10-20 |
| 审查方式 | 只读（SDK 许可证、头文件、官方文档、Xenia 源码、运行时文件） |
| 仓库分支 | `dev/dlss5-integration` |
| HEAD 提交 | `c28af1125d7a50c1cdfc4ba0c7e6c1df5b00f295` |
| 本阶段允许修改 | 仅 `docs/dlss-integration/` 下三个审计文档 |
| 前置报告 | [`milestone-0-audit.md`](milestone-0-audit.md)、[`milestone-0.1-baseline-review.md`](milestone-0.1-baseline-review.md) |

---

## A. 总结与状态

**总体状态：`PASS WITH FINDINGS`**

本阶段完成四项工作：
1. **许可证边界审查**：以本机 `DLSS-310.9.1/LICENSE.txt` 为首要证据，逐项给出条款定位与结论状态。本地研发允许；分发相关事项保留 `LICENSE REVIEW REQUIRED`。
2. **DLSS SR 输入契约核查**：以 310.9.1 头文件为权威、310.6.0 官方文档为辅，核实颜色 / 输出 / 抖动 / 运动矢量 / 深度 / 曝光的格式、尺寸、坐标系与必需性。
3. **Xenia 当前资源与 Presenter 流程分析**：确认 guest 输出真实格式（`R10G10B10A2_UNORM`）、内部状态（`PIXEL_SHADER_RESOURCE`）、UAV 能力与现有转换 / 生命周期流程。
4. **格式转换方案对比**：评估三个方案，给出技术可行性与推荐程度。

**主要结论**：
- 许可证：本地开发允许；将自有实现提交到 MIT Xenia 仓库、随二进制分发 SDK DLL 等保留 `LICENSE REVIEW REQUIRED`（§4e 与开源许可的张力，不作最终裁定）。
- DLSS 输入契约：颜色 / 输出 / 抖动 + D3D12 状态为 API 必需且 Xenia 可满足（需一次状态迁移）；MV / 深度为条件必需，Xenia 当前缺失，构成高质量 SR 的上游依赖。
- 格式兼容：guest 输出 `R10G10B10A2_UNORM` 属 SDK 支持的输入缓冲格式（§3.3 "any supported buffer format"），无需格式转换即可作为 DLSS 输入；颜色空间为 tone-mapping 后的感知线性 LDR，满足 `IsHDR=0` 要求。
- 推荐方案：**方案 A（直接使用现有 guest 输出纹理）+ 一次资源状态迁移**，作为最小可行路径。

---

## B. 审计范围及实际读取的资料

**仓库 / 文档**：
- `\\ein-9900k\Code\DLSS5_Research\xenia-canary\docs\dlss-integration\milestone-0-audit.md`（全文 382 行）
- `\\ein-9900k\Code\DLSS5_Research\xenia-canary\docs\dlss-integration\milestone-0.1-baseline-review.md`（本阶段新建，234 行）
- `\\ein-9900k\Code\DLSS5_Research\nvidia-dlss-reference\00-license-and-environment-audit.md`（许可 / 环境参考，125 行）
- `\\ein-9900k\Code\DLSS5_Research\nvidia-dlss-reference\01-dlss-sr-official-reference.md`（SR 参考，125 行）

**SDK 许可证**：
- `C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\LICENSE.txt`（107 行，v. 2024-03-14 + RTX Supplement）

**SDK 头文件（权威，310.9.1）**：
- `C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx_helpers_d3d.h`（D3D12 `NVSDK_NGX_D3D12_DLSS_Eval_Params` L225–261；D3D11 结构 L37–69）
- `C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\include\nvsdk_ngx_defs.h`（`NVSDK_NGX_PerfQuality_Value` L251–260）

**官方文档（310.6.0，页脚 31 March 2026）**：
- `C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\doc\DLSS_Programming_Guide_Release.pdf`（提取至 `C:\Users\einfa\AppData\Local\Temp\dlss_guide.txt`，182,349 字符）
  - §3.3 输入缓冲格式（L711-717）
  - §3.4 D3D12 资源状态（L733-745）
  - §3.6.1 运动矢量格式 / 计算（L793、L861）
  - §3.6.3 运动矢量缩放（L890-891）
  - §3.7.1.1 相位数（L949-956）
  - §3.7.3 抖动范围（L990）
  - LDR / IsHDR 颜色空间（L561-577）
  - 负 LOD bias（L750-753）

**Xenia 源码**：
- `src/xenia/ui/presenter.h`（`PaintResult` :340、`GuestOutputPaintConfig` :196、`kGuestOutputMailboxSize=3` :360、`GuestOutputPaintFlow` :439）
- `src/xenia/ui/presenter.cc`（`GetGuestOutputPaintFlow` :651、`PaintAndPresent` :1274、`ConsumeGuestOutput` :596、`kBilinear` :927/:930、`assert_not_zero(flow.effect_count)` :930）
- `src/xenia/ui/d3d12/d3d12_presenter.h`（`kGuestOutputFormat` :58-59、`kGuestOutputInternalState` :60-61、`kGuestOutputIntermediateFormat` :63-64、`kSwapChainFormat` :67）
- `src/xenia/ui/d3d12/d3d12_presenter.cc`（`RefreshGuestOutputImpl` :386、guest 输出创建 :407-431、`PaintAndPresentImpl` :463、`kBackBufferClearColor` :497、`effect_count=0` 回退 :530/:535/:671、SRV :602、后缓冲清黑 :929/:1037）
- `src/xenia/gpu/d3d12/d3d12_command_processor.cc`（`IssueSwap` :2250、`RequestSwapTexture` :2270、`RefreshGuestOutput` :2279）

**运行时部署目录**：`\\ein-9900k\Code\DLSS5_Research\runtime`（见第 E 节）

**未读取 / 不适用**：NVOF SDK 头文件（本阶段聚焦 DLSS SR 输入格式，NVOF 契约沿用 Milestone 0 结论）；Veyra-NRVideo 源码（本阶段不分析参考实现，沿用 Milestone 0 第 7 节结论）。

---

## C. 许可证决策表及原文证据

**首要证据源**：`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1\LICENSE.txt`（v. 2024-03-14，L64；RTX Supplement，L67-107，L106）。所有条款编号与行号均已逐行核对。

**条款定位**：
- §1a（L10，安装 / 使用 SDK）、§1b（L11，修改样例源）、§1c（L12，将 SDK 以目标代码形式嵌入应用并分发，须满足 §2）
- §2a（L15，应用须有"实质性附加功能"）、§2b（L16，派生源须含 NVIDIA 声明）、§2c（L17，SDK 须以至少同等保护条款分发）、§2d（L18，通知 / 执行）
- §4a（L23，禁止逆向）、§4b（L24，禁止独立分发 SDK）、§4c（L25，禁止背书）、§4d（L26，禁止 DRM 规避）、§4e（L27，不得使 SDK 受开源许可约束）、§4f（L28，禁止关键 / 生命安全系统）、§4g（L29，赔偿）
- §8.2（L41，你对你自己的应用与样例源派生作品持有权利）
- Supplement §1（L71，互操作 + 仅限 NVIDIA GPU）、§2（L73，游戏内容许可）、§3（L75，云服务 / vGPU 范围）、§4（L77，商业发布前通知）、§6.2（L84，公开发布 DLSS 集成若有问题须处理）、§7（L87-99，营销 / 商标）

**决策表**：

| # | 活动 | 原文条款 | 适用条件与例外 | 对本项目影响 | 结论状态 | 证据定位 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 本地阅读 SDK 文档 + 使用头文件 | §1a（L10） | 非独占、不可转让；安装使用被许可 | 本地研发评估被允许 | `SUPPORTED BY LICENSE TEXT` | LICENSE.txt:10 |
| 2 | 编写使用 SDK API 的自有代码 | §1a（L10）+ §1b（L11，修改样例源） | 自有代码权利见 §8.2（L41） | 自有集成代码归属 Xenia | `SUPPORTED BY LICENSE TEXT` | LICENSE.txt:10,11,41 |
| 3 | 本地构建中链接 SDK 库 | §1a / §1c（L10-12），受 §2a（L15）约束 | 应用须有"实质性附加功能"（Xenia 为完整模拟器，满足） | 本地链接允许 | `SUPPORTED BY LICENSE TEXT`（条件） | LICENSE.txt:10,12,15 |
| 4 | 将自有实现提交到公开 MIT Xenia 仓库 | §4e（L27，不得使 SDK 受开源许可约束）+ §2c（L17）+ §2b（L16） | §8.2（L41）你持有自有应用权利；但 §4e 禁止 SDK 受开源许可约束 | 自有 MIT 代码本身未被 §4e 明文禁止，但嵌入 SDK 目标代码后 SDK 可能受 MIT 约束 | `LICENSE REVIEW REQUIRED` | LICENSE.txt:27,17,16,41 |
| 5 | 发布含 SDK DLL 的 Xenia 二进制包 | §1c（L12）+ §2a-2d（L15-18）+ §4b（L24）+ Supp §4（L77，通知） | §4e / §2c 与 MIT 冲突；发布前须通知 NVIDIA | 分发 SDK 目标代码 + 通知义务 | `LICENSE REVIEW REQUIRED` | LICENSE.txt:12,15-18,24,77 |
| 6 | 不附带 DLL，仅分发自有实现（用户自装 SDK） | §1c 不触发；§1a"使用"仍适用；§4e 风险较低 | 用户需自备 SDK；§4e 风险取决于自有代码是否"基于"SDK | 分发策略需确认自有代码与 SDK 的耦合度 | `LICENSE REVIEW REQUIRED` | LICENSE.txt:12,10,27 |
| 7 | 公开 / 商业发布前通知 | Supp §4（L77） | 须向 NVIDIA 提交：公司名、SDK、应用名、平台、发布日期、链接 | 公开 / 商业发布前必须通知 | `SUPPORTED BY LICENSE TEXT`（义务存在） | LICENSE.txt:77 |

**严格区分（不得混淆）**：
- **本地开发 vs 公开分发**：本地开发（活动 1-3）被明文允许；公开分发（活动 4-6）受 §4e / §2c / §4b 约束，保留 `LICENSE REVIEW REQUIRED`。
- **使用 SDK vs 复制 / 修改 / 重分发 SDK 内容**：使用（§1a）被允许；复制 / 重分发（§4b）被禁止；修改样例源（§1b）被允许但派生须含声明（§2b）。
- **提交自有集成代码 vs 分发 SDK 二进制**：自有代码归属（§8.2）被允许；SDK 二进制分发须满足 §1c + §2 + §4b。
- **明文允许 vs 需进一步确认**：活动 1-3、7 明文允许 / 义务明确；活动 4-6 存在 §4e 张力，需 NVIDIA 或法律专业人士确认。

**进入后续开发前必须解决的事项**：
- §4e 开源兼容性裁定（活动 4-6）——`LICENSE REVIEW REQUIRED`。
- 公开 / 商业发布前 NVIDIA 通知（活动 7）——`UNVERIFIED`（操作未做）。
- **不**宣称所有 DLSS 实现代码必然不能开源；**不**宣称不附带 DLL 就一定可公开分发。

---

## D. DLSS 输入契约核查结果（C1）

**权威源**：头文件（310.9.1，权威）`nvsdk_ngx_helpers_d3d.h:225-261`（`NVSDK_NGX_D3D12_DLSS_Eval_Params`）、`nvsdk_ngx_defs.h:251-260`（`NVSDK_NGX_PerfQuality_Value`）；官方文档（310.6.0，辅助）`dlss_guide.txt`。头文件与文档版本不同时，**以头文件为权威**，文档描述需核对后引用，不得将 310.6.0 描述未经核对套用到 310.9.1。

### D1. 颜色输入契约

- **格式**：任何 API 支持的缓冲格式（`dlss_guide.txt` L713："Color input buffer (the main frame): any supported buffer format for the API."）。`R10G10B10A2_UNORM` 属 API 支持格式，**格式层面可接受**。
- **颜色空间 / 值域**：LDR 必须为感知线性（类 sRGB），值域 0.0–1.0，`IsHDR=0`（L561-572）。若为线性空间则 `IsHDR=1`（HDR 模式，L573-574）。Xenia guest 输出经 tone-mapping 后为感知线性 LDR，满足 `IsHDR=0`。
- **不得**仅凭格式名称断言可直接使用——需确认颜色空间（LDR 非线性）与值域，已从源码 / 文档核实。

### D2. 输出纹理契约

- **格式**：任何 API 支持的缓冲格式（L717）。
- **D3D12 状态**：输出须 UAV 状态 + `ALLOW_UNORDERED_ACCESS`（L740-742）。evaluate 后缓冲回到已知状态（L744-745）。

### D3. 抖动（jitter）契约

- **范围**：[-0.5, +0.5] 像素空间，渲染尺寸下，与 MV 同一坐标系（L990）。(0,0) 表示无抖动。
- **相位数**：Total Phases = 基相位 × (Target/Render)²（L949-956）。各模式有最小相位数要求（Ultra-Perf 72、Perf 32、Balanced 24、Quality 18、DLAA 8）。
- **逐帧更新**：jitter 每帧变化，Xenia 当前无 jitter 信号，可主机侧生成。

### D4. 运动矢量契约

- **格式**：RG32_FLOAT 或 RG16_FLOAT（L714、L793）。
- **坐标系**：当前帧→前一帧（[0,0] 左上角），符号 ±（L861）。
- **缩放**：`InMVScaleX/Y` = 1.0 无需缩放，绝不为 0.0f（L890-891）。
- **必需性**：**条件必需**——场景运动时必需；静态场景可用零矢量 + `InReset` 回退（质量退化）。**不得**仅凭参数结构体存在 `pInMotionVectors` 字段就推断对所有模式必需。

### D5. 深度 / 曝光 / Reset 契约

- **深度**：任何单通道或深度-模板格式（R32_FLOAT / D32_FLOAT / D24S8，L715-716）。**可选**，Xenia 当前无公开深度访问器。
- **曝光**：仅 Preset J/K；Preset L 自动曝光；1×1 纹理，首通道 R16F（§3.9）。**条件可选**。
- **Reset**：场景完全变化时置 1（头文件 L234）。**条件**。

### D6. 不同 SR 模式的输入差异

- `NVSDK_NGX_PerfQuality_Value`（`nvsdk_ngx_defs.h:251-260`）：**`MaxPerf`、`Balanced`、`MaxQuality`、`UltraPerformance`、`UltraQuality`、`DLAA`。无 `Off` 值。**
- 各模式对输入资源的要求**相同**（颜色 / 输出 / 抖动 / MV / 深度 / 曝光字段一致）；差异仅在于**最小相位数**与**质量 / 性能权衡**，不改变输入资源类型。
- 关闭 DLSS 须通过配置开关，不依赖枚举 `Off`。

**版本差异处理**：头文件 310.9.1 与文档 310.6.0 冲突处，以头文件为权威。本阶段引用的输入契约字段在头文件中均存在且语义一致；文档补充了坐标系 / 相位 / 颜色空间的解释性要求。

---

## E. Xenia 当前资源与 Presenter 流程分析（C2）

### E1. guest 输出真实资源

- **格式**：`R10G10B10A2_UNORM`（`src/xenia/ui/d3d12/d3d12_presenter.h:58-59`）。
- **内部状态**：`D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`（`d3d12_presenter.h:60-61`）。
- **UAV 能力**：创建时 `Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`（`d3d12_presenter.cc:422`），`Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN`（:420）。
- **尺寸**：`frontbuffer_width × frontbuffer_height`（:413-414），MipLevels=1、SampleDesc.Count=1（:416-418）。
- **创建方式**：`RefreshGuestOutputImpl`（`d3d12_presenter.cc:386`）在 `guest_output_resource_ref.second` 为空时经 `CreateCommittedResource` 创建（:423-427），初始状态 `kGuestOutputInternalState`。
- **回调契约**：guest 输出回调初始状态为 `kGuestOutputInternalState`，回调须在使用后迁回该状态（`d3d12_presenter.h:77-79`）。

### E2. 现有转换 / 生命周期流程

- **mailbox**：`kGuestOutputMailboxSize = 3`（`presenter.h:360`）无锁 SPSC 邮箱；`guest_output_resources_`（`d3d12_presenter.h:320`）。
- **paint refs**：`guest_output_resource_paint_refs`（`d3d12_presenter.h:267`）记录各 guest 输出资源与提交索引；`ConsumeGuestOutput`（`presenter.cc:596`）获取并递增引用计数。
- **Fence / 时间线**：`paint_completion_timeline->AwaitSubmissionAndUpdateCompleted(...)`（`d3d12_presenter.cc:586-591`）等待旧 guest 输出资源使用完成后再复用，不阻塞 CPU。
- **SRV**：新 guest 输出纹理创建 SRV（`d3d12_presenter.cc:601-616`）。
- **状态迁移**：现有代码已对资源做屏障迁移（如后缓冲 RTV→SRV，:935-941）。

### E3. `R10G10B10A2_UNORM` 与 DLSS 输入要求兼容性

- **格式**：`R10G10B10A2_UNORM` 属 DLSS 接受的输入缓冲格式（§3.3 "any supported buffer format"，`dlss_guide.txt` L713）。**格式层面可直接使用，无需转换**。
- **颜色空间**：guest 输出经 tone-mapping 后为感知线性 LDR（`IsHDR=0`），满足 LDR 契约（L561-572）。
- **状态**：guest 输出当前为 `PIXEL_SHADER_RESOURCE`，DLSS 输入要求 `NON_PIXEL_SHADER_RESOURCE`（L737-738）。**需一次资源状态迁移**（现有代码已具备屏障迁移能力，:935-941）。
- **UAV**：guest 输出已具备 `ALLOW_UNORDERED_ACCESS`，满足输出缓冲要求（L740-742）。
- **结论**：`R10G10B10A2_UNORM` 与 DLSS 输入要求**兼容**，无需格式转换；仅需一次 `PIXEL_SHADER_RESOURCE` → `NON_PIXEL_SHADER_RESOURCE` 状态迁移。不得仅凭格式名称断言，已从 SDK 文档 + 源码核实。

### E4. 效果链是否支持安全插入 GPU 处理步骤

- guest 输出绘制受 `effect_count` 门控（`d3d12_presenter.cc:559/:573`）；`effect_count ≥ 1` 时经中间纹理 + 最终效果绘制到后缓冲，`effect_count = 0` 时不绘制 + 后缓冲清黑（:929/:1037）。
- **安全插入点**：在 `GetGuestOutputPaintFlow`（`presenter.cc:651`）构造效果链时，可将 DLSS SR 作为**最终效果**插入（替换 / 前置 `kBilinear`），或作为**独立后端**在绘制 guest 输出前对输入纹理执行 `Evaluate`。两种路径均不破坏现有 mailbox / Fence / paint-refs 生命周期。
- **不得**擅自接管 `guest_output_resource_paint_refs` 或破坏现有资源所有权与 Fence 依赖（AGENTS.md §4）。

### E5. 输出到 swapchain 前的颜色空间 / 格式 / 尺寸转换

- guest 输出（`R10G10B10A2_UNORM`，感知线性 LDR）→ 交换链后缓冲（`B8G8R8A8_UNORM`，`d3d12_presenter.h:67`）。现有放大 / 锐化效果链在绘制过程中完成格式 / 尺寸适配（中间纹理 `kGuestOutputIntermediateFormat` 同为 `R10G10B10A2_UNORM`，:63-64）。
- **无额外颜色空间转换**：guest 输出到后缓冲仅经放大 / 锐化效果，无 gamma / 线性空间切换。插入 DLSS 后，DLSS 输出仍为感知线性 LDR，与后缓冲契约一致。

---

## F. 格式转换方案对比（C3）

### 方案 A：直接使用现有 guest 输出纹理

- **检查**：格式 `R10G10B10A2_UNORM`（SDK 支持）、颜色空间（感知线性 LDR，`IsHDR=0`）、分辨率（guest 前缓冲尺寸）、资源状态（`PIXEL_SHADER_RESOURCE`，需迁移至 `NON_PIXEL_SHADER_RESOURCE`）、SDK 接口约束（输入 SRV + 输出 UAV）。
- **结论**：格式 / 颜色空间 / 分辨率 / UAV 均满足；仅需一次状态迁移。MV 缺失（条件必需），深度缺失（可选），jitter 可主机侧生成。
- **技术可行性**：`PLAUSIBLE BUT UNVERIFIED`（格式与状态兼容已静态核实；DLSS `Evaluate` 执行属 `GPU_UNVERIFIED`）。
- **改动范围**：在 `GetGuestOutputPaintFlow` 插入 DLSS 最终效果 + 一次状态迁移；不改动上游渲染。
- **资源同步风险**：低——复用现有 mailbox / Fence / paint-refs，仅新增一次状态迁移屏障。
- **性能开销**：定性——SR `Evaluate` 本身有 GPU 开销，但无额外格式转换带宽。无数据，不编造具体数字。
- **尚缺验证**：DLSS `Evaluate` 在真实 GPU 上的执行与输出画质（`GPU_UNVERIFIED`）；状态迁移屏障的正确性（需单测 / 静态检查）。
- **推荐程度**：**推荐**——最小改动，复用现有生命周期，不破坏原始路径。

### 方案 B：独立中间纹理做格式 / 颜色空间转换

- **评估**：guest 输出已为感知线性 LDR，格式 / 颜色空间均满足 DLSS 输入要求，**无需格式 / 颜色空间转换**。仅在**选择 HDR 模式**（`IsHDR=1`，需线性空间）或**生产伴生 MV / 深度纹理**时才需要独立中间纹理。
- **GPU 操作**：若 HDR，需线性空间转换（tone-mapping 前 / 后）；若 MV，需 NVOF 或自生成；若深度，需深度缓冲访问器。
- **额外显存 / 带宽**：每多一张中间纹理增加一份显存与一次带宽读写。无数据，不编造具体数字。
- **执行顺序 / 状态切换**：转换须 guest 输出就绪后、DLSS `Evaluate` 前执行，并正确迁移状态。
- **与现有队列 / Fence / paint-refs 兼容性**：可兼容，但增加一次 GPU 操作与资源管理复杂度。
- **转换失败回退**：回退到原始效果链（`effect_count ≥ 1` 且 `kBilinear`）。
- **技术可行性**：`NOT SUPPORTED` 作为必需步骤（guest 输出已满足 LDR 输入）；`PLAUSIBLE BUT UNVERIFIED` 若选择 HDR 模式。
- **推荐程度**：**不推荐**作为 SR 的必需步骤；仅在 HDR 模式或伴生 MV / 深度需求时引入。

### 方案 C：调整资源生产方式

- **适用性**：仅在需要真实 MV / 深度 / 曝光时才适用（guest 输出本身已满足 DLSS 输入）。需修改上游渲染（guest 渲染 / 着色器翻译层注入 MV）、深度访问器或曝光 1×1 纹理。
- **改动范围**：大——触及上游渲染路径，兼容性风险高。
- **资源同步风险**：高——新增纹理生产需与现有 paint 时间线 / Fence 对齐。
- **技术可行性**：`PLAUSIBLE BUT UNVERIFIED`（上游改动可行性未验证）。
- **推荐程度**：**暂不推荐**——仅在 MV / 深度需求明确且方案 A 无法满足时引入。

**方案对比汇总**：

| 方案 | 技术可行性 | 改动范围 | 同步风险 | 推荐 |
| --- | --- | --- | --- | --- |
| A 直接复用 guest 输出 + 一次状态迁移 | `PLAUSIBLE BUT UNVERIFIED` | 小（Presenter 层） | 低 | **推荐** |
| B 独立中间纹理（HDR / MV / 深度） | `NOT SUPPORTED`（必需）/ `PLAUSIBLE BUT UNVERIFIED`（HDR） | 中 | 中 | 仅 HDR 时 |
| C 调整资源生产（上游渲染） | `PLAUSIBLE BUT UNVERIFIED` | 大（上游渲染） | 高 | 暂不推荐 |

---

## G. 进入下一阶段的前置条件

进入架构设计或实现前，**必须**满足以下条件（不得为让里程碑通过而降低）：

1. **许可证边界已明确**：§4e 开源兼容性裁定（活动 4-6）已由 NVIDIA 或法律专业人士明确，或已明确指定由谁进一步审查。当前状态 `LICENSE REVIEW REQUIRED`——**未满足**，为阻塞项。
2. **DLSS 输入格式及颜色空间方案有明确证据支撑**：方案 A（直接复用 `R10G10B10A2_UNORM` + 一次状态迁移）已静态核实，证据充分——**满足**。
3. **资源状态 / Fence / 生命周期接入方案不破坏现有 Presenter 契约**：复用 mailbox / Fence / paint-refs，仅新增一次状态迁移屏障，不接管 `guest_output_resource_paint_refs`——**满足**（静态核实）。
4. **无 GPU 环境下可执行的静态检查和主机侧测试策略**：需定义状态迁移屏障、效果链构造、回退逻辑的静态检查与 Mock 单测——**未满足**，需下一阶段定义。
5. **仍需真实 NVIDIA GPU 验证的事项已单独列出**：DLSS `Evaluate` 执行、输出画质、NVOF 执行、帧生成性能——已列于第 I 节——**满足**。

**结论**：前置条件 1（许可证）与 4（静态检查策略）未满足，为进入架构设计 / 实现的**阻塞项**。条件 2、3、5 已满足。

---

## H. 未决问题与风险

**许可证（高）**：
- §4e 开源兼容性裁定——`LICENSE REVIEW REQUIRED`。
- 公开 / 商业发布前 NVIDIA 通知——`UNVERIFIED`。

**输入数据（高）**：
- 运动矢量缺失（条件必需，硬阻塞高质量 SR）——需上游渲染注入或 NVOF 计算。
- 深度无公开访问器（可选）——需上游访问器。
- 曝光无直接信号（条件可选）——可默认 / 估算。

**同步 / 状态（中）**：
- guest 输出 `PIXEL_SHADER_RESOURCE` → DLSS 输入 `NON_PIXEL_SHADER_RESOURCE` 需一次状态迁移屏障——正确性需静态检查 + 单测。
- 不得破坏现有 mailbox / Fence / paint-refs 生命周期——静态核实可兼容，需单测确认。

**版本 / ABI（中）**：
- 头文件 310.9.1 vs 文档 310.6.0，以头文件为权威，需核对 ABI 稳定性。
- 运行时 DLL 版本：SR/RR/FG = 310.9.1.0、NR = 310.8.3.0、lite FG = 310.9.16.0，与 SDK 头文件一致但未加载验证。

**回退（高）**：
- `effect_count = 0` 不可用作回退（会丢失 guest 输出）；回退须构造 `effect_count ≥ 1` 且 `kBilinear` 的原始效果链。

**帧生成呈现机制（高）**：
- 交错生成 / 渲染帧、Reflex+HAGS 在模拟器中可行性未定——`GPU_UNVERIFIED`。

---

## I. 验证状态及环境限制

**证据等级**：
- `STATIC_VERIFIED`：第 D / E / F 节全部结论（输入契约、guest 输出资源、状态、UAV、效果链门控、方案可行性）——源码 + 头文件 + 官方文档逐条核实。
- `COMPILE_VERIFIED`：**未达到**（本阶段未构建）。
- `MOCK_VERIFIED`：**未达到**（本阶段未做 Mock 主机侧测试）。
- `GPU_UNVERIFIED`：DLSS `Evaluate` 执行、输出画质、NVOF 执行、帧生成性能。
- `RUNTIME_UNVERIFIED`：真实目标环境运行时行为。

**Hyper-V 环境无法验证**：真实 GPU 初始化、DLSS SR `Evaluate` 执行、NVOF 光流执行、DLSS 输出画质与帧生成性能、状态迁移屏障在真实 GPU 上的正确性。

**明确声明**：
- 本阶段**未**执行构建，**未**加载 NVIDIA DLL，**未**执行 DLSS `Evaluate` 或 NVOF 执行。
- **未**将 SDK 文件存在等同于运行时可用。
- **未**将静态审计等同于编译通过或运行时验证。
- **未**将运动矢量缺失直接推导为所有 DLSS SR 模式绝对不可用。

---

## J. 下一步建议

**最小且安全的下一步**（仅建议，不执行）：
在确认 §4e 开源兼容性裁定（`LICENSE REVIEW REQUIRED`）后，为方案 A（直接复用 guest 输出 + 一次状态迁移）定义**无 GPU 环境下的静态检查与主机侧 Mock 测试策略**，覆盖：状态迁移屏障、效果链构造、回退到原始效果链、配置开关门控。不接入任何后端、不加载 DLL、不改动渲染行为。

**前置条件**：
1. §4e 开源兼容性裁定已明确（`LICENSE REVIEW REQUIRED` → 由 NVIDIA / 法律确认）。
2. 确认方案 A 为 SR 最小可行路径（本阶段已静态核实）。

**修改范围**：仅 Presenter / 后端管理器层的配置结构与静态检查，不触及上游渲染。

**验收标准**：
1. 构建通过（届时方可标记 `COMPILE_VERIFIED`）。
2. 主开关默认关闭，子功能独立诊断。
3. 单测验证回退到原始效果链（`effect_count ≥ 1` 且 `kBilinear`），`effect_count = 0` 不用于回退。
4. 零 GPU 依赖、零 DLL 加载。

**停止条件**：完成本步后停止，不自动执行 Milestone 1。

---

## 附录：修改范围与 Git 验证

- **本阶段允许修改**：`docs/dlss-integration/milestone-0.1-baseline-review.md`（新建）、`docs/dlss-integration/milestone-0-audit.md`（追加 0.1 摘要 + 相对链接）、`docs/dlss-integration/milestone-0.2-feasibility-review.md`（新建）。
- **本阶段禁止**：修改 C++ 源码 / 头文件、修改 `GuestOutputPaintConfig`、新增后端结果枚举或接入 DLSS/NVOF、添加 / 复制 / 修改 SDK 文件及 DLL、修改 Veyra-NRVideo、执行构建、安装驱动、加载 NVIDIA DLL、使用 Mock / 伪造输出宣称 DLSS/NVOF 已成功、修改用户无关文件、自动进入 Milestone 1。
- **Git 验证**：本阶段仅新增 / 追加上述三个审计文档。`git status --short --branch` 应仅显示 `## dev/dlss5-integration` 与未跟踪的 `docs/dlss-integration/`（含三个文档），无其他差异；`git diff --stat` 为空（三个文档均未跟踪）。
