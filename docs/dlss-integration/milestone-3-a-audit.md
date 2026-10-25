# M3-A — DLSS SR 后端接入前技术审查与实施设计审计

- **里程碑**：M3-A（DLSS SR 接入前审查）。只读审查 + SDK 文档提取，未修改任何源码、未加载任何 DLL、未执行任何 GPU 操作、未运行真实 DLSS Evaluate、未提交代码。
- **仓库状态**：分支 `dev/dlss5-integration`，HEAD `4046a47`，工作树干净（`git status` 无未提交修改）。M2 基线提交 `858e587`。
- **权威 SDK**：`C:\SDKs\NVIDIA\DLSS\DLSS-310.9.1`（头文件 + `doc/DLSS_Programming_Guide_Release.pdf` 84 页正文已提取）。
- **架构参考**：`Veyra-NRVideo`（只读）。
- **总体状态**：**PASS WITH FINDINGS**。接入契约已按 SDK 头文件、SDK 文档与 Veyra 参考核实，设计可行。存在 1 个许可证阻塞项（§4e，公开发布前必须解决）与多项待 RTX 4090 实机确认项，均不阻塞 M3-B 的编译期集成，但阻塞「运行时已验证」的宣称。

---

## A. 总结

M2 已正式验收（接口骨架 + 纯函数 + 15 用例主机侧测试）。M3-A 审查确认 DLSS SR 可安全接入 Xenia D3D12 呈现路径，关键契约已核实：输入为 `R10G10B10A2_UNORM` 感知 LDR（`IsHDR=0`），输出需 `ALLOW_UNORDERED_ACCESS` + UAV 态，SDK 在 evaluate 后把输入/输出恢复至已知状态，后端只借用 guest 输出、自持输出纹理，失败时确定性回退 `kOriginalPath`。相对初版报告修正了 4 处：G 节状态、失败回退的命令提交语义、输出纹理契约（升级为 SDK 核实）、许可证阻塞范围。

## B. 拟修改文件清单（M3-B，本任务未修改）

| 文件 | 修改内容 | 修改理由 |
|---|---|---|
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h`（新建） | 实现 `IDLSSSuperSamplingBackend`：惰性初始化、`TryProcess(cmdlist, guest_output)`、`Shutdown()`、自持输出纹理与 feature handle、1:1 bypass 判定、SEH 边界 | M3 核心。将 SDK 封装进独立模块，隔离 DLL 加载与 GPU 命令，便于确定性回退与许可证隔离 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc`（新建） | SDK 调用序列：`Init_with_ProjectID`→`AllocateParameters`→`NGXResourceAllocCallback` 注册→`CreateFeature`(cmdlist)→`EvaluateFeature`(cmdlist)；状态迁移屏障；参数填充（`IsHDR=0`、`AutoExposure`） | 封装真实 SDK 执行，M2 的 `TryProcess` 是纯接口，M3 提供真实实现 |
| `src/xenia/ui/d3d12/d3d12_presenter.h` | 增加后端指针成员（`std::unique_ptr<IDLSSSuperSamplingBackend>`）、后端输出纹理引用、`GuestOutputEnhancementConfig` 及 mutex 保护 | 接线 Presenter，持有后端生命周期与配置门控 |
| `src/xenia/ui/d3d12/d3d12_presenter.cc` | `PaintAndPresentImpl` 内：SR 被请求时调用 `backend->TryProcess(cmdlist, guest_output)`；成功则 paint 输入=后端输出，失败/未请求走 `BuildOriginalPathFallbackFlow` 原始路径；失败时 `CommandList::Reset`+重录 | 在现有完整呈现流程中插入后端，复用 letterbox/输出位置语义 |
| `src/xenia/ui/guest_output_enhancement.h/.cc` | 不改逻辑；`TryProcess` 可能补充命令列表参数 | M2 接口骨架已满足，仅可能扩展签名 |
| `src/xenia/ui/dlss/CMakeLists.txt`（新建）+ `src/xenia/ui/CMakeLists.txt` | SDK 头文件目录、`nvngxruntime` header-only loader、运行时 DLL 搜索路径；挂载 `dlss/*` 源文件 | 编译期集成 |

> **许可证隔离原则**：NVIDIA SDK 依赖只允许出现在 `dlss/*` 模块。Presenter、配置结构体、路径选择纯函数一律不 `#include` 任何 `nvsdk_*`、不触碰 SDK。不复制 SDK 文件进 Xenia 源码树，不把 SDK DLL 打包进发行物（由 NVIDIA 驱动安装提供，Xenia 只按 `PathList` 指向运行时目录加载）。若 §4e 最终裁定不能随 Xenia 分发，删掉 `dlss/*` 模块即可，其余代码零改动。

## C. 验证证据（已确认事实 + 官方头文件与 SDK 文档证据）

### C.1 仓库基线（已确认）

- `git status` 干净；`git log` 显示 M2=`858e587`，HEAD=`4046a47`。
- M2 `guest_output_enhancement.h/.cc` 是纯独立文件：`rg` 在 `d3d12_presenter.cc/.h` 中搜索 `guest_output_enhancement`/`SelectOutputPath`/`IsSuperSamplingRequested` **全部为空**。M2 从未被 Presenter 引用，只是接口骨架 + 纯函数 + 配置结构体。

### C.2 SDK 头文件证据（权威版本 DLSS-310.9.1）

- **初始化** `nvsdk_ngx.h:247`：`NVSDK_NGX_D3D12_Init_with_ProjectID(const char* InProjectId, NVSDK_NGX_EngineType, const char* InEngineVersion, const wchar_t* InApplicationDataPath, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version)` — 运行时目录经 `NVSDK_NGX_FeatureCommonInfo.PathListInfo.Path` 传入；内部经 `NGXLoadLibrary` 加载 `nvngx_dlss.dll`。
- **参数块** `nvsdk_ngx.h:381/450`：`NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter**)` / `NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter*)`。
- **关闭** `nvsdk_ngx.h:282`：`NVSDK_NGX_D3D12_Shutdown1(ID3D12Device*)`。
- **特性创建** `nvsdk_ngx.h`：`NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)` — **接受命令列表，特性创建是 GPU 命令**。
- **特性评估** `nvsdk_ngx.h`：`NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback)` — 同样接受命令列表。
- **释放**：`NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle*)`。
- **便捷封装** `nvsdk_ngx_helpers_d3d.h:275/297`：`NGX_D3D12_CREATE_DLSS_EXT(pInCmdList, creationNodeMask, visibilityNodeMask, ppOutHandle, pInParams, pInDlssCreateParams)` 与 `NGX_D3D12_EVALUATE_DLSS_EXT(pInCmdList, pInHandle, pInParams, pInDlssEvalParams)` 是 `static inline` 封装，填充参数块后调底层 `CreateFeature`/`EvaluateFeature`。**实际使用模式是这两个 `_EXT` 封装**，不是裸调底层函数。
- **特性发现**：`NVSDK_NGX_D3D12_GetFeatureRequirements(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*)` — 可先于 `Init` 调用。
- **评估参数** `nvsdk_ngx_helpers_d3d.h`：`NVSDK_NGX_D3D12_DLSS_Eval_Params` 含 `Feature.pInColor`、`Feature.pInOutput`、`pInDepth`、`pInMotionVectors`、`InJitterOffsetX/Y`、`InRenderSubrectDimensions`、`InReset`、`InMVScaleX/Y`、`pInExposureTexture`、`InPreExposure`、`InExposureScale` 等（可选字段留 0/nullptr）。
- **创建参数** `nvsdk_ngx_params.h`：`NVSDK_NGX_DLSS_Create_Params` 含 `Feature.InWidth/Height/InTargetWidth/InTargetHeight`、`Feature.InPerfQualityValue`、`InFeatureCreateFlags`、`InEnableOutputSubrects`。
- **质量值** `nvsdk_ngx_defs.h`：`NVSDK_NGX_PerfQuality_Value` = `MaxPerf`/`Balanced`/`MaxQuality`/`UltraPerformance`/`UltraQuality`/`DLAA` — **无 `Off` 值**，关闭由 Xenia 侧 `GuestOutputEnhancementConfig` 控制。
- **功能枚举**：`NVSDK_NGX_Feature_SuperSampling`。
- **创建标志**：`NVSDK_NGX_DLSS_Feature_Flags_IsHDR`（`1<<0`）、`NVSDK_NGX_DLSS_Feature_Flags_AutoExposure`。
- **资源分配回调参数**：`NVSDK_NGX_Parameter_ResourceAllocCallback`。

### C.3 SDK 文档证据（`DLSS_Programming_Guide_Release.pdf` 310.9.1，正文已提取）

- **3.4 Resource States（权威）**：
  1. 输入缓冲（color/MV/depth/history/exposure）须处**读态** = `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`（被 compute shader 读取）。
  2. 输出缓冲须处 **UAV 态**，创建时带 `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`。
  3. **evaluate 之后**，DLSS 可能改变缓冲状态，但**始终把缓冲恢复回这些已知状态**（输入→`NON_PIXEL_SHADER_RESOURCE`，输出→UAV 态）。
- **3.1.2 LDR/HDR（权威）**：LDR 模式颜色范围 0.0–1.0、量化到 8 位、输入须为**感知线性**编码（如 sRGB，tone mapping 之后）→ 设 `IsHDR=0`。若输入在线性空间或不满足上述条件 → `IsHDR=1`（HDR 模式，高精度）。
- **6.2 D3D12 资源分配回调（权威）**：SDK 经 `NGXResourceAllocCallback(D3D12_RESOURCE_DESC* InDesc, int InState, CD3DX12_HEAP_PROPERTIES* InHeap, ID3D12Resource** OutResource)` 分配内部资源，app 实现为 `MyDevice->CreateCommittedResource(InHeap, D3D12_HEAP_FLAG_NONE, InDesc, (D3D12_RESOURCE_STATES)InState, nullptr, IID_PPV_ARGS(OutResource))`，经 `Params->Set(NVSDK_NGX_Parameter_ResourceAllocCallback, NGXResourceAllocCallback)` 注册。另有 `NGXResourceReleaseCallback(IUnknown*)` 释放。
- **6.4 错误码（权威）**：`NVSDK_NGX_Result_FAIL_RWFlagMissing`（输入/输出需 RW/UAV 访问）、`NVSDK_NGX_Result_FAIL_UnsupportedInputFormat`（输入/输出格式不支持）、`NVSDK_NGX_Result_FAIL_MissingInput`（创建时声明的输入在评估时未提供）、`NVSDK_NGX_Result_FAIL_UnableToInitializeFeature`（特性配置错误或系统不可用）。
- **3.5 Mip-Map Bias**：DLSS 激活时引擎须设负 LOD bias（`NativeBias + log2(RenderX/DisplayX) - 1.0 + ε`）。Xenia guest 输出已是最终渲染，此条是否适用为开放项。

### C.4 Veyra-NRVideo 参考证据（只读）

- `DlssSrBackend::create/evaluate` 用 `NGX_D3D12_CREATE_DLSS_EXT`/`NGX_D3D12_EVALUATE_DLSS_EXT`，均把 GPU 命令**记录**进传入的 `ID3D12GraphicsCommandList*`。
- **SEH 边界**：所有 SDK 入口（`Init`/`AllocateParameters`/`CreateFeature`/`EvaluateFeature`/`ReleaseFeature`/`Shutdown`）都包 `__try/__except`，因为 SDK 调用可能**崩溃**（access violation）而不只是返回失败码。
- **1:1 bypass**：`shouldBypass` 在 `inputW==outputW && inputH==outputH` 时跳过整个 SR 后端（Playbook §11）。
- **创建标志**：`InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure`。Veyra 设 `IsHDR=1` 因其输入为**线性 RGBA16F**；Xenia guest 输出是 `R10G10B10A2_UNORM` 感知 LDR，`IsHDR` 应为 `0`（按 §3.1.2，须核实 guest 输出确为感知线性而非线性空间）。
- **自动曝光**：`AutoExposure` 由 SDK 估算器处理，`InPreExposure=1.0f`/`InExposureScale=1.0f` 为默认——直接解决初版报告「曝光无直接信号」开放项。
- **失败语义**：Veyra 后端失败时只 `return false`，**不自己 Reset 命令列表**——命令列表善后是调用方（Presenter）的责任。
- **输出纹理**：Veyra 把 `output` 当作已存在的 `ID3D12Resource*` 传入，后端不创建。Veyra 视频管线的合成目标是 `ALLOW_RENDER_TARGET`+`COMMON` 态，但那是它的视频合成目标，**不是 DLSS SR 输出纹理的权威契约**（架构不同）。

### C.5 Xenia 资源契约证据（已确认）

- `d3d12_presenter.h`：`kGuestOutputFormat=R10G10B10A2_UNORM`，`kGuestOutputInternalState=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE`，`kGuestOutputIntermediateFormat=R10G10B10A2_UNORM`，`kSwapChainFormat=B8G8R8A8_UNORM`。
- `d3d12_presenter.cc::RefreshGuestOutputImpl`：guest 输出创建时 `Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`，初始态 `kGuestOutputInternalState`，`Layout=UNKNOWN`。尺寸变化时 `AwaitSubmissionAndUpdateCompleted` 后释放重建。refresh 回调结束后把 guest 输出迁回 `kGuestOutputInternalState`。
- `d3d12_presenter.cc::PaintAndPresentImpl`：`ConsumeGuestOutput` 取 guest 输出 ComPtr → `GetGuestOutputPaintFlow` 构造效果链 → 回退检查（`effect_count=0` 时不画、清黑）→ 创建/复用 paint ref + SRV → 效果循环（中间效果画中间纹理，最终效果画后缓冲）→ 状态迁移屏障 → `ExecuteCommandLists` → signal 时间线 → `Present`。
- `presenter.h`：`kGuestOutputMailboxSize=3`，`kMaxGuestOutputPaintEffects=6`。
- **paint refs**：`guest_output_resource_paint_refs` 不得接管，只能借用。

### C.6 调用流程与资源状态流程（M3-B 设计，SDK 核实）

**调用流程**：现有 paint 是 `guest 输出(W×H) → [kBilinear] → 后缓冲(SW×SH)`。接入 DLSS SR 后变为 `guest 输出(W×H) → [DLSS SR 后端] → 后端输出(TW×TH) → [kBilinear] → 后缓冲(SW×SH)`。后端输出是 paint 链首个效果的输入，DLSS SR 以「伪效果」形式插入链首（SDK GPU 命令，非 paint shader）。后端只借用 guest 输出（只读 SRV），自持输出纹理与 feature handle，绝不接管 `guest_output_resource_paint_refs`，绝不拥有 guest 输出资源。

**资源状态流（按 SDK §3.4 核实）**：
- guest 输出（输入）：`PIXEL_SHADER_RESOURCE`（refresh 后）→ **Xenia 屏障迁至** `NON_PIXEL_SHADER_RESOURCE`（DLSS 输入态）→ [DLSS evaluate 读] → **SDK 恢复** `NON_PIXEL_SHADER_RESOURCE` → paint 读的是**后端输出**而非 guest 输出，故本帧无需再迁 guest 输出 → 下帧 refresh 迁回 `PIXEL_SHADER_RESOURCE`。
- 后端输出（输出）：创建时 `ALLOW_UNORDERED_ACCESS`，初始 UAV 兼容态 → [DLSS evaluate 写] → **SDK 恢复** UAV 态 → **Xenia 屏障迁至** `NON_PIXEL_SHADER_RESOURCE`（或 `PIXEL_SHADER_RESOURCE`）供 paint 经 SRV 读 → paint 画到后缓冲。这是写后读（WAWAR）同步屏障。

### C.7 关键修正（相对初版报告的 4 处）

1. **G 节状态**：M2 从未接入 Presenter（C.1 已证）。`DLSS SR` 改为「接口骨架已实现（M2）；真实后端未实现；运行时未验证」；`Presenter 集成` 改为「尚未开始」（不是「已接线且关闭」）。
2. **命令提交语义**：初版「执行 DLSS 再选原始路径」省略了 Reset，不安全。Veyra 证实 `evaluate` 把命令**记录**进命令列表且失败时不自己 Reset。修正：命令列表是录制缓冲、只在末尾 `ExecuteCommandLists` 执行一次；所有可失败操作前置到执行前（CPU 侧校验不碰列表，GPU 侧的 `CreateFeature`/`EvaluateFeature` 作为首个命令组）；**失败 → `CommandList::Reset` → 重录原始路径**（Reset 清空所有已录制未执行命令，且此时 GPU 未执行任何命令，guest 输出状态迁移与后端输出写入均未发生，资源仍处原始状态）；惰性初始化失败要缓存（标 `kUnavailable`），后续帧直接走原始路径、不再尝试 SDK。结论：命令列表可安全继续用于原始路径，但前提是「失败→Reset→重录」，而非「失败→在同一列表上追加原始路径」。
3. **输出纹理契约**：初版「初始 `PIXEL_SHADER_RESOURCE`，UAV 写入后供 paint 读取」是设计草案，已升级为 SDK 核实（C.3 §3.4）：输出须 `ALLOW_UNORDERED_ACCESS`+UAV 态，SDK evaluate 后恢复输出至 UAV 态、输入至 `NON_PIXEL_SHADER_RESOURCE`；paint 读后端输出须经 UAV→shader-resource 屏障；app 须实现 `NGXResourceAllocCallback` 让 SDK 分配内部资源。
4. **许可证范围**：明确「本地研发不受阻塞，公开提交/分发须 §4e 解决前不得宣称可分发」，并把 SDK 依赖隔离在 `dlss/*` 模块（B 节隔离原则），不复制 SDK 文件、不打包 SDK DLL。

### C.8 已确认事实 / 待确认事项 / 设计建议

**已确认事实**：
- M2 纯独立，未接入 Presenter（C.1）。
- SDK 输入须 `NON_PIXEL_SHADER_RESOURCE`、输出须 `ALLOW_UNORDERED_ACCESS`+UAV 态，evaluate 后 SDK 恢复两缓冲至已知状态（§3.4）。
- guest 输出 `R10G10B10A2_UNORM` 感知 LDR → `IsHDR=0`（§3.1.2，前提 guest 输出确为感知线性）。
- app 须实现 `NGXResourceAllocCallback` 注册（§6.2）。
- `Init_with_ProjectID`/`AllocateParameters`/`Shutdown1`/`_EXT` 封装均存在且经 Veyra 验证。
- `nvngx_dlss.dll` 官方 310.9.1.0，Authenticode Valid，与 SDK `lib` 目录大小一致。

**待确认事项**：
- §4e 开源兼容性裁定（嵌入 SDK 目标代码到开源 Xenia 是否违禁）——**阻塞项**，需 NVIDIA/法律确认。
- guest 输出是否确为感知线性（非线性空间）——决定 `IsHDR`，须核实上游 tone mapping 路径。
- 输出纹理**格式**（是否须与 `TargetWidth/TargetHeight` 匹配的特定格式；`R10G10B10A2_UNORM` 是否受支持，否则 `FAIL_UnsupportedInputFormat`）。
- 输出纹理**注册方式**（裸 `ID3D12Resource*` 还是经描述符/参数块注册）与**写后读同步**细节。
- MV 缺失对 SR 质量影响（`pInMotionVectors=nullptr` 时）——需上游注入或 NVOF。
- 深度无公开访问器（可选，`pInDepth=nullptr`）。
- `3.5` Mip-Map 负 LOD bias 是否适用于 Xenia guest 输出。
- 头文件 310.9.1 vs 文档 310.6.0 ABI 稳定性（以头文件为权威，须核对字段对齐）。

**设计建议**：
- 后端模块 `dlss/*` 严格隔离 SDK，Presenter 不触碰 `nvsdk_*`。
- 失败回退走「Reset→重录」，惰性初始化失败缓存。
- 1:1 bypass：guest 输出已达目标分辨率时跳过 SR 后端。
- 所有 SDK 入口包 SEH。
- `AutoExposure` 用 SDK 估算器，不依赖 Xenia 提供曝光纹理。

## D. 环境限制

- 当前 Hyper-V 环境**无 NVIDIA 驱动、无真实 GPU**。无法执行：`Init_with_ProjectID` 成功路径、`CreateFeature`/`EvaluateFeature` 的 GPU 执行、`nvngx_dlss.dll` 真实加载、`NGXResourceAllocCallback` 真实分配、任何 DLSS 画质/性能评估。
- 本任务仅完成静态核实（头文件、SDK 文档提取、Veyra 参考、Xenia 契约），未运行任何构建或测试。M3-B 的编译与主机侧 Mock 测试计划在 F 节列出。
- 所有 GPU 相关项在实机验证前一律标记 `UNVERIFIED`。

## E. 剩余风险

- **许可证（阻塞）**：§4e 将 SDK 目标代码嵌入开源 Xenia 是否构成违禁，需 NVIDIA/法律裁定。未裁定前不得宣称可分发。已通过 B 节隔离原则降低爆炸半径。
- **ABI 稳定性**：头文件 310.9.1 vs 文档 310.6.0，以头文件为权威，须核对 `NVSDK_NGX_DLSS_Eval_Params`/`Create_Params` 字段对齐。
- **运行时 DLL 版本**：SR/RR/FG=310.9.1.0 与头文件一致，但未加载验证。`nvngx_dlssnr.dll` 社区 310.8.3 HashMismatch，不用于 SR。
- **MV 缺失**：Xenia 当前无逐像素运动矢量通道，`pInMotionVectors=nullptr` 时 SR 质量下降。
- **深度/曝光**：`pInDepth=nullptr`（无公开访问器）；曝光用 `AutoExposure` 估算器。
- **状态迁移屏障正确性**：guest 输出 `PIXEL_SHADER_RESOURCE→NON_PIXEL_SHADER_RESOURCE`、后端输出 UAV→shader-resource 的屏障须单测+真实 GPU 验证。
- **同步**：后端 `TryProcess` 追加到 paint 命令列表，须与现有 `paint_completion_timeline`、Fence、paint ref 依赖一致，不得破坏队列同步。失败回退走「Reset→重录」，不得「追加原始路径」。
- **回退确定性**：任何失败必须确定性回退 `kOriginalPath`，`effect_count` 永不为 0（避免清黑）。
- **SDK 崩溃**：SDK 入口可能 access violation，须 SEH 边界包裹，崩溃等同失败回退。

## F. 下一步（最小且安全）

**1. 当前环境可执行（M3-B 编译期）**：
- 实现 `dlss_super_sampling_backend.h/.cc`，挂接 CMake，`.\xb build --config release` 编译通过（不运行）。
- 补充 `guest_output_enhancement_test.cc` 的 Mock 后端用例，`.\xb build --config release --build-tests --target xenia-app` 运行主机侧测试。
- 静态分析（clang-tidy/MSVC `/W4`）确认无 SDK 头文件误用、无 `nvsdk_*` 泄漏进 `dlss/*` 之外。

**2. RTX 4090 实机验证清单**：
- 驱动安装后 `nvngx_dlss.dll` 可被 `Init_with_ProjectID` 加载。
- `GetFeatureRequirements` 报告 SR 支持。
- `NGXResourceAllocCallback` 被 SDK 调用且资源分配成功。
- `CreateFeature` 成功，`EvaluateFeature` 在 4090 上执行无 `FAIL_*`。
- guest 输出 `PIXEL_SHADER_RESOURCE→NON_PIXEL_SHADER_RESOURCE` 迁移正确；后端输出 UAV→shader-resource 迁移正确；无 fence 死锁。
- 后端输出画质 vs 原始 kBilinear 路径对比。
- 关闭时（`master_enabled=false`）确认零 DLL 加载、零 GPU 资源、零后端调用、零 SDK 初始化。
- `nvngx_dlss.dll` 310.9.1.0 在 4090 驱动下加载成功。
- 失败回退：模拟 `EvaluateFeature` 失败，确认「Reset→重录原始路径」安全、不清黑。
- 1:1 bypass：guest 输出=目标分辨率时跳过 SR。

**3. 等待用户审核**后进入 M3-B。本任务到此停止，不自动进入 M3-B。

## G. 各项能力状态清单（修正后）

- **DLSS SR**：接口骨架已实现（M2）；真实后端未实现；运行时未验证。
- **NVOF**：未实现（独立模块，非 SR 依赖）。
- **帧生成**：未实现（独立模块）。
- **实验性神经渲染**：仅研究（社区 `nvngx_dlssnr.dll` 310.8.3 HashMismatch，无 Xenia 契约）。
- **Presenter 集成**：尚未开始（M2 只定义接口+配置结构体+路径选择纯函数，未接入 `PaintAndPresentImpl`）。
- **最终画质与性能**：尚未评估（待 RTX 4090 实机）。

---

## 附录：相对初版报告的变更摘要

- **C.2**：初始化从 `NVSDK_NGX_D3D12_Init` 修正为 `NVSDK_NGX_D3D12_Init_with_ProjectID`（运行时目录经 `PathList` 传入）；关闭从 `Shutdown` 修正为 `Shutdown1`；补充 `AllocateParameters`/`DestroyParameters` 参数块；补充 `NGX_D3D12_*_EXT` 便捷封装（实际使用模式）。
- **C.3**：新增 SDK 文档核实（§3.4 状态契约、§3.1.2 IsHDR、§6.2 资源分配回调、§6.4 错误码、§3.5 Mip-Map bias）。
- **C.4**：新增 Veyra 参考（SEH 边界、1:1 bypass、`AutoExposure`、失败不自己 Reset、输出纹理由调用方创建）。
- **C.7**：4 处关键修正（G 节状态、命令提交语义、输出纹理契约、许可证范围）。
- **G**：DLSS SR 与 Presenter 集成状态修正。
