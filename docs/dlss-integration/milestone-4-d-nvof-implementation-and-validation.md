 # Milestone 4-D — NVOF 独立实现、MV / 深度数据验证与可行性验收

 状态：**PASS WITH FINDINGS**（主机侧编译期集成 + 宿主侧测试全部通过；真实 GPU 执行 UNVERIFIED，见 D/E 节）。

 ## A. 总结

 本里程碑独立实现了 NVIDIA Optical Flow（NVOF）后端模块（`src/xenia/ui/nvof/`），将其作为与 DLSS SR 完全正交的独立模块（AGENTS.md 独立性原则）。模块封装 NVOF SDK 5.0.7（`nvofapi64.dll`），实现 ABGR8 帧池、原始网格光流/代价纹理、全分辨率 MV/置信度输出、输入/输出 fence、私有描述符堆 + 根签名、ABGR8 转换与 densify 计算着色器，以及 SEH 边界包裹的全部 SDK 入口点。

 本次会话在前序代理已写完全部 NVOF 源码、构建失败的基础上，逐类修复了编译与链接错误，并修复了两处测试缺陷，使整树编译通过、宿主侧测试全绿。真实 GPU 执行（NVOF 运行、光流输出有效性、MV/深度真实数据）在本开发环境（Hyper-V，无 NVIDIA 驱动）无法执行，按红线第 3 条标记为 UNVERIFIED，不得宣称完整验收通过。

 范围遵守：仅修改独立 NVOF 模块、最小化 D3D12 集成点、MV/深度测试组件、独立测试代码；**未**将 NVOF 接入 `d3d12_presenter.cc` 上游渲染链路，**未**集成 DLSS SR / 帧生成，**未**修改上游渲染流程。

 ## B. 修改文件

 本会话（在“全部 NVOF 源码由前序代理编写”的基础上）修复了以下编译/链接/测试错误：

 - `src/xenia/ui/nvof/nvof_optical_flow_backend.cc`
   - 根签名改用本 SDK 的 `D3D12_ROOT_PARAMETER` 经典布局：`D3D12_ROOT_PARAMETER_TYPE_UAV` + `.Descriptor.*`（无 `.UnorderedAccessView`）、`.Constants.Num32BitValues`（非 `Num32BitValuesToSet`）、SRV 表用 `D3D12_DESCRIPTOR_RANGE` + `.DescriptorTable.pDescriptorRanges/.NumDescriptorRanges`，并为每个根参数补顶层 `.ShaderVisibility`。
   - 计算 PSO 改用 `pRootSignature`（非 `pCSRootSignature`）与 `D3D12_SHADER_BYTECODE CS`（直接 `CS.pShaderBytecode/BytecodeLength`，本 SDK 无 `D3D12_COMPUTE_SHADER_DESC` typedef）。
   - 描述符增量尺寸改用 `ID3D12Device::GetDescriptorHandleIncrementSize`（本 SDK 的 `ID3D12DescriptorHeap` 无 `GetDescriptorIncrementSize`）。
   - densify 前的输出 fence 等待改用 CPU 侧 `SetEventOnCompletion`+`WaitForSingleObject`（本 SDK 的 `ID3D12GraphicsCommandList`/`ID3D12Fence` 均无 `Wait`），并记录设计变更说明。
   - `NvOFAPICreateInstanceD3D12` 改为 `GetProcAddress` 解析后经函数指针调用（模块不链接任何 NVIDIA import library，保持“off 路径零 DLL 加载”契约）。
 - `src/xenia/ui/nvof/CMakeLists.txt`
   - `d3d12` 作为 `xenia-nvof` 的 PUBLIC 链接依赖（模块公共接口使用 D3D12 类型并直接调用 `D3D12SerializeRootSignature`）。
 - `src/xenia/ui/testing/nvof_backend_test.cc`
   - Catch2 `CHECK(a == X || a == Y)` 分解为先算 bool 再 `CHECK`（本版本 Catch2 断言内不支持 `operator||`）。
   - `FlowValidators.MagnitudeMatches` / `MvContract.PointsCurrentToPrevious` 的受控输入缓冲由 32×32 改为 64×64（校验器排除 16px 边缘余量，32×32 内部区为空被拒），并修正填充索引 `y*64+x`。

 保留的前序代理/用户既有改动（未回退）：`guest_output_enhancement.h`（`IOpticalFlowBackend` 接口 + `optical_flow_enabled` 配置）、`ui/CMakeLists.txt` 与 `testing/CMakeLists.txt` 的 NVOF 接线、`architecture-design.md`、`milestone-4-b/c-*.md`。

 ## C. 验证证据

 构建命令（`V:\DLSS5_Research\xenia-canary`）：`.\xb build --config release` → **Success!**（日志 `build_m4d_fix6.log` / `build_m4d_fix7.log`）。

 测试命令：`.\xenia-ui-tests.exe` → **All tests passed (1175 assertions in 32 test cases)**（日志 `test_m4d_run2.log`）。

 宿主侧已验证（CPU，无 GPU）：
 - 接口契约：null `command_list`/`input_frame` → 确定性输入契约失败（日志 `input contract failed: null command_list or input_frame`）。
 - 运行时缺失回退：本箱无 `nvofapi64.dll` → `LoadLibraryExW nvofapi64.dll failed (error=126); no NVIDIA driver present. Caching as unavailable` → 确定性 `kUnavailable`（绝不标记成功）。
 - 销毁顺序：`backend shutdown` 干净执行（`UnregisterResources` → 释放后端纹理 → `DestroySession`）。
 - 组件数值校验：`GridFlowDecoder`（S10.5→像素 /32）、`CpuDensify`（negate/cost 阈值语义）、`FlowValidators`（IsValidFlow/IsValidConfidence/MagnitudeMatches）、`DepthValidators`（全零深度契约）、`MvContract`（current→previous 方向）、`FlowVisualizer`（BMP 导出）。

 静态/编译期：整树 `/WX`（警告即错误）通过；SDK 头仅由 `nvof/*` 翻译单元包含（许可隔离）；无 NVIDIA import library 链接。

 ## D. 环境限制（未执行的 GPU 测试）

 测试机：Windows 11 专业工作站，Hyper-V（`HypervisorPresent=True`），视频适配器仅 Microsoft Hyper-V Video / Remote Display Adapter，**无 NVIDIA GPU/驱动**，`C:\Windows\System32\nvofapi64.dll` 不存在。

 因此以下项在本环境**不可执行**，标记 UNVERIFIED：真实 GPU 初始化、`nvOFExecuteD3D12` 真实执行、光流输出数值/可视化验证、全分辨率 MV/置信度真实数据有效性、深度真实数据、真实硬件上的失败回退（设备丢失/同步超时/输出无效）。真实 GPU 执行测试在本箱**自跳过**（预期，非失败）。

 SDK/构建环境（可重复）：NVOF SDK 5.0.7（`C:\SDKs\NVIDIA\Optical_Flow_SDK_5.0.7`）、Windows SDK 10.0.26100.0、MSVC 14.51.36231（VS 18 Enterprise）、`--config release`、`-std:c++20 /WX /arch:AVX /GL /LTCG`。

 ## E. 剩余风险

 - **NVOF 许可未审计**：模块为许可隔离（可整体删除），但 SDK 许可是否允许分发/再分发尚未法务结论。
 - **非标准 Windows SDK D3D12 头**：本环境 `d3d12.h` 缺少标准 `ID3D12GraphicsCommandList::Wait`、`ID3D12DescriptorHeap::GetDescriptorIncrementSize`、`D3D12_COMPUTE_SHADER_DESC`，根签名为经典布局。已按本 SDK 适配；若目标硬件使用标准 SDK，CPU 侧等待应改为命令队列 `Wait(output_fence)`（GPU 侧重排），`GetDescriptorHandleIncrementSize` 与标准 `GetDescriptorIncrementSize` 等价。这是 ABI/头差异风险。
 - **CPU 侧等待的性能代价**：densify 前的输出 fence 等待在本 SDK 下为 CPU 阻塞（`SetEventOnCompletion`+`WaitForSingleObject`），将 NVOF execute 与 densify 记录串行化（无 GPU 重叠）。正确（无竞态）但非最优；真实硬件上应评估命令队列等待。
 - **ABGR8 10→8 位截断**：R10G10B10A2→B8G8R8A8 转换有 2 位截断，光流精度损失待真实硬件量化。
 - **fence 耦合**：输入 fence（调用者信号到 frame_index）与输出 fence（NVOF 信号）的链式依赖需真实硬件验证时序。
 - **估算 MV vs 原生 MV 语义（红线 3）**：NVOF 估算光流与游戏原生运动矢量语义不同，不得冒充原生 MV；本里程碑仅暴露供验证，未接入消费端。
 - **销毁段错误防护**：`UnregisterResources` 必须先于后端纹理释放、`DestroySession` 最后（Veyra 已验证），真实硬件需确认无段错误。

 ## F. 下一步（最小且安全）

 在受支持的 RTX 4090 硬件上运行真实 GPU 测试台（`nvofapi64.dll` + NVIDIA 驱动），完成：真实 `nvOFExecuteD3D12` 执行、受控 +8px 位移输入的光流输出数值/可视化验证、全分辨率 MV/置信度与深度数据有效性、真实失败回退（设备丢失/同步超时）。通过后再进入 M4-E（SR A/B 对比）；在此之前不接入上游渲染、不集成 SR/FG、不启动下一里程碑。

 ## G. 各项能力状态清单

 - DLSS SR：运行时已验证（Milestone 3-B，前序）。
 - NVOF：编译期已集成；执行 UNVERIFIED（待 RTX 4090 真实 GPU）。
 - 帧生成：未实现。
 - 实验性神经渲染：仅研究。
 - Presenter 集成：尚未接线（本里程碑未接入 `d3d12_presenter.cc` / `PaintAndPresentImpl`）。
 - 最终画质与性能：尚未评估。

 已知限制与运行日志：见 C/D 节（`build_m4d_fix6.log`、`test_m4d_run2.log`、`test_m4d_run.log`）。真实 GPU 执行未通过/未验证，按红线不得宣称完整验收通过。
