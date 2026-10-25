/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_NVOF_NVOF_OPTICAL_FLOW_BACKEND_H_
#define XENIA_UI_NVOF_NVOF_OPTICAL_FLOW_BACKEND_H_

#include <cstdint>

// Must come BEFORE <wrl/client.h>: guest_output_enhancement.h -> presenter.h
// pulls in math.h (which breaks if min/max macros are defined) and later sets
// NOMINMAX before windows.h is reached.
#include "xenia/ui/guest_output_enhancement.h"

#include <wrl/client.h>

// Forward declarations. The D3D12 types are used only as pointers in this
// header; the full d3d12.h is included in the .cc. The NvOFHandle_st /
// NvOFGPUBufferHandle_st types are SDK handles (opaque pointers) forward-
// declared here so that the SDK headers are NOT pulled into this header
// (license isolation: the NVOF SDK dependency is confined to the nvof/*
// module).
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12Resource;
struct ID3D12Fence;
struct ID3D12DescriptorHeap;
struct ID3D12RootSignature;
struct ID3D12PipelineState;
struct NvOFHandle_st;
struct NvOFGPUBufferHandle_st;

namespace xe {
namespace ui {
namespace nvof {

// A real NVIDIA Optical Flow (NVOF) backend that wraps the NVIDIA Optical Flow
// SDK (nvofapi64.dll, SDK 5.0.7). It is an independent module, orthogonal to
// the DLSS SR backend (AGENTS.md independence principle).
//
// This module is the ONLY place in the codebase that depends on the NVOF SDK
// (license isolation). The SDK headers are included only in the .cc, and the
// runtime DLL (nvofapi64.dll, shipped with the NVIDIA driver) is resolved at
// runtime (NOT bundled with the build). If the NVOF license is ultimately ruled
// incompatible with distribution, this entire nvof/* module can be deleted with
// zero changes to the rest of the codebase.
//
// Ownership contract:
//   - The backend OWNS the NVOF SDK session (handle + function table + DLL),
//     its ABGR8 frame pool (two B8G8R8A8 textures, round-robin), the raw grid
//     flow (R16G16_SINT) and cost (R8_UINT) textures, the full-resolution MV
//     (R16G16_FLOAT) and confidence (R8_UNORM) outputs, its input/output
//     fences, its private view heap + root signature, its compiled shaders,
//     and its zero-fill upload buffer.
//   - The backend BORROWS the input frame (R10G10B10A2, read-only). It never
//     takes over guest_output_resource_paint_refs and never destroys the input.
//   - All SDK entry points (CreateInstance / CreateOpticalFlow / GetCaps / Init
//     / RegisterResource / Execute / UnregisterResource / Destroy) are wrapped
//     in SEH boundaries, because the SDK can crash (access violation), not just
//     return an error code. A crash is treated as a failure (deterministic
//     fallback), never as success.
//
// Lazy initialization: the first TryProcess call creates the SDK session
// (DLL load + CreateInstance + CreateOpticalFlow + GetCaps + Init + resource
// registration, with the first frame's commands recorded into the caller's
// command list). If that first creation fails, the backend is cached as
// unavailable (kUnavailable) and subsequent frames do not retry the SDK.
//
// Command submission semantics: the command list is a recording buffer that is
// executed once (ExecuteCommandLists) at the end of the frame. On failure the
// backend does NOT reset the command list itself - that is the caller's
// responsibility (mirrors the DLSS SR backend contract).
//
// Frame flow (all recorded into the caller's command list, in order):
//   1. ABGR8 conversion pass: input frame (R10G10B10A2) -> ABGR8 current.
//   2. (First frame only) zero-fill the ABGR8 previous frame so the NVOF
//      reference is valid (avoids reading uninitialized GPU memory).
//   3. NvOFExecuteD3D12: input = ABGR8 current, reference = ABGR8 previous.
//      Vectors point current -> previous (verified by a controlled +8px
//      displacement test on the target hardware).
//   4. Densify pass: raw grid flow (R16G16_SINT, S10.5) + cost (R8_UINT) ->
//      full-resolution MV (R16G16_FLOAT, pixel units) + confidence (R8_UNORM).
// The raw grid flow/cost and the full-resolution outputs are owned by the
// backend and exposed for validation (read-back).
class NVOFOpticalFlowBackend final : public IOpticalFlowBackend {
 public:
  // `device` is the D3D12 device (the module creates its resources and fences
  // from it). `input_width` / `input_height` are the guest output dimensions
  // (the NVOF input extent). `grid_size` is the requested output grid size
  // (validated against the driver capability list; fail-closed if unsupported).
  // `quality` is 0 = performance (FAST), 1 = balanced (MEDIUM), 2 = quality
  // (SLOW); any other value is an input-contract failure (never clamped).
  NVOFOpticalFlowBackend(ID3D12Device* device, uint32_t input_width,
                         uint32_t input_height, uint32_t grid_size,
                         uint32_t quality);
  ~NVOFOpticalFlowBackend() override;

  NVOFOpticalFlowBackend(const NVOFOpticalFlowBackend&) = delete;
  NVOFOpticalFlowBackend& operator=(const NVOFOpticalFlowBackend&) = delete;

  bool IsAvailable() const override;
  bool IsUnavailable() const override;

  BackendResult TryProcess(ID3D12GraphicsCommandList* command_list,
                           ID3D12CommandQueue* command_queue,
                           ID3D12Resource* input_frame,
                           uint32_t frame_index) override;

  ID3D12Resource* MotionVectorOutput() const override;
  ID3D12Resource* ConfidenceOutput() const override;
  ID3D12Resource* GridFlowOutput() const override;
  ID3D12Resource* GridCostOutput() const override;
  uint32_t OutputWidth() const override;
  uint32_t OutputHeight() const override;
  uint32_t GridWidth() const override;
  uint32_t GridHeight() const override;
  bool HasValidFlow() const override;
  ID3D12Fence* GetInputFence() const override;

  void Shutdown() override;

 private:
  enum class State { kNotInitialized, kAvailable, kUnavailable };

  // Creates the SDK session and all backend-owned resources. Records the first
  // frame's commands (conversion + zero-fill + execute + densify) into
  // `command_list`. Returns kSuccess, or a failure (kUnavailable when the
  // capability check / init / registration fails; kInitFailed for other init
  // errors). Fail-closed: any caps mismatch is an input-contract failure, never
  // a silent fallback.
  BackendResult Initialize(ID3D12GraphicsCommandList* command_list,
                           uint32_t frame_index);
  // Unregisters every registered resource from the NVOF session (reverse
  // order). The session MUST still be alive when this runs. Called by
  // Shutdown() BEFORE the backend-owned textures are released (releasing an
  // NVOF-registered ID3D12Resource after nvOFDestroy + FreeLibrary segfaults).
  void UnregisterResources();
  // Destroys the NVOF session: nvOFDestroy, then delete the function table,
  // then FreeLibrary. Called by Shutdown() AFTER UnregisterResources() and
  // AFTER the backend-owned textures are released.
  void DestroySession();

  ID3D12Device* device_;
  uint32_t input_width_;
  uint32_t input_height_;
  uint32_t grid_size_;
  uint32_t quality_;

  State state_ = State::kNotInitialized;
  // The grid extent of the raw NVOF flow (ceil(width / grid) x ceil(height / grid)).
  uint32_t grid_width_ = 0;
  uint32_t grid_height_ = 0;

  // True after a successful TryProcess with a valid flow (frame_index > 0 and no
  // failure). The first frame (frame_index == 0) always has an invalid flow
  // (no valid reference).
  bool has_valid_flow_ = false;

  // Backend-owned ABGR8 frame pool (round-robin, two textures). The NVOF input
  // extent. The current frame is written by the conversion pass; the previous
  // frame is the reference. Both start in UAV state (the conversion writes them
  // as unordered access).
  Microsoft::WRL::ComPtr<ID3D12Resource> frame_pool_[2];
  // Backend-owned raw grid flow (R16G16_SINT, S10.5) and cost (R8_UINT), at the
  // grid extent. The NVOF execute writes these every frame.
  Microsoft::WRL::ComPtr<ID3D12Resource> grid_flow_texture_;
  Microsoft::WRL::ComPtr<ID3D12Resource> grid_cost_texture_;
  // Backend-owned full-resolution MV (R16G16_FLOAT, pixel units) and confidence
  // (R8_UNORM, [0,1]) outputs, at the input extent. The densify pass writes
  // these every frame. The SR backend would consume MotionVectorOutput() (this
  // milestone only exposes them for validation).
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_vector_texture_;
  Microsoft::WRL::ComPtr<ID3D12Resource> confidence_texture_;

  // Backend-owned zero-fill upload buffer (CPU-visible, filled with zeros).
  // Used to zero-fill the ABGR8 previous frame on the first frame (so the NVOF
  // reference is valid). Must stay alive for the backend's lifetime: the
  // CopyTextureRegion that reads it is a GPU command executed at the end of the
  // FIRST frame, AFTER Initialize() returns.
  Microsoft::WRL::ComPtr<ID3D12Resource> zero_upload_buffer_;

  // Backend-owned fences. input_fence_ is signaled by the caller to
  // frame_index after the input frame is GPU-ready; the NVOF session waits on
  // it before running. output_fence_ is set by the NVOF session when the flow
  // is complete (an internal synchronization observed by the main command
  // list; the caller does not wait on it directly).
  Microsoft::WRL::ComPtr<ID3D12Fence> input_fence_;
  Microsoft::WRL::ComPtr<ID3D12Fence> output_fence_;
  // The next value to set on output_fence_ (starts at 1, increments per frame).
  uint64_t next_output_value_ = 1;

  // Backend-owned private view heap (CBV_SRV_UAV) and root signature, used by
  // the conversion and densify passes. The module does NOT add views to the
  // presenter's paint view heap (minimal-intrusion design).
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> view_heap_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> conversion_pso_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> densify_pso_;

  // Form-B orchestration (Milestone 4-E): the ABGR8 conversion pass is
  // recorded into a backend-owned command list (conversion_command_list_)
  // that is executed on the direct queue BEFORE the NVOF execute is
  // submitted to the OF engine. The NVOF execute is asynchronous (it submits
  // to its own hardware queue at call time, not into the caller's command
  // list), so the conversion pass must have run before the execute reads the
  // ABGR8 current frame. The input fence is signaled by the conversion pass
  // (command_list->Signal), and the OF engine waits on it. The densify pass is
  // recorded into the caller's command list (executed at the end, after the
  // SR block). See the design doc (milestone-4-e).
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> conversion_command_allocator_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> conversion_command_list_;

  // SDK-owned (allocated by the SDK, released via the SDK). The DLL module
  // handle, the function table, the session handle, and the registered buffer
  // handles.
  void* dll_ = nullptr;
  void* fn_list_ = nullptr;  // NV_OF_D3D12_API_FUNCTION_LIST (SDK type).
  NvOFHandle_st* of_handle_ = nullptr;
  NvOFGPUBufferHandle_st* h_frame_pool_[2] = {nullptr, nullptr};
  NvOFGPUBufferHandle_st* h_grid_flow_ = nullptr;
  NvOFGPUBufferHandle_st* h_grid_cost_ = nullptr;
};

}  // namespace nvof
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_NVOF_NVOF_OPTICAL_FLOW_BACKEND_H_
