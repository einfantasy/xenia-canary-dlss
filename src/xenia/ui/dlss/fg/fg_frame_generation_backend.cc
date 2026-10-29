/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/dlss/fg/fg_frame_generation_backend.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// The NVIDIA SDK headers are included ONLY in this translation unit (license
// isolation). The generic NGX core headers (nvsdk_ngx.h,
// nvsdk_ngx_helpers_d3d.h, nvsdk_ngx_params.h) are shared with the DLSS SR
// backend. The FG-specific headers (nvsdk_ngx_defs_dlssg.h,
// nvsdk_ngx_params_dlssg.h, nvsdk_ngx_helpers_dlssg_d3d.h) are confined to
// this module (the fg/* license-isolation invariant). They trigger /W4
// warnings, so they are wrapped in a warning suppression.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4267 4310 4324 4456 4458 4514 4611 4820)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_params_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#pragma warning(pop)

#include "xenia/base/logging.h"
#include "xenia/base/filesystem.h"

#include <filesystem>
#include <fstream>
#include <sstream>

// rapidjson (header-only, vendored in third_party) parses the ngx-local.json
// config. The include dir is added via CMake (xenia-fg target). It is wrapped
// in a warning suppression like the SDK headers, since it is third-party code.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4267 4310 4324 4456 4458 4514 4611 4820)
#include <rapidjson/document.h>
#pragma warning(pop)

namespace xe {
namespace ui {
namespace fg {

namespace {

// nvsdk_ngx_defs.h only forward-declares CD3DX12_HEAP_PROPERTIES. The
// complete definition comes from a newer Windows SDK's d3d12.h, where it
// publicly derives from D3D12_HEAP_PROPERTIES. This machine's Windows SDK
// (10.0.26100.0) lacks the definition, so we provide it here: an empty
// extension of D3D12_HEAP_PROPERTIES.
class CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES {};

const D3D12_HEAP_PROPERTIES kDefaultHeapProperties = {
    D3D12_HEAP_TYPE_DEFAULT};

// The resource allocation callback is a C function pointer with no way to pass
// the device. The backend is a single instance per device, so a file-scope
// device pointer (set in Initialize, cleared in Shutdown) is safe. It is kept
// in the anonymous namespace so it does not collide with the SR backend's own
// device pointer.
ID3D12Device* g_ngx_device = nullptr;

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
// free of C++ objects with destructors.
void NVSDK_CONV NGXAppLogCallback(const char* message,
                                  NVSDK_NGX_Logging_Level loggingLevel,
                                  NVSDK_NGX_Feature sourceComponent) {
  if (message) {
    XELOGI("FG: NGX SDK [level={} feature={}] {}",
           static_cast<int>(loggingLevel), static_cast<int>(sourceComponent),
           message);
  }
}

// ---------------------------------------------------------------------------
// SEH boundaries. The SDK can crash (access violation), not just return an
// error code. So every SDK entry point is wrapped in __try/__except. A crash
// is treated as a generic failure (the initial NVSDK_NGX_Result_Fail value);
// the raw exception code is reported via the seh_code out-param. The __try
// scope must not contain any C++ object with a destructor, so these are free
// functions with only POD locals.
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

__declspec(noinline) NVSDK_NGX_Result CallCreateDlssg(
    ID3D12GraphicsCommandList* command_list, unsigned int creation_node_mask,
    unsigned int visibility_node_mask, NVSDK_NGX_Handle** out_handle,
    NVSDK_NGX_Parameter* parameter_block,
    NVSDK_NGX_DLSSG_Create_Params* create_params, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NGX_D3D12_CREATE_DLSSG(command_list, creation_node_mask,
                                   visibility_node_mask, out_handle,
                                   parameter_block, create_params);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return result;
}

__declspec(noinline) NVSDK_NGX_Result CallEvaluateDlssg(
    ID3D12GraphicsCommandList* command_list, NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameter_block,
    NVSDK_NGX_D3D12_DLSSG_Eval_Params* eval_params,
    NVSDK_NGX_DLSSG_Opt_Eval_Params* opt_eval_params, uint32_t& seh_code) {
  seh_code = 0;
  NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
  __try {
    result = NGX_D3D12_EVALUATE_DLSSG(command_list, handle, parameter_block,
                                     eval_params, opt_eval_params);
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
// These match the DLSS SR backend's former hardcoded ProjectId/EngineVersion,
// so a missing config behaves exactly as before (no behavior change).
const char kDefaultNgxConfigJson[] = R"({
    "engineType": "custom",
    "engineVersion": "xenia-canary-dlss",
    "ngxProjectId": "8562ea46-f1a4-4c0c-9f18-fec4fa0db830"
})";

}  // namespace

FGFrameGenerationBackend::FGFrameGenerationBackend(
    ID3D12Device* device, std::vector<std::wstring> runtime_dirs,
    std::wstring config_path, NgxCoreHost* core_host)
    : device_(device),
      runtime_dirs_(std::move(runtime_dirs)),
      config_path_(std::move(config_path)),
      core_host_(core_host) {}

FGFrameGenerationBackend::~FGFrameGenerationBackend() { Shutdown(); }

bool FGFrameGenerationBackend::IsAvailable() const {
  return state_ == State::kAvailable;
}

bool FGFrameGenerationBackend::IsUnavailable() const {
  return state_ == State::kUnavailable;
}

ID3D12Resource* FGFrameGenerationBackend::ZeroDepthPlaceholder() const {
  return depth_texture_.Get();
}

ID3D12Resource* FGFrameGenerationBackend::ZeroMotionVectorPlaceholder() const {
  return motion_vectors_texture_.Get();
}

ID3D12Resource* FGFrameGenerationBackend::OutputResource() const {
  return output_interp_.Get();
}

ID3D12Resource* FGFrameGenerationBackend::OutputRealResource() const {
  return output_real_.Get();
}

uint32_t FGFrameGenerationBackend::OutputWidth() const {
  return color_width_;
}

uint32_t FGFrameGenerationBackend::OutputHeight() const {
  return color_height_;
}

void FGFrameGenerationBackend::Shutdown() {
  if (state_ == State::kNotInitialized) {
    return;
  }
  XELOGI("FG: backend shutdown (color={}x{}, render={}x{}).", color_width_,
         color_height_, render_width_, render_height_);
  ReleaseFeature();
  // Release the shared NGX core through the core owner (Milestone 5-B). The
  // core host performs the Shutdown1 (SEH boundary) only when the reference
  // count reaches zero (the DLSS SR backend may still hold a reference).
  core_host_->Release(device_);
  g_ngx_device = nullptr;
  output_interp_.Reset();
  output_real_.Reset();
  disable_status_.Reset();
  depth_texture_.Reset();
  motion_vectors_texture_.Reset();
  upload_buffer_.Reset();
  output_in_pixel_shader_resource_ = false;
  color_width_ = 0;
  color_height_ = 0;
  render_width_ = 0;
  render_height_ = 0;
  state_ = State::kNotInitialized;
}

void FGFrameGenerationBackend::ReleaseFeature() {
  if (feature_handle_) {
    uint32_t seh_code = 0;
    NVSDK_NGX_Result release_result =
        CallReleaseFeature(feature_handle_, seh_code);
    if (release_result != NVSDK_NGX_Result_Success) {
      XELOGW("FG: ReleaseFeature failed (result={}, seh=0x{:08X}); the SDK "
             "handle is discarded anyway.",
             static_cast<int>(release_result), seh_code);
    } else {
      XELOGD("FG: ReleaseFeature succeeded.");
    }
    feature_handle_ = nullptr;
  }
  if (parameter_block_) {
    // Destroy the parameter block through the core owner (Milestone 5-B). The
    // core owner wraps DestroyParameters in a SEH boundary and logs its result.
    core_host_->DestroyParameters(parameter_block_);
    parameter_block_ = nullptr;
  }
}

bool FGFrameGenerationBackend::EnsureConfigFile() const {
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

bool FGFrameGenerationBackend::LoadNgxConfig(std::string* project_id_out,
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

BackendResult FGFrameGenerationBackend::Initialize(
    ID3D12GraphicsCommandList* command_list, uint32_t color_width,
    uint32_t color_height, uint32_t render_width, uint32_t render_height) {
  if (!device_ || !command_list) {
    if (!init_early_failure_logged_) {
      XELOGE("FG: Initialize called with a null device or command_list; "
             "cannot create the SDK session. Returning kInputContractFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }

  // Input contract: all sizes must be non-zero and within the SDK's supported
  // range (color 4..8192 x 4..7680). A zero size is an input contract failure.
  if (!color_width || !color_height || !render_width || !render_height ||
      color_width < 4 || color_height < 4 || color_width > 8192 ||
      color_height > 7680) {
    if (!contract_failure_logged_) {
      XELOGW("FG: input contract failed: invalid size (color={}x{}, "
             "render={}x{}).",
             color_width, color_height, render_width, render_height);
      contract_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }

  // 1. Capability check (CPU side, before any command is recorded). The
  //     provider releases the adapter it used to create device_ (retaining it
  //     makes the NVIDIA driver reject later CreateCommittedResource calls
  //     with E_INVALIDARG). So the backend enumerates adapters itself and
  //     matches by LUID to find the physical adapter device_ sits on. The
  //     matched adapter is used only for the capability check, then released.
  Microsoft::WRL::ComPtr<ID3D12Device3> device3;
  bool luid_available = false;
  LUID adapter_luid{};
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&device3)))) {
    adapter_luid = device3->GetAdapterLuid();
    luid_available = true;
  }
  XELOGD("FG: Initialize entry (state={}, color={}x{}, render={}x{}, "
         "runtime_dirs={}, adapter_luid={:04X}:{:04X}).",
         static_cast<int>(state_), color_width, color_height, render_width,
         render_height, runtime_dirs_.size(),
         luid_available ? adapter_luid.HighPart : 0,
         luid_available ? adapter_luid.LowPart : 0);

  if (!luid_available) {
    if (!init_early_failure_logged_) {
      XELOGE("FG: device does not expose ID3D12Device3 (QueryInterface "
             "failed); cannot resolve the adapter LUID. Returning kInitFailed.");
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    if (!init_early_failure_logged_) {
      XELOGE("FG: CreateDXGIFactory1 failed; cannot enumerate adapters for "
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
      XELOGE("FG: no DXGI adapter with LUID {:04X}:{:04X} was found; cannot "
             "run the FG capability check. Returning kInitFailed.",
             adapter_luid.HighPart, adapter_luid.LowPart);
      init_early_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }

  // Build the PathList from the runtime directories. The SDK does not search
  // recursively, so each directory that may contain an NGX runtime DLL must be
  // listed explicitly.
  std::vector<const wchar_t*> path_list;
  path_list.reserve(runtime_dirs_.size());
  for (const std::wstring& dir : runtime_dirs_) {
    path_list.push_back(dir.c_str());
  }
  const size_t path_list_length = path_list.size();
  NVSDK_NGX_FeatureCommonInfo feature_common_info{};
  feature_common_info.PathListInfo.Length = static_cast<int>(path_list_length);
  feature_common_info.PathListInfo.Path =
      path_list_length ? path_list.data() : nullptr;
  NVSDK_NGX_FeatureDiscoveryInfo discovery{};
  discovery.SDKVersion = NVSDK_NGX_Version_API;
  discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
  discovery.Identifier.IdentifierType =
      NVSDK_NGX_Application_Identifier_Type_Project_Id;
  std::string project_id;
  std::string engine_version;
  if (!LoadNgxConfig(&project_id, &engine_version)) {
    if (!config_failure_logged_) {
      std::string config_path_utf8(config_path_.size(), '\0');
      if (!config_path_.empty() &&
          WideCharToMultiByte(CP_UTF8, 0, config_path_.c_str(),
                              static_cast<int>(config_path_.size()),
                              config_path_utf8.data(),
                              static_cast<int>(config_path_utf8.size()),
                              nullptr, nullptr) <= 0) {
        config_path_utf8.clear();
      }
      XELOGE("FG: failed to load the ngx-local.json config ({}); cannot "
             "determine the project identity. Returning kInitFailed.",
             config_path_utf8);
      config_failure_logged_ = true;
    }
    return BackendResult::kInitFailed;
  }
  discovery.Identifier.v.ProjectDesc.ProjectId = project_id.c_str();
  discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  discovery.Identifier.v.ProjectDesc.EngineVersion = engine_version.c_str();
  discovery.ApplicationDataPath = L".";
  discovery.FeatureInfo = &feature_common_info;

  NVSDK_NGX_FeatureRequirement requirement{};
  uint32_t seh_code = 0;
  NVSDK_NGX_Result requirement_result = CallGetFeatureRequirements(
      matched_adapter.Get(), &discovery, &requirement, seh_code);
  // The adapter is used only for the capability check above. Release it now.
  matched_adapter.Reset();
  if (requirement_result != NVSDK_NGX_Result_Success ||
      requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
    // Capability check failed: the GPU / driver does not support FG.
    XELOGE("FG: capability check failed (result={}, supported={}, "
           "seh=0x{:08X}); GPU/driver/runtime does not support FG. Caching "
           "as unavailable (no retry).",
           static_cast<int>(requirement_result),
           static_cast<int>(requirement.FeatureSupported), seh_code);
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  XELOGD("FG: capability check passed (result={}, supported={}); the "
         "GPU/driver/runtime supports FG. Proceeding with SDK init.",
         static_cast<int>(requirement_result),
         static_cast<int>(requirement.FeatureSupported));

  // 2. Initialize the shared NGX core through the core owner (Milestone 5-B).
  NgxCoreAcquireResult acquire_result =
      core_host_->Acquire(device_, runtime_dirs_, project_id, engine_version);
  if (acquire_result.result != 0) {  // 0 = NVSDK_NGX_Result_Success.
    XELOGE("FG: core Acquire failed (result={}, seh=0x{:08X}).",
           acquire_result.result, acquire_result.seh_code);
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(
        static_cast<NVSDK_NGX_Result>(acquire_result.result));
  }
  g_ngx_device = device_;
  XELOGD("FG: core Acquire succeeded (core_initialized={}).",
         acquire_result.core_initialized);

  // 3. Allocate the parameter block (CPU side) through the core owner.
  uint32_t alloc_seh = 0;
  parameter_block_ = core_host_->AllocateParameters(&alloc_seh);
  if (!parameter_block_) {
    XELOGE("FG: parameter block allocation failed (seh=0x{:08X}).", alloc_seh);
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Register the resource allocation / release callbacks so the SDK can
  // allocate its internal resources. (SDK guide section 6.2.)
  parameter_block_->Set(NVSDK_NGX_Parameter_ResourceAllocCallback,
                        reinterpret_cast<unsigned long long>(
                            &NGXResourceAllocCallback));
  parameter_block_->Set(NVSDK_NGX_Parameter_ResourceReleaseCallback,
                        reinterpret_cast<unsigned long long>(
                            &NGXResourceReleaseCallback));

  // 4. Create-time resource flags: backbuffer/mvecs/depth always provided;
  //    HUD/UI/UIAlpha/BidirectionalDistortionField never provided (declare,
  //    never pass dangling textures).
  const unsigned int always_flags =
      NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer |
      NVSDK_NGX_DLSSG_ResourceFlags_MVecs | NVSDK_NGX_DLSSG_ResourceFlags_Depth;
  const unsigned int never_flags =
      NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI |
      NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
      NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField;
  parameter_block_->Set(
      NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, always_flags);
  parameter_block_->Set(
      NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, never_flags);
  XELOGD("FG: ResourceAlwaysProvided=0x{:X} ResourceNeverProvided=0x{:X}",
         always_flags, never_flags);

  // 5. Create the backend output textures (ALLOW_UNORDERED_ACCESS, UAV state).
  //    output_interp_ is the generated frame N-0.5; output_real_ is the SDK's
  //    copy of the real frame N. Both are R10G10B10A2_UNORM (the same format as
  //    the backbuffer).
  // Captured by the lambda below; the first failing create writes its HRESULT.
  HRESULT fg_output_create_hr = S_OK;
  auto create_output_tex = [&]() -> Microsoft::WRL::ComPtr<ID3D12Resource> {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = color_width;
    td.Height = color_height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    const HRESULT create_hr = device_->CreateCommittedResource(
        &kDefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&r));
    if (FAILED(create_hr)) {
      fg_output_create_hr = create_hr;
      return nullptr;
    }
    return r;
  };
  output_interp_ = create_output_tex();
  output_real_ = create_output_tex();
  if (!output_interp_ || !output_real_) {
    // Same diagnostic as the SR branch: the ACTUAL HRESULT of this call plus
    // the device-removal reason (0x887A0005 removed / 0x887A0007 hung /
    // 0x80070057 E_INVALIDARG). The format-support query rules the R10G10B10A2
    // typed-UAV format in or out without guessing.
    const HRESULT device_removed_reason = device_->GetDeviceRemovedReason();
    XELOGE("FG: failed to create the backend output textures: "
           "create_hr=0x{:08X}, GetDeviceRemovedReason()=0x{:08X}, "
           "desc(format=0x{:X}, flags=0x{:X}, width={}, height={}, "
           "mip_levels=1, sample_count=1, layout=UNKNOWN), "
           "initial_state=UNORDERED_ACCESS, heap=DEFAULT. Caching as "
           "unavailable.",
           static_cast<uint32_t>(fg_output_create_hr),
           static_cast<uint32_t>(device_removed_reason),
           static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM),
           static_cast<uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
           color_width, color_height);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = {};
    format_support.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    const HRESULT support_hr = device_->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &format_support, sizeof(format_support));
    XELOGE("FG: CheckFeatureSupport(hr=0x{:08X}) "
           "Support1=0x{:X} Support2=0x{:X} typed_uav_supported={}",
           static_cast<uint32_t>(support_hr),
           static_cast<uint32_t>(format_support.Support1),
           static_cast<uint32_t>(format_support.Support2),
           (format_support.Support1 &
            D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0);
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGD("FG: backend output textures created ({}x{}, R10G10B10A2_UNORM, "
         "UAV state).",
         color_width, color_height);

  // 6. The disable-interpolation status buffer (4 bytes, UAV). The SDK writes
  //    a boolean true to the first byte when interpolation should be skipped
  //    this frame.
  {
    D3D12_RESOURCE_DESC ds_desc{};
    ds_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ds_desc.Width = 4;
    ds_desc.Height = 1;
    ds_desc.DepthOrArraySize = 1;
    ds_desc.MipLevels = 1;
    ds_desc.SampleDesc.Count = 1;
    ds_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ds_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(
            &kDefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &ds_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&disable_status_)))) {
      XELOGE("FG: failed to create the disable-interpolation status buffer.");
      state_ = State::kUnavailable;
      return BackendResult::kInitFailed;
    }
  }

  // 7. Zero-filled placeholder depth (guest resolution, R32_FLOAT) and
  //    motion-vector (guest resolution, R16G16_FLOAT) textures. Xbox 360
  //    games produce neither, so we feed all-zero values ("honest fallback").
  //    The SDK's compute shader binds both as UAVs, so after zero-init they
  //    must be left in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE.
  auto create_zero_tex = [&](DXGI_FORMAT fmt, uint32_t w, uint32_t h)
      -> Microsoft::WRL::ComPtr<ID3D12Resource> {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
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
  depth_texture_ = create_zero_tex(DXGI_FORMAT_R32_FLOAT, render_width,
                                   render_height);
  motion_vectors_texture_ = create_zero_tex(DXGI_FORMAT_R16G16_FLOAT,
                                            render_width, render_height);
  if (!depth_texture_ || !motion_vectors_texture_) {
    XELOGE("FG: failed to create zero-filled depth/motion textures.");
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Zero-init: CPU upload buffer (memset 0) + CopyTextureRegion per texture.
  const size_t zpp = 4;  // R32_FLOAT and R16G16_FLOAT are both 4 B/pixel.
  const size_t up_row =
      (static_cast<size_t>(render_width) * zpp + 255) & ~size_t(255);
  const size_t up_size = up_row * render_height;
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
      XELOGE("FG: failed to create upload buffer for zero-init.");
      state_ = State::kUnavailable;
      return BackendResult::kInitFailed;
    }
    void* mapped = nullptr;
    if (FAILED(upload_buffer_->Map(0, nullptr, &mapped))) {
      XELOGE("FG: failed to map upload buffer for zero-init.");
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
    fp.Footprint.Width = render_width;
    fp.Footprint.Height = render_height;
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
  XELOGD("FG: zero-filled depth (R32F) and motion-vector (R16G16F) textures "
         "created ({}x{}).",
         render_width, render_height);

  // 8. Create the FG feature (NGX_D3D12_CREATE_DLSSG, recorded as a GPU command
  //    into the caller's command list).
  NVSDK_NGX_DLSSG_Create_Params create_params{};
  create_params.Width = color_width;
  create_params.Height = color_height;
  create_params.NativeBackbufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  create_params.RenderWidth = render_width;
  create_params.RenderHeight = render_height;
  create_params.DynamicResolutionScaling = false;
  XELOGD("FG: CreateFeature start (color={}x{} -> render={}x{}).", color_width,
         color_height, render_width, render_height);
  NVSDK_NGX_Result create_result = CallCreateDlssg(
      command_list, /*creation_node_mask=*/1, /*visibility_node_mask=*/1,
      &feature_handle_, parameter_block_, &create_params, seh_code);
  if (create_result != NVSDK_NGX_Result_Success || !feature_handle_) {
    // GPU-side CreateFeature failure: the presenter will Reset the command
    // list. Cache as unavailable so we do not retry the SDK.
    XELOGE("FG: CreateFeature (GPU) failed (result={}, seh=0x{:08X}). "
           "Caching as unavailable.",
           static_cast<int>(create_result), seh_code);
    ReleaseFeature();
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(create_result);
  }

  color_width_ = color_width;
  color_height_ = color_height;
  render_width_ = render_width;
  render_height_ = render_height;
  state_ = State::kAvailable;
  XELOGI("FG: backend initialized (color={}x{}, render={}x{}).", color_width,
         color_height, render_width, render_height);
  return BackendResult::kSuccess;
}

BackendResult FGFrameGenerationBackend::TryProcess(
    ID3D12GraphicsCommandList* command_list, ID3D12Resource* backbuffer,
    ID3D12Resource* depth, ID3D12Resource* mvecs,
    const DlssFgFrameParams& frame_params) {
  // 0. Cached unavailable: do not retry the SDK.
  if (state_ == State::kUnavailable) {
    return BackendResult::kUnavailable;
  }
  // Input contract: the command list and the backbuffer must be non-null. The
  // depth and mvecs are OPTIONAL (nullptr -> internal zero placeholder).
  if (!command_list || !backbuffer) {
    if (!contract_failure_logged_) {
      XELOGW("FG: input contract failed: null command_list or backbuffer.");
      contract_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }

  // Lazy init / re-create on size change. The render (guest) size is the size of
  // the depth / motion-vector inputs (and of the backend's internal zero
  // placeholders) - the SDK's InternalWidth/Height == RenderWidth/RenderHeight.
  // The color (input + output) size is the size of the real-frame INPUT texture
  // (passed as `backbuffer`): the DLSS SR backend's output at display resolution
  // when SR is on, or the guest output resource at guest resolution when SR is
  // off. The SDK's creation Width/Height == the input color size, and the
  // backbuffer / outputInterp subrects must describe it. The depth/MV (render)
  // size may differ from the color size (the SR workflow). The presenter
  // upscales the output to the display backbuffer (1:1 when SR is on, upscale
  // when SR is off).
  const uint32_t render_width = frame_params.render_width;
  const uint32_t render_height = frame_params.render_height;
  const uint32_t color_width = backbuffer->GetDesc().Width;
  const uint32_t color_height = backbuffer->GetDesc().Height;
  if (!render_width || !render_height) {
    // The presenter should always pass a non-zero guest size. A zero size is an
    // input contract failure.
    if (!contract_failure_logged_) {
      XELOGW("FG: input contract failed: zero guest (render) size "
             "({}x{}).",
             render_width, render_height);
      contract_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }
  if (!color_width || !color_height) {
    // A zero input color size is an input contract failure.
    if (!contract_failure_logged_) {
      XELOGW("FG: input contract failed: zero input color size ({}x{}).",
             color_width, color_height);
      contract_failure_logged_ = true;
    }
    return BackendResult::kInputContractFailed;
  }

  bool just_initialized = false;
  if (state_ == State::kNotInitialized ||
      color_width != color_width_ || color_height != color_height_ ||
      render_width != render_width_ || render_height != render_height_) {
    if (state_ == State::kAvailable) {
      // Size changed: release the feature and re-create.
      ReleaseFeature();
      output_interp_.Reset();
      output_real_.Reset();
      disable_status_.Reset();
      depth_texture_.Reset();
      motion_vectors_texture_.Reset();
      upload_buffer_.Reset();
      state_ = State::kNotInitialized;
    }
    BackendResult init_result = Initialize(command_list, color_width,
                                           color_height, render_width,
                                           render_height);
    if (init_result != BackendResult::kSuccess) {
      return init_result;
    }
    output_in_pixel_shader_resource_ = false;
    just_initialized = true;
  }

  // The backend is now available and sized correctly. Record the per-frame
  // commands (barriers + EvaluateFeature) into the command list.

  // Barrier 1: backbuffer PIXEL_SHADER_RESOURCE -> NON_PIXEL_SHADER_RESOURCE
  // (the SDK's compute shader reads the input).
  D3D12_RESOURCE_BARRIER barrier_input{};
  barrier_input.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_input.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_input.Transition.pResource = backbuffer;
  barrier_input.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_input.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier_input.Transition.StateAfter =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_input);

  // Barrier 2 (not the first frame): backend outputs PIXEL_SHADER_RESOURCE ->
  // UNORDERED_ACCESS (the SDK writes the outputs in UAV state).
  if (output_in_pixel_shader_resource_) {
    D3D12_RESOURCE_BARRIER barrier_output_to_uav{};
    barrier_output_to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_output_to_uav.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier_output_to_uav.Transition.pResource = output_interp_.Get();
    barrier_output_to_uav.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_output_to_uav.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier_output_to_uav.Transition.StateAfter =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    command_list->ResourceBarrier(1, &barrier_output_to_uav);
    D3D12_RESOURCE_BARRIER barrier_output_real_to_uav{};
    barrier_output_real_to_uav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_output_real_to_uav.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier_output_real_to_uav.Transition.pResource = output_real_.Get();
    barrier_output_real_to_uav.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_output_real_to_uav.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier_output_real_to_uav.Transition.StateAfter =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    command_list->ResourceBarrier(1, &barrier_output_real_to_uav);
    output_in_pixel_shader_resource_ = false;
  }

  // Record EvaluateFeature (GPU command). The SDK reads the backbuffer (in
  // NPSR), the depth (NPSR), and the mvecs (NPSR), and writes the outputs (in
  // UAV). The SDK restores the inputs to NPSR and the outputs to UAV.
  NVSDK_NGX_D3D12_DLSSG_Eval_Params eval_params{};
  eval_params.pBackbuffer = backbuffer;
  // External MV / depth handoff. When the caller provides a valid external
  // motion-vector texture (from the NVOF backend) or an external depth texture,
  // bind those instead of the internal zero-filled placeholders. nullptr = use
  // the internal placeholder. Both are borrowed read-only (NPSR/UAV).
  eval_params.pDepth = depth ? depth : depth_texture_.Get();
  eval_params.pMVecs = mvecs ? mvecs : motion_vectors_texture_.Get();
  eval_params.pHudless = nullptr;
  eval_params.pUI = nullptr;
  eval_params.pUIAlpha = nullptr;
  eval_params.pBidirectionalDistortionField = nullptr;
  eval_params.pOutputInterpFrame = output_interp_.Get();
  eval_params.pOutputRealFrame = output_real_.Get();
  eval_params.pOutputDisableInterpolation = disable_status_.Get();

  NVSDK_NGX_DLSSG_Opt_Eval_Params opt_params{};
  // Count excludes the real frame; indices are one-based. multiFrameCount=1 -> 2x.
  opt_params.multiFrameCount = 1;
  opt_params.multiFrameIndex = 1;
  // A stationary synthetic camera. All temporal motion is in the mvec buffer,
  // but projection/inverse and the camera basis must still agree.
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      const float v = (r == c) ? 1.0f : 0.0f;
      opt_params.cameraViewToClip[r][c] = v;
      opt_params.clipToCameraView[r][c] = v;
      opt_params.clipToLensClip[r][c] = v;
      opt_params.clipToPrevClip[r][c] = v;
      opt_params.prevClipToClip[r][c] = v;
    }
  }
  opt_params.jitterOffset[0] = 0.0f;
  opt_params.jitterOffset[1] = 0.0f;
  // Motion-vector scale (Stage 4): the SDK multiplies the raw MV buffer by
  // mvecScale. Our densify output is in PIXEL units and points current ->
  // previous (kNegate=0, verified by Veyra's +8px displacement test). DLSSG
  // FG expects (current - previous) in NORMALIZED coordinates, so the scale
  // is (-1/render_w, -1/render_h): divide by the MV buffer size (guest
  // resolution, == render_width/render_height) to normalize, and negate to
  // flip the direction from current->previous to previous->current. With the
  // prior (1,1) scale the SDK read a 100-pixel displacement as a 100-screen
  // normalized displacement in the wrong direction, smearing the
  // interpolated frame.
  opt_params.mvecScale[0] = -1.0f / static_cast<float>(render_width);
  opt_params.mvecScale[1] = -1.0f / static_cast<float>(render_height);
  opt_params.cameraNear = 0.1f;
  opt_params.cameraFar = 1000.0f;
  opt_params.cameraFOV = 1.5707963f;  // pi/2
  // The camera renders at the render (guest) resolution, so its aspect ratio is
  // the render aspect, NOT the color (display) aspect. Using the color aspect
  // here (when SR is on and the color is the display size) mis-projects the
  // synthetic camera and corrupts the interpolated frame.
  opt_params.cameraAspectRatio =
      static_cast<float>(render_width) / static_cast<float>(render_height);
  opt_params.cameraUp[1] = 1.0f;
  opt_params.cameraRight[0] = 1.0f;
  opt_params.cameraFwd[2] = 1.0f;
  const float projectionX = 1.0f / opt_params.cameraAspectRatio;
  const float projectionZ =
      opt_params.cameraFar / (opt_params.cameraFar - opt_params.cameraNear);
  const float projectionW = -opt_params.cameraNear * projectionZ;
  opt_params.cameraViewToClip[0][0] = projectionX;
  opt_params.cameraViewToClip[2][2] = projectionZ;
  opt_params.cameraViewToClip[2][3] = 1.0f;
  opt_params.cameraViewToClip[3][2] = projectionW;
  opt_params.cameraViewToClip[3][3] = 0.0f;
  opt_params.clipToCameraView[0][0] = 1.0f / projectionX;
  opt_params.clipToCameraView[2][2] = 0.0f;
  opt_params.clipToCameraView[2][3] = 1.0f / projectionW;
  opt_params.clipToCameraView[3][2] = 1.0f;
  opt_params.clipToCameraView[3][3] = -projectionZ / projectionW;
  opt_params.colorBuffersHDR = false;  // guest output is SDR.
  opt_params.depthInverted = frame_params.depth_inverted;
  opt_params.cameraMotionIncluded = true;
  opt_params.reset = frame_params.in_reset ? 1u : 0u;
  opt_params.automodeOverrideReset = 0;
  opt_params.notRenderingGameFrames = 0;
  opt_params.orthoProjection = 0;
  opt_params.motionVectorsDilated = 0;
  // Zero is valid stationary motion, including either axis of a pan.
  opt_params.motionVectorsInvalidValue = 3.402823466e38f;
  opt_params.menuDetectionEnabled = 0;
  opt_params.backbufferSubrectSize = {color_width, color_height};
  opt_params.outputInterpSubrectSize = {color_width, color_height};
  opt_params.mvecsSubrectSize = {render_width, render_height};
  opt_params.depthSubrectSize = {render_width, render_height};

  // Monotonic real-frame id (uint64 via the C++ Set overload).
  parameter_block_->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,
                        static_cast<unsigned long long>(frame_params.frame_id));

  uint32_t seh_code = 0;
  NVSDK_NGX_Result eval_result = CallEvaluateDlssg(
      command_list, feature_handle_, parameter_block_, &eval_params,
      &opt_params, seh_code);
  if (eval_result != NVSDK_NGX_Result_Success) {
    // GPU-side EvaluateFeature failure: the presenter will Reset the command
    // list. The backbuffer and the backend outputs are still in their pre-
    // evaluate states (the barriers were recorded but not executed).
    XELOGE("FG: EvaluateFeature (GPU) failed (result={}, seh=0x{:08X}).",
           static_cast<int>(eval_result), seh_code);
    return BackendResult::kExecutionFailed;
  }

  // Barrier 3: backbuffer NON_PIXEL_SHADER_RESOURCE -> PIXEL_SHADER_RESOURCE
  // (the SDK restored it to NPSR; the next frame's paint expects it back in
  // PIXEL_SHADER_RESOURCE).
  D3D12_RESOURCE_BARRIER barrier_input_back{};
  barrier_input_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_input_back.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_input_back.Transition.pResource = backbuffer;
  barrier_input_back.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_input_back.Transition.StateBefore =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  barrier_input_back.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_input_back);

  // Barrier 4: backend outputs UAV -> PIXEL_SHADER_RESOURCE (the paint chain's
  // first effect reads them via an SRV; this is the write-after-read barrier).
  D3D12_RESOURCE_BARRIER barrier_output_back{};
  barrier_output_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_output_back.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_output_back.Transition.pResource = output_interp_.Get();
  barrier_output_back.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_output_back.Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier_output_back.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_output_back);
  D3D12_RESOURCE_BARRIER barrier_output_real_back{};
  barrier_output_real_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_output_real_back.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_output_real_back.Transition.pResource = output_real_.Get();
  barrier_output_real_back.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_output_real_back.Transition.StateBefore =
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  barrier_output_real_back.Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &barrier_output_real_back);
  output_in_pixel_shader_resource_ = true;

  if (just_initialized) {
    XELOGI("FG: first frame evaluated (color={}x{}, render={}x{}).", color_width,
           color_height, render_width, render_height);
  }
  return BackendResult::kSuccess;
}

}  // namespace fg
}  // namespace ui
}  // namespace xe
