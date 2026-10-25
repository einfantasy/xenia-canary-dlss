# Milestone 3.1（续）：切换 `NVSDK_NGX_DLSS_Hint_Render_Preset` 的官方机制调查与修改方案

- **文档性质**：INVESTIGATION + DESIGN。本轮**未修改任何代码**，仅只读分析（官方手册、SDK 头文件、当前源码）+ 设计输出。未编译、未运行、未加载 DLL、未执行 GPU 操作。
- **前置文档**：`milestone-3-1-dlss-sr-render-preset-investigation.md`（枚举用法调查，已交付）。本文回答其遗留问题：**如何切换「目前使用中的」preset**，并结合本项目给出修改方案。
- **仓库状态**：分支 `dev/dlss5-integration-sr-merge`，HEAD `fac5037`（质量档每帧传递 + 后端自建重建）。工作树干净。
- **证据标签约定**：`STATIC_VERIFIED`｜`SDK-DOC-SUPPORTED`｜`SDK-VERIFIED`｜`DESIGN PROPOSAL`｜`UNVERIFIED`｜`RUNTIME_VERIFIED_ON_RTX4090`。

---

## A. 总结

**官方只有两条「切换 preset」的路径，且都不是原地切换：**

1. **Model Selection API（create-time 重建）**——§3.12 + 附录 §9.1 条目 2 原文「Model Selection API (see section 3.12)」。Preset 是 create-time hint，**整个 feature 生命周期固定**（§3.2.2：「The Preset chosen for the init-time input size will persist across the lifetime of the feature (presets will not vary even if scaling ratio varies)」）。SDK **没有**原地更新参数的 API（头文件全量核对：无 `NVSDK_NGX_UpdateParameters` 或任何等价物）。切换 = `ReleaseFeature`（旧 handle 失效）→ 对新参数块 `SetUI` 新值 → `CreateFeature`（新 handle）。`SDK-VERIFIED` + `SDK-DOC-SUPPORTED`
2. **OTA（`NVSDK_NGX_UpdateFeature`）**——§2.4 + 附录 §9.1 条目 1 原文「Preset Updates via OTA (see section 2.4)」。这是 NVIDIA 向 opt-in 应用**推送**新模型/权重，须在独立线程调用、下次 `Init` 时生效；**不是**应用按需切换 K→L 的机制。当前后端从未调用（无 OTA opt-in）。`SDK-DOC-SUPPORTED` + `STATIC_VERIFIED`

**因此本项目切换 preset 的唯一途径是 create-time 重建，而这正是后端已具备的机制**：`fac5037` 已把质量档变更处理移入后端——`TryProcess` 每帧接收质量档，`created_perf_quality_` 与请求值不一致时 `ReleaseFeature()` + `Initialize()` 重建（:847-854），与尺寸变更同一通路，且已在 RTX 4090 运行时验证（M3-C）。**render preset 的切换完全镜像该模式**，新增一次 `SetUI` 调用与一个重建条件项即可。

**关键同步约束**（§5.5 原文）：「A feature handle should only be released once the command lists (Direct3D) ... that were used in Evaluate_XXX() calls are no longer in flights as that command list could still reference internal states and resources associated with the feature.」Presenter 在 `PaintAndPresentImpl` 开头等待 paint 时间线完成上一帧提交后才到达 `TryProcess`，重建发生的时刻上一帧含 Evaluate 的命令列表已执行完毕——该约束由现有时间线机制满足（质量档重建路径已按此模式运行时验证）。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`

**总体状态**：`PASS`（调查完成 + 设计给出，未实施）。

---

## 1. 官方「切换 preset」机制精读

### 1.1 不存在原地切换

- 头文件全量核对（`nvsdk_ngx.h`、`nvsdk_ngx_params.h`、`nvsdk_ngx_helpers*.h`）：参数修改 API 只有 `Set/SetI/SetF/SetUI/SetD3d12Resource`（写入参数块），**没有**「对已创建 feature 更新 create-time 参数」的入口。`SDK-VERIFIED`
- §3.2.2 明文：preset 随 feature 生命周期固定，「presets will not vary even if scaling ratio varies」——即使缩放比变了 preset 也不变，更遑论主动切换。`SDK-DOC-SUPPORTED`
- 官方 API 生命周期（`nvsdk_ngx.h:32-64` 注释）：设参数 → `GetScratchBufferSize` → `CreateFeature` → `EvaluateFeature` → `ReleaseFeature` → `Shutdown`。切换 preset 只能走 `ReleaseFeature` → 重新 `CreateFeature` 的循环。`SDK-VERIFIED`

### 1.2 ReleaseFeature 的同步与内存语义

- **同步**：§5.5「A feature handle should only be released once the command lists ... that were used in Evaluate_XXX() calls are no longer in flights」。`SDK-DOC-SUPPORTED`
- **内存**：§3.14 原文——`ReleaseFeature()` 后内存**默认不释放**，内部缓存以便「immediately re-create feature again」时复用；可用 `NVSDK_NGX_Parameter_SetI(ngxParams, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1)` 强制释放。即「release + 立即 re-create」是 SDK 预期的高效模式，重建不会造成 VRAM 抖动。`SDK-DOC-SUPPORTED`
- 头文件 :53-54 另注：「If feature with the same parameters already exists ... error code will be returned」——同参数 feature 可能已被缓存，重建同参数组合的开销更低。`SDK-VERIFIED`

### 1.3 OTA 不是切换机制

- §2.4：`NVSDK_NGX_UpdateFeature(ApplicationId, FeatureID)` 触发 OTA 更新（若有）；更新在下一次 `Init` 生效；须在独立线程调用（下载耗时不定）。这是 NVIDIA 推送模型更新，应用无法用它「按需选 preset」。`SDK-DOC-SUPPORTED`
- §3.12.1 的 OTA 语义（显式选定模型保留 / Default 可能漂移）只在该 opt-in 路径下发生。当前后端无此调用，preset 行为完全由部署的 310.9.1.0 运行时决定。`STATIC_VERIFIED`

### 1.4 结论

**切换目前使用中的 preset = ReleaseFeature（等命令列表执行完）→ SetUI 新值 → CreateFeature。** 无其他官方途径。

---

## 2. 当前后端的可复用机制（源码核对）

| 机制 | 位置 | 内容 |
|---|---|---|
| 惰性初始化 / 变更重建 | `dlss_super_sampling_backend.cc:847-862` | `state_ == kNotInitialized` 或尺寸变化或 `requested_quality != created_perf_quality_` → `ReleaseFeature()` + `output_texture_.Reset()` + `state_ = kNotInitialized` → `Initialize(...)` 重建。质量档重建**已运行时验证**（M3-C） |
| 每帧参数传递 | `TryProcess(..., frame_params, perf_quality)` | 质量档经接口每帧传入，后端与 `created_perf_quality_` 比较 |
| 创建参数填充 | `Initialize` 内 :610-626（AllocateParameters + 回调 Set）、:763-777（create_params + `CallCreateDlss`） | **render preset 的 `SetUI` 接线点在 :626 之后、:778 之前** |
| 同步保证 | Presenter `PaintAndPresentImpl` 开头 | 等待 paint 时间线完成上一帧提交后才到达 `TryProcess`，满足 §5.5「release 前命令列表已执行完」 |
| 失败语义 | `kInputContractFailed` + 缓存 `kUnavailable` | 越界质量档的处理范式，render preset 越界同构 |

`STATIC_VERIFIED`

---

## 3. 修改方案（DESIGN PROPOSAL，未实施）

### 3.1 设计决策

| 决策 | 内容 | 理由 |
|---|---|---|
| D1 | **单值全局 preset**，不是每模式 5 个独立值 | 当前后端只有单一质量档（4 档），单值映射到当前模式的参数字符串即可；每模式独立 preset 留作未来扩展，不引入 5 个 UI 值 |
| D2 | **默认 `kDefault`（0）= 完全不 Set** | 与当前已验证基线（Default 解析为 K/M/L）零行为变化，不引入新运行时变量 |
| D3 | **切换走既有重建通路**，不新增 SDK 调用面 | §1 结论：官方只有重建一条路；`fac5037` 已提供质量档重建，render preset 镜像之 |
| D4 | **许可隔离**：Xenia 侧枚举数值与 SDK 头文件显式数值完全一致（0/10/11/12/13），UI 层不碰 `nvsdk_*` | 沿用 `DlssSrPerfQuality` 的隔离模式；避免 §1.3（上一轮报告）的隐式编号陷阱 |
| D5 | **越界 = 输入契约失败**（`kInputContractFailed`，缓存 `kUnavailable`），绝不静默钳制 | 与 `perf_quality_` 越界处理同构；1-9、14-15 在 SDK 侧是弃用/保留（静默回退默认），必须挡住 |

### 3.2 修改文件清单

| 文件 | 修改 | 原因 |
|---|---|---|
| `src/xenia/ui/guest_output_enhancement.h` | 新增 `enum class DlssSrRenderPreset : uint8_t { kDefault = 0, kJ = 10, kK = 11, kL = 12, kM = 13 };`；`GuestOutputEnhancementConfig` 增加 `DlssSrRenderPreset render_preset = kDefault;` | 配置面；数值与 SDK 头文件一致；默认 kDefault |
| `src/xenia/ui/guest_output_enhancement.cc` | （若有默认构造/比较逻辑）同步 | 配置一致性 |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.h` | `TryProcess` 增加 `ui::DlssSrRenderPreset render_preset` 参数；`Initialize` 同步增加；新增成员 `ui::DlssSrRenderPreset created_render_preset_;` | 每帧传递 + 重建比较（镜像 `created_perf_quality_`） |
| `src/xenia/ui/dlss/dlss_super_sampling_backend.cc` | ① :626 之后、:778 之前：`render_preset != kDefault` 时 `NVSDK_NGX_Parameter_SetUI(parameter_block_, <按 perf_quality_ 选模式宏>, static_cast<unsigned int>(render_preset))`；② :847-854 重建条件追加 `requested_render_preset != created_render_preset_`；③ Initialize 成功处存 `created_render_preset_` 并记日志；④ 越界检查（不在 {0,10,11,12,13} → `kInputContractFailed`）；⑤ 新增 `RenderPresetName()` 日志辅助 | 接线 + 切换 + 诊断 |
| `src/xenia/ui/d3d12/d3d12_presenter.cc` | `SelectOutputPath` / 惰性创建处把 `config.render_preset` 传入 `TryProcess` | 配置到达后端 |
| `src/xenia/app/emulator_window.cc` | Post-processing 对话框 SR 树节点下新增 preset 选择控件（Default/J/K/L/M，默认 Default），仅 SR 激活时显示；cvar 持久化（镜像质量滑块链路） | UI 面 |
| `src/xenia/ui/testing/guest_output_enhancement_test.cc` | 新增测试用例（§3.4） | 回归保护 |

### 3.3 后端接线细节

**模式宏选择**（按 `perf_quality_`）：

| perf_quality_ | 参数宏 |
|---|---|
| kMaxPerf (0) | `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance` |
| kBalanced (1) | `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced` |
| kMaxQuality (2) | `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality` |
| kUltraPerformance (3) | `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance` |

**设置代码**（Initialize 内，:626 之后）：

```c
// Render preset (create-time, per-mode). kDefault = do not set at all
// (identical to the current verified baseline; the SDK resolves it to
// K/M/L per quality mode). Only explicit values are sent.
if (render_preset != ui::DlssSrRenderPreset::kDefault) {
  const char* mode_param = /* 按 perf_quality_ 选上表宏 */;
  NVSDK_NGX_Parameter_SetUI(parameter_block_, mode_param,
                           static_cast<unsigned int>(render_preset));
}
```

**重建条件**（:847-854 追加一项）：

```c
if (state_ == State::kNotInitialized ||
    input_width != input_width_ || input_height != input_height_ ||
    output_width != output_width_ || output_height != output_height_ ||
    requested_quality != created_perf_quality_ ||
    requested_render_preset != created_render_preset_) {   // 新增
  ...
}
```

**越界检查**（Initialize 开头，镜像质量档）：

```c
if (render_preset != kDefault && render_preset != kJ && render_preset != kK &&
    render_preset != kL && render_preset != kM) {
  // 不在 {0,10,11,12,13}：输入契约失败，缓存 kUnavailable，不钳制
  return BackendResult::kInputContractFailed;
}
```

**同步**：重建发生在 `TryProcess` 内，Presenter 已在 `PaintAndPresentImpl` 开头等待 paint 时间线完成上一帧提交（含上一帧 Evaluate 的命令列表已执行完毕）——满足 §5.5 release 前置条件。该同步已由质量档重建路径运行时验证（M3-C），render preset 重建继承之。`STATIC_VERIFIED` + `SDK-DOC-SUPPORTED`

### 3.4 测试用例（主机侧，无 GPU 依赖）

1. 默认 `render_preset == kDefault`，后端**不**调用 `SetUI`（断言默认路径不设置 UI 参数）。
2. `render_preset` 越界（如 1、9、14）→ `kInputContractFailed`，缓存 `kUnavailable`，不静默钳制。
3. 合法集合 {0,10,11,12,13} 全部接受（不触发契约失败）。
4. `render_preset` 变更（kDefault → kK）触发重建（`created_render_preset_` 更新，走 release+re-create 通路）。
5. `render_preset` 变更与质量档变更**同时**发生只触发一次重建（同一条件块）。
6. 后端不可用（`kUnavailable`）时 `render_preset` 变更不重试 SDK。
7. `render_preset == kDefault` 时，即使质量档变化，仍不调用 `SetUI`（保持 Default 解析）。

### 3.5 UI 控件（镜像质量滑块范式）

- 位置：Post-processing 对话框 → Resampling and sharpening 树 → DLSS 单选下 → SR 复选框下。仅 `d3d12_presenter && master_enabled && sr_enabled` 时绘制。
- 控件：`SliderInt`（0-4 映射到 Default/J/K/L/M）或 5 档单选；默认 Default；label 显示当前档位名。
- 持久化：关闭对话框时 `OVERRIDE_int(dlss_sr_render_preset, ...)`（仅值变化时）；启动时 `GetGuestOutputEnhancementConfigForCvars()` 读入。
- 变更时 Presenter 不主动重建——下一帧 `TryProcess` 检测到差异自建（与质量档同一机制，避免 UI 线程直接动 GPU 资源）。

### 3.6 风险与停止点

- **实施需用户明确授权**（本轮仅设计）。实施后停止，提交 A–G 验收报告，等待用户审核。
- 实机验证（切换 K→L→M 的画面变化与帧时间）推迟至 RTX 4090 阶段（M3-C 补充或 M4 前）。
- 若未来需要每模式独立 preset，扩展 `GuestOutputEnhancementConfig` 为 5 值数组，后端接线点不变——本设计不阻塞该扩展。

---

## B. 修改文件

| 文件 | 修改内容 | 原因 |
|---|---|---|
| `docs/dlss-integration/milestone-3-1-dlss-sr-render-preset-switch-design.md` | 新增（本文档） | 交付物：切换机制调查 + 修改方案 |

**未修改任何代码文件。**

## C. 验证证据

- **C.1 官方手册**（`DLSS_Programming_Guide_Release.pdf` 310.6.0，pypdf）：§3.2.2（p.11-12，preset 生命周期固定原文）、§5.5 Feature Disposal（p.53-54，ReleaseFeature 同步约束原文）、§2.4（p.5，`NVSDK_NGX_UpdateFeature` OTA）、§3.14（p.30，ReleaseFeature 内存缓存复用原文）、附录 §9.1（p.67，「Model Selection API」与「Preset Updates via OTA」两条官方路径）。
- **C.2 头文件**（310.9.1）：`nvsdk_ngx.h:32-64`（API 生命周期注释）、:574（`NVSDK_NGX_D3D12_ReleaseFeature`）、:695（`NVSDK_NGX_UpdateFeature`）；全 include 目录 grep 无 `UpdateParameters` 类 API。`nvsdk_ngx_defs.h:791`（`FreeMemOnReleaseFeature`）。
- **C.3 源码**：`dlss_super_sampling_backend.cc:847-862`（重建条件块）、:610-626（AllocateParameters + 回调 Set）、:763-777（create_params + CallCreateDlss）、:916-917（InPreExposure/InExposureScale）；`dlss_super_sampling_backend.h`（`created_perf_quality_` 与 TryProcess 每帧质量档签名）；Presenter `PaintAndPresentImpl` 开头的时间线等待（架构文档 B6，STATIC_VERIFIED）。
- **C.4 工作树**：`git status --short` 干净（仅本文档未跟踪）；HEAD `fac5037`。

## D. 环境限制

- Hyper-V 无 GPU：切换的实际画面/帧时间未验证（`UNVERIFIED`）。未加载 DLL、未构建。

## E. 剩余风险

- §5.5 同步由现有时间线机制满足，但该机制的运行时正确性仅由质量档/尺寸重建路径间接验证（M3-C）；render preset 重建复用同一通路，风险相同。`UNVERIFIED`（专项未做）
- OTA 非切换机制的结论基于头文件全量核对 + 手册 §2.4/§9.1；若未来 SDK 新增原地更新 API，本设计的重建通路仍有效（只是非最优）。
- 单值全局 preset（D1）限制了「每模式不同 preset」的可能；扩展路径已预留（§3.6）。

## F. 下一步（最小且安全）

- 用户授权后按 §3.2 清单实施（后端 + 配置 + UI + 测试），实施后停止，提交 A–G 验收报告。
- 默认 `kDefault`（完全不 Set）保证零行为变化，实施本身不改变当前已验证基线。
- 实机切换对比（K/L/M 画质与帧时间）推迟至 RTX 4090 阶段。

## G. 各项能力状态清单

- **DLSS SR**：编译期已集成（RTX 4090 运行时已验证 Balanced/Default=K 路径，画质未评估）。Render preset 切换机制：本轮完成调查（官方仅重建一条路）+ 修改方案设计，**未实施**。
- **NVOF**：未实现（M4 已规划）。
- **帧生成**：未实现。
- **实验性神经渲染**：仅研究。
- **Presenter 集成**：已接线且关闭（主开关默认 OFF），回退已验证。
- **最终画质与性能**：本地评估（运行时功能已验证，画质未评估）。
