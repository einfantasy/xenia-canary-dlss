/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/dlss/dlss_super_sampling_backend.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// The NVIDIA SDK headers are included ONLY in this translation unit (license
// isolation). They trigger /W4 warnings, so they are wrapped in a warning
// suppression.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4267 4310 4324 4456 4458 4514 4611 4820)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <nvsdk_ngx_params.h>
#pragma warning(pop)

#include "xenia/base/logging.h"
#include "xenia/base/filesystem.h"

#include <filesystem>
#include <fstream>
#include <sstream>

// rapidjson (header-only, vendored in third_party) parses the ngx-local.json
// config. The include dir is added via CMake (xenia-dlss target). It is wrapped
// in a warning suppression like the SDK headers, since it is third-party code.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4267 4310 4324 4456 4458 4514 4611 4820)
#include <rapidjson/document.h>
#pragma warning(pop)

namespace xe {
namespace ui {
namespace dlss {

namespace {

// nvsdk_ngx_defs.h only forward-declares CD3DX12_HEAP_PROPERTIES. The
// complete definition comes from a newer Windows SDK's d3d12.h, where it
// publicly derives from D3D12_HEAP_PROPERTIES. The official SDK sample
// (DLSS Programming Guide, section 6.2) passes InHeap straight to
// ID3D12Device::CreateCommittedResource, proving the first
// sizeof(D3D12_HEAP_PROPERTIES) bytes are a plain D3D12_HEAP_PROPERTIES.
// This machine's Windows SDK (10.0.26100.0) lacks the definition, so we
// provide it here: an empty extension of D3D12_HEAP_PROPERTIES, which lets
// the alloc callback upcast to const D3D12_HEAP_PROPERTIES*.
class CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES {};

// The default D3D12 heap properties (CPU page unknown, memory pool unknown,
// heap flags none). Defined locally so this module does not depend on the
// D3D12 provider utilities (keeping the SDK module self-contained and avoiding
// a circular dependency with the presenter target).
const D3D12_HEAP_PROPERTIES kDefaultHeapProperties = {
    D3D12_HEAP_TYPE_DEFAULT};

// The resource allocation callback is a C function pointer with no way to pass
// the device. The backend is a single instance per device, so a file-scope
// device pointer (set in Initialize, cleared in Shutdown) is safe.
ID3D12Device* g_ngx_device = nullptr;

// The app implements this so the SDK can allocate its internal resources.
// (SDK guide section 6.2.)
void NVSDK_CONV NGXResourceAllocCallback(D3D12_RESOURCE_DESC* in_desc,
                                        int in_state,
                                        CD3DX12_HEAP_PROPERTIES* in_heap,
                                        ID3D12Resource** out_resource) {
  if (g_ngx_device && in_desc && in_heap && out_resource) {
    g_ngx_device->CreateCommittedResource(
        in_heap, D3D12_HEAP_FLAG_NONE, in_desc,
        static_cast<D3D12_RESOURCE_STATES>(in_state), nullptr,
        IID_PPV_ARGS(out_resource));
  }
}

void NVSDK_CONV NGXResourceReleaseCallback(IUnknown* in_resource) {
  if (in_resource) {
    in_resource->Release();
  }
}

// C-style callback (NVSDK_CONV) that pipes NGX SDK log lines into Xenia's
// logging. The SDK may call it from any thread, so it must be thread-safe and
// free of C++ objects with destructors. Every line (ON and VERBOSE) is logged
// so the cause of a failed capability check (e.g. InvalidParameter) is visible
// in the target machine's log.
void NVSDK_CONV NGXAppLogCallback(const char* message,
                                  NVSDK_NGX_Logging_Level loggingLevel,
                                  NVSDK_NGX_Feature sourceComponent) {
  if (message) {
    XELOGI("DLSS SR: NGX SDK [level={} feature={}] {}",
           static_cast<int>(loggingLevel), static_cast<int>(sourceComponent),
           message);
  }
}

// ---------------------------------------------------------------------------
// SEH boundaries.
//
// The SDK can crash (access violation), not just return an error code. So every
// SDK entry point is wrapped in __try/__except. A crash is treated as a generic
// failure (the initial NVSDK_NGX_Result_Fail value); the raw exception code is
// reported via the seh_code out-param. The __try scope must not contain any C++
// object with a destructor, so these are free functions with only POD locals.
// ---------------------------------------------------------------------------

__declspec(noinline) NVSDK_NGX_Result CallGetFeatureRequirements(
    IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* discovery,
    NVSDK_NGX_FeatureRequirement* out_supported, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_GetFeatureRequirements(adapter, discovery,
                                                   out_supported);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallInitWithProjectId(
    const char* project_id, const char* engine_version,
    const wchar_t* app_data_path,
    const NVSDK_NGX_FeatureCommonInfo* feature_info,
    ID3D12Device* device, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_Init_with_ProjectID(
        project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, engine_version, app_data_path, device,
        feature_info, NVSDK_NGX_Version_API);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
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
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallCreateDlss(
    ID3D12GraphicsCommandList* command_list, NVSDK_NGX_Handle** out_handle,
    NVSDK_NGX_Parameter* parameter_block,
    NVSDK_NGX_DLSS_Create_Params* create_params, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NGX_D3D12_CREATE_DLSS_EXT(command_list, 1, 1, out_handle,
                                      parameter_block, create_params);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallEvaluateDlss(
    ID3D12GraphicsCommandList* command_list, NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameter_block,
    NVSDK_NGX_D3D12_DLSS_Eval_Params* eval_params, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NGX_D3D12_EVALUATE_DLSS_EXT(command_list, handle, parameter_block,
                                        eval_params);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallReleaseFeature(
    NVSDK_NGX_Handle* handle, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NVSDK_NGX_D3D12_ReleaseFeature(handle);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
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
  }
  return result;
}

// Maps an NVSDK_NGX_Result to a BackendResult for initialization failures.
BackendResult InitFailureToBackendResult(NVSDK_NGX_Result result) {
  switch (result) {
    case NVSDK_NGX_Result_Success:
      return BackendResult::kSuccess;
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
    case NVSDK_NGX_Result_FAIL_PlatformError:
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
      // Capability / platform / init failure: cache as unavailable.
      return BackendResult::kUnavailable;
    default:
      // Any other init error (invalid parameter, out of memory, ...).
      return BackendResult::kInitFailed;
  }
}

// Default ngx-local.json content, written when the config file is missing.
// These match the former hardcoded ProjectId/EngineVersion, so a missing
// config behaves exactly as before (no behavior change when no config exists).
const char kDefaultNgxConfigJson[] = R"({
    "engineType": "custom",
    "engineVersion": "xenia-canary-dlss",
    "ngxProjectId": "8562ea46-f1a4-4c0c-9f18-fec4fa0db830"
})";

// Display name of a quality preset for the log lines. The mapping from the
// Xenia-side enum to the SDK enum is a plain cast (same numeric values, see
// nvsdk_ngx_defs.h:251-260); the name is only for logging.
const char* PerfQualityName(ui::DlssSrPerfQuality quality) {
  switch (quality) {
    case ui::DlssSrPerfQuality::kMaxPerf:
      return "MaxPerf";
    case ui::DlssSrPerfQuality::kBalanced:
      return "Balanced";
    case ui::DlssSrPerfQuality::kMaxQuality:
      return "MaxQuality";
    case ui::DlssSrPerfQuality::kUltraPerformance:
      return "UltraPerformance";
  }
  return "Invalid";
}

  // Display name of a render preset for the log lines. The mapping from the
  // Xenia-side enum to the SDK parameter value is a plain cast (same numeric
  // values, see nvsdk_ngx_defs.h:70-88); the name is only for logging.
  const char* RenderPresetName(ui::DlssSrRenderPreset preset) {
    switch (preset) {
      case ui::DlssSrRenderPreset::kDefault:
        return "Default";
      case ui::DlssSrRenderPreset::kJ:
        return "J";
      case ui::DlssSrRenderPreset::kK:
        return "K";
      case ui::DlssSrRenderPreset::kL:
        return "L";
      case ui::DlssSrRenderPreset::kM:
        return "M";
    }
    return "Invalid";
  }

}  // namespace

DLSSSuperSamplingBackend::DLSSSuperSamplingBackend(
    ID3D12Device* device, std::vector<std::wstring> runtime_dirs,
    std::wstring config_path, ui::DlssSrPerfQuality perf_quality)
    : device_(device),
      runtime_dirs_(std::move(runtime_dirs)),
      config_path_(std::move(config_path)),
      created_perf_quality_(perf_quality) {}

DLSSSuperSamplingBackend::~DLSSSuperSamplingBackend() { Shutdown(); }

bool DLSSSuperSamplingBackend::IsAvailable() const {
  return state_ == State::kAvailable;
}

bool DLSSSuperSamplingBackend::IsUnavailable() const {
  return state_ == State::kUnavailable;
}

ID3D12Resource* DLSSSuperSamplingBackend::OutputResource() const {
  return output_texture_.Get();
}

uint32_t DLSSSuperSamplingBackend::OutputWidth() const {
  return output_width_;
}

uint32_t DLSSSuperSamplingBackend::OutputHeight() const {
  return output_height_;
}

void DLSSSuperSamplingBackend::Shutdown() {
  if (state_ == State::kNotInitialized) {
    return;
  }
  XELOGI("DLSS SR: backend shutdown (input={}x{}, output={}x{}).",
         input_width_, input_height_, output_width_, output_height_);
  ReleaseFeature();
  uint32_t seh_code = 0;
  NVSDK_NGX_Result shutdown_result = CallShutdown1(device_, seh_code);
  if (shutdown_result != NVSDK_NGX_Result_Success) {
    XELOGW("DLSS SR: SDK Shutdown1 failed (result={}, seh=0x{:08X}); the SDK "
           "session is discarded anyway.",
           static_cast<int>(shutdown_result), seh_code);
  } else {
    XELOGI("DLSS SR: SDK Shutdown1 succeeded.");
  }
  g_ngx_device = nullptr;
  output_texture_.Reset();
  upload_buffer_.Reset();
  output_in_pixel_shader_resource_ = false;
  input_width_ = 0;
  input_height_ = 0;
  output_width_ = 0;
  output_height_ = 0;
  state_ = State::kNotInitialized;
}

void DLSSSuperSamplingBackend::ReleaseFeature() {
  if (feature_handle_) {
    uint32_t seh_code = 0;
    NVSDK_NGX_Result release_result =
        CallReleaseFeature(feature_handle_, seh_code);
    if (release_result != NVSDK_NGX_Result_Success) {
      XELOGW("DLSS SR: ReleaseFeature failed (result={}, seh=0x{:08X}); the "
             "SDK handle is discarded anyway.",
             static_cast<int>(release_result), seh_code);
    } else {
      XELOGD("DLSS SR: ReleaseFeature succeeded.");
    }
    feature_handle_ = nullptr;
  }
  if (parameter_block_) {
    uint32_t seh_code = 0;
    NVSDK_NGX_Result destroy_result =
        CallDestroyParameters(parameter_block_, seh_code);
    if (destroy_result != NVSDK_NGX_Result_Success) {
      XELOGW("DLSS SR: DestroyParameters failed (result={}, seh=0x{:08X}); the "
             "parameter block is discarded anyway.",
             static_cast<int>(destroy_result), seh_code);
    } else {
      XELOGD("DLSS SR: DestroyParameters succeeded.");
    }
    parameter_block_ = nullptr;
  }
}

bool DLSSSuperSamplingBackend::EnsureConfigFile() const {
  const std::filesystem::path path(config_path_);
  if (std::filesystem::exists(path)) {
    // Existing config is the user's source of truth; never overwrite it.
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << kDefaultNgxConfigJson;
  out.flush();
  return static_cast<bool>(out);
}

bool DLSSSuperSamplingBackend::LoadNgxConfig(std::string* project_id_out,
                                            std::string* engine_version_out) const {
  std::ifstream in(config_path_);
  if (!in) {
    return false;
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  rapidjson::Document doc;
  doc.Parse(contents.str().c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return false;
  }
  if (!doc.HasMember("ngxProjectId") || !doc["ngxProjectId"].IsString() ||
      !doc.HasMember("engineVersion") || !doc["engineVersion"].IsString()) {
    return false;
  }
  *project_id_out = doc["ngxProjectId"].GetString();
  *engine_version_out = doc["engineVersion"].GetString();
  return !project_id_out->empty() && !engine_version_out->empty();
}

BackendResult DLSSSuperSamplingBackend::Initialize(
    ID3D12GraphicsCommandList* command_list, uint32_t input_width,
    uint32_t input_height, uint32_t output_width, uint32_t output_height,
      ui::DlssSrPerfQuality perf_quality, ui::DlssSrRenderPreset render_preset) {
  if (!device_ || !command_list) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: Initialize called with a null device or command_list; "
             "cannot create the SDK session. Returning kInputContractFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }

  // The quality preset is a create-time parameter, passed per frame and baked
  // into CreateFeature. An out-of-range value is a creation failure: cache as
  // unavailable (no retry, per the lazy-init contract) and report the input
  // contract failure to the presenter. It is never clamped or silently passed
  // to the SDK.
  if (static_cast<uint8_t>(perf_quality) > 3) {
    XELOGW("DLSS SR: invalid quality preset {} (must be 0..3; UltraQuality is "
           "not supported by the current DLSS runtime); caching the "
           "backend as unavailable.",
           static_cast<uint8_t>(perf_quality));
    state_ = State::kUnavailable;
    return BackendResult::kInputContractFailed;
  }

    // The render preset is also a create-time parameter (one value per quality
    // mode). Only Default (=0, "do not send the hint") and the four current
    // presets J/K/L/M (=10..13) are legal; 1..4 (A-D, removed), 5..6
    // (E-F, deprecated), 7..9 (G-I, reserved) and 14..15 (N-O, reserved) are
    // all unusable. An out-of-range value is an input contract failure: cache
    // as unavailable (no retry) and never clamp or silently pass to the SDK.
    const uint8_t render_preset_value = static_cast<uint8_t>(render_preset);
    if (render_preset_value != 0 &&
        (render_preset_value < 10 || render_preset_value > 13)) {
      XELOGW("DLSS SR: invalid render preset {} (legal values are 0 and "
             "10..13; the others are removed/deprecated/reserved in the SDK "
             "header); caching the backend as unavailable.",
             static_cast<int>(render_preset_value));
      state_ = State::kUnavailable;
      return BackendResult::kInputContractFailed;
    }

  // Diagnostic (logging only): log the device's adapter LUID so that on the
  // test machine we can confirm the device sits on a real physical adapter
  // (the RTX 4090) even when the capability check below fails. This is a
  // standard D3D12 QI (no SDK involvement, no state change) whose result feeds
  // only a log line. It distinguishes a structural QI failure (the device is a
  // valid NVIDIA device but ID3D12Device simply does not expose IDXGIDevice1)
  // from an environmental one (a real adapter is missing).
  Microsoft::WRL::ComPtr<ID3D12Device3> device3;
  bool luid_available = false;
  LUID adapter_luid{};
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&device3)))) {
    adapter_luid = device3->GetAdapterLuid();
    luid_available = true;
  }
  XELOGD("DLSS SR: Initialize entry (state={}, input={}x{}, output={}x{}, "
         "runtime_dirs={}, adapter_luid={:04X}:{:04X}).",
         static_cast<int>(state_), input_width, input_height, output_width,
         output_height, runtime_dirs_.size(),
         luid_available ? adapter_luid.HighPart : 0,
         luid_available ? adapter_luid.LowPart : 0);

  // 1. Capability check (CPU side, before any command is recorded). The provider
  //    releases the adapter it used to create device_ (retaining it makes the
  //    NVIDIA driver reject later CreateCommittedResource calls with E_INVALIDARG).
  //    So the backend enumerates adapters itself and matches by LUID to find the
  //    physical adapter device_ sits on. The matched adapter is used only for the
  //    capability check, then released. Reuses the LUID resolved by the
  //    diagnostic QI above (no second QueryInterface).
  if (!luid_available) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: device does not expose ID3D12Device3 (QueryInterface "
             "failed); cannot resolve the adapter LUID. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: CreateDXGIFactory1 failed; cannot enumerate adapters for "
             "the capability check. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  Microsoft::WRL::ComPtr<IDXGIAdapter1> matched_adapter;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enum_hr = factory->EnumAdapters1(i, adapter.GetAddressOf());
    if (enum_hr == DXGI_ERROR_NOT_FOUND || FAILED(enum_hr)) {
      break;  // end of enumeration (or unexpected error)
    }
    // Match by the adapter LUID embedded in its description. (GetDesc1 is used
    // rather than IDXGIAdapter1::GetLuid because this toolchain's dxgi.h trims
    // the latter; GetDesc1 is present in every SDK and carries the same LUID.)
    DXGI_ADAPTER_DESC1 desc1{};
    if (SUCCEEDED(adapter->GetDesc1(&desc1)) &&
        desc1.AdapterLuid.HighPart == adapter_luid.HighPart &&
        desc1.AdapterLuid.LowPart == adapter_luid.LowPart) {
      matched_adapter = adapter;
      break;
    }
  }
  if (!matched_adapter) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: no DXGI adapter with LUID {:04X}:{:04X} was found; cannot "
             "run the DLSS capability check. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  XELOGD("DLSS SR: matched DXGI adapter LUID {:04X}:{:04X} for the capability "
         "check.",
         adapter_luid.HighPart, adapter_luid.LowPart);
  // Build the PathList from the runtime directories. The SDK does not search
  // recursively, so each directory that may contain an NGX runtime DLL must be
  // listed explicitly.
  std::vector<const wchar_t*> path_list;
  path_list.reserve(runtime_dirs_.size());
  for (const std::wstring& runtime_dir : runtime_dirs_) {
    path_list.push_back(runtime_dir.c_str());
  }
  const uint32_t path_list_length = static_cast<uint32_t>(path_list.size());
  if (!path_list_length) {
    if (!init_early_failure_logged_) {
      XELOGE("DLSS SR: no runtime directories were provided (the runtime path "
             "list is empty); the NGX runtime DLLs cannot be located. "
             "Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  NVSDK_NGX_FeatureCommonInfo discovery_common{};
  discovery_common.PathListInfo.Path =
      const_cast<const wchar_t**>(path_list.data());
  discovery_common.PathListInfo.Length = path_list_length;
  // Pipe the SDK's internal log lines into Xenia's logging so the cause of a
  // failed capability check (e.g. InvalidParameter) is visible on the target.
  // If need to check logs from NGX SDK Side, toggle this comment.
  // discovery_common.LoggingInfo.LoggingCallback = NGXAppLogCallback;
  // discovery_common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
  // Load the ProjectId / EngineVersion from ngx-local.json (auto-generating a
  // default config if the file is missing). A missing/malformed config is a
  // persistent failure: log once and return kInitFailed (state stays
  // kNotInitialized, so Initialize is retried next frame; if the user fixes the
  // config, the backend recovers). No behavior change when no config exists:
  // the auto-generated defaults match the former hardcoded values.
  std::string project_id;
  std::string engine_version;
  if (!EnsureConfigFile() || !LoadNgxConfig(&project_id, &engine_version)) {
    if (!config_failure_logged_) {
      XELOGE("DLSS SR: failed to load the ngx-local.json config at {}; the "
             "DLSS ProjectId/EngineVersion are unavailable. Returning "
             "kInitFailed.",
             xe::path_to_utf8(config_path_));
      config_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  NVSDK_NGX_FeatureDiscoveryInfo discovery{};
  discovery.SDKVersion = NVSDK_NGX_Version_API;
  discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  discovery.Identifier.IdentifierType =
      NVSDK_NGX_Application_Identifier_Type_Project_Id;
  discovery.Identifier.v.ProjectDesc.ProjectId = project_id.c_str();
  discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  discovery.Identifier.v.ProjectDesc.EngineVersion = engine_version.c_str();
  // ApplicationDataPath is the same experimental runtime directory the
  // PathList points at (the SDK writes its logs/temp files there). It is an
  // absolute, writable local path -- the SDK rejects the relative ".".
  discovery.ApplicationDataPath = runtime_dirs_[0].c_str();
  discovery.FeatureInfo = &discovery_common;
  // Diagnostic (logging only): print the exact paths handed to the NGX SDK so
  // the runtime layout can be verified on the target machine. PathList is the
  // list of directories the SDK scans for nvngx_dlss.dll / nvngxruntime.dll;
  // ApplicationDataPath (the exe directory) is where the SDK stores logs and
  // temp files; the CWD is logged for comparison. The log sink is narrow
  // (UTF-8), so wide paths are converted first.
  XELOGI("DLSS SR: capability check ApplicationDataPath = {}",
         xe::path_to_utf8(discovery.ApplicationDataPath));
  NVSDK_NGX_FeatureRequirement requirement{};
  uint32_t seh_code = 0;
  NVSDK_NGX_Result requirement_result = CallGetFeatureRequirements(
      matched_adapter.Get(), &discovery, &requirement, seh_code);
  // The adapter is used only for the capability check above. Release it now;
  // the rest of Initialize (SDK init, param alloc, texture create, CreateFeature)
  // uses only device_.
  matched_adapter.Reset();
  if (requirement_result != NVSDK_NGX_Result_Success ||
      requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
    // Capability check failed: the GPU / driver does not support DLSS SR.
    XELOGE("DLSS SR: capability check failed (result={}, supported={}, "
           "seh=0x{:08X}); GPU/driver/runtime does not support DLSS SR. Caching "
           "as unavailable (no retry).",
           static_cast<int>(requirement_result),
           static_cast<int>(requirement.FeatureSupported), seh_code);
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  XELOGD("DLSS SR: capability check passed (result={}, supported={}); the "
         "GPU/driver/runtime supports DLSS SR. Proceeding with SDK init.",
         static_cast<int>(requirement_result),
         static_cast<int>(requirement.FeatureSupported));

  // 2. Initialize the SDK session (CPU side).
  XELOGD("DLSS SR: SDK Init_with_ProjectID start (path_list_length={}).",
         path_list_length);
  NVSDK_NGX_Result init_result = CallInitWithProjectId(
      project_id.c_str(), engine_version.c_str(), runtime_dirs_[0].c_str(),
      &discovery_common, device_, seh_code);
  if (init_result != NVSDK_NGX_Result_Success) {
    XELOGE("DLSS SR: SDK Init failed (result={}, seh=0x{:08X}).",
           static_cast<int>(init_result), seh_code);
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(init_result);
  }
  g_ngx_device = device_;
  XELOGD("DLSS SR: SDK Init succeeded.");

  // 3. Allocate the parameter block (CPU side).
  XELOGD("DLSS SR: AllocateParameters start.");
  NVSDK_NGX_Result alloc_result = CallAllocateParameters(&parameter_block_,
                                                        seh_code);
  if (alloc_result != NVSDK_NGX_Result_Success || !parameter_block_) {
    XELOGE("DLSS SR: parameter block allocation failed (result={}, "
           "seh=0x{:08X}).",
           static_cast<int>(alloc_result), seh_code);
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(alloc_result);
  }
  // Register the resource allocation / release callbacks so the SDK can
  // allocate its internal resources. (SDK guide section 6.2.)
  parameter_block_->Set(NVSDK_NGX_Parameter_ResourceAllocCallback,
                        reinterpret_cast<unsigned long long>(
                            &NGXResourceAllocCallback));
  parameter_block_->Set(NVSDK_NGX_Parameter_ResourceReleaseCallback,
                        reinterpret_cast<unsigned long long>(
                            &NGXResourceReleaseCallback));

    // Render preset hint (create-time, one value per quality mode). kDefault
    // means "do not send the hint at all": identical to the current verified
    // baseline (the SDK then resolves the mode's default preset). Only an
    // explicit J/K/L/M value is sent, and only to the parameter of the quality
    // mode this feature was created with.
    if (render_preset != ui::DlssSrRenderPreset::kDefault) {
      const char* render_preset_param = nullptr;
      switch (perf_quality) {
        case ui::DlssSrPerfQuality::kMaxPerf:
          render_preset_param =
              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
          break;
        case ui::DlssSrPerfQuality::kBalanced:
          render_preset_param =
              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
          break;
        case ui::DlssSrPerfQuality::kMaxQuality:
          render_preset_param =
              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
          break;
        case ui::DlssSrPerfQuality::kUltraPerformance:
          render_preset_param =
              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
          break;
      }
      if (render_preset_param) {
        NVSDK_NGX_Parameter_SetUI(
            parameter_block_, render_preset_param,
            static_cast<unsigned int>(render_preset));
        XELOGD("DLSS SR: render preset hint {} sent for the {} quality mode.",
               RenderPresetName(render_preset), PerfQualityName(perf_quality));
      }
    }

  // 4. Create the backend output texture (ALLOW_UNORDERED_ACCESS, UAV state).
  D3D12_RESOURCE_DESC output_desc{};
  output_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  output_desc.Alignment = 0;
  output_desc.Width = output_width;
  output_desc.Height = output_height;
  output_desc.DepthOrArraySize = 1;
  output_desc.MipLevels = 1;
  output_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  output_desc.SampleDesc.Count = 1;
  output_desc.SampleDesc.Quality = 0;
  output_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(device_->CreateCommittedResource(
          &kDefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &output_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(&output_texture_)))) {
    XELOGE("DLSS SR: CreateCommittedResource for the backend output texture "
           "failed ({}x{}, R10G10B10A2_UNORM, ALLOW_UNORDERED_ACCESS). "
           "Caching as unavailable.",
           output_width, output_height);
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  output_in_pixel_shader_resource_ = false;  // starts in UAV state.
  XELOGD("DLSS SR: backend output texture created ({}x{}, R10G10B10A2_UNORM, "
         "UAV state).",
         output_width, output_height);

  // 4a. Zero-filled placeholder depth and motion-vector textures. The DLSS SR
  //     feature (NGXDLAA) requires both inputs; Xbox 360 games produce neither,
  //     so we feed all-zero values ("honest fallback"). The SDK's compute
  //     shader binds both as UAVs (unordered access), so after zero-init they
  //     must be left in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE. The
  //     SDK's D3D12 helper performs no state transitions of its own, so leaving
  //     them in PIXEL_SHADER_RESOURCE faults the GPU and loses the device. The
  //     SDK reads them and never writes their contents.
  //     TODO: Replace with real depth estimation and NVOF-derived motion vectors.
  auto create_zero_tex = [&](DXGI_FORMAT fmt)
      -> Microsoft::WRL::ComPtr<ID3D12Resource> {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = input_width;
    td.Height = input_height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    if (FAILED(device_->CreateCommittedResource(&kDefaultHeapProperties,
            D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&r)))) {
      return nullptr;
    }
    return r;
  };
  depth_texture_ = create_zero_tex(DXGI_FORMAT_R32_FLOAT);
  motion_vectors_texture_ = create_zero_tex(DXGI_FORMAT_R16G16_FLOAT);
  if (!depth_texture_ || !motion_vectors_texture_) {
    XELOGE("DLSS SR: failed to create zero-filled depth/motion textures.");
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Zero-init: CPU upload buffer (memset 0) + CopyTextureRegion per texture.
  // The upload buffer is a MEMBER (upload_buffer_), not a local: the
  // CopyTextureRegion that reads it is a GPU command executed at the end of the
  // first frame, AFTER Initialize() returns. A local would be released too
  // early and fault the GPU (device removed 0x887A0006).
  const size_t zpp = 4;  // R32_FLOAT and R16G16_FLOAT are both 4 B/pixel.
  const size_t up_row =
      (static_cast<size_t>(input_width) * zpp + 255) & ~size_t(255);
  const size_t up_size = up_row * input_height;
  {
    D3D12_HEAP_PROPERTIES up_props{};
    up_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = up_size;
    up_desc.Height = 1;
    up_desc.DepthOrArraySize = 1;
    up_desc.MipLevels = 1;
    up_desc.SampleDesc.Count = 1;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&up_props, D3D12_HEAP_FLAG_NONE,
            &up_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload_buffer_)))) {
      XELOGE("DLSS SR: failed to create upload buffer for zero-init.");
      state_ = State::kUnavailable;
      return BackendResult::kInitFailed;
    }
    void* mapped = nullptr;
    if (FAILED(upload_buffer_->Map(0, nullptr, &mapped))) {
      XELOGE("DLSS SR: failed to map upload buffer for zero-init.");
      state_ = State::kUnavailable;
      return BackendResult::kInitFailed;
    }
    memset(mapped, 0, up_size);
    upload_buffer_->Unmap(0, nullptr);
  }
  auto zero_copy = [&](ID3D12Resource* tex, DXGI_FORMAT fmt) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &b);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    fp.Footprint.Format = fmt;
    fp.Footprint.Width = input_width;
    fp.Footprint.Height = input_height;
    fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = static_cast<UINT>(up_row);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload_buffer_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b2{};
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition.pResource = tex;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &b2);
  };
  zero_copy(depth_texture_.Get(), DXGI_FORMAT_R32_FLOAT);
  zero_copy(motion_vectors_texture_.Get(), DXGI_FORMAT_R16G16_FLOAT);
  XELOGD("DLSS SR: zero-filled depth (R32F) and motion-vector (R16G16F) "
         "textures created ({}x{}).",
         input_width, input_height);

  NVSDK_NGX_DLSS_Create_Params create_params{};
  create_params.Feature.InWidth = input_width;
  create_params.Feature.InHeight = input_height;
  create_params.Feature.InTargetWidth = output_width;
  create_params.Feature.InTargetHeight = output_height;
  create_params.Feature.InPerfQualityValue =
      static_cast<NVSDK_NGX_PerfQuality_Value>(
          static_cast<uint8_t>(perf_quality));
  create_params.InFeatureCreateFlags =
      static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
  create_params.InEnableOutputSubrects = 0;
  XELOGD("DLSS SR: CreateFeature start (input={}x{} -> output={}x{}).",
         input_width, input_height, output_width, output_height);
  NVSDK_NGX_Result create_result = CallCreateDlss(command_list, &feature_handle_,
                                                 parameter_block_,
                                                 &create_params, seh_code);
  if (create_result != NVSDK_NGX_Result_Success || !feature_handle_) {
    // GPU-side CreateFeature failure: the presenter will Reset the command
    // list. Cache as unavailable so we do not retry the SDK.
    XELOGE("DLSS SR: CreateFeature (GPU) failed (result={}, seh=0x{:08X}). "
           "Caching as unavailable.",
           static_cast<int>(create_result), seh_code);
    ReleaseFeature();
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(create_result);
  }

  input_width_ = input_width;
  input_height_ = input_height;
  output_width_ = output_width;
  output_height_ = output_height;
  created_perf_quality_ = perf_quality;
    created_render_preset_ = render_preset;
  state_ = State::kAvailable;
    XELOGI("DLSS SR: backend initialized (input={}x{}, output={}x{}, "
           "PerfQuality={}, RenderPreset={}).",
         input_width, input_height, output_width, output_height,
           PerfQualityName(perf_quality), RenderPresetName(render_preset));
  return BackendResult::kSuccess;
}

BackendResult DLSSSuperSamplingBackend::TryProcess(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* guest_output,
    uint32_t output_width, uint32_t output_height,
    const Presenter::GuestOutputProperties& guest_output_properties,
    const DlssSrFrameParams& frame_params,
      ui::DlssSrPerfQuality requested_quality,
      ui::DlssSrRenderPreset requested_render_preset,
      ID3D12Resource* external_motion_vectors,
      ID3D12Resource* external_depth) {
  // 0. Cached unavailable: do not retry the SDK.
  if (state_ == State::kUnavailable) {
    return BackendResult::kUnavailable;
  }
  if (!command_list || !guest_output) {
    if (!contract_failure_logged_) {
      XELOGW("DLSS SR: input contract failed: null command_list or "
             "guest_output.");
      contract_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }
  const uint32_t input_width = guest_output_properties.frontbuffer_width;
  const uint32_t input_height = guest_output_properties.frontbuffer_height;
  // Input contract: all sizes must be non-zero.
  if (!input_width || !input_height || !output_width || !output_height) {
    if (!contract_failure_logged_) {
      XELOGW("DLSS SR: input contract failed: zero size (input={}x{}, "
             "output={}x{}).",
             input_width, input_height, output_width, output_height);
      contract_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }
  // 1:1 bypass: the caller (presenter) should skip the backend in this case,
  // but guard here as well.
  if (input_width == output_width && input_height == output_height) {
    if (!contract_failure_logged_) {
      XELOGW("DLSS SR: input contract failed: 1:1 (input={}x{} == output={}x{}); "
             "the presenter should have bypassed the backend.",
             input_width, input_height, output_width, output_height);
      contract_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }

    // Lazy init / re-create on size, quality, or render-preset change.
  bool just_initialized = false;
  if (state_ == State::kNotInitialized ||
      input_width != input_width_ || input_height != input_height_ ||
      output_width != output_width_ || output_height != output_height_ ||
        requested_quality != created_perf_quality_ ||
        requested_render_preset != created_render_preset_) {
    if (state_ == State::kAvailable) {
        // Size, quality, or render preset changed: release the feature and
        // re-create. (The presenter has already waited for the previous
        // frame's command list to complete, so the release is safe per the
        // SDK's Feature Disposal section.)
      ReleaseFeature();
      output_texture_.Reset();
      state_ = State::kNotInitialized;
    }
    BackendResult init_result =
        Initialize(command_list, input_width, input_height, output_width,
                     output_height, requested_quality,
                     requested_render_preset);
    if (init_result != BackendResult::kSuccess) {
      return init_result;
    }
    // After a fresh Initialize the output texture is in UAV state, so the first
    // evaluate does not need a "to UAV" barrier.
    output_in_pixel_shader_resource_ = false;
    just_initialized = true;
  }

  // The backend is now available and sized correctly. Record the per-frame
  // commands (barriers + EvaluateFeature) into the command list.

  // Barrier 1: guest output PIXEL_SHADER_RESOURCE -> NON_PIXEL_SHADER_RESOURCE
  // (the SDK's compute shader reads the input).
  D3D12_RESOURCE_BARRIER barrier_input{};
  barrier_input.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_input.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_input.Transition.pResource = guest_output;
  barrier_input.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_input.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier_input.Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_input);

  // Barrier 2 (not the first frame): backend output PIXEL_SHADER_RESOURCE ->
  // UNORDERED_ACCESS (the SDK writes the output in UAV state).
  if (output_in_pixel_shader_resource_) {
    D3D12_RESOURCE_BARRIER barrier_output_to_uav{};
    barrier_output_to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_output_to_uav.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier_output_to_uav.Transition.pResource = output_texture_.Get();
    barrier_output_to_uav.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_output_to_uav.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier_output_to_uav.Transition.StateAfter =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    command_list->ResourceBarrier(1, &barrier_output_to_uav);
    output_in_pixel_shader_resource_ = false;
  }

  // Record EvaluateFeature (GPU command). The SDK reads the input (in
  // NON_PIXEL_SHADER_RESOURCE), writes the output (in UAV), and restores the
  // input to NON_PIXEL_SHADER_RESOURCE and the output to UAV.
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval_params{};
  eval_params.Feature.pInColor = guest_output;
  eval_params.Feature.pInOutput = output_texture_.Get();
  // External MV / depth handoff (Milestone 4-E). When the caller provides a
  // valid external motion-vector texture (from the NVOF backend) or an
  // external depth texture, bind those instead of the internal zero-filled
  // placeholders. Both are borrowed read-only (NPSR/UAV, the same state the
  // SDK's compute shader expects). nullptr = use the internal placeholder
  // (behavior identical to before this handoff existed). The SDK binds both
  // as UAVs and reads them without writing, so the NVOF MV output (written
  // in UAV state by the densify pass) is directly consumable.
  eval_params.pInDepth =
      external_depth ? external_depth : depth_texture_.Get();
  eval_params.pInMotionVectors = external_motion_vectors
      ? external_motion_vectors
      : motion_vectors_texture_.Get();
  eval_params.InJitterOffsetX = frame_params.jitter_x;
  eval_params.InJitterOffsetY = frame_params.jitter_y;
  eval_params.InReset = frame_params.in_reset ? 1 : 0;
  eval_params.InMVScaleX = 1.0f;
  eval_params.InMVScaleY = 1.0f;
  eval_params.InPreExposure = 1.0f;
  eval_params.InExposureScale = 1.0f;
  eval_params.InRenderSubrectDimensions.Width = input_width;
  eval_params.InRenderSubrectDimensions.Height = input_height;
  uint32_t seh_code = 0;
  NVSDK_NGX_Result eval_result =
      CallEvaluateDlss(command_list, feature_handle_, parameter_block_,
                      &eval_params, seh_code);
  if (eval_result != NVSDK_NGX_Result_Success) {
    // GPU-side EvaluateFeature failure: the presenter will Reset the command
    // list. The guest output and the backend output are still in their pre-
    // evaluate states (the barriers were recorded but not executed).
    XELOGE("DLSS SR: EvaluateFeature (GPU) failed (result={}, seh=0x{:08X}).",
           static_cast<int>(eval_result), seh_code);
    return BackendResult::kExecutionFailed;
  }

  // Barrier 3: guest output NON_PIXEL_SHADER_RESOURCE -> PIXEL_SHADER_RESOURCE
  // (the SDK restored it to NON_PIXEL_SHADER_RESOURCE; the next frame's
  // refresher expects it back in PIXEL_SHADER_RESOURCE).
  D3D12_RESOURCE_BARRIER barrier_input_back{};
  barrier_input_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_input_back.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_input_back.Transition.pResource = guest_output;
  barrier_input_back.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_input_back.Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barrier_input_back.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_input_back);

  // Barrier 4: backend output UAV -> PIXEL_SHADER_RESOURCE (the paint chain's
  // first effect reads it via an SRV; this is the write-after-read barrier).
  D3D12_RESOURCE_BARRIER barrier_output_back{};
  barrier_output_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_output_back.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_output_back.Transition.pResource = output_texture_.Get();
  barrier_output_back.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_output_back.Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier_output_back.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_output_back);
  output_in_pixel_shader_resource_ = true;

  if (just_initialized) {
    XELOGI("DLSS SR: first frame evaluated (input={}x{} -> output={}x{}).",
           input_width, input_height, output_width, output_height);
  }
  return BackendResult::kSuccess;
}

}  // namespace dlss
}  // namespace ui
}  // namespace xe
