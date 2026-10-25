/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/guest_output_enhancement.h"

#include "xenia/base/assert.h"

namespace xe {
namespace ui {

void BuildOriginalPathFallbackFlow(Presenter::GuestOutputPaintFlow* flow,
                                   const Presenter::GuestOutputProperties& properties) {
  assert_true(flow != nullptr);
  // Start from a clean flow. Value-initialization leaves the guest output
  // properties inactive; we then overwrite them with the real properties.
  *flow = {};
  flow->properties = properties;
  // The original path is always a single kBilinear effect. NEVER effect_count = 0
  // (that would black-screen and drop the guest output).
  flow->effect_count = 1;
  flow->effects[0] = Presenter::GuestOutputPaintEffect::kBilinear;
  flow->effect_output_sizes[0] =
      {properties.frontbuffer_width, properties.frontbuffer_height};
  flow->output_x = 0;
  flow->output_y = 0;
  // No letterbox in the minimal fallback flow; the full flow construction
  // (letterbox, output position) is handled by the existing
  // GetGuestOutputPaintFlow in the real integration.
  flow->letterbox_clear_rectangle_count = 0;
}

BackendResult SelectOutputPath(const GuestOutputEnhancementConfig& config,
                               IDLSSSuperSamplingBackend* backend,
                               ID3D12GraphicsCommandList* command_list,
                               ID3D12Resource* guest_output_resource,
                               uint32_t output_width,
                               uint32_t output_height,
                               const Presenter::GuestOutputProperties& guest_output_properties,
                               const DlssSrFrameParams& frame_params,
                               Presenter::GuestOutputPaintFlow* fallback_flow_out,
                               ID3D12Resource* external_motion_vectors,
                               ID3D12Resource* external_depth) {
  // 1. Not requested: master off, SR sub-switch off, or backend not initialized.
  //    The backend is never called in this case (no DLL load, no GPU work).
  if (!config.IsSuperSamplingRequested() || backend == nullptr) {
    if (fallback_flow_out) {
      BuildOriginalPathFallbackFlow(fallback_flow_out, guest_output_properties);
    }
    return BackendResult::kNotRequested;
  }

  // 2. Requested: ask the backend to process the frame.
  BackendResult result =
      backend->TryProcess(command_list, guest_output_resource, output_width,
                          output_height, guest_output_properties, frame_params,
                            config.perf_quality, config.render_preset,
                            external_motion_vectors, external_depth);

  // 3. Success: the backend produced usable output. Use the backend path.
  if (result == BackendResult::kSuccess) {
    return BackendResult::kSuccess;
  }

  // 4. Failure: fall back to the original path. Preserve the specific reason
  //    when it is one of the known failure states; otherwise normalize to the
  //    generic fallback state.
  if (fallback_flow_out) {
    BuildOriginalPathFallbackFlow(fallback_flow_out, guest_output_properties);
  }
  switch (result) {
    case BackendResult::kUnavailable:
    case BackendResult::kInitFailed:
    case BackendResult::kInputContractFailed:
    case BackendResult::kSyncConditionFailed:
    case BackendResult::kExecutionFailed:
    case BackendResult::kOutputInvalid:
      return result;  // Preserve the specific failure reason.
    case BackendResult::kNotRequested:
    case BackendResult::kOriginalPathFallback:
    case BackendResult::kSuccess:
      // Defensive: the backend should not return these. Normalize to the
      // generic fallback state.
      return BackendResult::kOriginalPathFallback;
  }
  return BackendResult::kOriginalPathFallback;
}

}  // namespace ui
}  // namespace xe
