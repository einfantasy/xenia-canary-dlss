/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/dlss/ngx/ngx_core_host.h"

#include <windows.h>
#include <d3d12.h>

// The generic NGX SDK headers are included ONLY in this translation unit
// (license isolation). They trigger /W4 warnings, so they are wrapped in a
// warning suppression (mirrors the DLSS SR backend).
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4267 4310 4324 4456 4458 4514 4611 4820)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>
#pragma warning(pop)

#include "xenia/base/logging.h"

namespace xe {
namespace ui {

namespace {

// ---------------------------------------------------------------------------
// SEH boundaries.
//
// The SDK can crash (access violation), not just return an error code. So every
// core lifecycle entry point is wrapped in __try/__except. A crash is treated
// as a failure (NVSDK_NGX_Result_FAIL_PlatformError); the raw exception code
// is reported via the seh_code out-param. The __try scope must not contain any
// C++ object with a destructor, so these are free functions with only POD
// locals.
// ---------------------------------------------------------------------------

__declspec(noinline) NVSDK_NGX_Result CallInitWithProjectId(
    const char* project_id, const char* engine_version,
    const wchar_t* app_data_path,
    const NVSDK_NGX_FeatureCommonInfo* feature_info, ID3D12Device* device,
    uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, engine_version, app_data_path,
        device, feature_info, NVSDK_NGX_Version_API);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallShutdown1(ID3D12Device* device,
                                                    uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_Shutdown1(device);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallAllocateParameters(
    NVSDK_NGX_Parameter** out_parameters, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_AllocateParameters(out_parameters);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallDestroyParameters(
    NVSDK_NGX_Parameter* parameters, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_DestroyParameters(parameters);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
    result = NVSDK_NGX_Result_FAIL_PlatformError;
  }
  return result;
}

}  // namespace

NgxCoreHost::NgxCoreHost(bool test_mode) : test_mode_(test_mode ? 1 : 0) {}

NgxCoreHost::~NgxCoreHost() {
  // Destroy any still-live parameter blocks before the core Shutdown1
  // (reverse-order teardown, defense in depth). In the normal path the
  // backends have already destroyed their blocks; this is a no-op then.
  if (initialized_ && ref_count_ > 0) {
    XELOGW("NGX core: owner destroyed with {} live reference(s); forcing "
           "core shutdown.",
           ref_count_);
    uint32_t seh_code = 0;
    NVSDK_NGX_Result shutdown_result = CallShutdown1(device_, seh_code);
    if (shutdown_result != NVSDK_NGX_Result_Success) {
      XELOGW("NGX core: forced Shutdown1 failed (result={}, seh=0x{:08X}).",
             static_cast<int>(shutdown_result), seh_code);
    }
  }
}

NgxCoreAcquireResult NgxCoreHost::Acquire(
    ID3D12Device* device, const std::vector<std::wstring>& runtime_dirs,
    const std::string& project_id, const std::string& engine_version) {
  NgxCoreAcquireResult acquire_result;
  std::lock_guard<std::mutex> lock(mutex_);
  if (test_mode_) {
    // Host-side reference-counting seam: no SDK call.
    bool now_initialized = !initialized_;
    if (!initialized_) {
      initialized_ = true;
      device_ = device;
      ++init_count_;
    }
    ++ref_count_;
    acquire_result.result = 0;  // NVSDK_NGX_Result_Success.
    acquire_result.core_initialized = now_initialized;
    return acquire_result;
  }
  if (!device || runtime_dirs.empty() || project_id.empty() ||
      engine_version.empty()) {
    XELOGE("NGX core: Acquire called with a null device, an empty runtime "
           "path list, or an empty project identity; cannot initialize the "
           "core. Returning a failure.");
    acquire_result.result = static_cast<int>(NVSDK_NGX_Result_FAIL_InvalidParameter);
    return acquire_result;
  }
  if (initialized_) {
    if (device_ != device) {
      XELOGE("NGX core: Acquire with a different device than the one the core "
             "was initialized with; the core is per-device. Returning a "
             "failure (reference count unchanged).");
      acquire_result.result = static_cast<int>(NVSDK_NGX_Result_FAIL_InvalidParameter);
      return acquire_result;
    }
    // The core is already initialized (by a previous consumer). Just bump the
    // reference count.
    ++ref_count_;
    XELOGD("NGX core: Acquire (reference count now {}).", ref_count_);
    acquire_result.result = 0;  // NVSDK_NGX_Result_Success.
    acquire_result.core_initialized = false;
    return acquire_result;
  }
  // Build the PathList from the runtime directories. The SDK does not search
  // recursively, so each directory that may contain an NGX runtime DLL must be
  // listed explicitly. (Built in the normal scope so the __try scope below has
  // no C++ objects with destructors.)
  std::vector<const wchar_t*> path_list;
  path_list.reserve(runtime_dirs.size());
  for (const std::wstring& runtime_dir : runtime_dirs) {
    path_list.push_back(runtime_dir.c_str());
  }
  const uint32_t path_list_length = static_cast<uint32_t>(path_list.size());
  NVSDK_NGX_FeatureCommonInfo feature_info{};
  feature_info.PathListInfo.Path = const_cast<const wchar_t**>(path_list.data());
  feature_info.PathListInfo.Length = path_list_length;
  // ApplicationDataPath is the first runtime directory (the SDK writes its
  // logs/temp files there). It must be an absolute, writable local path.
  const std::wstring app_data_path = runtime_dirs.front();
  XELOGI("NGX core: Init_with_ProjectID start (path_list_length={}, "
         "project_id={}, engine_version={}).",
         path_list_length, project_id, engine_version);
  uint32_t seh_code = 0;
  NVSDK_NGX_Result init_result = CallInitWithProjectId(
      project_id.c_str(), engine_version.c_str(), app_data_path.c_str(),
      &feature_info, device, seh_code);
  if (init_result != NVSDK_NGX_Result_Success) {
    XELOGE("NGX core: Init_with_ProjectID failed (result={}, seh=0x{:08X}).",
           static_cast<int>(init_result), seh_code);
    acquire_result.result = static_cast<int>(init_result);
    acquire_result.seh_code = seh_code;
    return acquire_result;
  }
  initialized_ = true;
  device_ = device;
  ref_count_ = 1;
  ++init_count_;
  XELOGI("NGX core: initialized (shared core session for device {}; "
         "reference count {}).",
         static_cast<const void*>(device), ref_count_);
  acquire_result.result = 0;  // NVSDK_NGX_Result_Success.
  acquire_result.core_initialized = true;
  return acquire_result;
}

void NgxCoreHost::Release(ID3D12Device* device) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (test_mode_) {
    // Host-side reference-counting seam: no SDK call.
    if (ref_count_ > 0) {
      --ref_count_;
      if (ref_count_ == 0) {
        initialized_ = false;
        device_ = nullptr;
        ++shutdown_count_;
      }
    }
    return;
  }
  if (!initialized_) {
    return;  // no-op (the core was never initialized).
  }
  if (device != nullptr && device_ != nullptr && device != device_) {
    XELOGW("NGX core: Release with a device that does not match the core's "
           "device; ignoring (reference count unchanged).");
    return;
  }
  if (ref_count_ > 0) {
    --ref_count_;
  }
  if (ref_count_ == 0) {
    // The last consumer is gone: shut down the core session.
    uint32_t seh_code = 0;
    NVSDK_NGX_Result shutdown_result = CallShutdown1(device_, seh_code);
    if (shutdown_result != NVSDK_NGX_Result_Success) {
      XELOGW("NGX core: Shutdown1 failed (result={}, seh=0x{:08X}); the core "
             "session is discarded anyway.",
             static_cast<int>(shutdown_result), seh_code);
    } else {
      XELOGI("NGX core: Shutdown1 succeeded (reference count reached zero).");
    }
    initialized_ = false;
    device_ = nullptr;
    ++shutdown_count_;
  } else {
    XELOGD("NGX core: Release (reference count now {}).", ref_count_);
  }
}

NVSDK_NGX_Parameter* NgxCoreHost::AllocateParameters(uint32_t* seh_code) {
  if (seh_code) {
    *seh_code = 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (test_mode_) {
    // Host-side seam: return a stable non-null sentinel (no real SDK block).
    return reinterpret_cast<NVSDK_NGX_Parameter*>(0x1);
  }
  if (!initialized_) {
    XELOGE("NGX core: AllocateParameters before the core is initialized.");
    return nullptr;
  }
  NVSDK_NGX_Parameter* parameters = nullptr;
  uint32_t local_seh = 0;
  NVSDK_NGX_Result alloc_result = CallAllocateParameters(&parameters, local_seh);
  if (alloc_result != NVSDK_NGX_Result_Success || !parameters) {
    XELOGE("NGX core: AllocateParameters failed (result={}, seh=0x{:08X}).",
           static_cast<int>(alloc_result), local_seh);
    if (seh_code) {
      *seh_code = local_seh;
    }
    return nullptr;
  }
  if (seh_code) {
    *seh_code = local_seh;
  }
  return parameters;
}

void NgxCoreHost::DestroyParameters(NVSDK_NGX_Parameter* parameters,
                                    uint32_t* seh_code) {
  if (seh_code) {
    *seh_code = 0;
  }
  if (parameters == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (test_mode_) {
    return;  // no-op (the sentinel is not a real SDK block).
  }
  if (!initialized_) {
    XELOGE("NGX core: DestroyParameters after the core was shut down.");
    return;
  }
  uint32_t local_seh = 0;
  NVSDK_NGX_Result destroy_result = CallDestroyParameters(parameters, local_seh);
  if (destroy_result != NVSDK_NGX_Result_Success) {
    XELOGW("NGX core: DestroyParameters failed (result={}, seh=0x{:08X}); the "
           "parameter block is discarded anyway.",
           static_cast<int>(destroy_result), local_seh);
  }
  if (seh_code) {
    *seh_code = local_seh;
  }
}

uint32_t NgxCoreHost::RefCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ref_count_;
}

bool NgxCoreHost::IsInitialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

uint32_t NgxCoreHost::InitCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return init_count_;
}

uint32_t NgxCoreHost::ShutdownCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return shutdown_count_;
}

}  // namespace ui
}  // namespace xe
