/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Minimal, host-testable skeleton for the NVIDIA graphics enhancement backend
// (Milestone 2). This file defines the backend interface, the independent
// BackendResult type, and the default-off configuration gate. It does NOT
// create real SDK instances, load any NVIDIA DLL, or perform any GPU work.
// All real backend execution is deferred to Milestone 3.
//
// Design authority: docs/dlss-integration/architecture-design.md (sections D2,
// E1, F1-F3, H). This file implements the "generic Presenter layer" portion of
// that design: the backend interface, the result contract, the config gate, and
// the pure path-selection / original-path fallback logic.

#ifndef XENIA_UI_GUEST_OUTPUT_ENHANCEMENT_H_
#define XENIA_UI_GUEST_OUTPUT_ENHANCEMENT_H_

#include "xenia/ui/presenter.h"

// Forward declarations of the D3D12 types used by the backend interface.
// The backend records its GPU commands (CreateFeature / EvaluateFeature and
// the surrounding state-transition barriers) into the caller's command list,
// and reads the guest output resource (borrowed, read-only). Keeping these as
// forward-declared pointers avoids pulling the full d3d12.h into this header.
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace xe {
namespace ui {

// ---------------------------------------------------------------------------
// DlssSrPerfQuality
//
// Xenia-side subset of NVSDK_NGX_PerfQuality_Value (DLSS SDK 310.9.1,
// nvsdk_ngx_defs.h:251-260). The UI/config layer never includes any nvsdk_*
// header (license isolation: SDK dependencies are confined to the dlss/*
// module); the backend maps this value to the SDK enum at feature creation.
// Two SDK values are intentionally absent:
//   - UltraQuality (=4): not supported by the current DLSS runtime version;
//     the UI does not expose it and the backend never sends it.
//   - DLAA (=5): means input size == output size (1.0 scaling), which has no
//     meaning for the guest output path.
// ---------------------------------------------------------------------------
enum class DlssSrPerfQuality : uint8_t {
  kMaxPerf = 0,
  kBalanced = 1,
  kMaxQuality = 2,
  kUltraPerformance = 3,
};

// ---------------------------------------------------------------------------
// DlssSrRenderPreset
//
// Xenia-side subset of NVSDK_NGX_DLSS_Hint_Render_Preset (DLSS SDK 310.9.1,
// nvsdk_ngx_defs.h:70-88). The values are the SDK header's EXPLICIT numeric
// values (0/10/11/12/13), NOT a contiguous 0..4 ordering: the PDF's implicit
// A..O letters occupy slots 1-15, and A-D (removed), E-F (deprecated), G-I
// (reserved), and N-O (reserved) are all unusable. The UI/config layer never
// includes any nvsdk_* header (license isolation: SDK dependencies are
// confined to the dlss/* module); the backend maps this value to the SDK
// parameter at feature creation. Only Default and the four current presets are
// exposed:
//   - kJ (=10): similar to K; slightly less ghosting at the cost of extra
//     flickering. K is generally recommended over J.
//   - kK (=11): default for DLAA/Balanced/Quality modes; best image quality.
//   - kL (=12): default for Ultra Perf mode.
//   - kM (=13): default for Perf mode.
//   - kDefault (=0): "do not send the hint at all" - the SDK then resolves the
//     mode's default preset (identical to the current verified baseline).
// ---------------------------------------------------------------------------
enum class DlssSrRenderPreset : uint8_t {
  kDefault = 0,
  kJ = 10,
  kK = 11,
  kL = 12,
  kM = 13,
};

// ---------------------------------------------------------------------------
// BackendResult
//
// An independent result type for the DLSS SR backend, orthogonal to
// Presenter::PaintResult. It answers three questions that PaintResult does not:
//   1. Was the backend requested?              -> kNotRequested
//   2. Did the backend produce usable output?  -> kSuccess / a failure reason
//   3. Was the original path selected as a fallback? -> kOriginalPathFallback
//
// Presenter::PaintResult (final presentation outcome) remains a separate,
// orthogonal enum. kSuccess here means ONLY that the backend produced usable
// output; it does NOT mean the frame was successfully presented. The two
// concepts must never be conflated.
// ---------------------------------------------------------------------------
enum class BackendResult {
  // The backend was not requested (master switch off, SR sub-switch off, or the
  // backend was not initialized). The original path is used.
  kNotRequested,
  // The backend was requested and successfully produced usable output. This
  // does NOT imply the frame was successfully presented (see PaintResult).
  kSuccess,
  // The backend was requested but is unavailable (runtime missing, capability
  // check failed).
  kUnavailable,
  // The backend was requested but initialization failed.
  kInitFailed,
  // The backend was requested but the input contract was not satisfied
  // (format / size / MV / depth / jitter).
  kInputContractFailed,
  // The backend was requested but resource state or synchronization conditions
  // were not met (state transition failure, Fence timeout).
  kSyncConditionFailed,
  // The backend was requested but execution failed.
  kExecutionFailed,
  // The backend was requested but output validation failed.
  kOutputInvalid,
  // A fallback to the original path was selected (generic fallback state).
  kOriginalPathFallback,
};

// True when the given BackendResult means the original path should be used
// (i.e. any state other than kSuccess). Every non-success state falls back to
// the original path per the architecture design (section H).
constexpr bool IsOriginalPathFallback(BackendResult result) {
  return result != BackendResult::kSuccess;
}

// ---------------------------------------------------------------------------
// GuestOutputEnhancementConfig
//
// A sidecar configuration struct (no GPU resources) that gates the NVIDIA
// enhancement backend. All switches default to OFF: the master switch and
// every sub-feature switch. A sub-feature is only effective when the master
// switch is also on (effective SR = master && sr).
//
// Thread safety: in Milestone 2 this struct is a plain value type - nothing
// stores it or reads it concurrently, so NO mutex exists yet and no protection
// is implemented here. When it is wired into the Presenter (Milestone 3), it
// MUST be read/written with the same mutex pattern as the existing guest
// output paint config (guest_output_paint_config_mutex_). That protection is
// a requirement of the integration, not behavior already in place.
//
// This struct holds NO GPU resources. SDK-specific parameters (SR PerfQuality,
// NVOF parameters) are owned by the backend module itself, and per-frame
// parameters (jitter, InReset) are passed as call arguments to the backend.
// ---------------------------------------------------------------------------
struct GuestOutputEnhancementConfig {
  // Master switch. When false, no backend is requested, no DLL is loaded, no
  // GPU resources are created, and no extra GPU steps are performed.
  bool master_enabled = false;

  // DLSS Super Resolution sub-switch.
  bool super_sampling_enabled = false;

  // NVIDIA Optical Flow sub-switch (independent module; not a dependency of SR).
  bool optical_flow_enabled = false;

  // DLSS SR quality preset (create-time parameter of the SDK feature). Only
  // meaningful when super_sampling_enabled (and master_enabled). The preset is
  // baked into CreateFeature, so changing it requires a backend rebuild.
  // The default is kBalanced to match the backend's former hardcoded value, so
  // enabling SR for the first time never triggers a rebuild.
  DlssSrPerfQuality perf_quality = DlssSrPerfQuality::kBalanced;

  // DLSS SR render preset (create-time parameter of the SDK feature, one value
  // per quality mode). Only meaningful when super_sampling_enabled (and
  // master_enabled). Like perf_quality it is baked into CreateFeature, so
  // changing it requires a backend rebuild. kDefault (=0) means "do not send
  // the hint at all" (the SDK resolves the mode's default preset), so the
  // default is behavior-neutral versus the verified baseline.
  DlssSrRenderPreset render_preset = DlssSrRenderPreset::kDefault;

  // Frame Generation sub-switch.
  bool frame_generation_enabled = false;

  // Experimental neural rendering sub-switch.
  bool experimental_neural_rendering_enabled = false;

  // Effective SR request = master && SR sub-switch.
  bool IsSuperSamplingRequested() const {
    return master_enabled && super_sampling_enabled;
  }

  bool IsOpticalFlowRequested() const {
    return master_enabled && optical_flow_enabled;
  }

  bool IsFrameGenerationRequested() const {
    return master_enabled && frame_generation_enabled;
  }

  bool IsExperimentalRequested() const {
    return master_enabled && experimental_neural_rendering_enabled;
  }
};

// ---------------------------------------------------------------------------
// IDLSSSuperSamplingBackend
//
// DlssSrFrameParams
//
// Per-frame parameters for the DLSS SR backend. These change per frame (the
// jitter offset advances for temporal anti-aliasing; InReset is set on scene
// changes) and are passed as call arguments to the backend's per-frame call,
// NOT stored in a persistent config object. (See the architecture design,
// section F1.)
// ---------------------------------------------------------------------------
struct DlssSrFrameParams {
  // Jitter offset in input/render pixel space. Advances per frame.
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;
  // Set to true on a scene change; resets the backend's temporal history.
  bool in_reset = false;
};
// ---------------------------------------------------------------------------
// IDLSSSuperSamplingBackend
//
// The minimal DLSS SR backend interface. A real implementation (Milestone 3)
// will own its own output textures and only borrow the guest output (read-only,
// via SRV) - it never takes ownership of the guest output resource and never
// takes over guest_output_resource_paint_refs.
//
// In Milestone 2 this interface is defined but NOT implemented with a real SDK
// instance. It is defined so that the path-selection logic can be tested with a
// stub backend that returns a configurable BackendResult. The stub performs no
// real GPU work and must never be interpreted as a real DLSS execution.
// ---------------------------------------------------------------------------
class IDLSSSuperSamplingBackend {
 public:
  virtual ~IDLSSSuperSamplingBackend() = default;

  // Whether the backend is currently available (runtime present and capable).
  virtual bool IsAvailable() const = 0;

  // Attempt to process one guest output frame. Returns a BackendResult: kSuccess,
  // or one of the specific failure reasons (kUnavailable, kInitFailed,
  // kInputContractFailed, kSyncConditionFailed, kExecutionFailed,
  // kOutputInvalid).
  //
  // The backend owns its own output texture and only borrows the guest output
  // (read-only). It records its GPU work (the SDK CreateFeature /
  // EvaluateFeature calls plus the surrounding state-transition barriers) into
  // `command_list`. The command list is a recording buffer: it is only executed
  // once at the end (ExecuteCommandLists). On failure the backend does NOT reset
  // the command list itself - that is the presenter's responsibility (see the
  // architecture design, section on command submission semantics).
  //
  // `guest_output` is the guest output resource to read (borrowed). Its size is
  // given by `guest_output_properties.frontbuffer_width/height`. The backend
  // output is written to a texture it owns, at `output_width` x `output_height`
  // (the target resolution).
  //
  // `perf_quality` is the DLSS SR quality preset for this frame. It is a
  // create-time parameter baked into the SDK's CreateFeature, so a change from
  // the quality the feature was created with triggers a backend re-create
  // (release + re-create), exactly like a change to the output size.
  virtual BackendResult TryProcess(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* guest_output,
      uint32_t output_width,
      uint32_t output_height,
      const Presenter::GuestOutputProperties& guest_output_properties,
      const DlssSrFrameParams& frame_params,
      DlssSrPerfQuality perf_quality,
      // `render_preset` is the DLSS SR render preset for this frame. Like
      // `perf_quality` it is a create-time parameter (one value per quality
      // mode), so a change from the preset the feature was created with
      // triggers a backend re-create (release + re-create). kDefault (=0)
      // means "do not send the hint" (the SDK resolves the mode's default
      // preset).
      DlssSrRenderPreset render_preset) = 0;

  // The backend's owned output texture, valid after a successful TryProcess
  // (the backend created it during initialization). The presenter uses these
  // to create the SRV the first paint effect reads from. The default
  // implementations return nullptr/0 so host-test stub backends need no
  // overrides; the real DLSS backend overrides them.
  virtual ID3D12Resource* OutputResource() const { return nullptr; }
  virtual uint32_t OutputWidth() const { return 0; }
  virtual uint32_t OutputHeight() const { return 0; }

  // Release backend-owned resources. The presenter never destroys backend
  // resources directly; this is the backend's responsibility.
  virtual void Shutdown() = 0;
};

// ---------------------------------------------------------------------------
// BuildOriginalPathFallbackFlow
//
// Builds the original-path fallback flow: effect_count = 1 and effects[0] =
// kBilinear. This is the original effect chain that the presenter uses when the
// backend is not used. It is NEVER effect_count = 0 (that would black-screen
// and drop the guest output). Letterbox and output position are left at their
// defaults here; the full flow construction (letterbox, output position) is
// handled by the existing GetGuestOutputPaintFlow in the real integration.
// ---------------------------------------------------------------------------
void BuildOriginalPathFallbackFlow(Presenter::GuestOutputPaintFlow* flow,
                                   const Presenter::GuestOutputProperties& properties);

// ---------------------------------------------------------------------------
// SelectOutputPath
//
// A pure, host-testable path-selection function. It decides whether the frame
// goes through the DLSS SR backend or falls back to the original path, and
// builds the original-path fallback flow when needed. No GPU work, no allocation
// beyond the flow itself, no side effects.
//
// It returns a BackendResult:
//   - kNotRequested  when the config does not request SR, or the backend is
//                    nullptr (not initialized).
//   - kSuccess       when the backend succeeded.
//   - a specific failure reason when the backend failed for that reason.
//   - kOriginalPathFallback when the backend returned an unexpected result.
//
// When the original path is used (result != kSuccess), *fallback_flow_out is
// filled with the original effect chain (effect_count = 1, kBilinear). It is
// NEVER effect_count = 0.
// ---------------------------------------------------------------------------
BackendResult SelectOutputPath(const GuestOutputEnhancementConfig& config,
                               IDLSSSuperSamplingBackend* backend,
                               ID3D12GraphicsCommandList* command_list,
                               ID3D12Resource* guest_output_resource,
                               uint32_t output_width,
                               uint32_t output_height,
                               const Presenter::GuestOutputProperties& guest_output_properties,
                               const DlssSrFrameParams& frame_params,
                               Presenter::GuestOutputPaintFlow* fallback_flow_out);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_GUEST_OUTPUT_ENHANCEMENT_H_
