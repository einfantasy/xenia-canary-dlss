# M3-B SR 修复方案 — 零初始化上传缓冲提升为成员（upload-buffer lifetime）

- **状态**：已实施、已实机验证（RTX 4090）、已提交。修复提交 `59d4fa9`；配套诊断日志提交 `6129fea`（见 B3）。
- **修复对象**：DLSS SR 初始化成功、但**第一帧处理结束后 D3D12 设备被移除**（`0x887A0006`，`DXGI_ERROR_DEVICE_REMOVED`）——首帧 `Evaluate` 路径上的设备丢失。
- **交叉对比结论**：本项目与 `Veyra-NRVideo` 在 first frame 处理前后的实现方式上，**唯一偏离**就是该上传缓冲的生命周期——Veyra 将等价缓冲存为**类成员**保活，本项目误写成 `Initialize()` 的**局部变量**。其余（barrier、格式、尺寸、SDK 调用序列）与参考一致。
- **总体状态**：**PASS**（修复已实施并在目标硬件验证通过）。

---

## A. 总结

`DLSSSuperSamplingBackend::Initialize()` 在录制零初始化命令时，用一个 `D3D12_HEAP_TYPE_UPLOAD` 缓冲 `up_buf`（CPU 侧 `memset 0`）作为 `CopyTextureRegion` 的源，把零数据拷贝进 depth（`R32_FLOAT`）和 motion-vector（`R16G16_FLOAT`）占位纹理。这个 `CopyTextureRegion` 是**GPU 命令**，录制在 Presenter 的首帧命令列表里，**在第一帧末尾才执行**——即 `Initialize()` 早已返回之后。

`up_buf` 是 `Initialize()` 的**局部** `ComPtr<ID3D12Resource>`：函数返回时 `ComPtr` 析构，上传缓冲即被释放。GPU 随后读到**已释放的显存**，触发 GPU fault，D3D12 设备被移除（`0x887A0006`）。这是典型的「CPU 侧资源生命周期短于 GPU 命令执行时刻」错误，与 GPU、驱动、SDK 无关，在任何真实硬件上必然复现。

交叉对比 `Veyra-NRVideo`：其 `EnhanceGraph` 的零初始化上传缓冲 `upDepth_` / `upZeroDepth_` / `upZeroMotion_`（及 FSR 变体）全部是**类成员** `ComPtr<ID3D12Resource>`（`include/veyra/pipeline/EnhanceGraph.h:809-811, 1014`），初始化时创建（`src/pipeline/EnhanceGraph.cpp:700-702`），`uploadTex()` 用成员缓冲做 `CopyTextureRegion`（`EnhanceGraph.cpp:915, 940-942`），**析构时才 `Reset()`**（`EnhanceGraph.cpp:3692-3693`）。本项目把同样的缓冲写成局部变量，是两者间**唯一**的结构性偏离。

修复：把 `up_buf` 提升为成员 `upload_buffer_`，随后端存活，`Shutdown()` 中显式 `Reset()`。无 barrier、格式、尺寸或 SDK 调用序列的任何变化。

---

## B. 逐文件改动

### B1. `src/xenia/ui/dlss/dlss_super_sampling_backend.h`（新增成员）

成员区（第 157 行附近，`depth_texture_` / `motion_vectors_texture_` 之后）新增：

```cpp
  // One-time CPU upload buffer used to zero-fill depth_texture_ /
  // motion_vectors_texture_. Must stay alive for the backend's lifetime: the
  // CopyTextureRegion that reads it is a GPU command executed at the end of the
  // FIRST frame, AFTER Initialize() returns. A local would be released too
  // early and fault the GPU (device removed 0x887A0006). Mirrors Veyra's
  // member upload buffers (upDepth_/upZeroDepth_/upZeroMotion_).
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer_;
```

### B2. `src/xenia/ui/dlss/dlss_super_sampling_backend.cc`（局部 → 成员 + Shutdown 清理）

- `Initialize()` 零初始化段（第 575-632 行）：
  - 删局部声明 `Microsoft::WRL::ComPtr<ID3D12Resource> up_buf;`；
  - 创建/`Map`/`memset`/`Unmap` 全部改用成员 `upload_buffer_`（第 599 / 605 / 611 行）；
  - `zero_copy` lambda 的拷贝源改用 `upload_buffer_.Get()`（第 632 行）；
  - 段首加注释说明为何必须是成员（第 578-581 行）。
- `Shutdown()`（第 282 行）：在 `output_texture_.Reset()` 之后新增 `upload_buffer_.Reset();`。
- 其余（`CreateFeature` 参数、SDK 调用、失败路径缓存 `kUnavailable`）**未动**。

### B3. `src/xenia/gpu/d3d12/d3d12_command_processor.cc`（诊断日志，独立提交 `6129fea`）

- `BeginSubmission` 设备健康检查（第 3719-3726 行）：`GetDeviceRemovedReason()` 失败后、折叠成 bool 之前，新增 `XELOGE` 记录原始 HRESULT（`0x887A0006` 等），区分真实 GPU fault 与 TDR/挂起。纯诊断，无行为变化。

---

## C. 所有权与生命周期契约

- `upload_buffer_` 由后端独占拥有（成员 `ComPtr`），随后端存活；`Shutdown()` 显式 `Reset()`，与 `output_texture_` 同处清理。
- 该缓冲是一次性（one-shot）的：零初始化命令只录制一次，首帧执行完毕后缓冲不再被任何 GPU 命令引用；保留到后端析构只为安全，无额外 GPU 侧副作用（尺寸 = `ceil(input_width * 4 / 256) * input_height` 字节，极小）。
- 不接管 `guest_output_resource_paint_refs`，不改变 Xenia 资源生命周期、队列同步或 Fence 依赖。

---

## D. 错误处理 / 边界

- 上传缓冲创建失败 / `Map` 失败：与修复前相同的失败路径——`XELOGE` + `state_ = State::kUnavailable` + 返回 `kInitFailed`，Presenter 回退 `kOriginalPath`。失败语义未变。
- 1:1 bypass、尺寸契约、`kUnavailable` 缓存不重试等既有边界均未触碰。

---

## E. 许可证隔离（不变）

修复不涉及 SDK 调用序列、头文件或第三方代码的任何变化；SDK 仍在受控隔离区内。许可证状态不变（§4e 仍 `LICENSE REVIEW REQUIRED`）。

---

## F. 验证计划 / 验证结果

- **验证环境**：物理 NVIDIA RTX 4090（测试机），**非** Hyper-V。
- **验证结果**：用户确认修复后**正常工作**——首帧 `Evaluate` 后设备**不再被移除**，无 `0x887A0006`，DLSS SR 按设计运行。
- **回归面**：`Shutdown()` 新增一行 `Reset()`，无其它行为改动；原路径（FSR / 原始 paint）不受影响。
- **本地限制**：Hyper-V 无 NVIDIA 驱动，本地无法复现/验证 GPU 执行；上述结论全部来自 RTX 4090 实机。

---

## G. 影响面 / 爆炸半径

- 仅 `dlss_super_sampling_backend.{h,cc}` 的上传缓冲生命周期 + `Shutdown()` 一行清理 + 一条诊断日志。
- 无 ABI 变化（后端为内部类，无对外 ABI 契约）；无 barrier、格式、尺寸、SDK 调用变化。
- 爆炸半径限于 DLSS SR 后端自身；Presenter、共享内存、GPU 提交路径均未被触碰（除一条诊断日志）。

---

## H. A–G 交付报告（按 AGENTS.md §7）

### A. 总结
M3-B 首帧设备丢失的根因（局部上传缓冲生命周期）已修复并在 RTX 4090 验证通过。修复提交 `59d4fa9`，诊断日志提交 `6129fea`。总体状态：**PASS**。

### B. 修改文件
- `src/xenia/ui/dlss/dlss_super_sampling_backend.h`：新增成员 `upload_buffer_`（第 157 行）。
- `src/xenia/ui/dlss/dlss_super_sampling_backend.cc`：`Initialize()` 局部 `up_buf` → 成员 `upload_buffer_`（第 578-632 行）；`Shutdown()` 新增 `upload_buffer_.Reset()`（第 282 行）。
- `src/xenia/gpu/d3d12/d3d12_command_processor.cc`：`BeginSubmission` 新增设备移除原因诊断日志（第 3725-3726 行）。

### C. 验证证据
- RTX 4090 实机：首帧 `Evaluate` 后设备不再被移除，无 `0x887A0006`，DLSS SR 正常运行（用户确认）。
- 交叉对比 Veyra-NRVideo：`upDepth_`/`upZeroDepth_`/`upZeroMotion_` 为类成员（`EnhanceGraph.h:809-811`），析构才 `Reset`（`EnhanceGraph.cpp:3692-3693`）——本项目修复后与其一致。
- 本轮为纯生命周期修复，未重新编译验证以外的其它项。

### D. 环境限制
本机 Hyper-V 无 NVIDIA 驱动，无本地 GPU 运行证据；全部运行时结论来自 RTX 4090 测试机。

### E. 剩余风险
- C3 画质隐患（零 depth 线性、零 MV 限制时序降噪、输入在色调映射后、曝光策略）仍 `UNVERIFIED`。
- 许可证 §4e 仍 `LICENSE REVIEW REQUIRED`。
- 诊断日志仅在设备移除失败路径触发，无性能影响。

### F. 下一步
最小且安全的下一步：C3 画质调查（零 depth / 零 MV / 色调映射后输入 / 曝光策略），在独立轮次处理。

### G. 各项能力状态清单
- DLSS SR：**运行时已验证**（首帧设备丢失已修复）；画质未评估
- NVOF：未实现
- 帧生成：未实现
- 实验性神经渲染：仅研究
- Presenter 集成：已接线且回退已验证
- 最终画质与性能：尚未评估（待 RTX 4090 实机）
