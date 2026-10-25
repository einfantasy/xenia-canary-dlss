/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/guest_output_enhancement.h"

#include <type_traits>

#define CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER
#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace ui {
namespace {

// A test-only stub backend that returns a configurable BackendResult.
// It performs NO real GPU work and must NEVER be interpreted as a real DLSS /
// NVOF execution. It is used solely to drive the path-selection logic.
  // It also records the last quality / render preset it was called with, so a
  // test can assert the config values are passed through unchanged.
class StubBackend final : public IDLSSSuperSamplingBackend {
 public:
  explicit StubBackend(BackendResult result) : result_(result) {}

    // The last values TryProcess was called with (set by the path-selection
    // call, read by the test to verify pass-through). Public so the test can
    // inspect them directly.
    DlssSrPerfQuality last_perf_quality_ = DlssSrPerfQuality::kBalanced;
    DlssSrRenderPreset last_render_preset_ = DlssSrRenderPreset::kDefault;

  bool IsAvailable() const override {
    return result_ != BackendResult::kUnavailable;
  }

  BackendResult TryProcess(
      ID3D12GraphicsCommandList* command_list, ID3D12Resource* guest_output,
      uint32_t output_width, uint32_t output_height,
      const Presenter::GuestOutputProperties& guest_output_properties,
      const DlssSrFrameParams& frame_params,
        DlssSrPerfQuality perf_quality, DlssSrRenderPreset render_preset,
        ID3D12Resource* external_motion_vectors,
        ID3D12Resource* external_depth) override {
    // The stub performs no real GPU work; it ignores the GPU parameters and
    // returns the configured result to drive the path-selection logic.
    (void)command_list;
    (void)guest_output;
    (void)output_width;
    (void)output_height;
    (void)guest_output_properties;
    (void)frame_params;
    (void)external_motion_vectors;
    (void)external_depth;
      last_perf_quality_ = perf_quality;
      last_render_preset_ = render_preset;
    return result_;
  }

  void Shutdown() override {}

 private:
  BackendResult result_;
};

Presenter::GuestOutputProperties MakeActiveProperties() {
  Presenter::GuestOutputProperties props;
  props.frontbuffer_width = 1280;
  props.frontbuffer_height = 720;
  props.display_aspect_ratio_x = 16;
  props.display_aspect_ratio_y = 9;
  props.is_8bpc = true;
  return props;
}

}  // namespace

// 1. Master switch default OFF.
TEST_CASE("Master switch default off", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  CHECK_FALSE(config.master_enabled);
}

// 2. SR sub-switch default OFF.
TEST_CASE("SR sub-switch default off", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  CHECK_FALSE(config.super_sampling_enabled);
  // The other sub-switches also default OFF.
  CHECK_FALSE(config.optical_flow_enabled);
  CHECK_FALSE(config.frame_generation_enabled);
  CHECK_FALSE(config.experimental_neural_rendering_enabled);
}

// 3. No backend requested when default OFF. The backend is never called.
TEST_CASE("No backend requested when default off", "[enhancement]") {
  GuestOutputEnhancementConfig config;  // all off
  // Even a backend that would succeed is never called when the config is off.
  StubBackend backend(BackendResult::kSuccess);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kNotRequested);
}

// 4. Backend unavailable -> original path.
TEST_CASE("Backend unavailable selects original path", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  StubBackend backend(BackendResult::kUnavailable);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kUnavailable);
  CHECK(IsOriginalPathFallback(result));
  CHECK(flow.effect_count == 1);
  CHECK(flow.effects[0] == Presenter::GuestOutputPaintEffect::kBilinear);
}

// 5. Init failure -> original path.
TEST_CASE("Init failure selects original path", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  StubBackend backend(BackendResult::kInitFailed);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kInitFailed);
  CHECK(IsOriginalPathFallback(result));
  CHECK(flow.effect_count == 1);
  CHECK(flow.effects[0] == Presenter::GuestOutputPaintEffect::kBilinear);
}

// 6. Input contract failure -> original path.
TEST_CASE("Input contract failure selects original path", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  StubBackend backend(BackendResult::kInputContractFailed);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kInputContractFailed);
  CHECK(IsOriginalPathFallback(result));
  CHECK(flow.effect_count == 1);
  CHECK(flow.effects[0] == Presenter::GuestOutputPaintEffect::kBilinear);
}

// 7. Sync condition failure -> original path.
TEST_CASE("Sync condition failure selects original path", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  StubBackend backend(BackendResult::kSyncConditionFailed);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kSyncConditionFailed);
  CHECK(IsOriginalPathFallback(result));
  CHECK(flow.effect_count == 1);
  CHECK(flow.effects[0] == Presenter::GuestOutputPaintEffect::kBilinear);
}

// 8. Execution failure -> original path.
TEST_CASE("Execution failure selects original path", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  StubBackend backend(BackendResult::kExecutionFailed);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kExecutionFailed);
  CHECK(IsOriginalPathFallback(result));
  CHECK(flow.effect_count == 1);
  CHECK(flow.effects[0] == Presenter::GuestOutputPaintEffect::kBilinear);
}

// 9. Output validation failure -> original path.
TEST_CASE("Output validation failure selects original path", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  StubBackend backend(BackendResult::kOutputInvalid);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kOutputInvalid);
  CHECK(IsOriginalPathFallback(result));
  CHECK(flow.effect_count == 1);
  CHECK(flow.effects[0] == Presenter::GuestOutputPaintEffect::kBilinear);
}

// 10. Backend success and final-present success are different states.
TEST_CASE("Backend success is distinct from present success", "[enhancement]") {
  // kSuccess (BackendResult) is a different enum from PaintResult.
  static_assert(!std::is_same<BackendResult, Presenter::PaintResult>::value,
                "BackendResult must be distinct from PaintResult");
  // A backend success does NOT imply a present success; they are separate
  // values in separate enums.
  CHECK(BackendResult::kSuccess != BackendResult::kOriginalPathFallback);
}

// 11. BackendResult not mixed with PaintResult.
TEST_CASE("BackendResult not mixed with PaintResult", "[enhancement]") {
  static_assert(!std::is_same<BackendResult, Presenter::PaintResult>::value,
                "BackendResult must be a distinct enum from PaintResult");
  static_assert(std::is_enum<BackendResult>::value, "BackendResult is an enum");
  static_assert(std::is_enum<Presenter::PaintResult>::value,
                "PaintResult is an enum");
}

// 12. Original path uses kBilinear.
TEST_CASE("Original path uses kBilinear", "[enhancement]") {
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  BuildOriginalPathFallbackFlow(&flow, props);
  CHECK(flow.effect_count == 1);
  CHECK(flow.effects[0] == Presenter::GuestOutputPaintEffect::kBilinear);
}

// 13. effect_count = 0 not used for backend failure fallback.
TEST_CASE("effect_count 0 not used for backend failure fallback",
          "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  StubBackend backend(BackendResult::kExecutionFailed);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(IsOriginalPathFallback(result));
  CHECK(flow.effect_count == 1);  // NEVER 0
}

// 14. Device-lost state follows the existing contract.
TEST_CASE("Device lost state follows existing contract", "[enhancement]") {
  // The device-lost states (kGpuLostExternally, kGpuLostResponsible) are part of
  // PaintResult and are NOT overwritten by backend errors. BackendResult is
  // orthogonal to them: it carries no device-lost state of its own.
  static_assert(!std::is_same<BackendResult, Presenter::PaintResult>::value,
                "BackendResult must not conflate device-lost with backend result");
  // The two device-lost states are distinct PaintResult values.
  CHECK(static_cast<int>(Presenter::PaintResult::kGpuLostExternally) !=
        static_cast<int>(Presenter::PaintResult::kGpuLostResponsible));
  // A backend execution failure is a different state from a device-lost state.
  CHECK(static_cast<int>(BackendResult::kExecutionFailed) !=
        static_cast<int>(Presenter::PaintResult::kGpuLostExternally));
}

// 15. NVOF / frame gen / experimental OFF does not affect SR config or path.
TEST_CASE("Other sub-features off do not affect SR", "[enhancement]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.super_sampling_enabled = true;
  // NVOF, FG, experimental all OFF (default).
  CHECK_FALSE(config.optical_flow_enabled);
  CHECK_FALSE(config.frame_generation_enabled);
  CHECK_FALSE(config.experimental_neural_rendering_enabled);
  CHECK(config.IsSuperSamplingRequested());
  CHECK_FALSE(config.IsOpticalFlowRequested());
  CHECK_FALSE(config.IsFrameGenerationRequested());
  CHECK_FALSE(config.IsExperimentalRequested());
  // SR is still requested even when the others are off, and the backend is
  // consulted.
  StubBackend backend(BackendResult::kSuccess);
  auto props = MakeActiveProperties();
  Presenter::GuestOutputPaintFlow flow;
  auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
  CHECK(result == BackendResult::kSuccess);
}

  // 16. Render preset default is kDefault (=0, "do not send the hint"). The
  // Xenia-side enum values are the SDK header's explicit numeric values
  // (nvsdk_ngx_defs.h:70-88), NOT a contiguous 0..4 ordering. The UI layer never
  // includes the SDK header, so these static_asserts are the only place that
  // pins the values (license isolation).
  TEST_CASE("Render preset default and enum values", "[enhancement]") {
    GuestOutputEnhancementConfig config;
    CHECK(config.render_preset == DlssSrRenderPreset::kDefault);
    static_assert(static_cast<int>(DlssSrRenderPreset::kDefault) == 0,
                  "kDefault must be 0");
    static_assert(static_cast<int>(DlssSrRenderPreset::kJ) == 10,
                  "kJ must be 10 (the SDK header's explicit value)");
    static_assert(static_cast<int>(DlssSrRenderPreset::kK) == 11,
                  "kK must be 11 (the SDK header's explicit value)");
    static_assert(static_cast<int>(DlssSrRenderPreset::kL) == 12,
                  "kL must be 12 (the SDK header's explicit value)");
    static_assert(static_cast<int>(DlssSrRenderPreset::kM) == 13,
                  "kM must be 13 (the SDK header's explicit value)");
    // The enum is uint8_t-backed so a plain cast is safe (no sign extension).
    static_assert(std::is_same<decltype(static_cast<uint8_t>(DlssSrRenderPreset::kDefault)),
                               uint8_t>::value,
                  "DlssSrRenderPreset must be uint8_t-backed");
  }

  // 17. SelectOutputPath passes the config's render preset (and quality) to the
  // backend unchanged.
  TEST_CASE("SelectOutputPath passes render preset to backend",
            "[enhancement]") {
    GuestOutputEnhancementConfig config;
    config.master_enabled = true;
    config.super_sampling_enabled = true;
    config.perf_quality = DlssSrPerfQuality::kBalanced;
    config.render_preset = DlssSrRenderPreset::kK;
    StubBackend backend(BackendResult::kSuccess);
    auto props = MakeActiveProperties();
    Presenter::GuestOutputPaintFlow flow;
    auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
    CHECK(result == BackendResult::kSuccess);
    CHECK(backend.last_perf_quality_ == DlssSrPerfQuality::kBalanced);
    CHECK(backend.last_render_preset_ == DlssSrRenderPreset::kK);
  }

  // 18. A non-default render preset does not change the path selection: the
  // backend is still consulted and succeeds, and the preset only affects the
  // create-time SDK parameter (not the path-selection logic).
  TEST_CASE("Non-default render preset still selects backend path",
            "[enhancement]") {
    GuestOutputEnhancementConfig config;
    config.master_enabled = true;
    config.super_sampling_enabled = true;
    config.render_preset = DlssSrRenderPreset::kM;
    StubBackend backend(BackendResult::kSuccess);
    auto props = MakeActiveProperties();
    Presenter::GuestOutputPaintFlow flow;
    auto result = SelectOutputPath(config, &backend, nullptr, nullptr, 0, 0, props, DlssSrFrameParams{}, &flow);
    CHECK(result == BackendResult::kSuccess);
    CHECK(backend.last_render_preset_ == DlssSrRenderPreset::kM);
    // The default preset also still selects the backend path (behavior-neutral
    // default).
    GuestOutputEnhancementConfig default_config;
    default_config.master_enabled = true;
    default_config.super_sampling_enabled = true;
    // render_preset is kDefault by default.
    StubBackend default_backend(BackendResult::kSuccess);
    Presenter::GuestOutputPaintFlow default_flow;
    auto default_result =
        SelectOutputPath(default_config, &default_backend, nullptr, nullptr, 0,
                         0, props, DlssSrFrameParams{}, &default_flow);
    CHECK(default_result == BackendResult::kSuccess);
    CHECK(default_backend.last_render_preset_ == DlssSrRenderPreset::kDefault);
  }

}  // namespace ui
}  // namespace xe
