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

namespace xe {
namespace ui {

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
  // kInputContractFailed, kSyncConditionFailed, kExecutionFailed, kOutputInvalid).
  // The backend owns its own output and only borrows the guest output.
  virtual BackendResult TryProcess(
      const Presenter::GuestOutputProperties& guest_output) = 0;

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
                               const Presenter::GuestOutputProperties& guest_output,
                               Presenter::GuestOutputPaintFlow* fallback_flow_out);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_GUEST_OUTPUT_ENHANCEMENT_H_
