/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_DLSS_DLSS_SUPER_SAMPLING_BACKEND_H_
#define XENIA_UI_DLSS_DLSS_SUPER_SAMPLING_BACKEND_H_

#include <cstdint>
#include <string>
#include <vector>

// Must come BEFORE <wrl/client.h>: guest_output_enhancement.h -> presenter.h
// pulls in math.h (which breaks if min/max macros are defined) and later
// sets NOMINMAX before windows.h is reached. Including wrl/client.h first
// would let windows.h define the min/max macros too early (C2589).
#include "xenia/ui/guest_output_enhancement.h"

// ComPtr is available transitively through the chain above, but the backend
// owns a ComPtr member directly, so include it explicitly (after the Xenia
// header, to preserve the include order above).
#include <wrl/client.h>

// Forward declarations. The D3D12 types are used only as pointers in this
// header; the full d3d12.h is included in the .cc. The NVSDK_NGX_* types are
// SDK handles (opaque pointers) forward-declared here so that the SDK headers
// are NOT pulled into this header (license isolation: the SDK dependency is
// confined to the dlss/* module).
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIAdapter1;
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace xe {
namespace ui {
namespace dlss {

// A real DLSS Super Resolution backend that wraps the NVIDIA NGX SDK.
//
// This module is the ONLY place in the codebase that depends on the NVIDIA SDK
// (license isolation). The presenter holds an instance of this class through
// the IDLSSSuperSamplingBackend interface and never includes any nvsdk_* header
// itself. If the SDK license (section 4e) is ultimately ruled incompatible with
// distribution, this entire dlss/* module can be deleted with zero changes to
// the rest of the codebase.
//
// Ownership contract:
//   - The backend OWNS its own output texture (created with
//     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, starting in UAV state) and
//     the SDK feature handle + parameter block.
//   - The backend BORROWS the guest output (read-only). It never takes over
//     guest_output_resource_paint_refs and never destroys the guest output.
//   - All SDK entry points (Init / AllocateParameters / CreateFeature /
//     EvaluateFeature / ReleaseFeature / Shutdown) are wrapped in SEH
//     boundaries, because the SDK can crash (access violation), not just return
//     an error code. A crash is treated as a failure (deterministic fallback).
//
// Lazy initialization: the first TryProcess call creates the SDK session
// (capability check + Init_with_ProjectID + AllocateParameters + CreateFeature,
// the last recorded as a GPU command into the caller's command list). If that
// first creation fails, the backend is cached as unavailable (kUnavailable) and
// subsequent frames do not retry the SDK.
//
// Command submission semantics: the command list is a recording buffer that is
// executed once (ExecuteCommandLists) at the end of the frame. On failure the
// backend does NOT reset the command list itself - that is the presenter's
// responsibility. The backend only records commands; the presenter decides when
// to execute and when to reset.
class DLSSSuperSamplingBackend final : public IDLSSSuperSamplingBackend {
 public:
  // `runtime_dirs` is the list of directories to search for the NGX runtime
  // DLLs (nvngxruntime.dll, nvngx_dlss.dll). They are passed to the SDK via
  // NVSDK_NGX_FeatureCommonInfo.PathListInfo.Path. None are used until the
  // backend is actually initialized (lazy).
  // `config_path` is the absolute path to the ngx-local.json config file
  // (the NGX ProjectId and EngineVersion are read from it). The backend
  // auto-generates a default config if the file is missing. It is not used
  // until the backend is actually initialized (lazy).
  // `perf_quality` is the DLSS SR quality preset (a create-time parameter,
  // baked into CreateFeature). The backend maps it to the SDK enum
  // NVSDK_NGX_PerfQuality_Value; an out-of-range value is an input contract
  // failure (deterministic fallback), never a silent clamp.
  // The render preset is NOT passed to the constructor: it is a per-frame
  // parameter of TryProcess (like perf_quality) and the backend defaults to
  // kDefault (=0, "do not send the hint") until the first TryProcess. The
  // first TryProcess always initializes, so no re-create is ever triggered by
  // a constructor/preset mismatch.
  // The provider releases the adapter it used to create `device` (retaining it
  // makes the NVIDIA driver reject later CreateCommittedResource calls with
  // E_INVALIDARG). So the backend enumerates adapters itself in Initialize()
  // and matches by LUID (ID3D12Device3::GetAdapterLuid) to find the physical
  // adapter `device` sits on; it is used only for the capability check and
  // released immediately after.
  DLSSSuperSamplingBackend(ID3D12Device* device,
                           std::vector<std::wstring> runtime_dirs,
                           std::wstring config_path,
                           ui::DlssSrPerfQuality perf_quality);
  ~DLSSSuperSamplingBackend() override;

  DLSSSuperSamplingBackend(const DLSSSuperSamplingBackend&) = delete;
  DLSSSuperSamplingBackend& operator=(const DLSSSuperSamplingBackend&) = delete;

  bool IsAvailable() const override;
  // True when the first creation failed and the backend will not retry the SDK.
  bool IsUnavailable() const;

  BackendResult TryProcess(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* guest_output,
      uint32_t output_width,
      uint32_t output_height,
      const Presenter::GuestOutputProperties& guest_output_properties,
      const DlssSrFrameParams& frame_params,
        ui::DlssSrPerfQuality perf_quality,
        ui::DlssSrRenderPreset render_preset,
        ID3D12Resource* external_motion_vectors,
        ID3D12Resource* external_depth) override;

  // Getters for the backend output. The presenter uses these to create the SRV
  // (in its view heap) and to know the output size. The output texture is owned
  // by the backend.
  ID3D12Resource* OutputResource() const override;
  uint32_t OutputWidth() const override;
  uint32_t OutputHeight() const override;

  void Shutdown() override;

 private:
  enum class State { kNotInitialized, kAvailable, kUnavailable };

  // Creates the SDK session and the output texture. Records CreateFeature into
  // `command_list`. Returns kSuccess, or a failure (kUnavailable when the
  // capability check / Init fails, kInitFailed for other init errors). The
  // quality preset is passed in (it is a create-time parameter); the backend
  // stores it as created_perf_quality_ on success so a later change is
  // detected as a re-create.
    // The render preset is passed in too (also a create-time parameter, one
    // value per quality mode); the backend stores it as created_render_preset_
    // on success so a later change is detected as a re-create. kDefault (=0)
    // means "do not send the hint at all". An out-of-range value (not 0 and not
    // 10..13) is an input contract failure.
  BackendResult Initialize(ID3D12GraphicsCommandList* command_list,
                           uint32_t input_width, uint32_t input_height,
                           uint32_t output_width, uint32_t output_height,
                             ui::DlssSrPerfQuality perf_quality,
                             ui::DlssSrRenderPreset render_preset);
  // Ensures the ngx-local.json config exists at config_path_. If it is missing,
  // the parent directory is created and a default config (matching the former
  // hardcoded ProjectId/EngineVersion, so a missing config is behavior-neutral)
  // is written. An existing file is never overwritten (it is the user's source
  // of truth). Returns true on success.
  bool EnsureConfigFile() const;
  // Reads config_path_ and extracts ngxProjectId and engineVersion. Returns true
  // on success (both populated and non-empty), false on malformed JSON or
  // missing/empty keys.
  bool LoadNgxConfig(std::string* project_id_out,
                    std::string* engine_version_out) const;
  // Releases the SDK feature and parameter block. Does NOT release the output
  // texture (that is done by Shutdown / the destructor).
  void ReleaseFeature();

  ID3D12Device* device_;
  std::vector<std::wstring> runtime_dirs_;
  std::wstring config_path_;
  // Quality preset used by Initialize() when filling the create parameters.
  // The quality preset the current feature was created with. Initialized to the
  // constructor's quality; updated by Initialize() on every (re-)creation. A
  // per-frame TryProcess whose requested quality differs from this value
  // triggers a re-create (release + re-create), just like a size change.
  ui::DlssSrPerfQuality created_perf_quality_;

    // The render preset the current feature was created with. Initialized to
    // kDefault (the constructor does not take a render preset; the first
    // TryProcess always initializes, so this value is only ever compared AFTER
    // a successful Initialize). Updated by Initialize() on every (re-)creation.
    // A per-frame TryProcess whose requested render preset differs from this
    // value triggers a re-create (release + re-create), just like a quality
    // change.
    ui::DlssSrRenderPreset created_render_preset_ =
        ui::DlssSrRenderPreset::kDefault;

  State state_ = State::kNotInitialized;
  // The backend's identity: input size x output size. Any change requires a
  // re-create (the DLSS feature is created for a specific size pair).
  uint32_t input_width_ = 0;
  uint32_t input_height_ = 0;
  uint32_t output_width_ = 0;
  uint32_t output_height_ = 0;

  // Backend-owned output texture (ALLOW_UNORDERED_ACCESS, starts in UAV state).
  Microsoft::WRL::ComPtr<ID3D12Resource> output_texture_;
  // Zero-filled placeholder buffers for the DLSS SR feature's required depth
  // and motion-vector inputs. Xbox 360 games do not produce these, so we feed
  // all-zero values ("honest fallback"). The SDK never modifies them.
  // TODO: Replace with real depth estimation and NVOF-derived motion vectors.
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_texture_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_vectors_texture_;
  // One-time CPU upload buffer used to zero-fill depth_texture_ /
  // motion_vectors_texture_. Must stay alive for the backend's lifetime: the
  // CopyTextureRegion that reads it is a GPU command executed at the end of the
  // FIRST frame, AFTER Initialize() returns. A local would be released too
  // early and fault the GPU (device removed 0x887A0006). Mirrors Veyra's
  // member upload buffers (upDepth_/upZeroDepth_/upZeroMotion_).
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer_;
  // True after the per-frame final barrier transitions the output texture to
  // PIXEL_SHADER_RESOURCE. Used to decide whether a "to UAV" barrier is needed
  // before the next evaluate. Initially false (the output starts in UAV).
  bool output_in_pixel_shader_resource_ = false;

  // True after the first input-contract failure has been logged. The early
  // contract checks (null pointer / zero size / 1:1) fire every frame, so the
  // failure is logged only once to avoid per-frame spam. Diagnostic only -
  // the presenter separately logs the fallback this frame.
  bool contract_failure_logged_ = false;

  // True after the first config load failure (missing/malformed ngx-local.json)
  // has been logged. The failure leaves state_ as kNotInitialized, so Initialize
  // is retried every frame; the failure is logged only once to avoid per-frame
  // spam. (A user who fixes the config at runtime is picked up on the next
  // frame.)
  bool config_failure_logged_ = false;

  // True after the first early Initialize failure (null device/command_list,
  // QueryInterface, GetAdapter, or empty runtime path list) has been logged.
  // These failures leave state_ as kNotInitialized, so Initialize is retried
  // every frame; the failure is logged only once to avoid per-frame spam.
  bool init_early_failure_logged_ = false;

  // SDK-owned (allocated by the SDK, released via the SDK).
  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  NVSDK_NGX_Parameter* parameter_block_ = nullptr;
};

}  // namespace dlss
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_DLSS_DLSS_SUPER_SAMPLING_BACKEND_H_
