/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_FG_FG_FRAME_GENERATION_BACKEND_H_
#define XENIA_UI_FG_FG_FRAME_GENERATION_BACKEND_H_

#include <cstdint>
#include <string>
#include <vector>

// Must come BEFORE any Windows header: guest_output_enhancement.h ->
// presenter.h pulls in math.h (which breaks if min/max macros are defined)
// and later sets NOMINMAX before windows.h is reached.
#include "xenia/ui/guest_output_enhancement.h"
#include "xenia/ui/dlss/ngx/ngx_core_host.h"

// Forward declarations. The D3D12 types are used only as pointers in this
// header; the full d3d12.h is included in the .cc. The NVSDK_NGX_* types are
// SDK handles (opaque pointers) forward-declared here so that the SDK headers
// are NOT pulled into this header (license isolation: the FG SDK dependency is
// confined to the fg/* module).
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace xe {
namespace ui {
namespace fg {

// A DLSS Frame Generation (FG / DLSSG) backend that wraps the NVIDIA NGX SDK.
//
// This module is the ONLY place in the codebase that depends on the NVIDIA FG
// (DLSSG) SDK surface (license isolation). The FG SDK headers (nvsdk_ngx_defs_dlssg.h,
// nvsdk_ngx_params_dlssg.h, nvsdk_ngx_helpers_dlssg_d3d.h) are included only
// in the .cc, and the FG runtime DLL (nvngx_dlssg.dll, staged in
// runtime/experimental) is resolved at runtime by the static NGX core
// (NOT bundled with the build). If the FG license is ultimately ruled
// incompatible with distribution, this entire fg/* module can be deleted with
// zero changes to the rest of the codebase.
//
// Ownership contract:
//   - The backend OWNS its interpolated-output texture (the generated frame
//     N-0.5, R10G10B10A2_UNORM, UAV), its OutputReal texture (the SDK's copy of
//     the real frame N, UAV), its disable-interpolation status buffer (4
//     bytes, UAV), the SDK feature handle + parameter block, and its
//     zero-filled depth (guest resolution, R32_FLOAT) and zero-filled
//     motion-vector (guest resolution, R16G16_FLOAT) placeholders.
//   - The backend BORROWS the backbuffer (display resolution, read-only) and,
//     optionally, an external depth (guest resolution) and external motion
//     vectors (guest resolution, R16G16_FLOAT, from the NVOF backend). It
//     never takes over guest_output_resource_paint_refs and never destroys a
//     borrowed resource.
//   - All SDK entry points (CreateFeature / EvaluateFeature / ReleaseFeature)
//     are wrapped in SEH boundaries, because the SDK can crash (access
//     violation), not just return an error code. A crash is treated as a
//     failure (deterministic fallback).
//
// Shared core: the backend acquires/releases the shared NGX core through the
// injected NgxCoreHost (Milestone 5-B prerequisite P0-1). It never calls the
// core Init/Shutdown itself. The core Shutdown1 runs only when the last
// consumer (this backend or the DLSS SR backend) releases.
//
// Lazy initialization: the first TryProcess call creates the SDK session
// (capability check + core Acquire + AllocateParameters + CreateFeature, the
// last recorded as a GPU command into the caller's command list). If that
// first creation fails, the backend is cached as unavailable (kUnavailable)
// and subsequent frames do not retry the SDK.
//
// Command submission semantics: the command list is a recording buffer that is
// executed once (ExecuteCommandLists) at the end of the frame. On failure the
// backend does NOT reset the command list itself - that is the presenter's
// responsibility. The backend only records commands; the presenter decides
// when to execute and when to reset.
class FGFrameGenerationBackend final : public IFrameGenerationBackend {
 public:
  // `device` is the D3D12 device the FG feature is created on. The backend
  // creates its owned output textures on this device (during Initialize). It
  // is borrowed (non-owning); the provider owns the device.
  // `runtime_dirs` is the list of directories to search for the NGX runtime
  // DLLs (nvngxruntime.dll, nvngx_dlssg.dll). They are passed to the SDK via
  // NVSDK_NGX_FeatureCommonInfo.PathListInfo.Path. None are used until the
  // backend is actually initialized (lazy).
  // `config_path` is the absolute path to the ngx-local.json config file (the
  // NGX ProjectId and EngineVersion are read from it). The backend
  // auto-generates a default config if the file is missing. It is not used
  // until the backend is actually initialized (lazy).
  // `core_host` is the shared NGX core owner (non-owning, injected by the
  // presenter). The backend acquires/releases the core through it instead of
  // calling Init_with_ProjectID / Shutdown1 / AllocateParameters /
  // DestroyParameters itself. It is shared with the DLSS SR backend, so the
  // core Shutdown1 runs only when the last consumer releases.
  FGFrameGenerationBackend(ID3D12Device* device,
                           std::vector<std::wstring> runtime_dirs,
                           std::wstring config_path,
                           NgxCoreHost* core_host);
  ~FGFrameGenerationBackend() override;

  FGFrameGenerationBackend(const FGFrameGenerationBackend&) = delete;
  FGFrameGenerationBackend& operator=(const FGFrameGenerationBackend&) = delete;

  bool IsAvailable() const override;
  // True when the first creation failed and the backend will not retry the SDK.
  bool IsUnavailable() const override;

  // The backend's owned zero-filled depth (guest resolution, R32_FLOAT, NPSR)
  // and zero-filled motion-vector (guest resolution, R16G16_FLOAT, NPSR)
  // placeholders, valid after a successful Initialize. The presenter passes
  // these (or nullptr) as the depth / mvecs arguments of TryProcess. nullptr
  // when the backend is not initialized.
  ID3D12Resource* ZeroDepthPlaceholder() const override;
  ID3D12Resource* ZeroMotionVectorPlaceholder() const override;

  BackendResult TryProcess(ID3D12GraphicsCommandList* command_list,
                           ID3D12Resource* backbuffer,
                           ID3D12Resource* depth,
                           ID3D12Resource* mvecs,
                           const DlssFgFrameParams& frame_params) override;

  ID3D12Resource* OutputResource() const override;
  ID3D12Resource* OutputRealResource() const override;
  uint32_t OutputWidth() const override;
  uint32_t OutputHeight() const override;

  void Shutdown() override;

  // The injected core host (non-owning). Exposed for the host-side ownership
  // tests (the test asserts the backend holds the same core host the
  // presenter injected).
  NgxCoreHost* core_host() const { return core_host_; }

 private:
  enum class State { kNotInitialized, kAvailable, kUnavailable };

  ID3D12Device* device_ = nullptr;  // non-owning (the provider owns it).
  std::vector<std::wstring> runtime_dirs_;
  std::wstring config_path_;
  NgxCoreHost* core_host_ = nullptr;  // non-owning (the presenter owns it).
  State state_ = State::kNotInitialized;
  // The backend's identity: color (backbuffer) size x internal (depth/MV) size.
  // Any change requires a re-create (the FG feature is created for a specific
  // size pair).
  uint32_t color_width_ = 0;
  uint32_t color_height_ = 0;
  uint32_t render_width_ = 0;
  uint32_t render_height_ = 0;

  // Backend-owned output textures (ALLOW_UNORDERED_ACCESS, start in UAV state).
  // output_interp_ is the generated frame N-0.5; output_real_ is the SDK's
  // copy of the real frame N. Both are R10G10B10A2_UNORM.
  Microsoft::WRL::ComPtr<ID3D12Resource> output_interp_;
  Microsoft::WRL::ComPtr<ID3D12Resource> output_real_;
  // Backend-owned disable-interpolation status buffer (4 bytes, UAV). The SDK
  // writes a boolean true to the first byte when interpolation should be
  // skipped this frame.
  Microsoft::WRL::ComPtr<ID3D12Resource> disable_status_;
  // Backend-owned zero-filled depth (guest resolution, R32_FLOAT, NPSR) and
  // motion-vector (guest resolution, R16G16_FLOAT, NPSR) placeholders. Xbox 360
  // produces neither, so the backend feeds all-zero values ("honest fallback")
  // when the caller passes nullptr. The SDK reads them and never writes their
  // contents.
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_texture_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_vectors_texture_;
  // One-time CPU upload buffer used to zero-fill depth_texture_ /
  // motion_vectors_texture_. Must stay alive for the backend's lifetime: the
  // CopyTextureRegion that reads it is a GPU command executed at the end of the
  // FIRST frame, AFTER Initialize() returns.
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer_;

  // True after the per-frame final barrier transitions the outputs to
  // PIXEL_SHADER_RESOURCE. Used to decide whether a "to UAV" barrier is needed
  // before the next evaluate. Initially false (the outputs start in UAV).
  bool output_in_pixel_shader_resource_ = false;

  // True after the first input-contract failure has been logged. The early
  // contract checks fire every frame, so the failure is logged only once.
  bool contract_failure_logged_ = false;
  // True after the first early Initialize failure (null device/command_list,
  // adapter enumeration, empty runtime path list) has been logged.
  bool init_early_failure_logged_ = false;
  // True after the first config load failure has been logged.
  bool config_failure_logged_ = false;

  // SDK-owned (allocated by the SDK, released via the SDK).
  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  NVSDK_NGX_Parameter* parameter_block_ = nullptr;

  // Creates the SDK session and the output textures. Records CreateFeature
  // into `command_list`. Returns kSuccess, or a failure (kUnavailable when the
  // capability check / core Acquire / CreateFeature fails, kInitFailed for
  // other init errors).
  BackendResult Initialize(ID3D12GraphicsCommandList* command_list,
                           uint32_t color_width, uint32_t color_height,
                           uint32_t render_width, uint32_t render_height);
  // Writes the default ngx-local.json config if the file is missing (the
  // existing file is the user's source of truth and is never overwritten).
  bool EnsureConfigFile() const;
  // Parses the ngx-local.json config, reading the NGX ProjectId and
  // EngineVersion. Returns false (and leaves the out-params unchanged) on a
  // missing / malformed / incomplete config.
  bool LoadNgxConfig(std::string* project_id_out,
                    std::string* engine_version_out) const;
  // Releases the SDK feature and parameter block. Does NOT release the output
  // textures (that is done by Shutdown / the destructor).
  void ReleaseFeature();
};

}  // namespace fg
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_FG_FG_FRAME_GENERATION_BACKEND_H_
