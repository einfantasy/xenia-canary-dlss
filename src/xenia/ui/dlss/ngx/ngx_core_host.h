/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Shared NGX core owner (Milestone 5-B, prerequisite P0-1). The NVIDIA NGX
// SDK core is a per-device singleton: one Init_with_ProjectID per device, and
// a Shutdown1 that destroys the ENTIRE core session (every feature created
// through it: DLSS SR, Frame Generation, ...). Two independent backends (the
// DLSS SR backend and the DLSS Frame Generation backend) therefore share one
// core owner that reference-counts the core:
//
//   - The first successful Acquire() initializes the core (Init_with_ProjectID,
//     SEH boundary) and increments the reference count to 1.
//   - Later Acquire() calls (same device) just increment the reference count.
//   - Release() decrements the reference count; Shutdown1 (SEH boundary) runs
//     ONLY when the count reaches zero (the last consumer is gone).
//
// Parameter blocks (AllocateParameters / DestroyParameters) are also routed
// through this owner so reverse-order teardown is guaranteed (the SDK requires
// parameter blocks to be destroyed before the core Shutdown1; Veyra's
// NgxCoreHost follows the same rule).
//
// This module is the ONLY place in the codebase that calls the NGX core
// lifecycle entry points (Init_with_ProjectID / Shutdown1 / AllocateParameters
// / DestroyParameters). Feature-level entry points (GetFeatureRequirements,
// CreateFeature, EvaluateFeature, ReleaseFeature) stay with each backend
// module (dlss/* for SR, fg/* for FG).
//
// License isolation: the SDK headers are included ONLY in ngx_core_host.cc
// (never in this header); the SDK types are forward-declared here as opaque
// pointers. The SDK runtime DLLs are resolved at runtime by the static NGX
// core (zero DLL loads on the off path); this module links the static NGX core
// library (nvsdk_ngx_d.lib), which statically imports NO NVIDIA DLL.
//
// Thread safety: the reference count and the initialized flag are protected by
// a mutex. In the current integration both consumers (SR / FG backends) are
// created and driven on the paint thread, so no cross-thread race exists yet;
// the mutex is the requirement for the future UI-thread / paint-thread split.

#ifndef XENIA_UI_NGX_NGX_CORE_HOST_H_
#define XENIA_UI_NGX_NGX_CORE_HOST_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations. The D3D12 device is used only as a pointer in this
// header; the full d3d12.h is included in the .cc. NVSDK_NGX_Parameter is an
// SDK handle (opaque pointer) forward-declared so that the SDK headers are NOT
// pulled into this header (license isolation).
struct ID3D12Device;
struct NVSDK_NGX_Parameter;

namespace xe {
namespace ui {

// The result of an Acquire() call. The raw SDK result is carried as a plain
// int (the NVSDK_NGX_Result enum value) so that this header does not include
// the SDK; the caller (which does include the SDK headers) casts it back.
//   - result == 0 (NVSDK_NGX_Result_Success) means the Acquire succeeded.
//   - core_initialized is true when THIS call performed the Init_with_ProjectID
//     (the core was not initialized before); false when the core was already
//     initialized and this call only incremented the reference count.
struct NgxCoreAcquireResult {
  int result = -1;
  uint32_t seh_code = 0;
  bool core_initialized = false;
};

// A reference-counted, per-device owner of the shared NGX core session.
//
// The owner is NOT a process-wide singleton: it is per device. The presenter
// owns one instance and passes a non-owning pointer to each backend that
// consumes the core (the DLSS SR backend, the DLSS FG backend). The owner
// never destroys the device (the provider owns the device); it only manages
// the SDK core session lifetime.
//
// In test mode (constructed with test_mode = true) the owner performs pure
// reference-counting with NO SDK calls: Acquire() increments the count and
// marks the core initialized, Release() decrements it (and "shuts down" the
// core when the count reaches zero). The init/shutdown call counts are exposed
// so host-side tests can assert the reference-counting logic (one Init for N
// consumers, one Shutdown when the last consumer releases). Test mode is a
// host-side testing seam ONLY; it must never be interpreted as a real NGX core
// session, and it must never be used on the production path.
class NgxCoreHost {
 public:
  // `test_mode` selects the host-side reference-counting seam (no SDK calls).
  // The production path always uses test_mode = false.
  explicit NgxCoreHost(bool test_mode = false);
  ~NgxCoreHost();

  NgxCoreHost(const NgxCoreHost&) = delete;
  NgxCoreHost& operator=(const NgxCoreHost&) = delete;

  // Acquire a reference to the shared NGX core for `device`. If the core is
  // not yet initialized, it is initialized (Init_with_ProjectID, SEH boundary)
  // with the given runtime directories (the PathList the SDK scans for the
  // feature DLLs) and project identity (ProjectId / EngineVersion read from
  // ngx-local.json by the caller). On success the reference count is
  // incremented. On failure the reference count is unchanged and the raw SDK
  // result (plus any SEH exception code) is returned.
  //
  // The core is bound to the device of its FIRST successful Acquire. A later
  // Acquire with a DIFFERENT device is an input-contract failure (the core is
  // per-device); it returns a failure and leaves the reference count
  // unchanged.
  //
  // In test mode the Acquire performs no SDK call: it increments the reference
  // count and marks the core initialized (the init call count is incremented).
  NgxCoreAcquireResult Acquire(
      ID3D12Device* device,
      const std::vector<std::wstring>& runtime_dirs,
      const std::string& project_id,
      const std::string& engine_version);

  // Release a reference to the core for `device`. When the reference count
  // reaches zero the core is shut down (Shutdown1, SEH boundary). Safe to call
  // when the core is not initialized (no-op). A Release with a device that does
  // not match the core's device is a no-op (the reference count is unchanged).
  //
  // In test mode the Release performs no SDK call: it decrements the reference
  // count and marks the core uninitialized when the count reaches zero (the
  // shutdown call count is incremented).
  void Release(ID3D12Device* device);

  // Allocate a parameter block through the core owner. The block is tracked so
  // that a still-live block at owner destruction is destroyed before the core
  // Shutdown1 (reverse-order teardown, defense in depth). Returns nullptr on
  // failure (the caller checks). In test mode the allocation is a no-op that
  // returns a non-null sentinel (no real SDK block exists).
  NVSDK_NGX_Parameter* AllocateParameters(uint32_t* seh_code = nullptr);

  // Destroy a parameter block allocated through this owner. In test mode the
  // destruction is a no-op.
  void DestroyParameters(NVSDK_NGX_Parameter* parameters,
                         uint32_t* seh_code = nullptr);

  // The current reference count (diagnostics / host-side tests).
  uint32_t RefCount() const;
  // Whether the core is currently initialized (reference count > 0).
  bool IsInitialized() const;
  // The number of core initializations performed (1 after the first successful
  // Acquire; exposed for the host-side reference-counting tests).
  uint32_t InitCount() const;
  // The number of core shutdowns performed (1 after the reference count
  // reaches zero; exposed for the host-side reference-counting tests).
  uint32_t ShutdownCount() const;

 private:
  int test_mode_ = 0;  // 0 = production, 1 = host-side reference-counting seam.
  mutable std::mutex mutex_;
  bool initialized_ = false;
  ID3D12Device* device_ = nullptr;  // non-owning; the provider owns the device.
  uint32_t ref_count_ = 0;
  uint32_t init_count_ = 0;
  uint32_t shutdown_count_ = 0;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_NGX_NGX_CORE_HOST_H_
