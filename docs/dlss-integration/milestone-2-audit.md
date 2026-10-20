# Milestone 2 交付审查报告（DLSS SR 最小接口骨架与主机侧测试）

> 审查类型：独立交付审查 + M2 收尾验证
> 审查日期：2026-10-20
> 审查对象：Milestone 2 完整变更集（M1 基线 `0a89c9db58` 之上的未提交工作树）
> 审查方式：只读（源码检查、diff 核对、构建/测试证据复核）；不修改代码、不加载 DLL、不执行 GPU 操作
> 结论：**PASS WITH FINDINGS** → 收尾修正后**正式验收**

---

## A. 总结

Milestone 2 的交付物（最小后端接口骨架、默认关闭配置、独立结果契约、原始路径回退构造器、路径选择函数与主机侧测试）全部实现并通过验证。独立交付审查结论为 `PASS WITH FINDINGS`：唯一阻塞级发现 **F3**（注释措辞把尚未实现的互斥保护描述为已完成行为）已在收尾阶段修正；其余观察项均属 Milestone 3 集成范围，不构成 M2 阻塞。`PaintResult` 可见性调整经审查后保留现状。全量 Release 构建重新执行并通过（`COMPILE_VERIFIED`），主机侧单测全部通过。M2 满足全部原定验收条件，**正式验收**。

## B. 实际修改文件（M2 完整变更集）

以下为本里程碑相对 M1 基线（commit `0a89c9db58`）的全部代码变更，均处于未提交状态，随本次收尾一并提交。

| 文件 | 变更 | 目的 |
| --- | --- | --- |
| `src/xenia/ui/guest_output_enhancement.h` | 新增（202 行） | 定义 `BackendResult` 九状态枚举、`IsOriginalPathFallback`、`GuestOutputEnhancementConfig`（主开关 + SR 子开关，默认全 OFF）、`IDLSSSuperSamplingBackend` 接口、`BuildOriginalPathFallbackFlow`、`SelectOutputPath`。收尾修正了其中线程安全注释（第 84–89 行，见 F3）。 |
| `src/xenia/ui/guest_output_enhancement.cc` | 新增 | 上述函数/构造器的实现（纯主机侧，无 GPU 依赖）。 |
| `src/xenia/ui/presenter.h` | +2 / -1 | 将 `PaintResult` 由 `protected` 移至 `public`（@339），并在其后重新插入 `protected:`（@495），以便后端与测试访问 guest 输出绘制类型。收尾阶段审查后**保留现状**（理由见下）。 |
| `src/xenia/ui/CMakeLists.txt` | +5 | 在 `XENIA_BUILD_TESTS` 下挂载 `testing/` 子目录。 |
| `src/xenia/ui/testing/CMakeLists.txt` | 新增 | 声明 `xenia-ui-tests` 测试目标。 |
| `src/xenia/ui/testing/guest_output_enhancement_test.cc` | 新增 | 15 用例 / 45 断言的主机侧测试。 |
| `docs/dlss-integration/architecture-design.md` | 修改 | 反映 M2 完成状态（头状态行、K2 更新为已完成、审计索引补充本报告）。 |
| `docs/dlss-integration/milestone-2-audit.md` | 新增 | 本报告。 |

### `PaintResult` 可见性审查结论（收尾）

- **现状**：`presenter.h` 将 `PaintResult` 从 `protected` 改为 `public`，并在其后重插 `protected:`。生产代码实际只访问 `GuestOutputProperties`、`GuestOutputPaintEffect`、`GuestOutputPaintFlow`；`PaintResult` 目前**仅**被测试代码引用。
- **替代方案评估**：收窄可见性（保持 `protected`，仅测试可见）技术上可行，但会带来两个代价：(1) 访问限定符碎片化——需为测试单独开一个 `public` 窗口或引入测试友元，破坏类内现有的连续 `public`/`protected` 分段风格；(2) 削弱测试的**编译期独立性证明**——测试直接以 `public` 类型访问后端结果，正是「后端结果契约与 `PaintResult` 正交、可独立引用」这一设计决策（决策 4）的编译期佐证。
- **决策**：全部相关类型均为**无状态值类型**，扩大可见性不引入任何可变状态或生命周期风险。综合项目既有风格与测试独立性价值，**保留现状**，不做收窄重构。此项为低风险、非阻塞。

## C. 验证证据

按证据类型区分。所有硬件相关项在真实 RTX 4090 验证前保持 `UNVERIFIED`。

### C1. 编译（COMPILE_VERIFIED）
- 命令：`python xenia-build.py build --config release`（全量 Release，MSVC `/WX` 严格警告即错误）。
- 结果：`Success!`，0 错误。
- 产物：`xenia_canary.exe`（17,190,400 B，构建时间戳 19:11:40）、`xenia-ui.lib`（19:04:41）。
- 结论：当前工作树代码可完整编译，标记 `COMPILE_VERIFIED`。

### C2. 测试（主机侧）
- 命令：`python xenia-build.py build --config release --build-tests --target xenia-ui-tests`。
- 结果：`All tests passed (45 assertions in 15 test cases)`。
- 横幅：`dev/dlss5-integration@0a89c9d`（与当前基线一致）。
- 覆盖：`BackendResult` 九状态语义、`IsOriginalPathFallback` 判定、`GuestOutputEnhancementConfig` 默认全 OFF 且「有效 SR = 主开关 && SR 子开关」、`BuildOriginalPathFallbackFlow` 的 `effect_count = 1` + `effects[0] = kBilinear`（并断言 `effect_count` 绝不为 0）、`SelectOutputPath` 路径选择逻辑。测试直接调用**生产路径选择逻辑**（`SelectOutputPath` / 回退构造器），非 Mock 替身。

### C3. 静态检查
- 采用严格 MSVC `/WX`（警告即错误），无独立 lint/clang-tidy。编译期静态约束已通过。

### C4. 运行时验证
- **未执行**。Hyper-V 环境无 NVIDIA 驱动、无真实 GPU。零 DLL 加载、零 GPU 资源创建、零真实 DLSS/NVOF 调用。全部硬件相关项 `UNVERIFIED`。

## D. 环境限制

- 开发机运行于 Hyper-V，**无法安装 NVIDIA 驱动、无法执行真实 GPU/NVOF/DLSS 渲染**。
- 本里程碑仅在主机侧完成：源码检查、编译、基于值类型的单元测试、静态检查。
- 真实 RTX 4090 上的初始化、后端 Evaluate、输出有效性、回退正确性均**未验证**，待 Milestone 3 在目标硬件上完成。

## E. 剩余风险

1. **Presenter 接线（M3）**：`SelectOutputPath` 目前无生产调用者；与 `PaintAndPresentImpl` 的接线、以及后端 `TryProcess` 的实际调用，均在 M3 完成。在此之前，默认关闭路径不触及后端。
2. **资源生命周期 / Fence / 队列同步（M3）**：`GuestOutputEnhancementConfig` 不含 GPU 资源；真实的 D3D12 资源状态迁移、Fence 依赖与队列同步需在 M3 后端接入时实现并验证，不得提前接管 `guest_output_resource_paint_refs`。
3. **线程安全（M3 要求）**：`guest_output_enhancement.h:84-89` 已明确——M2 阶段该结构为纯值类型，**不存在**互斥保护；接入 Presenter 后**必须**复用现有 `guest_output_paint_config_mutex_` 模式。这是 M3 的**要求**，不是已实现行为。
4. **`IsAvailable()` 未使用**：接口已定义但无调用者，M3 后端接入时启用。
5. **回退构造器为最小实现**：`BuildOriginalPathFallbackFlow` 仅保证 `effect_count = 1` + `kBilinear` 契约；完整回退语义（失败分类记录）在 M3 完善。
6. **提交状态**：M2 变更此前一直未提交；本次提交解决该问题。
7. **许可证**：本里程碑**未引入**任何 NVIDIA SDK / DLL；集成代码在许可证问题澄清前不提交至公开 Xenia 仓库（决策 6）。M3 接入前需确认 SDK 许可条件。

## F. M2 验收判定

对照 M2 原定验收条件逐项核对：

- [x] 构建通过 → 全量 Release 构建 `Success!`，`COMPILE_VERIFIED`。
- [x] 主开关默认关闭 → `GuestOutputEnhancementConfig` 主开关与全部子开关默认 OFF（测试断言）。
- [x] 单测验证回退到原始效果链（`effect_count = 1` + `kBilinear`），`effect_count = 0` 不用于回退 → 测试直接断言。
- [x] 零 GPU 依赖、零 DLL 加载 → 纯主机侧代码，无 SDK / DLL / GPU 资源。

**结论**：M2 满足全部验收条件，**正式验收**。阻塞级发现 F3 已修正；非阻塞观察项均属 M3 范围。可进入 Milestone 3（需用户明确指示）。

## G. 各项能力状态清单

- **DLSS SR**：编译期已集成（接口骨架 + 回退 + 路径选择 + 主机侧测试）；运行时 / 输出未验证（`UNVERIFIED`）。
- **NVOF**：未实现（仅默认关闭的子开关占位）。
- **帧生成（FG）**：未实现（仅默认关闭的子开关占位）。
- **实验性神经渲染（NR）**：未实现 / 仅研究（不在 M2 范围）。
- **Presenter 集成**：尚未开始（接口与契约已就绪，`SelectOutputPath` 待 M3 接线）。
- **最终画质与性能**：尚未评估（待目标硬件 RTX 4090 验证）。

---

## 附：交付物符号索引

- `BackendResult`：九状态枚举，与 `PaintResult` 正交。
- `IsOriginalPathFallback(BackendResult)`：`result != kSuccess` 即为回退。
- `GuestOutputEnhancementConfig`：旁挂配置（主开关 + SR 子开关，默认全 OFF；有效 SR = 主 && SR）。
- `IDLSSSuperSamplingBackend`：`IsAvailable` / `TryProcess` / `Shutdown`。
- `BuildOriginalPathFallbackFlow`：`effect_count = 1`、`effects[0] = kBilinear`，严禁 `effect_count = 0`。
- `SelectOutputPath`：路径选择函数（M3 接线）。
- F3 注释：`src/xenia/ui/guest_output_enhancement.h:84-89`。
