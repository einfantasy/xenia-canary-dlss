/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/dlss/fg/fg_frame_generation_backend.h"
#include "xenia/ui/guest_output_enhancement.h"
#include "xenia/ui/dlss/ngx/ngx_core_host.h"

#include <cstdint>
#include <string>
#include <vector>

#define CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER
#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace ui {
namespace {

using fg::FGFrameGenerationBackend;

// The real backend's copy/move constructors are deleted, so each test
// constructs its backend directly on the stack (with a null device - no GPU
// work). The production path uses a real device; the GPU-side SEH boundary is
// exercised on the target hardware.

}  // namespace

// 1. Config gating.

// FG sub-switch defaults OFF; not requested by default.
TEST_CASE("FG sub-switch default off", "[fg]") {
  GuestOutputEnhancementConfig config;
  CHECK_FALSE(config.frame_generation_enabled);
  CHECK_FALSE(config.IsFrameGenerationRequested());
}

// Master switch OFF -> FG not requested even when the sub-switch is ON.
TEST_CASE("FG not requested when master off", "[fg]") {
  GuestOutputEnhancementConfig config;
  config.frame_generation_enabled = true;
  CHECK_FALSE(config.master_enabled);
  CHECK_FALSE(config.IsFrameGenerationRequested());
}

// Master ON + FG sub-switch ON -> requested.
TEST_CASE("FG requested when master and FG on", "[fg]") {
  GuestOutputEnhancementConfig config;
  config.master_enabled = true;
  config.frame_generation_enabled = true;
  CHECK(config.IsFrameGenerationRequested());
}

// 2. Input contract (host-side, no GPU work).

// The real backend's input-contract check fires before any GPU work: a null
// command list or back buffer returns kInputContractFailed (no crash, no
// GetDesc on a null pointer). The backend is constructed with a null device so
// no real D3D12 resource is dereferenced.
TEST_CASE("FG real backend null inputs -> input contract failed", "[fg]") {
  NgxCoreHost core_host(true);  // test mode (no SDK call).
  FGFrameGenerationBackend backend(nullptr, std::vector<std::wstring>{},
                                   std::wstring(L""), &core_host);
  // All-null inputs fail the contract (no GetDesc on the null back buffer).
  CHECK(backend.TryProcess(nullptr, nullptr, nullptr, nullptr,
                           DlssFgFrameParams{}) ==
        BackendResult::kInputContractFailed);
  // Every non-success state falls back to the original path.
  CHECK(IsOriginalPathFallback(BackendResult::kInputContractFailed));
  // A zero guest (render) size is also an input contract failure (the backend
  // checks render_width/height after the null checks).
  DlssFgFrameParams zero_size_params;
  zero_size_params.render_width = 0;
  zero_size_params.render_height = 0;
  // Note: with a null command list the null check fires first, so this also
  // returns kInputContractFailed (the zero-size check is a separate branch).
  CHECK(backend.TryProcess(nullptr, nullptr, nullptr, nullptr,
                           zero_size_params) ==
        BackendResult::kInputContractFailed);
}

// The core host's Acquire with a null device (production mode) returns a clean
// failure (no crash, no reference-count change). The real SDK-crash SEH
// boundary is exercised on the target hardware (Milestone 5-C).
TEST_CASE("NGX core host Acquire null device -> clean failure", "[ngx]") {
  NgxCoreHost core_host(false);  // production mode.
  NgxCoreAcquireResult result =
      core_host.Acquire(nullptr, {}, "proj", "engine");
  CHECK(result.result != 0);
  CHECK(core_host.RefCount() == 0);
  CHECK_FALSE(core_host.IsInitialized());
}

// 5. Resource ownership.

// The real backend owns no GPU resources when constructed with a null device
// (no Initialize has run); its output getters return nullptr/0 and its Shutdown
// is clean and idempotent (it does not release a borrowed resource, since none
// was borrowed). The backend is not available after construction.
TEST_CASE("FG real backend owns no real resources (null device)", "[fg]") {
  NgxCoreHost core_host(true);
  FGFrameGenerationBackend backend(nullptr, std::vector<std::wstring>{},
                                   std::wstring(L""), &core_host);
  CHECK_FALSE(backend.IsAvailable());
  CHECK_FALSE(backend.IsUnavailable());
  CHECK(backend.OutputResource() == nullptr);
  CHECK(backend.OutputRealResource() == nullptr);
  CHECK(backend.OutputWidth() == 0);
  CHECK(backend.OutputHeight() == 0);
  // Zero-depth / motion-vector placeholders are also null (no Initialize).
  CHECK(backend.ZeroDepthPlaceholder() == nullptr);
  CHECK(backend.ZeroMotionVectorPlaceholder() == nullptr);
  backend.Shutdown();
  // After shutdown the backend is still not available.
  CHECK_FALSE(backend.IsAvailable());
  // Shutdown is idempotent (a second call is a no-op).
  backend.Shutdown();
  CHECK_FALSE(backend.IsAvailable());
}

// The backend holds the injected core host (the presenter passes the same core
// host to the SR and FG backends).
TEST_CASE("FG real backend holds the injected core host", "[fg]") {
  NgxCoreHost core_host(true);
  FGFrameGenerationBackend backend(nullptr, std::vector<std::wstring>{},
                                   std::wstring(L""), &core_host);
  CHECK(backend.core_host() == &core_host);
}

// 6. Shared core owner (reference counting).

// SR + FG both acquire -> one Init; SR releases -> core still alive; FG
// releases -> Shutdown1. (Test mode: pure reference counting, no SDK call.)
TEST_CASE("NGX core host shared reference counting", "[ngx]") {
  NgxCoreHost core_host(true);
  auto device = reinterpret_cast<ID3D12Device*>(0x1);
  std::vector<std::wstring> runtime_dirs = {L"."};
  std::string project_id = "proj";
  std::string engine_version = "engine";
  // The SR backend acquires (the first consumer initializes the core).
  auto sr_acquire =
      core_host.Acquire(device, runtime_dirs, project_id, engine_version);
  CHECK(sr_acquire.result == 0);
  CHECK(sr_acquire.core_initialized);
  CHECK(core_host.RefCount() == 1);
  CHECK(core_host.InitCount() == 1);
  // The FG backend acquires (the core is already initialized).
  auto fg_acquire =
      core_host.Acquire(device, runtime_dirs, project_id, engine_version);
  CHECK(fg_acquire.result == 0);
  CHECK_FALSE(fg_acquire.core_initialized);
  CHECK(core_host.RefCount() == 2);
  CHECK(core_host.InitCount() == 1);  // still one Init.
  // The SR backend releases (the FG backend still holds a reference).
  core_host.Release(device);
  CHECK(core_host.RefCount() == 1);
  CHECK(core_host.IsInitialized());
  CHECK(core_host.ShutdownCount() == 0);  // no shutdown yet.
  // The FG backend releases (the last consumer; the core shuts down).
  core_host.Release(device);
  CHECK(core_host.RefCount() == 0);
  CHECK_FALSE(core_host.IsInitialized());
  CHECK(core_host.ShutdownCount() == 1);
}

// A Release when the core was never initialized is a no-op (no crash).
TEST_CASE("NGX core host release when never initialized is a no-op", "[ngx]") {
  NgxCoreHost core_host(true);
  auto device = reinterpret_cast<ID3D12Device*>(0x1);
  core_host.Release(device);
  CHECK(core_host.RefCount() == 0);
  CHECK_FALSE(core_host.IsInitialized());
  CHECK(core_host.ShutdownCount() == 0);
}

}  // namespace ui
}  // namespace xe
