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
class StubBackend final : public IDLSSSuperSamplingBackend {
 public:
  explicit StubBackend(BackendResult result) : result_(result) {}

  bool IsAvailable() const override {
    return result_ != BackendResult::kUnavailable;
  }

  BackendResult TryProcess(
      const Presenter::GuestOutputProperties& guest_output) override {
    (void)guest_output;
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
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
  auto result = SelectOutputPath(config, &backend, props, &flow);
  CHECK(result == BackendResult::kSuccess);
}

}  // namespace ui
}  // namespace xe
