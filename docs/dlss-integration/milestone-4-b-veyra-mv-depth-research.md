# Milestone 4-B — Veyra 仓库研究与 MV / 深度信息获取方案

- **状态**：Milestone 4-B（K4.1.2）**完成**——纯只读研究，本轮未修改任何 Veyra 或 Xenia 生产代码。
- **范围**：① 对照 `nvidia-nvof-reference/sdk-docs`（NVOF SDK 5.0.7 文档）学习 Veyra 仓库中与渲染输出、运动矢量、深度信息及 NVIDIA 图形增强技术相关的实现；② 确认 MV 与深度信息的真实来源、生成或提取流程、坐标约定、格式与帧间对应关系；③ 评估可移植性并与 Xenia 候选接口逐项对照；④ 提出至少一套具有明确数据来源、实现路径和限制说明的 MV/深度获取方案。
- **撰写前强制阅读**：`AGENTS.md`、`docs/dlss-integration/architecture-design.md`（K4.1）、`docs/dlss-integration/milestone-3-c-handoff.md`、`nvidia-nvof-reference/sdk-docs/`。
- **证据标签约定**：`STATIC_VERIFIED`（源码/静态分析层面）、`SDK-VERIFIED`（SDK 头文件/文档/样本层面）、`VEYRA-VERIFIED`（Veyra 受控实验实证）、`DESIGN PROPOSAL`（设计提案，未实施）、`UNVERIFIED`（未经真实 GPU 验证）。
- **停止点**：报告交付即停，等待用户审核。未经单独批准，不进入 4-C（Xenia 代码审查）或任何代码修改。

---

## A. 总结

- **核心结论**：Veyra 的 MV 来源是 **NVOF 估算光流**（非游戏原生 MV），深度是**合成零深度**或 **Depth Anything V2 单目估计**（非游戏原生深度）。该"估算 MV + 零深度"方案**完整可移植到 Xenia**——Xenia 的 guest output（`R10G10B10A2_UNORM`）正是 NVOF 的输入帧，唯一格式鸿沟是 `R10G10B10A2 → ABGR8` 转换 pass。
- **总体状态**：**PASS WITH FINDINGS**。
  - Findings：
    1. MV 是估算光流而非原生 MV，语义差异必须显式标记（红线 3，不伪装）。
    2. `R10G10B10A2 → ABGR8` 存在 10bit→8bit 截断损失（待量化）。
    3. NVOF 许可证（`LicenseAgreement.pdf`）未审计。
    4. FG 的 `mvecScale` 在 Veyra 中是 "diagnostic candidate"（未定）。
    5. 所有执行项本地 `UNVERIFIED`（Hyper-V 无 NVIDIA 驱动，需 RTX 4090）。

---

## B. 修改文件

**无。** 本轮为纯只读研究，未修改 Veyra 或 Xenia 的任何生产代码。Xenia 工作区仅有用户自己改动的 `docs/dlss-integration/architecture-design.md`（未触碰）。

---

## C. 验证证据（代码索引 + 数据契约 + 兼容性对照 + 方案）

### C.1 Veyra 相关实现的文件 / 类 / 函数 / 着色器 / 资源流向索引

**MV（运动矢量）数据链**：

| 层 | 文件 | 类 / 函数 / 着色器 | 职责 |
|---|---|---|---|
| 光流会话 | `src/ngx/NvOfSession.cpp` / `include/veyra/ngx/NvOfSession.h` | `NvOfSession::initialize()` | 资源契约校验（inputA/inputB=B8G8R8A8 全分辨率，flowOut=R16G16_SINT 网格，costOut=R8_UINT 网格，可选 reverse 对），caps 两次查询（网格尺寸/宽高范围），资源注册，fence 建立 |
| 光流会话 | 同上 | `NvOfSession::execute()` | `inputFrame=当前帧B`、`referenceFrame=前一帧A`，向量指向 **current→previous**，`disableTemporalHints=TRUE`，输出 fence |
| 光流会话 | 同上 | `unregisterAll()` / `shutdown()` | 拆毁顺序 **unregister→释放纹理→destroy→FreeLibrary**（Veyra 实测：destroy+FreeLibrary 后释放已注册资源会段错误） |
| 稠密化 | `shaders/NvofDensify.hlsl` | `NvofDensify` | 网格 S10.5 定点 `/32` → 全分辨率 float2（最近邻复制），cost→confidence=(255−cost)/255，边界/光度/场景切换校验，negate 开关 |
| 重缩放 | `shaders/FlowAdapt.hlsl` | `FlowAdapt` | 按 SR/NR/FG 消费方重缩放 |
| 统一契约 | `include/veyra/pipeline/GuidanceFrame.h` | `GuidanceFrame` | **motion=R16G16_FLOAT（RG16F，current→previous，workingExtent 像素）**，depth=R32F，confidence=R8_UNORM，provenance/sourceSequence/sourceEpoch/depthAgeFrames/requiresReset |
| 指导提供 | `include/veyra/guidance/IGuidanceProvider.h` | `IGuidanceProvider` | `initialize`/`produce`/`reset`/`provenance`；`GuidanceConfig{workingWidth/Height, lookaheadFrames, enableDepth, depthIntervalFrames}` |
| 零回退 | `include/veyra/guidance/ZeroGuidanceProvider.h` | `ZeroGuidanceProvider` | 诚实回退，provenance=Zero，绝不误当真实运动 |
| SR 后端 | `src/ngx/DlssSrBackend.cpp` | `DlssSrBackend::evaluate()` | `pInColor`+`pInDepth`(可空)+`pInMotionVectors`(RG16F)，`InMVScaleX/Y=1.0`，`InJitterOffsetX/Y`，创建标志 `IsHDR\|AutoExposure` |
| FG 后端 | `src/ngx/DlssFgBackend.cpp` | `DlssFgBackend::evaluate()` | `pBackbuffer`+`pDepth`(合成常量)+`pMVecs`，`mvecScaleX/Y`(diagnostic)，`cameraMotionIncluded=1`，`depthInverted=0`，`motionVectorsInvalidValue=FLT_MAX`，合成静止相机 near=0.1/far=1000/FOV=π/2 |
| NR 实例 | `src/pipeline/NrInstance.cpp` | `NrInstance::create()` | `motion_`=R16G16_FLOAT（RG16F），`zeroDepth_`=零深度，输入=R16G16B16A16_FLOAT 线性 |
| NR 参数 | `include/veyra/ngx/DlssNrParameters.h` | `dlssnr::kMVec/kDepth/kColor/kOutput/kMVecScaleX/kMVecScaleY/kDepthInverted` | Feature 18 参数名映射 |
| FG 质量补丁 | `src/ngx/transfusion/quality_fix.h` / `quality_explained_warp.h` | `quality_fix::Patch` | 对 NVIDIA 专有 FG 融合内核的 PTX 质量补丁（valid-warp 策略），**非 MV 来源** |

**深度数据链**：
- **零深度**：`ZeroGuidanceProvider` / `nrZeroDepth_` / `depthTex_`（合成常量，诚实回退）。
- **DAV2 单目估计**：`DepthAnythingProvider`（Playbook §12，Depth Anything V2 Small FP16，ONNX Runtime DML，sequential session，GPU binding，低延迟模式默认关）。provenance=Dav2/NvofDav2。

**资源流向**：`采集帧 A/B (ABGR8) → NvOfSession.execute → flowOut(R16G16_SINT 网格)+costOut(R8) → NvofDensify → GuidanceFrame.motion(RG16F 全分辨率) + confidence(R8) → FlowAdapt → SR pInMotionVectors / FG pMVecs / NR MVec`。深度：`零深度 或 DAV2 估计 → GuidanceFrame.depth(R32F) → SR pInDepth / FG pDepth / NR Depth`。

### C.2 MV 与深度的实际来源、生成/提取流程与数据契约

**MV 来源分类**（按 AGENTS.md 约束逐项区分）：
- **游戏原生输出**：**无**。Veyra 处理采集/串流视频，非游戏内钩子，无原生 MV。
- **渲染管线提取**：**无**（它是视频播放器，非游戏注入）。
- **额外计算生成**：**NVOF 估算光流**（MV 的真实来源）。
- **NVOF 估算光流**：**是**，即 MV 本体。R16G16_SINT 网格 → RG16F 全分辨率，current→previous，像素单位。

**MV 数据契约**（逐项）：

| 维度 | 契约 | 证据 |
|---|---|---|
| 坐标空间 | 屏幕像素空间，post-SR workingExtent | `GuidanceFrame.h` |
| 方向约定 | **current→previous**（反向流） | `NvOfSession::execute` 注释；`SR_MOTION_RESPONSE` 受控实验（准确构造运动=current→previous 改善 SR 误差，反向恶化）`VEYRA-VERIFIED` |
| 数值范围 | NVOF 原始 ±[0,1023] 1/32 精度有符号定点（S10.5）；稠密化后 float 像素 | NVOF SDK `core.md`；`NvofDensify /32` |
| 分辨率 | 原始网格 `ceil(w/grid)×ceil(h/grid)`（grid=4/8/16）；稠密化到全 workingExtent | `NvOfSession` caps |
| 抖动处理 | 无（NVOF 流无抖动；输入是全分辨率 sRGB 帧） | — |
| 相机运动 | `cameraMotionIncluded=1`（FG）：相机运动已烘焙进 mvec，合成静止相机（identity 矩阵） | `DlssFgBackend::evaluate` |
| 动态物体覆盖 | 密集流覆盖所有像素；遮挡/新出现物体由 cost 拒绝 + 可选双向一致性 + 光度校验处理 | `NvofDensify`；`BIDIRECTIONAL_FLOW` 实验 |
| 帧间关系 | 连续真实帧（A=前，B=当前）；多帧生成 2X-6X 在 A/B 间插值；时间 hint 禁用 | `DlssFgBackend` |

**深度数据契约**（逐项）：

| 维度 | 契约 | 证据 |
|---|---|---|
| 深度约定 | 合成零/常量深度；`depthInverted=0` | `DlssFgBackend`；`GuidanceFrame` |
| 投影关系 | 合成静止相机，identity view 矩阵，投影由 aspect 推导（tan(FOV/2)=1）；**非真实投影** | `DlssFgBackend` |
| 范围 | [0,1] 常量（零或 .5/.9） | — |
| 精度 | N/A（常量）；DAV2 时为 R32F 归一化相对深度 | — |
| 反投影参数 | cameraNear=0.1, cameraFar=1000, FOV=π/2, aspect=w/h；因深度为常量，反投影退化 | `DlssFgBackend` |
| **实测结论** | **SR 完全忽略深度（输出无变化）；FG 对深度几乎无响应**——不足以支持引入深度模型 | `SR_FG_DEPTH_RESPONSE` 受控实验 `VEYRA-VERIFIED` |

**关键事实**：Veyra **没有**任何游戏原生 MV 或深度。MV=NVOF 估算，深度=零或 DAV2 估计。

### C.3 Veyra 依赖分析（是否依赖游戏引擎 / 特定渲染 API / 专有接口 / Xenia 无法访问的数据）

| 依赖 | Veyra | 对 Xenia 的影响 |
|---|---|---|
| 游戏引擎 | **无**（纯视频处理） | 无影响 |
| 渲染 API | **D3D12**（唯一） | **兼容**——Xenia 候选集成点也是 D3D12 |
| 专有接口 | **NVOF SDK**（`nvofapi64.dll` System32）+ **NGX SDK**（DLSS SR/FG/NR Feature 18） | 这正是 Xenia 要集成的（DLSS SDK 310.9.1、NVOF SDK 5.0.7），非障碍 |
| Xenia 无法访问的数据 | **无**——MV 源是 NVOF 对 guest output 帧的估算，Xenia 有这些帧（R10G10B10A2）；深度是合成零，无不可访问数据 | **方案完整可移植** |
| 唯一格式鸿沟 | Veyra NVOF 输入=ABGR8；Xenia guest output=**R10G10B10A2_UNORM** | **需 ABGR8 转换 pass**（3-C handoff 已识别） |

### C.4 兼容性对照（逐项，不因 Veyra 用某格式就认定兼容）

| 项 | Veyra 契约 | Xenia 候选接口 | 判定 |
|---|---|---|---|
| MV 格式 | GuidanceFrame.motion = R16G16_FLOAT (RG16F) | SR `motion_vectors_texture_` = R16G16F | **格式匹配** |
| MV 方向 | current→previous | SR MV 契约 current→previous | **匹配**（Veyra 实证，Xenia 需复验） |
| MV 尺度 | SR `InMVScaleX/Y=1.0`（输出像素单位） | SR MVScale 1.0 | **匹配** |
| MV 尺度（FG） | mvecScale "diagnostic candidate"（{1,1} 或 {1/w,1/h} 未定） | FG 未实现（M5） | **待验证** |
| 深度格式 | R32F（零或 DAV2） | SR `depth_texture_` = R32F 零 | **匹配**（均为零占位） |
| NVOF 输入 | B8G8R8A8 (ABGR8) | guest output R10G10B10A2_UNORM | **不匹配 → 需 ABGR8 转换 pass** |
| NVOF 会话契约 | 加载/创建/caps/init/注册/执行/注销/销毁 + 拆毁顺序 + fence + disableTemporalHints + FORWARD | Xenia M4-B 方案已据此设计 | **可直接复用** |
| 稠密化 | NvofDensify（SINT/32→float2, cost→confidence, 校验, negate） | 待移植 | **可复用，需小适配**（working extent、grid） |
| 统一契约 | GuidanceFrame（motion/depth/confidence/provenance/sequence/epoch/depthAge/reset） | 待建立等价结构 | **可复用概念** |
| 帧池/fence | 4-6 帧滚动 + 双 fence 点 + 消费方 `Wait` | Xenia mailbox refresher 时间线 | **需适配** |

### C.5 MV / 深度信息获取方案（明确数据来源、实现路径、限制）

**方案 A（推荐）：NVOF 估算光流 + 合成零深度**（Veyra 已验证路径的直接移植）
- **数据来源**：NVOF SDK 对连续 guest output 帧（A=前、B=当前）估算密集光流，稠密化到全分辨率 RG16F。深度=合成零深度。
- **实现路径**：
  1. **ABGR8 转换 pass**：R10G10B10A2（guest output）→ B8G8R8A8（NVOF 输入），PS 计算，复用 Presenter 命令列表，资源状态转换。
  2. **NVOF 会话**（按 Veyra NvOfSession 契约）：4-6 帧池，inputA/inputB ABGR8，flowOut R16G16_SINT 网格，costOut R8，fence 同步，disableTemporalHints，FORWARD，拆毁顺序 unregister→释放→shutdown。
  3. **稠密化 shader**（按 NvofDensify）：网格 SINT/32 → 全分辨率 float2，cost→confidence，边界/光度校验，negate。
  4. **喂给 SR 后端**：稠密化 RG16F 流替换 `motion_vectors_texture_` 零占位，MVScale 1.0，current→previous。
  5. **保留零深度**（Veyra 实证 SR 忽略深度、FG 几乎无响应，深度模型无实测收益）。
- **限制**：
  - MV 是**估算光流，非原生 MV**。必须显式标记语义差异（红线 3，不伪装）：无相机运动分离、无逐物体运动、遮挡由 cost/一致性而非深度处理。
  - R10G10B10A2→ABGR8 有 **10bit→8bit 截断损失**（待量化）。
  - NVOF 非线程安全 → 会话单线程持有。
  - fence 与 Xenia 现有队列/Fence 耦合，任何失败必须回退 kOriginalPath。
  - current→previous 方向由 Veyra 实证（其 GPU/运行时），Xenia 需复验（M4-D/M4-E）。
  - 所有执行本地 UNVERIFIED（Hyper-V 无 NVIDIA 驱动，需 RTX 4090）。

**方案 B（备选，用于 FG）：NVOF 光流 + DAV2 单目深度估计**
- **数据来源**：NVOF 流（同 A）+ Depth Anything V2 Small FP16 单目深度估计。
- **实现路径**：建 ONNX Runtime DML 会话（sequential、固定 shape、GPU binding），间隔推理 + motion reprojection + P02/P98/EMA，输出 R32F 归一化深度。
- **限制**：**成本高**（额外 ONNX Runtime DML 会话 + 模型 + sequential 调度），且 Veyra 实证 FG 对深度几乎无响应 → 对视频内容（Xenia）不值得。**标记为待验证 / 低优先级**。

**方案 C（不推荐）：真实 guest 深度（Xbox 360 Z 缓冲 + guest 投影矩阵反投影）**
- **数据来源**：guest Z 缓冲 + guest 内存中的 view/projection 矩阵。
- **限制**：模拟器中 guest 投影矩阵在 guest 内存，非直接可访问的 D3D12 矩阵，需逐游戏矩阵提取，**成本高、不确定**。鉴于 Veyra 实证 FG 对深度几乎无响应，**不推荐**。

### C.6 可复用 / 需适配 / 无法直接复用 / 缺证据 分类

| 类别 | 项 |
|---|---|
| **可直接复用** | `NvOfSession` 生命周期与契约（加载/创建/caps/init/注册/执行/注销/销毁、拆毁顺序、fence、disableTemporalHints、FORWARD）；`NvofDensify` 稠密化逻辑（SINT/32→float2、cost→confidence、校验、negate）；零/常量深度诚实回退；GuidanceFrame 统一契约概念 |
| **需适配** | ABGR8 转换 pass（R10G10B10A2→ABGR8）；MV 尺度/方向接入 Xenia SR 后端；帧池 + fence 与 Xenia mailbox refresher 集成 |
| **无法直接复用** | FG 合成相机 + mvecScale diagnostic（Veyra 专有，需 Xenia FG/M5 自行实证）；`transfusion` PTX 补丁（NVIDIA 专有 FG 内核质量 hack，版本特定）；`AdaMfgUnlock`/`AmpereMfgUnlock`/`NvapiArchSpoof`（驱动/架构伪装，非必需）；`DepthAnythingProvider`（需独立 ONNX Runtime DML + 模型，默认关） |
| **缺证据 / 待验证** | current→previous 方向在 Xenia GPU/运行时的匹配（Veyra 实证，需复验）；R10G10B10A2→ABGR8 截断画质损失；NVOF 许可证；FG mvecScale；真实 GPU 执行 |

### C.7 待验证事项清单

1. **NVOF 许可证**：`LicenseAgreement.pdf` 未审计，条款可能不同于 DLSS §4e（M4-A 首要闸门）。
2. **方向复验**：current→previous 在 Xenia RTX 4090 + SDK 310.9.1/NVOF 5.0.7 上的匹配（Veyra 用 +8px/frame 合成右移图案做方向 gate）。
3. **ABGR8 截断损失**：10bit→8bit 量化对 SR/FG 画质的影响量化。
4. **FG mvecScale**：{1,1} vs {1/w,1/h} 的诊断候选，需合成平移片实证（Veyra 明确不写死）。
5. **NVOF 会话拆毁**：unregister→释放→destroy→FreeLibrary 顺序在 Xenia 设备/驱动下的段错误防护。
6. **fence 耦合**：与 Xenia 现有队列/Fence 依赖的同步正确性。
7. **真实 GPU 执行**：所有 NVOF 加载/执行/稠密化/fence/画质项本地 UNVERIFIED。

---

## D. 环境限制

- 开发环境为 Hyper-V，**无 NVIDIA 驱动、无真实 GPU**：NVOF 加载、`NvOFExecuteD3D12` 执行、稠密化 shader 运行、fence 实际行为、DLSS 画质全部 `UNVERIFIED`，直到 RTX 4090 测试机验证。
- 本轮为**纯只读研究**，未运行构建/测试（无代码修改）。所有结论基于源码静态阅读 + SDK 头文件/文档 + Veyra 受控实验文档。
- 测试机：物理 RTX 4090，驱动 617.42，Win 10.0.26100.9549；DLSS SDK 310.9.1、NVOF SDK 5.0.7。

---

## E. 剩余风险

1. **NVOF 许可证**未审计（可能与 DLSS §4e 不同）——M4 第一道闸门。
2. **R10G10B10A2→ABGR8 截断损失**：10bit→8bit 是否可接受，需量化论证。
3. **`nvofapi64.dll` 驱动自带版本漂移**：不同驱动版本 NVOF 行为可能不同。
4. **fence/生命周期同步**：与 Xenia 现有队列/Fence 的耦合，任何失败必须回退 kOriginalPath（M3 的 device-removed 0x887A0006 先例警示）。
5. **FG mvecScale 未定**：Veyra 是"diagnostic candidate"，Xenia FG（M5）需自行实证。
6. **ABI**：NVOF 5.0.7 与未来驱动自带版本的二进制兼容性未验证。
7. **估算 MV 语义**：NVOF 流 ≠ 原生 MV，若后续误当作原生 MV 消费会引入未预期的伪影（红线 3）。

---

## F. 下一步（最小且安全）

4-B 已完成并等待审核。**不进入**正式实现或上游渲染流程修改。最小安全下一步是 **Milestone 4-C（本地 Xenia 代码审查与 NVOF 集成点分析）**——在用户批准 4-B 方案 A 后，对 Xenia D3D12 设备/队列/资源生命周期/paint 路径做只读审查，确认 ABGR8 转换 pass 的插入点、帧池与 fence 的具体接线、以及 `motion_vectors_texture_` 的 SRV 绑定。零代码风险。

---

## G. 各项能力状态清单

| 能力 | 状态 | 备注 |
|---|---|---|
| DLSS SR | 运行时已验证（RTX 4090） | 3×3 上采样；**画质未评估**（C3 悬置）；深度/MV 当前为零占位 |
| NVOF | **未实现**（4-B 完成研究） | 配置钩子 `optical_flow_enabled` 已就位；方案 A 已设计，待 4-C 接线、4-D 验证 |
| 帧生成 | 未实现 | `DlssSrFrameParams` 全 0；FG 的 mvecScale 为待验证 diagnostic |
| 实验性神经渲染 | 仅研究 | 无代码；NR 的 MVec/Depth 契约已明确（RG16F + R32F 零） |
| Presenter 集成 | 已接线且关闭（默认关） | 测试配置 `dlss_enabled=true`、`dlss_sr_enabled=true`；回退已验证 |
| 最终画质与性能 | 尚未评估 | M4-E 处理 |

---

## H. 关键代码引用速查

| 契约点 | 文件:位置 |
|---|---|
| MV 统一契约（RG16F, current→previous, workingExtent） | `Veyra-NRVideo/include/veyra/pipeline/GuidanceFrame.h` |
| NVOF 会话契约与拆毁顺序 | `Veyra-NRVideo/src/ngx/NvOfSession.cpp`（initialize/execute/unregisterAll/shutdown） |
| 稠密化 S10.5/32→RG16F + cost→confidence | `Veyra-NRVideo/shaders/NvofDensify.hlsl` |
| SR 消费端（pInMotionVectors, InMVScale=1.0, IsHDR\|AutoExposure） | `Veyra-NRVideo/src/ngx/DlssSrBackend.cpp`（evaluate） |
| FG 消费端（cameraMotionIncluded=1, depthInverted=0, motionVectorsInvalidValue=FLT_MAX, 合成相机） | `Veyra-NRVideo/src/ngx/DlssFgBackend.cpp`（evaluate） |
| NR 消费端（MVec/Depth/Color 参数名） | `Veyra-NRVideo/include/veyra/ngx/DlssNrParameters.h` |
| SR 忽略深度 / FG 几乎无响应实证 | `Veyra-NRVideo/docs/SR_FG_DEPTH_RESPONSE_2026-09-08.md` |
| 方向 current→previous 实证 | `Veyra-NRVideo/docs/SR_MOTION_RESPONSE_2026-09-08.md` |
| 双向流 / 遮挡一致性 | `Veyra-NRVideo/docs/BIDIRECTIONAL_FLOW_EXPERIMENT_2026-09-08.md` |
| NVOF S10.5 定点格式 | `nvidia-nvof-reference/sdk-docs/api/core.md` |
| Xenia SR 后端零占位 MV/Depth + TODO 钩子 | `xenia-canary/src/xenia/ui/dlss/dlss_super_sampling_backend.h` |
| Xenia 配置钩子（optical_flow_enabled） | `xenia-canary/src/xenia/ui/guest_output_enhancement.h` |
| Xenia guest output 格式 R10G10B10A2 | `xenia-canary/src/xenia/ui/d3d12/d3d12_presenter.h`（kGuestOutputFormat） |
