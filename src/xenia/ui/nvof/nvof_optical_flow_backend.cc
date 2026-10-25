/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/nvof/nvof_optical_flow_backend.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstring>

// The NVOF SDK headers are included ONLY in this translation unit (license
// isolation). They trigger /W4 warnings, so they are wrapped in a warning
// suppression.
//
// IMPORTANT (SDK contract): only NvOFGetMaxSupportedApiVersion and
// NvOFAPICreateInstanceD3D12 are real exported symbols of nvofapi64.dll.
// Every other entry point (NvCreateOpticalFlowD3D12, NvOFInit, NvOFExecuteD3D12,
// NvOFRegisterResourceD3D12, NvOFUnregisterResourceD3D12, NvOFDestroy,
// NvOFGetLastError, NvOFGetCaps) is a FUNCTION-TABLE typedef populated by
// NvOFAPICreateInstanceD3D12. They MUST be called through the function table
// (fn->nvOFXxx), never as free functions.
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4267 4310 4324 4456 4458 4514 4611 4820)
#include <nvOpticalFlowCommon.h>
#include <nvOpticalFlowD3D12.h>
#pragma warning(pop)

#include "xenia/base/logging.h"

namespace xe {
namespace ui {
namespace nvof {

namespace {

// The default D3D12 heap properties. Defined locally so this module does not
// depend on the D3D12 provider utilities (keeping the NVOF module self-contained
// and avoiding a circular dependency with the presenter target).
const D3D12_HEAP_PROPERTIES kDefaultHeapProperties = {
    D3D12_HEAP_TYPE_DEFAULT};
const D3D12_HEAP_PROPERTIES kUploadHeapProperties = {
    D3D12_HEAP_TYPE_UPLOAD};

// Densify semantics (fixed, documented):
//   - kNegate = 0: the raw NVOF flow already points current -> previous (given
//     inputFrame = current, referenceFrame = previous, verified by Veyra's
//     +8px displacement test). Preserve that direction; do NOT negate.
//   - kCostThreshold = 128: cells with cost >= 128 (confidence <= 0.49) are
//     zeroed. Cost is a rejection signal (higher cost = less reliable).
const uint32_t kNegate = 0;
const uint32_t kCostThreshold = 128;

// ---------------------------------------------------------------------------
// Embedded HLSL shaders (compiled at runtime via D3DCompile).
//
// The ABGR8 conversion pass transfers the guest output (R10G10B10A2_UNORM,
// 10-bit) to the NVOF input format (B8G8R8A8_UNORM, 8-bit). The input is
// sampled as (R,G,B,A); the output is written as (B,G,R,A) so that the pixel
// values are preserved in the B8G8R8A8 memory layout (R in the lowest byte,
// matching NV_OF_BUFFER_FORMAT_ABGR8).
//
// The densify pass expands the raw grid flow (R16G16_SINT, S10.5 fixed-point)
// and cost (R8_UNORM) to a full-resolution MV (R16G16_FLOAT, pixel units) and
// confidence (R8_UNORM, [0,1]). /32 converts S10.5 to pixels; grid replication
// is nearest-neighbor. Cost is a rejection signal: higher cost = less
// reliable, so confidence = (255 - cost) / 255. Cells with cost >= threshold
// are zeroed. The constants are bound via a root 32-bit-constants parameter
// (NOT a CBV), so the cbuffer is declared at b0 with 7 uint32 members.
// ---------------------------------------------------------------------------
const char kAbgr8ConvertHlsl[] = R"(
Texture2D<float4> inputTex : register(t0);
RWTexture2D<float4> outputTex : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 globalId : SV_DispatchThreadID)
{
    float4 v = inputTex[globalId.xy];   // (R, G, B, A), 10-bit
    outputTex[globalId.xy] = float4(v.z, v.y, v.x, v.w);   // (B, G, R, A)
}
)";

const char kDensifyHlsl[] = R"(
Texture2D<int2> rawFlow : register(t0);
Texture2D<uint> rawCost : register(t1);
RWTexture2D<float2> flowOut : register(u0);
RWTexture2D<float> confOut : register(u1);

cbuffer DensifyConstants : register(b0)
{
    uint gridW;
    uint gridH;
    uint workingW;
    uint workingH;
    uint gridSize;
    uint negate;
    uint costThreshold;
}

[numthreads(8, 8, 1)]
void main(uint3 globalId : SV_DispatchThreadID)
{
    if (globalId.x >= workingW || globalId.y >= workingH) return;
    uint gx = min(globalId.x / gridSize, gridW - 1);
    uint gy = min(globalId.y / gridSize, gridH - 1);
    int2 raw = rawFlow[uint2(gx, gy)];
    float2 v = float2(raw) / 32.0;   // S10.5 -> pixels
    if (negate != 0) v = -v;
    uint cost = rawCost[uint2(gx, gy)];
    float confidence = float(255u - min(cost, 255u)) / 255.0;
    if (cost >= costThreshold) { v = float2(0, 0); confidence = 0.0; }
    flowOut[globalId.xy] = v;
    confOut[globalId.xy] = confidence;
}
)";

// ---------------------------------------------------------------------------
// SEH boundaries. The SDK can crash (access violation), not just return an
// error code. So every SDK entry point is wrapped in __try/__except. A crash
// is treated as a generic failure (NV_OF_ERR_GENERIC); the raw exception code
// is reported via the seh_code out-param. The __try scope must not contain any
// C++ object with a destructor, so these are free functions with only POD
// locals. Each wrapper takes the FUNCTION-TABLE pointer to the specific SDK
// call (a POD function pointer), because the SDK entry points are not exported
// symbols - they are reached through NV_OF_D3D12_API_FUNCTION_LIST.
// ---------------------------------------------------------------------------

// NvOFAPICreateInstanceD3D12 and NvOFGetMaxSupportedApiVersion are the two real
// exported symbols of nvofapi64.dll (not function-table entries). They are
// resolved via GetProcAddress after LoadLibrary and called through these
// function pointers, because the module links NO NVIDIA import library (the
// DLL is loaded dynamically at Initialize time). The typedefs are defined here
// (before the SEH wrappers) so the wrappers can take them as parameters.
typedef NV_OF_STATUS (NVOFAPI* PFN_NvOFAPICreateInstanceD3D12)(
    uint32_t apiVer, NV_OF_D3D12_API_FUNCTION_LIST* functionList);
typedef NV_OF_STATUS (NVOFAPI* PFN_NvOFGetMaxSupportedApiVersion)(uint32_t*);

__declspec(noinline) NV_OF_STATUS CallCreateInstanceD3D12(
    PFN_NvOFAPICreateInstanceD3D12 fn, uint32_t apiVer,
    NV_OF_D3D12_API_FUNCTION_LIST* functionList, uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(apiVer, functionList);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFGetMaxSupportedApiVersion(
    PFN_NvOFGetMaxSupportedApiVersion fn, uint32_t* version, uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(version);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallCreateOpticalFlowD3D12(
    PFNNVCREATEOPTICALFLOWD3D12 fn, ID3D12Device* device, NvOFHandle* out_handle,
    uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(device, out_handle);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFGetCaps(
    PFNNVOFGETCAPS fn, NvOFHandle handle, NV_OF_CAPS capsParam, uint32_t* capsVal,
    uint32_t* size, uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(handle, capsParam, capsVal, size);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFInit(
    PFNNVOFINIT fn, NvOFHandle handle, const NV_OF_INIT_PARAMS* initParams,
    uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(handle, initParams);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFRegisterResourceD3D12(
    PFNNVOFREGISTERRESOURCED3D12 fn, NvOFHandle handle,
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12* params, uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(handle, params);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFUnregisterResourceD3D12(
    PFNNVOFUNREGISTERRESOURCED3D12 fn, NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12* params,
    uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(params);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFExecuteD3D12(
    PFNNVOFEXECUTED3D12 fn, NvOFHandle handle,
    const NV_OF_EXECUTE_INPUT_PARAMS_D3D12* in,
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12* out, uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(handle, in, out);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFDestroy(
    PFNNVOFDESTROY fn, NvOFHandle handle, uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(handle);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

__declspec(noinline) NV_OF_STATUS CallNvOFGetLastError(
    PFNNVOFGETLASTERROR fn, NvOFHandle handle, char* lastError, uint32_t* size,
    uint32_t& seh_code) {
  seh_code = 0;
  NV_OF_STATUS status = NV_OF_ERR_GENERIC;
  __try {
    status = fn(handle, lastError, size);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    seh_code = static_cast<uint32_t>(GetExceptionCode());
  }
  return status;
}

// Maps an NV_OF_STATUS to a BackendResult for initialization failures.
BackendResult InitFailureToBackendResult(NV_OF_STATUS status) {
  switch (status) {
    case NV_OF_SUCCESS:
      return BackendResult::kSuccess;
    case NV_OF_ERR_OF_NOT_AVAILABLE:
    case NV_OF_ERR_UNSUPPORTED_DEVICE:
    case NV_OF_ERR_DEVICE_DOES_NOT_EXIST:
      // Capability / device failure: cache as unavailable.
      return BackendResult::kUnavailable;
    default:
      // Any other init error (invalid parameter, out of memory, ...).
      return BackendResult::kInitFailed;
  }
}

// Loads d3dcompiler.dll and returns a pointer to D3DCompile (or nullptr).
// The module is self-contained and loads d3dcompiler itself (it does not
// depend on the D3D12 provider's shader-compilation internals).
typedef HRESULT(WINAPI* PFN_D3DCompile)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
    LPCSTR pEntryPoint, LPCSTR pShaderModel, UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

PFN_D3DCompile LoadD3DCompile() {
  HMODULE lib = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!lib) {
    lib = LoadLibraryExW(L"d3dcompiler.dll", nullptr,
                         LOAD_LIBRARY_SEARCH_SYSTEM32);
  }
  if (!lib) {
    return nullptr;
  }
  return reinterpret_cast<PFN_D3DCompile>(
      GetProcAddress(lib, "D3DCompile"));
}

// Compiles a single-stage shader (the entry point is "main"). Returns the
// bytecode blob (the caller releases it) and sets ok to false on failure.
Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
    PFN_D3DCompile d3d_compile, const char* source, const char* name,
    const char* shader_model, bool* ok) {
  *ok = false;
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> error_blob;
  if (d3d_compile) {
    HRESULT hr = d3d_compile(source, strlen(source), name, nullptr, nullptr,
                             "main", shader_model, 0, 0, blob.GetAddressOf(),
                             error_blob.GetAddressOf());
    if (SUCCEEDED(hr) && blob) {
      *ok = true;
      return blob;
    }
    if (error_blob) {
      XELOGE("NVOF: shader {} compile failed: {}", name,
             static_cast<const char*>(error_blob->GetBufferPointer()));
    }
  }
  return nullptr;
}

}  // namespace

NVOFOpticalFlowBackend::NVOFOpticalFlowBackend(
    ID3D12Device* device, uint32_t input_width, uint32_t input_height,
    uint32_t grid_size, uint32_t quality)
    : device_(device),
      input_width_(input_width),
      input_height_(input_height),
      grid_size_(grid_size),
      quality_(quality) {}

NVOFOpticalFlowBackend::~NVOFOpticalFlowBackend() { Shutdown(); }

bool NVOFOpticalFlowBackend::IsAvailable() const {
  return state_ == State::kAvailable;
}

bool NVOFOpticalFlowBackend::IsUnavailable() const {
  return state_ == State::kUnavailable;
}

ID3D12Resource* NVOFOpticalFlowBackend::MotionVectorOutput() const {
  return motion_vector_texture_.Get();
}

ID3D12Resource* NVOFOpticalFlowBackend::ConfidenceOutput() const {
  return confidence_texture_.Get();
}

ID3D12Resource* NVOFOpticalFlowBackend::GridFlowOutput() const {
  return grid_flow_texture_.Get();
}

ID3D12Resource* NVOFOpticalFlowBackend::GridCostOutput() const {
  return grid_cost_texture_.Get();
}

uint32_t NVOFOpticalFlowBackend::OutputWidth() const {
  return input_width_;
}

uint32_t NVOFOpticalFlowBackend::OutputHeight() const {
  return input_height_;
}

uint32_t NVOFOpticalFlowBackend::GridWidth() const {
  return grid_width_;
}

uint32_t NVOFOpticalFlowBackend::GridHeight() const {
  return grid_height_;
}

bool NVOFOpticalFlowBackend::HasValidFlow() const {
  return has_valid_flow_;
}

ID3D12Fence* NVOFOpticalFlowBackend::GetInputFence() const {
  return input_fence_.Get();
}

void NVOFOpticalFlowBackend::Shutdown() {
  if (state_ == State::kNotInitialized) {
    return;
  }
  XELOGI("NVOF: backend shutdown (input={}x{}, grid={}x{}).", input_width_,
         input_height_, grid_width_, grid_height_);
  // Ownership order (verified by Veyra, 2026-09-04): unregister the resources
  // from the NVOF session FIRST (the session must still be alive), THEN release
  // the backend-owned textures (now safe - no longer registered with NVOF), and
  // ONLY THEN destroy the session (nvOFDestroy + FreeLibrary). Releasing an
  // NVOF-registered ID3D12Resource after nvOFDestroy + FreeLibrary segfaults
  // inside the resource destruction path.
  UnregisterResources();
  // Release the backend-owned resources (safe now that they are unregistered).
  frame_pool_[0].Reset();
  frame_pool_[1].Reset();
  grid_flow_texture_.Reset();
  grid_cost_texture_.Reset();
  motion_vector_texture_.Reset();
  confidence_texture_.Reset();
  zero_upload_buffer_.Reset();
  // Destroy the SDK session (nvOFDestroy, then delete the function table, then
  // FreeLibrary).
  DestroySession();
  // Release the backend-owned fences, heap, signature, and PSOs.
  input_fence_.Reset();
  output_fence_.Reset();
  view_heap_.Reset();
  root_signature_.Reset();
  conversion_pso_.Reset();
  densify_pso_.Reset();
  has_valid_flow_ = false;
  grid_width_ = 0;
  grid_height_ = 0;
  next_output_value_ = 1;
  state_ = State::kNotInitialized;
}

// Unregisters every registered resource from the NVOF session (reverse order).
// The session MUST still be alive when this runs. Mirrors Veyra's observed
// teardown order: unregister -> (caller releases textures) -> destroy ->
// FreeLibrary.
void NVOFOpticalFlowBackend::UnregisterResources() {
  auto fn = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(fn_list_);
  if (!of_handle_ || !fn || !fn->nvOFUnregisterResourceD3D12) {
    return;
  }
  auto unregister = [&](NvOFGPUBufferHandle_st* h, const char* name) {
    if (!h) {
      return;
    }
    NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{};
    params.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle>(h);
    uint32_t seh_code = 0;
    NV_OF_STATUS status = CallNvOFUnregisterResourceD3D12(
        fn->nvOFUnregisterResourceD3D12, &params, seh_code);
    if (status == NV_OF_SUCCESS) {
      XELOGD("NVOF: unregistered {}.", name);
      h = nullptr;
    } else {
      XELOGW("NVOF: unregister {} failed (status={}, seh=0x{:08X}); the "
             "handle is discarded anyway.",
             name, static_cast<int>(status), seh_code);
    }
  };
  unregister(h_grid_cost_, "gridCost");
  unregister(h_grid_flow_, "gridFlow");
  unregister(h_frame_pool_[1], "framePool1");
  unregister(h_frame_pool_[0], "framePool0");
}

// Destroys the NVOF session: nvOFDestroy, then delete the function table, then
// FreeLibrary. MUST run AFTER UnregisterResources() and AFTER the backend-owned
// textures are released.
void NVOFOpticalFlowBackend::DestroySession() {
  auto fn = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(fn_list_);
  if (of_handle_ && fn && fn->nvOFDestroy) {
    uint32_t seh_code = 0;
    NV_OF_STATUS status = CallNvOFDestroy(fn->nvOFDestroy, of_handle_, seh_code);
    if (status != NV_OF_SUCCESS) {
      XELOGW("NVOF: nvOFDestroy failed (status={}, seh=0x{:08X}); the session "
             "is discarded anyway.",
             static_cast<int>(status), seh_code);
    } else {
      XELOGI("NVOF: nvOFDestroy succeeded.");
    }
    of_handle_ = nullptr;
  }
  if (fn_list_) {
    delete static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(fn_list_);
    fn_list_ = nullptr;
  }
  if (dll_) {
    FreeLibrary(static_cast<HMODULE>(dll_));
    dll_ = nullptr;
  }
}

BackendResult NVOFOpticalFlowBackend::Initialize(
    ID3D12GraphicsCommandList* command_list, uint32_t frame_index) {
  if (!device_ || !command_list) {
    XELOGE("NVOF: Initialize called with a null device or command_list; "
           "cannot create the SDK session. Returning kInputContractFailed.");
    return BackendResult::kInputContractFailed;
  }

  // The quality is a create-time parameter. Any value other than 0..2 is an
  // input-contract failure (never clamped).
  if (quality_ > 2) {
    XELOGW("NVOF: invalid quality {} (must be 0..2); caching the backend as "
           "unavailable.",
           quality_);
    state_ = State::kUnavailable;
    return BackendResult::kInputContractFailed;
  }

  // 1. Load the NVOF SDK runtime DLL (nvofapi64.dll, shipped with the NVIDIA
  //    driver). On a machine without an NVIDIA driver this fails, which is the
  //    deterministic "runtime missing" fallback.
  dll_ = LoadLibraryExW(L"nvofapi64.dll", nullptr,
                        LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll_) {
    XELOGE("NVOF: LoadLibraryExW nvofapi64.dll failed (error={}); no NVIDIA "
           "driver present. Caching as unavailable.",
           GetLastError());
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  XELOGI("NVOF: nvofapi64.dll loaded.");

  // 2. Query the driver's max supported API version.
  auto get_max_ver = reinterpret_cast<PFN_NvOFGetMaxSupportedApiVersion>(
      GetProcAddress(static_cast<HMODULE>(dll_),
                     "NvOFGetMaxSupportedApiVersion"));
  if (!get_max_ver) {
    XELOGE("NVOF: NvOFGetMaxSupportedApiVersion export missing. Caching as "
           "unavailable.");
    FreeLibrary(static_cast<HMODULE>(dll_));
    dll_ = nullptr;
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  uint32_t max_ver = 0;
  uint32_t seh_code = 0;
  NV_OF_STATUS ver_status =
      CallNvOFGetMaxSupportedApiVersion(get_max_ver, &max_ver, seh_code);
  if (ver_status != NV_OF_SUCCESS) {
    XELOGE("NVOF: NvOFGetMaxSupportedApiVersion failed (status={}, "
           "seh=0x{:08X}). Caching as unavailable.",
           static_cast<int>(ver_status), seh_code);
    FreeLibrary(static_cast<HMODULE>(dll_));
    dll_ = nullptr;
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  XELOGI("NVOF: driver max API version = 0x{:X}.", max_ver);

  // 3. Create the D3D12 API function table. NvOFAPICreateInstanceD3D12 is the
  //    sole real exported entry point; it populates the function table.
  //    It is resolved via GetProcAddress (the module links NO NVIDIA import
  //    library).
  auto create_instance = reinterpret_cast<PFN_NvOFAPICreateInstanceD3D12>(
      GetProcAddress(static_cast<HMODULE>(dll_),
                     "NvOFAPICreateInstanceD3D12"));
  if (!create_instance) {
    XELOGE("NVOF: NvOFAPICreateInstanceD3D12 export missing. Caching as "
           "unavailable.");
    FreeLibrary(static_cast<HMODULE>(dll_));
    dll_ = nullptr;
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  fn_list_ = new NV_OF_D3D12_API_FUNCTION_LIST();
  NV_OF_D3D12_API_FUNCTION_LIST* fn =
      static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(fn_list_);
  NV_OF_STATUS create_status =
      CallCreateInstanceD3D12(create_instance, max_ver, fn, seh_code);
  if (create_status != NV_OF_SUCCESS || !fn->nvCreateOpticalFlowD3D12 ||
      !fn->nvOFInit || !fn->nvOFExecuteD3D12 ||
      !fn->nvOFRegisterResourceD3D12 || !fn->nvOFUnregisterResourceD3D12 ||
      !fn->nvOFDestroy) {
    XELOGE("NVOF: NvOFAPICreateInstanceD3D12 failed (status={}, "
           "seh=0x{:08X}). Caching as unavailable.",
           static_cast<int>(create_status), seh_code);
    delete fn;
    fn_list_ = nullptr;
    FreeLibrary(static_cast<HMODULE>(dll_));
    dll_ = nullptr;
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  XELOGI("NVOF: D3D12 API function table populated.");

  // 4. Create the NVOF session (through the function table).
  NV_OF_STATUS create_status2 =
      CallCreateOpticalFlowD3D12(fn->nvCreateOpticalFlowD3D12, device_,
                                 &of_handle_, seh_code);
  if (create_status2 != NV_OF_SUCCESS || !of_handle_) {
    XELOGE("NVOF: nvCreateOpticalFlowD3D12 failed (status={}, seh=0x{:08X}). "
           "Caching as unavailable.",
           static_cast<int>(create_status2), seh_code);
    delete fn;
    fn_list_ = nullptr;
    FreeLibrary(static_cast<HMODULE>(dll_));
    dll_ = nullptr;
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  XELOGI("NVOF: NVOF session created.");

  // 5. Capability query (two-call protocol for the grid sizes; scalar caps use
  //    element count 1). Fail-closed: any failure or unsupported value is an
  //    input-contract failure, never a silent fallback.
  uint32_t elem_count = 0;
  NV_OF_STATUS caps_status = CallNvOFGetCaps(
      fn->nvOFGetCaps, of_handle_, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,
      nullptr, &elem_count, seh_code);
  if (caps_status != NV_OF_SUCCESS || elem_count == 0 || elem_count > 64) {
    XELOGE("NVOF: grid caps first call failed (status={}, elem_count={}, "
           "seh=0x{:08X}). Caching as unavailable.",
           static_cast<int>(caps_status), elem_count, seh_code);
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  std::vector<uint32_t> supported_grids(elem_count, 0);
  uint32_t filled = elem_count;
  caps_status = CallNvOFGetCaps(
      fn->nvOFGetCaps, of_handle_, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,
      supported_grids.data(), &filled, seh_code);
  if (caps_status != NV_OF_SUCCESS || filled == 0) {
    XELOGE("NVOF: grid caps second call failed (status={}, filled={}, "
           "seh=0x{:08X}). Caching as unavailable.",
           static_cast<int>(caps_status), filled, seh_code);
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  supported_grids.resize(filled);
  // Scalar caps (width/height range).
  auto scalar_cap = [&](NV_OF_CAPS cap, const char* name, uint32_t& out)
      -> bool {
    uint32_t v = 0;
    uint32_t one = 1;
    uint32_t sc_seh = 0;
    NV_OF_STATUS sc_status =
        CallNvOFGetCaps(fn->nvOFGetCaps, of_handle_, cap, &v, &one, sc_seh);
    if (sc_status != NV_OF_SUCCESS) {
      XELOGE("NVOF: scalar cap {} query failed (status={}, seh=0x{:08X}).",
             name, static_cast<int>(sc_status), sc_seh);
      return false;
    }
    out = v;
    return true;
  };
  uint32_t min_width = 0, max_width = 0, min_height = 0, max_height = 0;
  if (!scalar_cap(NV_OF_CAPS_WIDTH_MIN, "WIDTH_MIN", min_width) ||
      !scalar_cap(NV_OF_CAPS_WIDTH_MAX, "WIDTH_MAX", max_width) ||
      !scalar_cap(NV_OF_CAPS_HEIGHT_MIN, "HEIGHT_MIN", min_height) ||
      !scalar_cap(NV_OF_CAPS_HEIGHT_MAX, "HEIGHT_MAX", max_height)) {
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  const bool width_ok = min_width <= input_width_ && input_width_ <= max_width;
  const bool height_ok = min_height <= input_height_ &&
                        input_height_ <= max_height;
  XELOGI("NVOF: caps width=[{},{}] height=[{},{}] requested {}x{} "
         "width_ok={} height_ok={}.",
         min_width, max_width, min_height, max_height, input_width_,
         input_height_, width_ok, height_ok);
  if (!width_ok || !height_ok) {
    XELOGE("NVOF: requested extent {}x{} outside capability range. Caching "
           "as unavailable.",
           input_width_, input_height_);
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  bool grid_supported = false;
  for (uint32_t g : supported_grids) {
    if (g == grid_size_) {
      grid_supported = true;
      break;
    }
  }
  if (!grid_supported) {
    XELOGE("NVOF: requested grid {} not in capability list. Caching as "
           "unavailable.",
           grid_size_);
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  grid_width_ = (input_width_ + grid_size_ - 1) / grid_size_;
  grid_height_ = (input_height_ + grid_size_ - 1) / grid_size_;
  XELOGI("NVOF: grid extent = {}x{} (grid_size={}).", grid_width_,
         grid_height_, grid_size_);

  // 6. Initialize the NVOF session (through the function table).
  //    disableTemporalHints = NV_OF_TRUE matches Veyra's verified probe path
  //    (each frame is an independent pair; temporal hints from a previous
  //    frame are not used as a hint). Enabling temporal hints is a future
  //    optimization, not part of this validated milestone.
  NV_OF_INIT_PARAMS init{};
  init.width = input_width_;
  init.height = input_height_;
  init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid_size_);
  init.mode = NV_OF_MODE_OPTICALFLOW;
  init.perfLevel = quality_ == 0 ? NV_OF_PERF_LEVEL_FAST
                    : quality_ == 1 ? NV_OF_PERF_LEVEL_MEDIUM
                                    : NV_OF_PERF_LEVEL_SLOW;
  init.enableExternalHints = NV_OF_FALSE;
  init.enableOutputCost = NV_OF_TRUE;
  init.hPrivData = nullptr;
  init.enableRoi = NV_OF_FALSE;
  init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
  init.enableGlobalFlow = NV_OF_FALSE;
  init.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
  NV_OF_STATUS init_status = CallNvOFInit(fn->nvOFInit, of_handle_, &init,
                                          seh_code);
  if (init_status != NV_OF_SUCCESS) {
    XELOGE("NVOF: nvOFInit failed (status={}, seh=0x{:08X}). Caching as "
           "unavailable.",
           static_cast<int>(init_status), seh_code);
    DestroySession();
    state_ = State::kUnavailable;
    return InitFailureToBackendResult(init_status);
  }
  XELOGI("NVOF: session initialized ({}x{}, grid={}, quality={}).",
         input_width_, input_height_, grid_size_, quality_);

  // 7. Create the ABGR8 frame pool (two B8G8R8A8 textures, UAV state).
  for (int i = 0; i < 2; ++i) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = input_width_;
    desc.Height = input_height_;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(
            &kDefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&frame_pool_[i])))) {
      XELOGE("NVOF: failed to create ABGR8 frame pool[{}]. Caching as "
             "unavailable.",
             i);
      DestroySession();
      state_ = State::kUnavailable;
      return BackendResult::kInitFailed;
    }
  }
  XELOGI("NVOF: ABGR8 frame pool created (2x {}x{}).", input_width_,
         input_height_);

  // 8. Create the raw grid flow (R16G16_SINT) and cost (R8_UNORM) textures at
  //    the grid extent. They are read by the densify pass (SRV), so they start
  //    in PIXEL_SHADER_RESOURCE. The NVOF execute writes them (NVOF manages
  //    its own internal state); the output fence synchronizes the write before
  //    the densify reads.
  auto create_grid_tex = [&](DXGI_FORMAT fmt,
                             Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = grid_width_;
    desc.Height = grid_height_;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateCommittedResource(
            &kDefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&out)))) {
      return false;
    }
    return true;
  };
  if (!create_grid_tex(DXGI_FORMAT_R16G16_SINT, grid_flow_texture_) ||
      !create_grid_tex(DXGI_FORMAT_R8_UNORM, grid_cost_texture_)) {
    XELOGE("NVOF: failed to create grid flow/cost textures. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: grid flow (R16G16_SINT) and cost (R8_UNORM) textures created "
         "({}x{}).",
         grid_width_, grid_height_);

  // 9. Create the full-resolution MV (R16G16_FLOAT) and confidence (R8_UNORM)
  //    outputs at the input extent. They are written by the densify pass (UAV),
  //    so they start in UNORDERED_ACCESS.
  auto create_full_tex = [&](DXGI_FORMAT fmt,
                             Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = input_width_;
    desc.Height = input_height_;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(
            &kDefaultHeapProperties, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&out)))) {
      return false;
    }
    return true;
  };
  if (!create_full_tex(DXGI_FORMAT_R16G16_FLOAT, motion_vector_texture_) ||
      !create_full_tex(DXGI_FORMAT_R8_UNORM, confidence_texture_)) {
    XELOGE("NVOF: failed to create MV/confidence textures. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: MV (R16G16_FLOAT) and confidence (R8_UNORM) textures created "
         "({}x{}).",
         input_width_, input_height_);

  // 10. Create the zero-fill upload buffer (CPU-visible, filled with zeros).
  //     Used to zero-fill the ABGR8 previous frame on the first frame.
  const size_t zero_size =
      static_cast<size_t>(input_width_) * input_height_ * 4;
  D3D12_RESOURCE_DESC zero_desc{};
  zero_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  zero_desc.Width = zero_size;
  zero_desc.Height = 1;
  zero_desc.DepthOrArraySize = 1;
  zero_desc.MipLevels = 1;
  zero_desc.SampleDesc.Count = 1;
  zero_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(
          &kUploadHeapProperties, D3D12_HEAP_FLAG_NONE, &zero_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&zero_upload_buffer_)))) {
    XELOGE("NVOF: failed to create zero-fill upload buffer. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  void* mapped = nullptr;
  if (FAILED(zero_upload_buffer_->Map(0, nullptr, &mapped))) {
    XELOGE("NVOF: failed to map zero-fill upload buffer. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  memset(mapped, 0, zero_size);
  zero_upload_buffer_->Unmap(0, nullptr);
  XELOGI("NVOF: zero-fill upload buffer created ({} bytes).", zero_size);

  // 11. Create the input/output fences.
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&input_fence_))) ||
      FAILED(device_->CreateFence(1, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&output_fence_)))) {
    XELOGE("NVOF: failed to create input/output fences. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: input/output fences created (output starts at value 1).");

  // 12. Create the private view heap + root signature.
  //     Descriptor layout (7 descriptors, indices 0..6):
  //       0: input frame SRV        (conversion t0)
  //       1: (reserved)
  //       2: frame pool UAV         (conversion u0)
  //       3: grid flow SRV          (densify t0)
  //       4: grid cost SRV          (densify t1)
  //       5: motion vector UAV      (densify u0)
  //       6: confidence UAV         (densify u1)
  //     Root signature: [0] SRV descriptor table (t0/t1), [1] UAV u0,
  //     [2] UAV u1, [3] 32-bit constants (7, densify cbuffer b0).
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 7;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&heap_desc,
                                           IID_PPV_ARGS(&view_heap_)))) {
    XELOGE("NVOF: failed to create private view heap. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Root signature for the densify pass:
  //   [0] SRV descriptor table (t0 = grid-flow, t1 = grid-cost),
  //   [1] UAV root descriptor (u0 = full-resolution MV),
  //   [2] UAV root descriptor (u1 = confidence),
  //   [3] 32-bit constants (b0, the densify cbuffer: 7 values).
  //
  // NOTE: this Windows SDK's D3D12_ROOT_PARAMETER is the classic layout -
  // a struct with ParameterType, an anonymous union (.DescriptorTable /
  // .Constants / .Descriptor), and a top-level ShaderVisibility. The UAV root
  // parameter uses .Descriptor (there is NO .UnorderedAccessView member), the
  // constants count is Num32BitValues (not Num32BitValuesToSet), and the SRV
  // table references a D3D12_DESCRIPTOR_RANGE via .DescriptorTable
  // (.pDescriptorRanges / .NumDescriptorRanges), not NumDescriptors.
  D3D12_DESCRIPTOR_RANGE srv_range{};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 2;
  srv_range.BaseShaderRegister = 0;
  srv_range.RegisterSpace = 0;
  srv_range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_ROOT_PARAMETER root_params[4] = {};
  root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[0].DescriptorTable.NumDescriptorRanges = 1;
  root_params[0].DescriptorTable.pDescriptorRanges = &srv_range;
  root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  root_params[1].Descriptor.ShaderRegister = 0;
  root_params[1].Descriptor.RegisterSpace = 0;
  root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  root_params[2].Descriptor.ShaderRegister = 1;
  root_params[2].Descriptor.RegisterSpace = 0;
  root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_params[3].Constants.ShaderRegister = 0;
  root_params[3].Constants.RegisterSpace = 0;
  root_params[3].Constants.Num32BitValues = 7;
  root_params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rs_desc{};
  rs_desc.NumParameters = 4;
  rs_desc.pParameters = root_params;
  Microsoft::WRL::ComPtr<ID3DBlob> rs_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> rs_error;
  if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                         rs_blob.GetAddressOf(),
                                         rs_error.GetAddressOf())) ||
      FAILED(device_->CreateRootSignature(0, rs_blob.Get(),
                                          rs_blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root_signature_)))) {
    XELOGE("NVOF: failed to create root signature. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: private view heap + root signature created.");

  // 13. Compile the shaders (ABGR8 conversion + densify) via D3DCompile.
  auto d3d_compile = LoadD3DCompile();
  if (!d3d_compile) {
    XELOGE("NVOF: failed to load d3dcompiler. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  bool ok = false;
  Microsoft::WRL::ComPtr<ID3DBlob> convert_blob = CompileShader(
      d3d_compile, kAbgr8ConvertHlsl, "nvof_abgr8_convert", "cs_5_1", &ok);
  if (!ok || !convert_blob) {
    XELOGE("NVOF: failed to compile the ABGR8 conversion shader. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  Microsoft::WRL::ComPtr<ID3DBlob> densify_blob = CompileShader(
      d3d_compile, kDensifyHlsl, "nvof_densify", "cs_5_1", &ok);
  if (!ok || !densify_blob) {
    XELOGE("NVOF: failed to compile the densify shader. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: shaders compiled (ABGR8 conversion + densify).");

  // 14. Create the PSOs (conversion + densify).
  //     D3D12_COMPUTE_PIPELINE_STATE_DESC.CS is a D3D12_SHADER_BYTECODE (this
  //     SDK has no D3D12_COMPUTE_SHADER_DESC typedef), and the root signature
  //     member is pRootSignature (not pCSRootSignature).
  D3D12_COMPUTE_PIPELINE_STATE_DESC cps_desc{};
  cps_desc.pRootSignature = root_signature_.Get();
  cps_desc.CS.pShaderBytecode = convert_blob->GetBufferPointer();
  cps_desc.CS.BytecodeLength = convert_blob->GetBufferSize();
  cps_desc.NodeMask = 0;
  if (FAILED(device_->CreateComputePipelineState(&cps_desc,
                                                 IID_PPV_ARGS(&conversion_pso_)))) {
    XELOGE("NVOF: failed to create the conversion PSO. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC densify_cps_desc{};
  densify_cps_desc.pRootSignature = root_signature_.Get();
  densify_cps_desc.CS.pShaderBytecode = densify_blob->GetBufferPointer();
  densify_cps_desc.CS.BytecodeLength = densify_blob->GetBufferSize();
  densify_cps_desc.NodeMask = 0;
  if (FAILED(device_->CreateComputePipelineState(
          &densify_cps_desc, IID_PPV_ARGS(&densify_pso_)))) {
    XELOGE("NVOF: failed to create the densify PSO. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: PSOs created (conversion + densify).");

  // 15. Register the frame pool and grid flow/cost with the NVOF session.
  //     Each registration supplies explicit fence points. The registration has
  //     its own GPU work, so we wait on a registration fence before success.
  //     Fence chaining (matches Veyra): the first resource waits on the caller's
  //     input fence (at its current completed value); each subsequent resource
  //     waits on the registration fence at the previous registration count.
  //     This orders the registrations on the GPU.
  Microsoft::WRL::ComPtr<ID3D12Fence> registration_completion;
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&registration_completion)))) {
    XELOGE("NVOF: failed to create the registration-completion fence. "
           "Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  const uint64_t input_ready = input_fence_->GetCompletedValue();
  if (input_ready == UINT64_MAX) {
    XELOGE("NVOF: input fence reports device removal before registration.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kSyncConditionFailed;
  }
  uint64_t registered = 0;
  auto register_resource = [&](ID3D12Resource* resource,
                               NvOFGPUBufferHandle_st** handle_out,
                               const char* name) -> bool {
    NvOFGPUBufferHandle handle = nullptr;
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 reg{};
    reg.resource = resource;
    reg.hOFGpuBuffer = &handle;
    reg.inputFencePoint = (registered == 0)
                              ? NV_OF_FENCE_POINT{input_fence_.Get(), input_ready}
                              : NV_OF_FENCE_POINT{
                                    registration_completion.Get(), registered};
    reg.outputFencePoint = NV_OF_FENCE_POINT{registration_completion.Get(),
                                             registered + 1};
    uint32_t reg_seh = 0;
    NV_OF_STATUS reg_status = CallNvOFRegisterResourceD3D12(
        fn->nvOFRegisterResourceD3D12, of_handle_, &reg, reg_seh);
    *handle_out = reinterpret_cast<NvOFGPUBufferHandle_st*>(handle);
    if (reg_status != NV_OF_SUCCESS) {
      XELOGE("NVOF: register {} failed (status={}, seh=0x{:08X}).", name,
             static_cast<int>(reg_status), reg_seh);
      char detail[512] = {};
      uint32_t detail_size = sizeof(detail);
      uint32_t err_seh = 0;
      NV_OF_STATUS err_status = CallNvOFGetLastError(
          fn->nvOFGetLastError, of_handle_, detail, &detail_size, err_seh);
      XELOGE("NVOF: register {} SDK detail (status={}): {}", name,
             static_cast<int>(err_status), detail);
      return false;
    }
    XELOGI("NVOF: registered {} (completion fence value {}).", name,
           registered + 1);
    ++registered;
    return true;
  };
  if (!register_resource(frame_pool_[0].Get(), &h_frame_pool_[0],
                         "framePool0") ||
      !register_resource(frame_pool_[1].Get(), &h_frame_pool_[1],
                         "framePool1") ||
      !register_resource(grid_flow_texture_.Get(), &h_grid_flow_,
                         "gridFlow") ||
      !register_resource(grid_cost_texture_.Get(), &h_grid_cost_,
                         "gridCost")) {
    XELOGE("NVOF: resource registration failed. Rolling back. Caching as "
           "unavailable.");
    UnregisterResources();
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kUnavailable;
  }
  // Wait for the registration to complete (a one-time initialization wait,
  // never a per-frame wait). Use SetEventOnCompletion + WaitForSingleObject
  // (a proper fence wait, not a busy-wait).
  if (registered > 0) {
    HANDLE registration_event =
        CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!registration_event) {
      XELOGE("NVOF: failed to create the registration completion event. "
             "Caching as unavailable.");
      UnregisterResources();
      DestroySession();
      state_ = State::kUnavailable;
      return BackendResult::kInitFailed;
    }
    const HRESULT set_event_hr =
        registration_completion->SetEventOnCompletion(registered,
                                                      registration_event);
    const DWORD waited =
        SUCCEEDED(set_event_hr)
            ? WaitForSingleObject(registration_event, 5000)
            : WAIT_FAILED;
    const uint64_t completed =
        registration_completion->GetCompletedValue();
    if (waited != WAIT_OBJECT_0 ||
        registration_completion->GetCompletedValue() < registered) {
      XELOGE("NVOF: registration completion timed out or failed (completed={}, "
             "requested={}, wait={}).",
             completed, registered, waited);
      CloseHandle(registration_event);
      UnregisterResources();
      DestroySession();
      state_ = State::kUnavailable;
      return BackendResult::kSyncConditionFailed;
    }
    CloseHandle(registration_event);
  }
  XELOGI("NVOF: all resources registered and registration completed.");

  // Form B (Milestone 4-E): create the backend-owned command allocator +
  // command list for the ABGR8 conversion pass. The conversion pass is
  // executed on the direct queue BEFORE the NVOF execute is submitted to the
  // OF engine (the NVOF execute is asynchronous; it submits to its own
  // hardware queue at call time, not into the caller's command list). The
  // input fence is signaled by the conversion pass, and the OF engine waits
  // on it. See the design doc (milestone-4-e).
  if (FAILED(device_->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_COMPUTE,
          IID_PPV_ARGS(&conversion_command_allocator_))) ||
      FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                       conversion_command_allocator_.Get(),
                                       nullptr,
                                       IID_PPV_ARGS(&conversion_command_list_)))) {
    XELOGE("NVOF: failed to create the conversion command list/allocator. "
           "Caching as unavailable.");
    UnregisterResources();
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }

  state_ = State::kAvailable;
  XELOGI("NVOF: backend initialized ({}x{}, grid={}x{}, quality={}, "
         "frame_index={}).",
         input_width_, input_height_, grid_width_, grid_height_, quality_,
         frame_index);
  return BackendResult::kSuccess;
}

BackendResult NVOFOpticalFlowBackend::TryProcess(
    ID3D12GraphicsCommandList* command_list, ID3D12CommandQueue* command_queue,
    ID3D12Resource* input_frame, uint32_t frame_index) {
  // 0. Cached unavailable: do not retry the SDK.
  if (state_ == State::kUnavailable) {
    return BackendResult::kUnavailable;
  }
  if (!command_list || !command_queue || !input_frame) {
    XELOGW("NVOF: input contract failed: null command_list, command_queue "
           "or input_frame.");
    return BackendResult::kInputContractFailed;
  }

  // Lazy init on the first frame.
  if (state_ == State::kNotInitialized) {
    BackendResult init_result = Initialize(command_list, frame_index);
    if (init_result != BackendResult::kSuccess) {
      return init_result;
    }
  }

  auto fn = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(fn_list_);

  // Descriptor-handle helpers. The D3D12 API exposes only the HEAP-START
  // handle; a per-descriptor handle is the heap start advanced by
  // index * increment. cpu_view(i) is the CPU handle for CreateShaderResourceView
  // / CreateUnorderedAccessView; gpu_view(i) is the GPU handle for the root
  // descriptor table / root UAV.
  // This SDK exposes the descriptor increment size on the DEVICE
  // (ID3D12Device::GetDescriptorHandleIncrementSize), not on the heap
  // (ID3D12DescriptorHeap has no GetDescriptorIncrementSize here).
  const uint32_t view_inc = device_->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu_view = [&](uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        view_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<uint64_t>(i) * view_inc;
    return h;
  };
  auto gpu_view = [&](uint32_t i) {
    D3D12_GPU_DESCRIPTOR_HANDLE h =
        view_heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<uint64_t>(i) * view_inc;
    return h;
  };

  // Record the per-frame commands. Form-B orchestration (Milestone 4-E):
  //   1. ABGR8 conversion (input R10G10B10A2 -> ABGR8 current), recorded into
  //      the backend-owned conversion_command_list_; the input fence is then
  //      signaled by the GPU (after the conversion); then (first frame only)
  //      the ABGR8 previous frame is zero-filled. This conversion submission is
  //      executed on the direct queue and the CPU waits for it to complete
  //      before the NVOF execute is submitted (the NVOF execute is
  //      asynchronous: it submits to the OF engine's own hardware queue, not
  //      into the caller's command list, so the conversion must have run first).
  //   2. NvOFExecuteD3D12 (input = ABGR8 current, reference = ABGR8 previous),
  //      submitted to the OF engine.
  //   3. CPU wait on the output fence (the NVOF async execute must complete
  //      before the densify reads the grid flow/cost).
  //   4. Densify (grid flow/cost -> full-resolution MV + confidence), recorded
  //      into the caller's command list (executed at the end, after the SR
  //      block).
  const uint32_t current_index = frame_index % 2;
  const uint32_t previous_index =
      (frame_index == 0) ? current_index : (frame_index - 1) % 2;

  // Reset the backend-owned conversion command allocator/list for this frame.
  // Safe to reset here: the previous frame's conversion commands have fully
  // completed (the previous frame's TryProcess CPU-waited on the output fence,
  // which implies its conversion pass already completed, because the OF engine
  // waited on the input fence that the conversion pass signaled).
  conversion_command_allocator_->Reset();
  conversion_command_list_->Reset(conversion_command_allocator_.Get(), nullptr);

  // --- 1a. ABGR8 conversion pass (compute shader, UAV write), recorded into
  //         the backend-owned conversion command list. ---
  D3D12_CPU_DESCRIPTOR_HANDLE input_srv_handle = cpu_view(0);
  device_->CreateShaderResourceView(input_frame, nullptr, input_srv_handle);
  D3D12_CPU_DESCRIPTOR_HANDLE current_uav_handle = cpu_view(2);
  device_->CreateUnorderedAccessView(frame_pool_[current_index].Get(), nullptr,
                                     nullptr, current_uav_handle);

  conversion_command_list_->SetComputeRootSignature(root_signature_.Get());
  conversion_command_list_->SetPipelineState(conversion_pso_.Get());
  conversion_command_list_->SetComputeRootDescriptorTable(0, gpu_view(0));
  // Root UAV parameters require GPU descriptor handles (not CPU handles).
  conversion_command_list_->SetComputeRootUnorderedAccessView(1, gpu_view(2).ptr);
  const uint32_t dispatch_x = (input_width_ + 7) / 8;
  const uint32_t dispatch_y = (input_height_ + 7) / 8;
  conversion_command_list_->Dispatch(dispatch_x, dispatch_y, 1);

  // --- 1c. (First frame only) zero-fill the ABGR8 previous frame, recorded
  //         into the backend-owned conversion command list. ---
  if (frame_index == 0) {
    D3D12_RESOURCE_BARRIER barrier_prev{};
    barrier_prev.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_prev.Transition.pResource = frame_pool_[previous_index].Get();
    barrier_prev.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier_prev.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier_prev.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    conversion_command_list_->ResourceBarrier(1, &barrier_prev);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    fp.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    fp.Footprint.Width = input_width_;
    fp.Footprint.Height = input_height_;
    fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = input_width_ * 4;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = frame_pool_[previous_index].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = zero_upload_buffer_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    conversion_command_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER barrier_prev_back{};
    barrier_prev_back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_prev_back.Transition.pResource = frame_pool_[previous_index].Get();
    barrier_prev_back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier_prev_back.Transition.StateAfter =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier_prev_back.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    conversion_command_list_->ResourceBarrier(1, &barrier_prev_back);
  }

  // --- 1d. Close the conversion command list, submit it on the direct queue,
  //         and signal the input fence (GPU) once the conversion completes.
  //         The NVOF OF engine waits on this fence before running, so it cannot
  //         read an unwritten ABGR8 current frame. Note: ID3D12GraphicsCommandList
  //         has no Signal method in this SDK; the fence is signaled from the
  //         command queue (it is signaled when all previously submitted command
  //         lists on this queue - just the conversion - have completed). ---
  conversion_command_list_->Close();
  {
    ID3D12CommandList* conv_list = conversion_command_list_.Get();
    command_queue->ExecuteCommandLists(1, &conv_list);
    command_queue->Signal(input_fence_.Get(), frame_index);
  }

  // --- 1e. CPU wait for the conversion to complete (input fence). The NVOF
  //         execute must be submitted after the conversion pass has executed
  //         (otherwise the OF engine waits on the input fence, which is
  //         signaled by the conversion pass; submitting the execute before the
  //         conversion runs would serialize the two and risk a deadlock). The
  //         input fence is signaled by the conversion pass to frame_index.
  //         On the first frame (frame_index == 0) the fence is already at 0
  //         (pre-signaled), so this wait completes immediately; that is fine
  //         because the first frame's flow is invalid and discarded. ---
  {
    HANDLE input_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!input_event) {
      XELOGE("NVOF: failed to create the input completion event.");
      has_valid_flow_ = false;
      return BackendResult::kSyncConditionFailed;
    }
    const HRESULT set_input_event_hr = input_fence_->SetEventOnCompletion(
        frame_index, input_event);
    const DWORD input_waited = SUCCEEDED(set_input_event_hr)
                                   ? WaitForSingleObject(input_event, 5000)
                                   : WAIT_FAILED;
    const uint64_t input_completed = input_fence_->GetCompletedValue();
    if (input_waited != WAIT_OBJECT_0 || input_completed < frame_index) {
      XELOGE("NVOF: input fence wait timed out or failed (completed={}, "
             "requested={}, wait={}).",
             input_completed, frame_index, input_waited);
      CloseHandle(input_event);
      has_valid_flow_ = false;
      return BackendResult::kSyncConditionFailed;
    }
    CloseHandle(input_event);
  }

  // --- 2. NvOFExecuteD3D12 (through the function table). ---
  NV_OF_FENCE_POINT in_fence_point{};
  in_fence_point.fence = input_fence_.Get();
  in_fence_point.value = frame_index;
  NV_OF_FENCE_POINT out_fence_point{};
  out_fence_point.fence = output_fence_.Get();
  out_fence_point.value = next_output_value_;

  NV_OF_EXECUTE_INPUT_PARAMS_D3D12 exec_in{};
  exec_in.inputFrame =
      reinterpret_cast<NvOFGPUBufferHandle>(h_frame_pool_[current_index]);
  exec_in.referenceFrame =
      reinterpret_cast<NvOFGPUBufferHandle>(h_frame_pool_[previous_index]);
  exec_in.externalHints = nullptr;
  // Match Veyra's verified probe path: temporal hints disabled (each frame is an
  // independent pair). Enabling temporal hints is a future optimization.
  exec_in.disableTemporalHints = NV_OF_TRUE;
  exec_in.numRois = 0;
  exec_in.roiData = nullptr;
  exec_in.numFencePoints = 1;
  exec_in.fencePoint = &in_fence_point;

  NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 exec_out{};
  exec_out.outputBuffer =
      reinterpret_cast<NvOFGPUBufferHandle>(h_grid_flow_);
  exec_out.outputCostBuffer =
      reinterpret_cast<NvOFGPUBufferHandle>(h_grid_cost_);
  exec_out.bwdOutputBuffer = nullptr;
  exec_out.bwdOutputCostBuffer = nullptr;
  exec_out.globalFlowBuffer = nullptr;
  exec_out.fencePoint = &out_fence_point;

  NV_OF_STATUS exec_status = NV_OF_ERR_GENERIC;
  {
    uint32_t exec_seh = 0;
    exec_status = CallNvOFExecuteD3D12(fn->nvOFExecuteD3D12, of_handle_,
                                       &exec_in, &exec_out, exec_seh);
    if (exec_status != NV_OF_SUCCESS) {
      XELOGE("NVOF: nvOFExecuteD3D12 failed (status={}, seh=0x{:08X}, "
             "frame_index={}).",
             static_cast<int>(exec_status), exec_seh, frame_index);
      has_valid_flow_ = false;
      return BackendResult::kExecutionFailed;
    }
  }
  ++next_output_value_;
  XELOGD("NVOF: nvOFExecuteD3D12 recorded (frame_index={}, output fence "
         "value {}).",
         frame_index, out_fence_point.value);

  // --- 3. Wait on the output fence before the densify reads the grid
  //     flow/cost. NVOF's execute is asynchronous (it submits to its own
  //     internal command queue and signals the output fence when done). The
  //     densify dispatch must run after that fence is signaled, otherwise it
  //     races the execute and reads unwritten grid flow/cost.
  //
  //     DESIGN NOTE (this SDK): unlike the standard D3D12 API, this SDK's
  //     ID3D12GraphicsCommandList has no Wait() method (and ID3D12Fence has no
  //     CPU Wait either), so a GPU-side wait cannot be recorded into the
  //     caller's command list. We therefore do a CPU-side fence wait here
  //     (SetEventOnCompletion + WaitForSingleObject, the same pattern as the
  //     one-time registration wait in Initialize). This blocks the CPU until
  //     the NVOF execute completes, then the densify dispatch is recorded. It
  //     is correct (no race) but serializes the execute with the densify
  //     recording (no GPU overlap); on real hardware a command-queue
  //     Wait(output_fence) would be preferable. Real-GPU behavior is
  //     UNVERIFIED on this box (no NVIDIA driver).
  {
    HANDLE output_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!output_event) {
      XELOGE("NVOF: failed to create the output completion event.");
      has_valid_flow_ = false;
      return BackendResult::kSyncConditionFailed;
    }
    const HRESULT set_event_hr = output_fence_->SetEventOnCompletion(
        out_fence_point.value, output_event);
    const DWORD waited =
        SUCCEEDED(set_event_hr)
            ? WaitForSingleObject(output_event, 5000)
            : WAIT_FAILED;
    const uint64_t completed = output_fence_->GetCompletedValue();
    if (waited != WAIT_OBJECT_0 ||
        output_fence_->GetCompletedValue() < out_fence_point.value) {
      XELOGE("NVOF: output fence wait timed out or failed (completed={}, "
             "requested={}, wait={}).",
             completed, out_fence_point.value, waited);
      CloseHandle(output_event);
      has_valid_flow_ = false;
      return BackendResult::kSyncConditionFailed;
    }
    CloseHandle(output_event);
  }

  // --- 4. Densify pass (compute shader, UAV write). ---
  D3D12_CPU_DESCRIPTOR_HANDLE grid_flow_srv = cpu_view(3);
  device_->CreateShaderResourceView(grid_flow_texture_.Get(), nullptr,
                                    grid_flow_srv);
  D3D12_CPU_DESCRIPTOR_HANDLE grid_cost_srv = cpu_view(4);
  device_->CreateShaderResourceView(grid_cost_texture_.Get(), nullptr,
                                    grid_cost_srv);
  D3D12_CPU_DESCRIPTOR_HANDLE mv_uav = cpu_view(5);
  device_->CreateUnorderedAccessView(motion_vector_texture_.Get(), nullptr,
                                     nullptr, mv_uav);
  D3D12_CPU_DESCRIPTOR_HANDLE conf_uav = cpu_view(6);
  device_->CreateUnorderedAccessView(confidence_texture_.Get(), nullptr,
                                     nullptr, conf_uav);

  command_list->SetComputeRootSignature(root_signature_.Get());
  command_list->SetPipelineState(densify_pso_.Get());
  command_list->SetComputeRootDescriptorTable(0, gpu_view(3));
  command_list->SetComputeRootUnorderedAccessView(1, gpu_view(5).ptr);
  command_list->SetComputeRootUnorderedAccessView(2, gpu_view(6).ptr);
  // Bind the densify cbuffer via root 32-bit constants (order matches the HLSL
  // cbuffer DensifyConstants: gridW, gridH, workingW, workingH, gridSize,
  // negate, costThreshold).
  const uint32_t densify_constants[7] = {grid_width_, grid_height_,
                                         input_width_, input_height_,
                                         grid_size_, kNegate, kCostThreshold};
  command_list->SetComputeRoot32BitConstants(3, 7, densify_constants, 0);
  command_list->Dispatch(dispatch_x, dispatch_y, 1);

  // Update the valid-flow flag. The first frame (frame_index == 0) has an
  // invalid flow (no valid reference); subsequent frames are valid.
  has_valid_flow_ = (frame_index > 0);

  return BackendResult::kSuccess;
}

}  // namespace nvof
}  // namespace ui
}  // namespace xe
