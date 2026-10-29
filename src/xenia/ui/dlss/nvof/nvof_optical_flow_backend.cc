/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/dlss/nvof/nvof_optical_flow_backend.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
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
// Densify semantics (fixed, documented):
//   - kNegate = 0: the raw NVOF flow already points current -> previous (given
//     inputFrame = current, referenceFrame = previous, verified by Veyra's
//     +8px displacement test). Preserve that direction; do NOT negate.
//   - kCostThreshold = 32: cells with cost >= 32 (confidence <= ~0.88) are
//     zeroed. Cost is a rejection signal (higher cost = less reliable). This
//     matches Veyra's verified densify threshold (EnhanceGraph dc[6]=32). The
//     higher prior value (128) kept unreliable low-confidence MVs as nonzero,
//     which DLSS SR smeared during temporal reprojection on rapidly changing
//     UI (speedometer/altimeter). Zeroing more aggressively reduces smear at
//     the cost of more ghosting (a zero MV reprojects stale history from the
//     same position); this is the verified Veyra reference point.
const uint32_t kNegate = 0;
const uint32_t kCostThreshold = 32;

// Stage-3 discard sentinel (experimental, hardware-unverified): for pixels
// whose content changed (cost >= costThreshold), the densify pass writes
// float2(sentinel, sentinel) to the MV instead of (0,0). DLSS SR has no
// per-pixel invalid-MV value, so this probes how the SR SDK interprets an
// extreme MV on a "changed" pixel. 0.0f preserves the current zero-MV
// behavior and is the safe default; candidates for controlled hardware
// testing are 65504.0, -65504.0 and NaN (the MV texture is R16G16_FLOAT
// half). Verification must follow the Veyra E-1-style displacement probe.
const float kDiscardSentinel = 0.0f;

// Packs a float's IEEE-754 bits so it can travel through a uint cbuffer.
uint32_t PackFloatBits(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  return bits;
}

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
// (NOT a CBV), so the cbuffer is declared at b0 with 9 uint32 members.
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
Texture2D<float4> previousColor : register(t2);   // ABGR8 previous frame
Texture2D<float4> currentColor : register(t3);    // ABGR8 current frame
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
    uint validationFlags;   // bit0 bounds, bit1 photometric (Veyra convention)
    uint discardSentinelBits;   // packed float: Stage-3 sentinel (see C++)
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
    // Stage-3: "content changed" pixels get the sentinel MV (0.0f default =
    // zero MV, current behavior). The out-of-bounds case below stays zeroed
    // (different semantics: reference outside the frame, not content change).
    float sentinel = asfloat(discardSentinelBits);
    if (cost >= costThreshold) {
      v = float2(sentinel, sentinel);
      confidence = 0.0;
    }
    else if (validationFlags != 0) {
        // Photometric/bounds validation (ported from Veyra's NvofDensify.hlsl):
        // reject out-of-frame reprojections and gate large luma disagreement.
        // The ABGR8 textures are sampled as (B, G, R, A), so .rgb is (B, G, R);
        // the luma weights are applied to that order, matching Veyra's behavior.
        float2 q = float2(globalId.xy) + v;
        bool outside = any(q < 0) || any(q > float2(workingW - 1, workingH - 1));
        if ((validationFlags & 1) != 0 && outside) { v = float2(0, 0); confidence = 0.0; }
        else if ((validationFlags & 2) != 0) {
            int2 a = (int2)floor(q);
            float2 t = frac(q);
            int2 limit = int2(workingW - 1, workingH - 1);
            float3 top    = lerp(previousColor[clamp(a, int2(0,0), limit)].rgb,
                                 previousColor[clamp(a + int2(1,0), int2(0,0), limit)].rgb, t.x);
            float3 bottom = lerp(previousColor[clamp(a + int2(0,1), int2(0,0), limit)].rgb,
                                 previousColor[clamp(a + int2(1,1), int2(0,0), limit)].rgb, t.x);
            float error = abs(dot(currentColor[globalId.xy].rgb - lerp(top, bottom, t.y),
                                  float3(.2126, .7152, .0722)));
            float trust = 1.0 - smoothstep(.03, .15, error);
            v *= trust;
            confidence *= trust;
        }
    }
    flowOut[globalId.xy] = v;
    confOut[globalId.xy] = confidence;
}
)";

// The zero-fill pass writes (0,0,0,0) to the ABGR8 previous frame (the
// frame-pool UAV). It replaces the old D2D CopyTextureRegion zero-fill: a
// compute dispatch writing zeros into the UAV avoids the UAV->COPY_DEST->UAV
// transition on a resource that is ALREADY registered with the NVOF SDK (the
// D2D copy into a registered resource was the top suspect for the driver
// access violation). This matches Veyra-NRVideo's pattern of writing ABGR8
// frames with compute dispatches (never a D2D copy).
const char kZeroFillHlsl[] = R"(
RWTexture2D<float4> targetTex : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 globalId : SV_DispatchThreadID)
{
    targetTex[globalId.xy] = float4(0.0, 0.0, 0.0, 0.0);
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

// Diagnostic (step 1): prints the DXBC magic, shader version, flags, the full
// chunk-offset table and the derived code-chunk size of a bytecode blob, plus
// a WELL-FORMED / MALFORMED verdict. The DXBC header (as UINT words, read
// directly to avoid an SDK header dependency): word 0 is the magic, words 1-4
// are the 16-byte program ID, word 5 is the header Version (Major = high word,
// Minor = low word), word 6 Flags, and words 7-12 are the chunk-offset table
// (cOffsetCBuffer, cOffsetIMmediates, cOffsetCode, cOffsetDebug,
// cOffsetDebugSource, cOffsetExtra). The code-chunk size is the blob size minus
// cOffsetCode. A well-formed cs_5_1 blob has magic 0x43425844, version 5.1 and
// a non-zero code chunk. NOTE: the previous version of this function read the
// WRONG offsets (words[8] = cOffsetIMmediates as "cBytesCode", and words[29] -
// a non-existent header field - as "instruction count"), which made EVERY blob
// (including the valid no-op) print "version=0.1 cBytesCode=52 instructions=0
// => MALFORMED". That verdict was a false positive and must not be used as a
// judgment basis; the authoritative bytecode check is D3DReflect
// (DescribeShaderBindings below), which actually parses the chunk table.
void DescribeBytecode(
    const char* name,
    Microsoft::WRL::ComPtr<ID3DBlob>& blob) {
  if (!blob) {
    XELOGE("NVOF: bytecode {} is null.", name);
    return;
  }

  const auto* data = static_cast<const uint8_t*>(
      blob->GetBufferPointer());
  const size_t size = blob->GetBufferSize();

  // DXBC container header:
  // 0x00: magic ("DXBC")
  // 0x04: 16-byte checksum
  // 0x14: container version
  // 0x18: total container size
  // 0x1C: chunk count
  constexpr size_t kHeaderSize = 32;
  constexpr uint32_t kDxbcMagic = 0x43425844;  // "DXBC"
  constexpr uint32_t kShdrMagic = 0x52444853;  // "SHDR"
  constexpr uint32_t kShexMagic = 0x58454853;  // "SHEX"

  auto read_u32 = [&](size_t offset, uint32_t* value) -> bool {
    if (offset > size || size - offset < sizeof(uint32_t)) {
      return false;
    }

    std::memcpy(value, data + offset, sizeof(uint32_t));
    return true;
  };

  auto fourcc = [](uint32_t value) -> char const* {
    switch (value) {
      case kShdrMagic:
        return "SHDR";
      case kShexMagic:
        return "SHEX";
      default:
        return nullptr;
    }
  };

  if (size < kHeaderSize) {
    XELOGE("NVOF: bytecode {} too small for DXBC header ({} bytes).",
           name, size);
    return;
  }

  uint32_t magic = 0;
  uint32_t container_version = 0;
  uint32_t declared_size = 0;
  uint32_t chunk_count = 0;

  if (!read_u32(0, &magic) ||
      !read_u32(20, &container_version) ||
      !read_u32(24, &declared_size) ||
      !read_u32(28, &chunk_count)) {
    XELOGE("NVOF: bytecode {} header is truncated.", name);
    return;
  }

  if (magic != kDxbcMagic) {
    XELOGE("NVOF: bytecode {} has invalid magic 0x{:08X}.",
           name, magic);
    return;
  }

  if (declared_size != size) {
    XELOGE(
        "NVOF: bytecode {} container size mismatch "
        "(declared={}, actual={}).",
        name, declared_size, size);
    return;
  }

  // Chunk offset table starts immediately after the 32-byte header.
  const size_t table_offset = kHeaderSize;
  if (chunk_count >
      (size - table_offset) / sizeof(uint32_t)) {
    XELOGE("NVOF: bytecode {} has an invalid chunk count ({}).",
           name, chunk_count);
    return;
  }

  const size_t table_size =
      static_cast<size_t>(chunk_count) * sizeof(uint32_t);
  const size_t chunks_start = table_offset + table_size;

  bool valid = true;
  bool found_shader = false;

  uint32_t shader_kind = 0;
  uint32_t shader_version = 0;
  uint32_t shader_length = 0;

  for (uint32_t i = 0; i < chunk_count; ++i) {
    uint32_t chunk_offset_u32 = 0;

    if (!read_u32(table_offset + i * sizeof(uint32_t),
                  &chunk_offset_u32)) {
      valid = false;
      break;
    }

    const size_t chunk_offset = chunk_offset_u32;

    // Each chunk has a 4-byte FourCC and a 4-byte data size.
    if (chunk_offset < chunks_start ||
        chunk_offset > size ||
        size - chunk_offset < 8) {
      valid = false;
      break;
    }

    uint32_t chunk_kind = 0;
    uint32_t chunk_size = 0;

    if (!read_u32(chunk_offset, &chunk_kind) ||
        !read_u32(chunk_offset + 4, &chunk_size)) {
      valid = false;
      break;
    }

    const size_t payload_offset = chunk_offset + 8;

    if (chunk_size > size - payload_offset) {
      valid = false;
      break;
    }

    if (chunk_kind == kShdrMagic ||
        chunk_kind == kShexMagic) {
      if (chunk_size < 8) {
        valid = false;
        break;
      }

      // Shader token stream:
      // token 0 = shader version (program type / major / minor)
      // token 1 = shader length in DWORDs
      if (!read_u32(payload_offset, &shader_version) ||
          !read_u32(payload_offset + 4, &shader_length)) {
        valid = false;
        break;
      }

      if (shader_length < 2 ||
          static_cast<uint64_t>(shader_length) * 4 != chunk_size) {
        valid = false;
        break;
      }

      shader_kind = chunk_kind;
      found_shader = true;
    }
  }

  if (!valid) {
    XELOGE("NVOF: bytecode {} has a malformed DXBC chunk table.",
           name);
    return;
  }

  const uint32_t program_type = (shader_version >> 16) & 0xFFFF;
  const uint32_t shader_major = (shader_version >> 4) & 0xF;
  const uint32_t shader_minor = shader_version & 0xF;

  XELOGI(
      "NVOF: bytecode {} ({} bytes): "
      "container_version={} declared_size={} chunks={} "
      "=> WELL-FORMED DXBC container",
      name, size, container_version, declared_size, chunk_count);

  if (found_shader) {
    XELOGI(
        "NVOF: bytecode {}: shader_chunk={} "
        "program_type={} shader_model={}.{} token_dwords={}",
        name, fourcc(shader_kind), program_type, shader_major,
        shader_minor, shader_length);
  } else {
    XELOGW(
        "NVOF: bytecode {}: valid DXBC container but no SHDR/SHEX chunk found.",
        name);
  }
}

// D3DReflect is loaded from the same d3dcompiler module as D3DCompile (the
// module is self-contained and loads d3dcompiler itself, so it does not depend
// on the D3D12 provider's shader-reflection internals).
//
// IMPORTANT: the real d3dcompiler_47.dll export is the MODERN 4-argument form
// (verified against the DLL's export table and the system <d3dcompiler.h>):
//     HRESULT D3DReflect(LPCVOID pSrcData, SIZE_T SrcDataSize,
//                        REFIID pInterface, void** ppReflector);
// The legacy 6-argument form (with pTarget and ppErrorMsgs) is NOT exported,
// so calling it through a 6-arg prototype would pass a string where a GUID is
// expected and corrupt the call. We use the 4-arg prototype. There is no error
// blob output; a rejected bytecode simply yields a failing HRESULT.
typedef HRESULT(WINAPI* PFN_D3DReflect)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, REFIID pInterface, void** ppReflector);

PFN_D3DReflect LoadD3DReflect() {
  HMODULE lib = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!lib) {
    lib = LoadLibraryExW(L"d3dcompiler.dll", nullptr,
                         LOAD_LIBRARY_SEARCH_SYSTEM32);
  }
  if (!lib) {
    return nullptr;
  }
  return reinterpret_cast<PFN_D3DReflect>(GetProcAddress(lib, "D3DReflect"));
}

// The Type field of D3D12_SHADER_INPUT_BIND_DESC is a D3D_SHADER_INPUT_TYPE
// (the D3D10-style enum in d3dcommon.h), NOT the D3D12_SIT_* names. The reduced
// system SDK only defines D3D_SIT_* (cbuffer=0, tbuffer=1, texture=2, sampler=3,
// uav_rwtyped=4, structured=5, uav_rwstructured=6). D3DReflect for a D3D12
// compute shader reports RWTexture2D as D3D_SIT_UAV_RWTYPED (4) and Texture2D as
// D3D_SIT_TEXTURE (2).
const char* DescribeSitType(UINT type) {
  switch (type) {
    case D3D_SIT_CBUFFER:
      return "cbuffer";
    case D3D_SIT_TBUFFER:
      return "tbuffer";
    case D3D_SIT_TEXTURE:
      return "texture(SRV)";
    case D3D_SIT_SAMPLER:
      return "sampler";
    case D3D_SIT_UAV_RWTYPED:
      return "uav(rwtyped)";
    case D3D_SIT_STRUCTURED:
      return "structured";
    case D3D_SIT_UAV_RWSTRUCTURED:
      return "uav(rwstructured)";
    default:
      return "unknown";
  }
}

// Diagnostic (steps 1 + 2): the authoritative bytecode check and the UAV/SRV
// binding cross-check. D3DReflect parses the DXBC chunk offset table and fails
// (E_NOTIMPL/E_INVALIDARG) on a truncated or corrupted blob - unlike
// DescribeBytecode's header hint, this is the real parse. On success it returns
// a shader reflector; we then enumerate every resource binding with
// GetResourceBindingDesc and print its name, type, bind point, bind count and
// register space. That is the shader's ACTUAL declared binding layout, which we
// compare against the root signature: a shader that dereferences a register the
// RS does not cover (or that covers with a mismatched type/visibility) is
// rejected by CreateComputePipelineState with E_INVALIDARG.
void DescribeShaderBindings(PFN_D3DReflect d3d_reflect, const char* name,
                            const void* bytecode, SIZE_T size) {
  if (!d3d_reflect) {
    XELOGE("NVOF: D3DReflect unavailable; skipping binding dump for {}.", name);
    return;
  }
  // The modern 4-arg D3DReflect takes (bytecode, size, riid, void** ppReflector)
  // and infers the shader type from the bytecode itself (no "cs_5_1" target, no
  // error blob). It returns an IUnknown*; we cast it to ID3D12ShaderReflection.
  void* reflector_unknown = nullptr;
  const HRESULT hr = d3d_reflect(bytecode, size, IID_ID3D12ShaderReflection,
                                 &reflector_unknown);
  if (FAILED(hr)) {
    XELOGE("NVOF: D3DReflect {} failed (hr=0x{:08X}){}: bytecode rejected by "
           "the CPU-side parser.",
           name, static_cast<unsigned int>(hr),
           std::string());
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12ShaderReflection> reflection(
      static_cast<ID3D12ShaderReflection*>(reflector_unknown));
  XELOGI("NVOF: D3DReflect {} OK (bytecode parses).", name);
  UINT i = 0;
  for (;;) {
    D3D12_SHADER_INPUT_BIND_DESC bind{};
    if (FAILED(reflection->GetResourceBindingDesc(i, &bind))) {
      break;
    }
    XELOGI("NVOF:   binding[{}] name=\"{}\" type={} bindPoint={} bindCount={} "
           "space={}",
           i, bind.Name, DescribeSitType(bind.Type), bind.BindPoint,
           bind.BindCount, bind.Space);
    ++i;
  }
}

// Diagnostic (step 3): on the real-device CreateComputePipelineState failure,
// re-attempt the same PSO creation on a throwaway device with the D3D12 debug
// layer enabled and drain its Info Queue. The debug layer is only active when
// the device is created with D3D12_CREATE_DEVICE_DEBUG, and our device is
// created by Xenia without that flag, so the real device's queue is always
// empty here - this is the only way to get the debug layer's message.
//
// The reduced system SDK declares a 3-argument D3D12CreateDevice (no Flags) and
// omits D3D_FEATURE_LEVEL_1_1, D3D12_CREATE_DEVICE_DEBUG and
// ID3D12Device::GetInfoQueue. So we (a) dynamically load the real 4-argument
// D3D12CreateDevice from d3d12.dll to pass the debug flag, and (b) obtain the
// Info Queue via QueryInterface(IID_ID3D12InfoQueue) rather than GetInfoQueue.
// ID3D12InfoQueue, D3D12_MESSAGE and IID_ID3D12InfoQueue come from the system
// <d3d12sdklayers.h>. The D3D12_MESSAGE layout (with the ID field) and the
// GetMessage(UINT64, D3D12_MESSAGE*, SIZE_T*) signature are the v0 interface.
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice4)(
    IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, UINT Flags,
    REFIID riid, ID3D12Device** ppDevice);

void ProbePsoWithDebugLayer(const char* name, ID3D12RootSignature* rs,
                            const void* bytecode, SIZE_T size) {
  HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!d3d12) {
    XELOGE("NVOF: debug layer probe ({}): failed to load d3d12.dll.", name);
    return;
  }
  auto create_device = reinterpret_cast<PFN_D3D12CreateDevice4>(
      GetProcAddress(d3d12, "D3D12CreateDevice"));
  if (!create_device) {
    XELOGE("NVOF: debug layer probe ({}): D3D12CreateDevice not found.", name);
    return;
  }
  const UINT kDebugFlag = 0x00000001;      // D3D12_CREATE_DEVICE_DEBUG
  const UINT kBgpOnlyFlag = 0x00001000;    // D3D12_CREATE_DEVICE_BGP_ONLY
  const D3D_FEATURE_LEVEL kFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(0x00010100);
  Microsoft::WRL::ComPtr<ID3D12Device> debug_device;
  const HRESULT create_hr = create_device(
      nullptr, kFeatureLevel, kDebugFlag | kBgpOnlyFlag, IID_ID3D12Device,
      debug_device.ReleaseAndGetAddressOf());
  if (FAILED(create_hr)) {
    XELOGE("NVOF: debug layer probe ({}): failed to create a debug device "
           "(hr=0x{:08X}).",
           name, static_cast<unsigned int>(create_hr));
    return;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs;
  desc.CS.pShaderBytecode = bytecode;
  desc.CS.BytecodeLength = size;
  desc.NodeMask = 0;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr =
      debug_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
  XELOGI("NVOF: debug layer probe ({}): CreateComputePipelineState hr=0x{:08X}.",
         name, static_cast<unsigned int>(hr));
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;
  if (SUCCEEDED(debug_device->QueryInterface(IID_PPV_ARGS(&queue)))) {
    for (UINT64 i = 0;; ++i) {
      D3D12_MESSAGE msg{};
      SIZE_T len = sizeof(msg);
      if (FAILED(queue->GetMessage(i, &msg, &len))) {
        break;
      }
      XELOGE("NVOF: debug layer ({}): severity={} category={} id=0x{:08X}: {}",
             name, static_cast<unsigned int>(msg.Severity),
             static_cast<unsigned int>(msg.Category),
             static_cast<unsigned int>(msg.ID), msg.pDescription);
    }
  } else {
    XELOGW("NVOF: debug layer probe ({}): QueryInterface(ID3D12InfoQueue) "
           "failed; no messages available.",
           name);
  }
}

// Logs the device's removed reason for a named stage. The device pointer is
// logged so a reader can confirm this is the SAME ID3D12Device that executed
// the stage's work (a stale or wrong device pointer would report a cached
// error). The removed reason is queried via GetDeviceRemovedReason() at the
// call site; it is NOT a cached value and NOT a generic HRESULT from another
// call. S_OK means the device is alive; any other value is the real reason
// the driver removed it (e.g. 0x887A0001 = DXGI_ERROR_INVALID_CALL,
// 0x887A0005 = DXGI_ERROR_DEVICE_REMOVED). A GPU shader fault normally yields
// DEVICE_REMOVED, NOT INVALID_CALL, so an INVALID_CALL here points at a
// driver-side validation rejection rather than a compute dispatch fault.
void LogDeviceState(ID3D12Device* device, const char* stage) {
  const HRESULT removed_reason = device->GetDeviceRemovedReason();
  XELOGI("NVOF: stage={}, device={:p}, removed_reason=0x{:08X}{}",
         stage, static_cast<void*>(device),
         static_cast<uint32_t>(removed_reason),
         SUCCEEDED(removed_reason) ? " (alive)" : "");
}

// Waits on a fence and logs the target value, the completed value, and the
// wait result. The wait result (WAIT_OBJECT_0 / WAIT_TIMEOUT / WAIT_FAILED)
// is distinct from the device removed reason: a timed-out wait means the GPU
// work did not complete, which is NOT the same as the device being removed.
// The caller must CloseHandle the returned event. Returns true on success.
bool WaitFenceAndLog(ID3D12Fence* fence, uint64_t target, const char* stage,
                     ID3D12Device* device) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event) {
    XELOGE("NVOF: stage={}: failed to create the fence completion event.",
           stage);
    return false;
  }
  const HRESULT set_hr = fence->SetEventOnCompletion(target, event);
  const DWORD waited = SUCCEEDED(set_hr) ? WaitForSingleObject(event, 5000)
                                         : WAIT_FAILED;
  const uint64_t completed = fence->GetCompletedValue();
  // UINT64_MAX means the device was removed: the fence never actually
  // completed, and SetEventOnCompletion spuriously signals the event (the
  // fence is "complete" at UINT64_MAX, which is >= any target). A wait that
  // returns WAIT_OBJECT_0 with completed == UINT64_MAX is a DEVICE FAILURE,
  // NOT a sync success. Treat it as a device exception immediately.
  const bool device_removed = (completed == UINT64_MAX);
  const bool sync_ok =
      (waited == WAIT_OBJECT_0) && !device_removed && (completed >= target);
  XELOGI("NVOF: stage={}: fence target={}, completed={}, set_hr=0x{:08X}, "
         "wait={}",
         stage, target, completed, static_cast<uint32_t>(set_hr), waited,
         sync_ok ? " (sync ok)" : " (sync FAIL)");
  if (device_removed) {
    XELOGE("NVOF: stage={}: fence GetCompletedValue()=UINT64_MAX -- the "
           "device was removed; the fence never completed and the wait "
           "result is spurious. Treating as a device exception.",
           stage);
  }
  // Re-query the device state AFTER the wait: a GPU fault during the waited
  // work removes the device, and the wait may still "succeed" (the fence is
  // signaled) while the device is already gone.
  LogDeviceState(device, stage);
  CloseHandle(event);
  return sync_ok;
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
  // Destroy the SDK session (nvOFDestroy, then delete the function table, then
  // FreeLibrary).
  DestroySession();
  // Release the backend-owned fences, heap, signature, and PSOs.
  input_fence_.Reset();
  output_fence_.Reset();
  view_heap_.Reset();
  root_signature_.Reset();
  conversion_rs_.Reset();
  conversion_pso_.Reset();
  densify_pso_.Reset();
  zero_fill_rs_.Reset();
  zero_fill_pso_.Reset();
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

// Binds the private view heap to the backend-owned conversion command list.
// SetDescriptorHeaps returns void (no HRESULT), so a failed bind is NOT
// observable via a return code; correctness rests on (a) the object-validity
// guard below, (b) the D3D12 Debug Layer (if enabled on the target), and (c)
// the 4090 runtime verification. Must be called after every Reset (Reset
// clears all bound descriptor heaps) and after command-list creation.
// Precondition: view_heap_ is a successfully created SHADER_VISIBLE heap
// (created at Initialize before the command list; a creation failure returns
// kInitFailed early, so it is always valid at the call sites).
void NVOFOpticalFlowBackend::BindViewHeap(const char* stage) {
  if (!view_heap_.Get() || !conversion_command_list_.Get()) {
    XELOGE("NVOF: BindViewHeap({}): view_heap_ or conversion_command_list_ "
           "is null; skipping bind.",
           stage);
    return;
  }
  ID3D12DescriptorHeap* heaps[] = {view_heap_.Get()};
  conversion_command_list_->SetDescriptorHeaps(1, heaps);
  XELOGI("NVOF: BindViewHeap({}): conversion_command_list={:p}, view_heap={:p}, "
         "gpu_heap_start=0x{:016X}.",
         stage, static_cast<void*>(conversion_command_list_.Get()),
         static_cast<void*>(view_heap_.Get()),
         static_cast<unsigned long long>(
             view_heap_->GetGPUDescriptorHandleForHeapStart().ptr));
}

BackendResult NVOFOpticalFlowBackend::Initialize(
    ID3D12GraphicsCommandList* command_list, ID3D12CommandQueue* command_queue,
    uint32_t frame_index) {
  if (!device_ || !command_list || !command_queue) {
    XELOGE("NVOF: Initialize called with a null device, command_list, or "
           "command_queue; "
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
  {
    const HRESULT dr = device_->GetDeviceRemovedReason();
    if (FAILED(dr)) {
      XELOGE("NVOF: device already removed right after nvCreateOpticalFlowD3D12 "
             "(0x{:08X}) -- the session-creation GPU work faulted.",
             static_cast<uint32_t>(dr));
    }
  }

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
  {
    const HRESULT dr = device_->GetDeviceRemovedReason();
    if (FAILED(dr)) {
      XELOGE("NVOF: device removed right after nvOFInit (0x{:08X}) -- the "
             "session-init GPU work (un-fenced, on the SDK queue) faulted.",
             static_cast<uint32_t>(dr));
    }
  }

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

  // 8. Create the raw grid flow (R16G16_SINT) and cost (R8_UINT) textures at
  //    the grid extent. They are read by the densify pass (SRV), so they start
  //    in PIXEL_SHADER_RESOURCE. The NVOF execute writes them (NVOF manages
  //    its own internal state); the output fence synchronizes the write before
  //    the densify reads.
  //    The cost texture MUST be R8_UINT (the SDK and Veyra's verified contract
  //    both require DXGI_FORMAT_R8_UINT, not R8_UNORM). The SDK writes 8-bit
  //    cost values; the densify shader reads it as Texture2D<uint>. R8_UINT is
  //    the contract-correct format (R8_UNORM was a silent deviation that the
  //    SDK happened to accept at registration but is not the verified pattern).
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
      !create_grid_tex(DXGI_FORMAT_R8_UINT, grid_cost_texture_)) {
    XELOGE("NVOF: failed to create grid flow/cost textures. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: grid flow (R16G16_SINT) and cost (R8_UINT) textures created "
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

  // 11. Create the input/output fences. BOTH start at value 0: the first
  //     input-fence signal is frame_index + 1 == 1 (strictly above the start,
  //     so the signal is a real GPU wait, not a no-op), and the first
  //     output-fence signal is next_output_value_ == 1 (likewise strictly
  //     above the start, so the stage-C CPU wait really waits for the NVOF
  //     execute). Creating the output fence at value 1 made the first
  //     execute's output-fence point (value 1) a no-op signal and the
  //     stage-C wait trivially satisfied, letting the densify pass race the
  //     asynchronous NVOF execute (the input fence had the same bug class,
  //     fixed earlier: Signal to frame_index + 1, not frame_index).
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&input_fence_))) ||
      FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&output_fence_)))) {
    XELOGE("NVOF: failed to create input/output fences. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: input/output fences created (both start at value 0).");

  // 12. Create the private view heap + root signature.
  //     Descriptor layout (9 descriptors, indices 0..8):
  //       0: input frame SRV        (conversion t0)
  //       1: frame pool UAV         (conversion u0)
  //       2: spare UAV              (conversion UAV-table u1, valid but unused)
  //       3: grid flow SRV          (densify t0)
  //       4: grid cost SRV          (densify t1)
  //       5: ABGR8 previous SRV     (densify t2, validation)
  //       6: ABGR8 current SRV      (densify t3, validation)
  //       7: motion vector UAV      (densify u0)
  //       8: confidence UAV         (densify u1)
  //       9: ABGR8 previous UAV     (zero-fill u0)
  //     Root signature: [0] SRV descriptor table (t0/t1/t2/t3), [1] UAV
  //     descriptor table (u0/u1, two descriptors), [2] 32-bit constants
  //     (9, densify cbuffer b0).
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 10;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&heap_desc,
                                           IID_PPV_ARGS(&view_heap_)))) {
    XELOGE("NVOF: failed to create private view heap. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Root signature for the densify pass:
  //   [0] SRV descriptor table (t0 = grid-flow, t1 = grid-cost,
  //       t2 = ABGR8 previous, t3 = ABGR8 current, for validation),
  //   [1] UAV descriptor table (u0 = full-resolution MV, u1 = confidence),
  //   [2] 32-bit constants (b0, the densify cbuffer: 9 values).
  //
  // NOTE: the UAVs are a descriptor table, NOT root UAV descriptors. A root
  // UAV descriptor (D3D12_ROOT_PARAMETER_TYPE_UAV) is legal D3D12, but the
  // RTX 4090 driver rejects a compute PSO whose shader dereferences a root
  // UAV (E_INVALIDARG from CreateComputePipelineState), while the same shader
  // binds fine from a UAV descriptor table (proven by the SRV-only vs UAV-only
  // comparison in the diagnostics below). This matches Veyra-NRVideo's
  // ComputePass, which also uses a UAV descriptor table (root parameter [2]).
  //
  // The versioned root-signature API (D3D12_ROOT_PARAMETER1 +
  // D3D12_VERSIONED_ROOT_SIGNATURE_DESC + D3D12SerializeVersionedRootSignature)
  // is used here, matching Veyra-NRVideo's proven ComputePass pattern. The
  // versioned types keep the classic member names (.ShaderVisibility,
  // .Constants.Num32BitValues, .DescriptorTable.NumDescriptorRanges /
  // .pDescriptorRanges, .Descriptor.ShaderRegister / .RegisterSpace); the only
  // additions are D3D12_DESCRIPTOR_RANGE1.Flags and the versioned wrapper
  // (Desc_1_1 + Version = D3D_ROOT_SIGNATURE_VERSION_1_1). The device accepts
  // both classic and versioned blobs (the earlier E_INVALIDARG was caused by
  // passing the ID3DBlob COM object to CreateRootSignature instead of its
  // GetBufferPointer() data); the versioned path is kept for parity with
  // Veyra-NRVideo.
  D3D12_DESCRIPTOR_RANGE1 srv_range{};
  srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srv_range.NumDescriptors = 4;
  srv_range.BaseShaderRegister = 0;
  srv_range.RegisterSpace = 0;
  // Both the SRV and UAV tables are marked volatile, matching Veyra-NRVideo's
  // proven ComputePass (which uses DESCRIPTORS_VOLATILE on both). The flag is
  // an optimization hint, not required for correctness (descriptors may be
  // rewritten without it); it was never the cause of the earlier E_INVALIDARG
  // (that was the root UAV, now replaced by this table).
  srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
  srv_range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_DESCRIPTOR_RANGE1 uav_range{};
  uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  uav_range.NumDescriptors = 2;
  uav_range.BaseShaderRegister = 0;
  uav_range.RegisterSpace = 0;
  uav_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
  uav_range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_ROOT_PARAMETER1 root_params[3] = {};
  root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[0].DescriptorTable.NumDescriptorRanges = 1;
  root_params[0].DescriptorTable.pDescriptorRanges = &srv_range;
  root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[1].DescriptorTable.NumDescriptorRanges = 1;
  root_params[1].DescriptorTable.pDescriptorRanges = &uav_range;
  root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_params[2].Constants.ShaderRegister = 0;
  root_params[2].Constants.RegisterSpace = 0;
  root_params[2].Constants.Num32BitValues = 9;
  root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc{};
  rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  rs_desc.Desc_1_1.NumParameters = 3;
  rs_desc.Desc_1_1.pParameters = root_params;
  Microsoft::WRL::ComPtr<ID3DBlob> rs_blob;
  Microsoft::WRL::ComPtr<ID3DBlob> rs_error;
  // The two calls below are reported separately. D3D12SerializeVersionedRootSignature
  // is a CPU-side validator whose error blob is human-readable text naming the
  // exact invalid field; ID3D12Device::CreateRootSignature is a device call.
  // Merging them into one HRESULT (the previous code) hid which call actually
  // failed, so the cause of "failed to create root signature" was not
  // recoverable from the log.
  const HRESULT serialize_hr = D3D12SerializeVersionedRootSignature(
      &rs_desc, rs_blob.GetAddressOf(), rs_error.GetAddressOf());
  if (FAILED(serialize_hr)) {
    if (rs_error) {
      const std::string err_text(
          reinterpret_cast<const char*>(rs_error->GetBufferPointer()),
          rs_error->GetBufferSize());
      XELOGE("NVOF: D3D12SerializeRootSignature failed (hr=0x{:08X}); error "
             "blob ({} bytes): {}",
             static_cast<unsigned int>(serialize_hr),
             rs_error->GetBufferSize(), err_text);
    } else {
      XELOGE("NVOF: D3D12SerializeRootSignature failed (hr=0x{:08X}); no error "
             "blob returned.",
             static_cast<unsigned int>(serialize_hr));
    }
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Diagnostic: before the real CreateRootSignature, try to create a MINIMAL
  // root signature (a single 32-bit constant, ALL visibility) on the same
  // device. This isolates whether the device can create ANY root signature in
  // our context, or whether it rejects our specific 4-parameter description.
  {
    D3D12_ROOT_PARAMETER1 test_param{};
    test_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    test_param.Constants.ShaderRegister = 0;
    test_param.Constants.RegisterSpace = 0;
    test_param.Constants.Num32BitValues = 1;
    test_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC test_desc{};
    test_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    test_desc.Desc_1_1.NumParameters = 1;
    test_desc.Desc_1_1.pParameters = &test_param;
    Microsoft::WRL::ComPtr<ID3DBlob> test_blob, test_err;
    const HRESULT test_serialize_hr = D3D12SerializeVersionedRootSignature(
        &test_desc, test_blob.GetAddressOf(), test_err.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D12RootSignature> test_rs;
    HRESULT test_create_hr = E_INVALIDARG;
    if (SUCCEEDED(test_serialize_hr)) {
      test_create_hr = device_->CreateRootSignature(
          0, test_blob->GetBufferPointer(), test_blob->GetBufferSize(),
          IID_PPV_ARGS(&test_rs));
    }
    XELOGI("NVOF: diagnostic minimal root signature (serialize hr=0x{:08X}, "
           "create hr=0x{:08X}).",
           static_cast<unsigned int>(test_serialize_hr),
           static_cast<unsigned int>(test_create_hr));
  }
  const HRESULT create_rs_hr = device_->CreateRootSignature(
      0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
      IID_PPV_ARGS(&root_signature_));
  if (FAILED(create_rs_hr)) {
    XELOGE("NVOF: ID3D12Device::CreateRootSignature failed (hr=0x{:08X}, "
           "device={}, blob size={} bytes).",
           static_cast<unsigned int>(create_rs_hr),
           static_cast<void*>(device_),
           rs_blob->GetBufferSize());
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: private view heap + root signature created ({}-byte blob).",
         rs_blob->GetBufferSize());

  // 13. Compile the shaders (ABGR8 conversion + densify) via D3DCompile.
  auto d3d_compile = LoadD3DCompile();
  if (!d3d_compile) {
    XELOGE("NVOF: failed to load d3dcompiler. Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Diagnostic (steps 1 + 2): D3DReflect is the authoritative bytecode check
  // and the binding cross-check (it parses the DXBC chunk table and reports
  // every resource the shader actually declares). Loaded from the same
  // d3dcompiler module as D3DCompile.
  auto d3d_reflect = LoadD3DReflect();
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
  Microsoft::WRL::ComPtr<ID3DBlob> zerofill_blob = CompileShader(
      d3d_compile, kZeroFillHlsl, "nvof_zerofill", "cs_5_1", &ok);
  if (!ok || !zerofill_blob) {
    XELOGE("NVOF: failed to compile the zero-fill shader. Caching as "
           "unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: shaders compiled (ABGR8 conversion + densify + zero-fill).");
  DescribeBytecode("conversion", convert_blob);
  DescribeBytecode("densify", densify_blob);
  DescribeBytecode("zero-fill", zerofill_blob);
  // Diagnostic (steps 1 + 2): authoritative bytecode parse + actual binding
  // layout for the two real shaders. A shader that dereferences a register the
  // RS does not cover (or that covers with a mismatched type/visibility) is
  // rejected by CreateComputePipelineState with E_INVALIDARG - so these dumps
  // must be compared against the root signature above.
  DescribeShaderBindings(d3d_reflect, "conversion",
                         convert_blob->GetBufferPointer(),
                         convert_blob->GetBufferSize());
  DescribeShaderBindings(d3d_reflect, "densify",
                         densify_blob->GetBufferPointer(),
                         densify_blob->GetBufferSize());

  // 14. Create the PSOs (conversion + densify).
  //     D3D12_COMPUTE_PIPELINE_STATE_DESC.CS is a D3D12_SHADER_BYTECODE, and
  //     the root signature member is pRootSignature.
  //     Diagnostic (before the real PSOs): every CreateComputePipelineState
  //     attempt is reported with its HRESULT. testA/testB passed while the
  //     real conversion PSO fails, so the matrix is extended to isolate the
  //     cause:
  //       testA: minimal RS (1 constant) + no-op shader (baseline)
  //       testB: full RS + no-op shader
  //       testC: exact-bindings RS (SRV table t0-t3 + UAV u0 - exactly what
  //              the conversion shader binds) + conversion bytecode
  //       testE: full RS + densify bytecode
  //     DescribeBytecode() prints each bytecode's DXBC magic, shader version
  //     and instruction count, so a corrupted blob is visible directly.
  XELOGI("NVOF: conversion bytecode = {} bytes, densify bytecode = {} bytes.",
         convert_blob->GetBufferSize(), densify_blob->GetBufferSize());
  {
    D3D12_ROOT_PARAMETER1 test_param{};
    test_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    test_param.Constants.ShaderRegister = 0;
    test_param.Constants.RegisterSpace = 0;
    test_param.Constants.Num32BitValues = 1;
    test_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC test_desc{};
    test_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    test_desc.Desc_1_1.NumParameters = 1;
    test_desc.Desc_1_1.pParameters = &test_param;
    Microsoft::WRL::ComPtr<ID3DBlob> test_rs_blob, test_rs_err;
    D3D12SerializeVersionedRootSignature(&test_desc,
                                         test_rs_blob.GetAddressOf(),
                                         test_rs_err.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D12RootSignature> test_rs;
    if (test_rs_blob) {
      device_->CreateRootSignature(
          0, test_rs_blob->GetBufferPointer(), test_rs_blob->GetBufferSize(),
          IID_PPV_ARGS(&test_rs));
    }
    // Exact-bindings RS: the SRV table t0-t3 and the root UAV u0 that the
    // conversion shader dereferences. This is now the "root UAV control": the
    // real RS no longer has a root UAV (it uses a UAV descriptor table), so
    // testC is EXPECTED to FAIL with E_INVALIDARG, documenting that a root UAV
    // descriptor is the cause. If it starts passing, the driver behavior
    // changed.
    D3D12_DESCRIPTOR_RANGE1 test_srv_range{};
    test_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    test_srv_range.NumDescriptors = 4;
    test_srv_range.BaseShaderRegister = 0;
    test_srv_range.RegisterSpace = 0;
    test_srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    test_srv_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER1 test_conv_params[2] = {};
    test_conv_params[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    test_conv_params[0].DescriptorTable.NumDescriptorRanges = 1;
    test_conv_params[0].DescriptorTable.pDescriptorRanges = &test_srv_range;
    test_conv_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    test_conv_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    test_conv_params[1].Descriptor.ShaderRegister = 0;
    test_conv_params[1].Descriptor.RegisterSpace = 0;
    test_conv_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC test_conv_desc{};
    test_conv_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    test_conv_desc.Desc_1_1.NumParameters = 2;
    test_conv_desc.Desc_1_1.pParameters = test_conv_params;
    Microsoft::WRL::ComPtr<ID3DBlob> test_conv_rs_blob, test_conv_rs_err;
    D3D12SerializeVersionedRootSignature(
        &test_conv_desc, test_conv_rs_blob.GetAddressOf(),
        test_conv_rs_err.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D12RootSignature> test_conv_rs;
    if (test_conv_rs_blob) {
      device_->CreateRootSignature(
          0, test_conv_rs_blob->GetBufferPointer(),
          test_conv_rs_blob->GetBufferSize(), IID_PPV_ARGS(&test_conv_rs));
    }
    static const char kNoOpHlsl[] = "[numthreads(1, 1, 1)]\nvoid main() {}\n";
    bool no_op_ok = false;
    Microsoft::WRL::ComPtr<ID3DBlob> no_op_blob = CompileShader(
        d3d_compile, kNoOpHlsl, "nvof_no_op", "cs_5_1", &no_op_ok);
    if (no_op_ok) {
      DescribeBytecode("no-op", no_op_blob);
      DescribeShaderBindings(d3d_reflect, "no-op",
                             no_op_blob->GetBufferPointer(),
                             no_op_blob->GetBufferSize());
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC test_cps_desc{};
    test_cps_desc.NodeMask = 0;
    if (no_op_ok && test_rs) {
      test_cps_desc.pRootSignature = test_rs.Get();
      test_cps_desc.CS.pShaderBytecode = no_op_blob->GetBufferPointer();
      test_cps_desc.CS.BytecodeLength = no_op_blob->GetBufferSize();
      Microsoft::WRL::ComPtr<ID3D12PipelineState> test_pso_a;
      const HRESULT test_a_hr = device_->CreateComputePipelineState(
          &test_cps_desc, IID_PPV_ARGS(&test_pso_a));
      test_cps_desc.pRootSignature = root_signature_.Get();
      Microsoft::WRL::ComPtr<ID3D12PipelineState> test_pso_b;
      const HRESULT test_b_hr = device_->CreateComputePipelineState(
          &test_cps_desc, IID_PPV_ARGS(&test_pso_b));
      XELOGI("NVOF: diagnostic compute PSOs (no-op {} bytes; minimal RS "
             "hr=0x{:08X}, full RS hr=0x{:08X}).",
             no_op_blob->GetBufferSize(), static_cast<unsigned int>(test_a_hr),
             static_cast<unsigned int>(test_b_hr));
    }
    if (test_conv_rs) {
      // testC: exact-bindings RS + conversion bytecode.
      test_cps_desc.pRootSignature = test_conv_rs.Get();
      test_cps_desc.CS.pShaderBytecode = convert_blob->GetBufferPointer();
      test_cps_desc.CS.BytecodeLength = convert_blob->GetBufferSize();
      Microsoft::WRL::ComPtr<ID3D12PipelineState> test_pso_c;
      const HRESULT test_c_hr = device_->CreateComputePipelineState(
          &test_cps_desc, IID_PPV_ARGS(&test_pso_c));
      // testE: full RS + densify bytecode.
      test_cps_desc.pRootSignature = root_signature_.Get();
      test_cps_desc.CS.pShaderBytecode = densify_blob->GetBufferPointer();
      test_cps_desc.CS.BytecodeLength = densify_blob->GetBufferSize();
      Microsoft::WRL::ComPtr<ID3D12PipelineState> test_pso_e;
      const HRESULT test_e_hr = device_->CreateComputePipelineState(
          &test_cps_desc, IID_PPV_ARGS(&test_pso_e));
      XELOGI("NVOF: diagnostic compute PSOs 2 (exact-bindings RS + "
             "conversion hr=0x{:08X}, full RS + densify hr=0x{:08X}).",
             static_cast<unsigned int>(test_c_hr),
             static_cast<unsigned int>(test_e_hr));
    } else {
      XELOGI("NVOF: diagnostic compute PSOs 2 skipped (exact-bindings RS "
             "created={}).",
             test_conv_rs != nullptr);
    }
    // testF: SRV ONLY (t0) + full RS. testG: UAV ONLY (u0) + full RS. These
    // isolate which binding the driver rejects: the descriptor-table SRV or
    // the root UAV. The SRV result is fed to an InterlockedAdd on a global so
    // the compiler cannot eliminate the load (and thus keeps the t0 binding in
    // the bytecode); the UAV store is inherently observable. Both are created
    // against the full RS, the same one the real conversion PSO uses.
    static const char kSrvOnlyHlsl[] =
        "Texture2D<float4> srvOnly : register(t0);\n"
        "groupshared uint gs_sink_f;\n"
        "[numthreads(1, 1, 1)]\n"
        "void main() {\n"
        "  float4 v = srvOnly[uint2(0, 0)];\n"
        "  InterlockedAdd(gs_sink_f, uint(v.x + v.y + v.z + v.w) + 1);\n"
        "}\n";
    static const char kUavOnlyHlsl[] =
        "RWTexture2D<float4> uavOnly : register(u0);\n"
        "[numthreads(1, 1, 1)]\n"
        "void main() { uavOnly[uint2(0, 0)] = float4(1, 1, 1, 1); }\n";
    bool srv_ok = false, uav_ok = false;
    Microsoft::WRL::ComPtr<ID3DBlob> srv_blob = CompileShader(
        d3d_compile, kSrvOnlyHlsl, "nvof_srv_only", "cs_5_1", &srv_ok);
    Microsoft::WRL::ComPtr<ID3DBlob> uav_blob = CompileShader(
        d3d_compile, kUavOnlyHlsl, "nvof_uav_only", "cs_5_1", &uav_ok);
    if (srv_ok) {
      DescribeBytecode("srv-only", srv_blob);
      DescribeShaderBindings(d3d_reflect, "srv-only",
                             srv_blob->GetBufferPointer(),
                             srv_blob->GetBufferSize());
    }
    if (uav_ok) {
      DescribeBytecode("uav-only", uav_blob);
      DescribeShaderBindings(d3d_reflect, "uav-only",
                             uav_blob->GetBufferPointer(),
                             uav_blob->GetBufferSize());
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC bind_cps_desc{};
    bind_cps_desc.NodeMask = 0;
    bind_cps_desc.pRootSignature = root_signature_.Get();
    if (srv_ok) {
      bind_cps_desc.CS.pShaderBytecode = srv_blob->GetBufferPointer();
      bind_cps_desc.CS.BytecodeLength = srv_blob->GetBufferSize();
      Microsoft::WRL::ComPtr<ID3D12PipelineState> test_pso_f;
      const HRESULT test_f_hr = device_->CreateComputePipelineState(
          &bind_cps_desc, IID_PPV_ARGS(&test_pso_f));
      if (uav_ok) {
        bind_cps_desc.CS.pShaderBytecode = uav_blob->GetBufferPointer();
        bind_cps_desc.CS.BytecodeLength = uav_blob->GetBufferSize();
        Microsoft::WRL::ComPtr<ID3D12PipelineState> test_pso_g;
        const HRESULT test_g_hr = device_->CreateComputePipelineState(
            &bind_cps_desc, IID_PPV_ARGS(&test_pso_g));
        XELOGI("NVOF: diagnostic compute PSOs 3 (full RS + SRV-only "
               "hr=0x{:08X}, UAV-only hr=0x{:08X}).",
               static_cast<unsigned int>(test_f_hr),
               static_cast<unsigned int>(test_g_hr));
      } else {
        XELOGI("NVOF: diagnostic compute PSOs 3 (full RS + SRV-only "
               "hr=0x{:08X}, UAV-only skipped).",
               static_cast<unsigned int>(test_f_hr));
      }
    } else {
      XELOGI("NVOF: diagnostic compute PSOs 3 skipped (SRV-only compiled={}, "
             "UAV-only compiled={}).",
             srv_ok, uav_ok);
    }
  }
  // The conversion pass needs only t0 (input SRV) and u0 (frame-pool UAV).
  // The shared root_signature_ has a 4-wide SRV table (t0-t3, shared with the
  // densify pass). If the conversion pass used that RS, its SRV table range
  // would span heap slots 0-3, which include the UAV descriptors at slots 1-2
  // (frame-pool and confidence UAVs). The NVIDIA driver validates the entire
  // SRV table range and raises a GPU fault (0x3221225477) on a UAV descriptor
  // inside an SRV table range. So the conversion pass uses its own RS with a
  // 1-wide SRV table (just t0) and a 1-wide UAV table (just u0), both
  // type-consistent. This matches Veyra-NRVideo's pattern of a dedicated RS
  // (and heap) per compute pass, where table ranges never mix descriptor
  // types.
  D3D12_DESCRIPTOR_RANGE1 conv_srv_range{};
  conv_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  conv_srv_range.NumDescriptors = 1;
  conv_srv_range.BaseShaderRegister = 0;
  conv_srv_range.RegisterSpace = 0;
  conv_srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
  conv_srv_range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_DESCRIPTOR_RANGE1 conv_uav_range{};
  conv_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  conv_uav_range.NumDescriptors = 1;
  conv_uav_range.BaseShaderRegister = 0;
  conv_uav_range.RegisterSpace = 0;
  conv_uav_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
  conv_uav_range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_ROOT_PARAMETER1 conv_params[2] = {};
  conv_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  conv_params[0].DescriptorTable.NumDescriptorRanges = 1;
  conv_params[0].DescriptorTable.pDescriptorRanges = &conv_srv_range;
  conv_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  conv_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  conv_params[1].DescriptorTable.NumDescriptorRanges = 1;
  conv_params[1].DescriptorTable.pDescriptorRanges = &conv_uav_range;
  conv_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC conv_rs_desc{};
  conv_rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  conv_rs_desc.Desc_1_1.NumParameters = 2;
  conv_rs_desc.Desc_1_1.pParameters = conv_params;
  Microsoft::WRL::ComPtr<ID3DBlob> conv_rs_blob, conv_rs_err;
  const HRESULT conv_serialize_hr = D3D12SerializeVersionedRootSignature(
      &conv_rs_desc, conv_rs_blob.GetAddressOf(), conv_rs_err.GetAddressOf());
  if (FAILED(conv_serialize_hr)) {
    if (conv_rs_err) {
      const std::string err_text(
          reinterpret_cast<const char*>(conv_rs_err->GetBufferPointer()),
          conv_rs_err->GetBufferSize());
      XELOGE("NVOF: D3D12SerializeVersionedRootSignature (conversion RS) "
             "failed (hr=0x{:08X}); error blob ({} bytes): {}",
             static_cast<unsigned int>(conv_serialize_hr),
             conv_rs_err->GetBufferSize(), err_text);
    } else {
      XELOGE("NVOF: D3D12SerializeVersionedRootSignature (conversion RS) "
             "failed (hr=0x{:08X}); no error blob.",
             static_cast<unsigned int>(conv_serialize_hr));
    }
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  const HRESULT conv_rs_hr = device_->CreateRootSignature(
      0, conv_rs_blob->GetBufferPointer(), conv_rs_blob->GetBufferSize(),
      IID_PPV_ARGS(&conversion_rs_));
  if (FAILED(conv_rs_hr)) {
    XELOGE("NVOF: ID3D12Device::CreateRootSignature (conversion RS) failed "
           "(hr=0x{:08X}).",
           static_cast<unsigned int>(conv_rs_hr));
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: conversion RS created (1 SRV + 1 UAV, {}-byte blob).",
         conv_rs_blob->GetBufferSize());
  D3D12_COMPUTE_PIPELINE_STATE_DESC cps_desc{};
  cps_desc.pRootSignature = conversion_rs_.Get();
  cps_desc.CS.pShaderBytecode = convert_blob->GetBufferPointer();
  cps_desc.CS.BytecodeLength = convert_blob->GetBufferSize();
  cps_desc.NodeMask = 0;
  XELOGI("NVOF: conversion PSO inputs: root_signature={}, shader_bytecode={}, "
         "shader_size={} bytes, node_mask={}, flags={}.",
         static_cast<void*>(cps_desc.pRootSignature),
         static_cast<const void*>(cps_desc.CS.pShaderBytecode),
         cps_desc.CS.BytecodeLength, static_cast<unsigned>(cps_desc.NodeMask),
         static_cast<unsigned>(cps_desc.Flags));
  const HRESULT conversion_pso_hr = device_->CreateComputePipelineState(
      &cps_desc, IID_PPV_ARGS(&conversion_pso_));
  if (FAILED(conversion_pso_hr)) {
    XELOGE("NVOF: ID3D12Device::CreateComputePipelineState (conversion) "
           "failed (hr=0x{:08X}, bytecode={} bytes).",
           static_cast<unsigned int>(conversion_pso_hr),
           convert_blob->GetBufferSize());
    // Diagnostic (step 3): re-attempt on a debug-layer device and drain the
    // Info Queue (our device was created by Xenia without D3D12_CREATE_DEVICE_DEBUG,
    // so its queue is always empty).
    ProbePsoWithDebugLayer("conversion", root_signature_.Get(),
                           convert_blob->GetBufferPointer(),
                           convert_blob->GetBufferSize());
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC densify_cps_desc{};
  densify_cps_desc.pRootSignature = root_signature_.Get();
  densify_cps_desc.CS.pShaderBytecode = densify_blob->GetBufferPointer();
  densify_cps_desc.CS.BytecodeLength = densify_blob->GetBufferSize();
  densify_cps_desc.NodeMask = 0;
  XELOGI("NVOF: densify PSO inputs: root_signature={}, shader_bytecode={}, "
         "shader_size={} bytes, node_mask={}, flags={}.",
         static_cast<void*>(densify_cps_desc.pRootSignature),
         static_cast<const void*>(densify_cps_desc.CS.pShaderBytecode),
         densify_cps_desc.CS.BytecodeLength,
         static_cast<unsigned>(densify_cps_desc.NodeMask),
         static_cast<unsigned>(densify_cps_desc.Flags));
  const HRESULT densify_pso_hr = device_->CreateComputePipelineState(
      &densify_cps_desc, IID_PPV_ARGS(&densify_pso_));
  if (FAILED(densify_pso_hr)) {
    XELOGE("NVOF: ID3D12Device::CreateComputePipelineState (densify) failed "
           "(hr=0x{:08X}, bytecode={} bytes).",
           static_cast<unsigned int>(densify_pso_hr),
           densify_blob->GetBufferSize());
    // Diagnostic (step 3): re-attempt on a debug-layer device and drain the
    // Info Queue.
    ProbePsoWithDebugLayer("densify", root_signature_.Get(),
                           densify_blob->GetBufferPointer(),
                           densify_blob->GetBufferSize());
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  // Zero-fill RS/PSO (Phase A): the zero-fill pass writes zeros to the ABGR8
  // previous frame (the frame-pool UAV). It uses a dedicated root signature
  // with a 1-wide UAV table (u0), matching the per-pass dedicated-RS pattern
  // (the conversion pass has its own 1-SRV + 1-UAV RS; the densify pass uses
  // the shared RS). The zero-fill shader binds only u0, so an SRV-free RS keeps
  // the table range type-consistent (no SRV/UAV mix, which the RTX 4090
  // driver faults on).
  D3D12_DESCRIPTOR_RANGE1 zf_uav_range{};
  zf_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  zf_uav_range.NumDescriptors = 1;
  zf_uav_range.BaseShaderRegister = 0;
  zf_uav_range.RegisterSpace = 0;
  zf_uav_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
  zf_uav_range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_ROOT_PARAMETER1 zf_params[1] = {};
  zf_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  zf_params[0].DescriptorTable.NumDescriptorRanges = 1;
  zf_params[0].DescriptorTable.pDescriptorRanges = &zf_uav_range;
  zf_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC zf_rs_desc{};
  zf_rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  zf_rs_desc.Desc_1_1.NumParameters = 1;
  zf_rs_desc.Desc_1_1.pParameters = zf_params;
  Microsoft::WRL::ComPtr<ID3DBlob> zf_rs_blob, zf_rs_err;
  const HRESULT zf_serialize_hr = D3D12SerializeVersionedRootSignature(
      &zf_rs_desc, zf_rs_blob.GetAddressOf(), zf_rs_err.GetAddressOf());
  if (FAILED(zf_serialize_hr)) {
    XELOGE("NVOF: D3D12SerializeVersionedRootSignature (zero-fill RS) failed "
           "(hr=0x{:08X}).",
           static_cast<unsigned int>(zf_serialize_hr));
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  const HRESULT zf_rs_hr = device_->CreateRootSignature(
      0, zf_rs_blob->GetBufferPointer(), zf_rs_blob->GetBufferSize(),
      IID_PPV_ARGS(&zero_fill_rs_));
  if (FAILED(zf_rs_hr)) {
    XELOGE("NVOF: ID3D12Device::CreateRootSignature (zero-fill RS) failed "
           "(hr=0x{:08X}).",
           static_cast<unsigned int>(zf_rs_hr));
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC zf_cps_desc{};
  zf_cps_desc.pRootSignature = zero_fill_rs_.Get();
  zf_cps_desc.CS.pShaderBytecode = zerofill_blob->GetBufferPointer();
  zf_cps_desc.CS.BytecodeLength = zerofill_blob->GetBufferSize();
  zf_cps_desc.NodeMask = 0;
  const HRESULT zf_pso_hr = device_->CreateComputePipelineState(
      &zf_cps_desc, IID_PPV_ARGS(&zero_fill_pso_));
  if (FAILED(zf_pso_hr)) {
    XELOGE("NVOF: ID3D12Device::CreateComputePipelineState (zero-fill) "
           "failed (hr=0x{:08X}, bytecode={} bytes).",
           static_cast<unsigned int>(zf_pso_hr),
           zerofill_blob->GetBufferSize());
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }
  XELOGI("NVOF: PSOs created (conversion + densify + zero-fill; zero-fill "
         "RS {}-byte blob).",
         zf_rs_blob->GetBufferSize());

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
  // Form B (Milestone 4-E): create the backend-owned command allocator +
  // command list for the ABGR8 conversion pass. The conversion pass is
  // executed on the direct queue BEFORE the NVOF execute is submitted to the
  // OF engine (the NVOF execute is asynchronous; it submits to its own
  // hardware queue at call time, not into the caller's command list). The
  // input fence is signaled by the conversion pass, and the OF engine waits
  // on it. See the design doc (milestone-4-e).
  //
  // The list/allocator MUST be D3D12_COMMAND_LIST_TYPE_DIRECT (matching the
  // presenter's paint/FG/capture lists, which are all DIRECT). A COMPUTE-type
  // list submitted to the DIRECT queue removed the device (0x887A0001)
  // immediately after ExecuteCommandLists - the control test proved the
  // removal is caused by the list type, not the list contents (an empty
  // COMPUTE list also removed the device, and skipping the submission
  // entirely let the device survive). A compute Dispatch is valid on a DIRECT
  // list (DIRECT lists do compute + copy + graphics), so this is safe.
  //
  // NOTE: this block is created BEFORE the SDK registration (moved here from
  // after the registration) so the pre-registration zero-fill (below) can
  // record its compute dispatches into this command list. The per-frame code
  // still resets this command list every frame (unchanged).
  if (FAILED(device_->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&conversion_command_allocator_))) ||
      FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        conversion_command_allocator_.Get(),
                                        nullptr,
                                        IID_PPV_ARGS(&conversion_command_list_)))) {
    XELOGE("NVOF: failed to create the conversion command list/allocator. "
           "Caching as unavailable.");
    DestroySession();
    state_ = State::kUnavailable;
    return BackendResult::kInitFailed;
  }

  // Bind the private view heap before the pre-registration zero-fill records
  // its compute dispatches. A freshly created command list has no descriptor
  // heaps bound; the first SetComputeRootDescriptorTable on it dereferences a
  // GPU descriptor handle with no heap to resolve against, crashing the
  // NVIDIA driver (the true root cause of the zf.3 / pre-zf.3 crash).
  BindViewHeap("pre-zf");

  // --- Pre-registration zero-fill (fixes the zf.3 crash) ---
  // Zero-fill frame_pool_[0] and frame_pool_[1] BEFORE registering them with
  // the NVOF SDK. The crash at zf.3 (SetComputeRootDescriptorTable(0,
  // gpu_view(9))) is a driver access violation in nvwgf2umx.dll triggered by
  // binding a UAV of a resource that is ALREADY registered with the SDK. By
  // zero-filling before registration, the SDK sees already-zeroed resources
  // and no per-frame zero-fill UAV binding is needed. This is a one-time
  // initialization cost.
  //
  // The zero-fill is a compute dispatch (same kZeroFillHlsl shader as the
  // per-frame zero-fill), recorded into the backend-owned conversion command
  // list and submitted on the caller's command queue. A temporary fence
  // CPU-waits for completion before the SDK registration proceeds.
  XELOGI("NVOF: pre-zf.start: zero-filling frame_pool_[0] and frame_pool_[1] "
         "BEFORE SDK registration.");
  {
    const uint32_t dispatch_x = (input_width_ + 7) / 8;
    const uint32_t dispatch_y = (input_height_ + 7) / 8;
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
    // Create the zero-fill UAV descriptors. Slot 1 = frame_pool_[0] UAV,
    // slot 9 = frame_pool_[1] UAV. (The per-frame code re-creates these slots
    //  every frame, overwriting these; the zero-fill's use is one-time.)
    device_->CreateUnorderedAccessView(frame_pool_[0].Get(), nullptr, nullptr,
                                       cpu_view(1));
    device_->CreateUnorderedAccessView(frame_pool_[1].Get(), nullptr, nullptr,
                                       cpu_view(9));
    XELOGI("NVOF: pre-zf.desc-created: slot 1 = frame_pool_[0] UAV, "
           "slot 9 = frame_pool_[1] UAV.");
    // Record the zero-fill dispatches (one per resource).
    conversion_command_list_->SetComputeRootSignature(zero_fill_rs_.Get());
    LogDeviceState(device_, "pre-zf.1.setcompute-rootsignature");
    conversion_command_list_->SetPipelineState(zero_fill_pso_.Get());
    LogDeviceState(device_, "pre-zf.2.setpipelinestate");
    conversion_command_list_->SetComputeRootDescriptorTable(0, gpu_view(1));
    LogDeviceState(device_, "pre-zf.3.bind-slot1-framepool0-uav");
    conversion_command_list_->Dispatch(dispatch_x, dispatch_y, 1);
    LogDeviceState(device_, "pre-zf.4.dispatch-framepool0");
    conversion_command_list_->SetComputeRootDescriptorTable(0, gpu_view(9));
    LogDeviceState(device_, "pre-zf.5.bind-slot9-framepool1-uav");
    conversion_command_list_->Dispatch(dispatch_x, dispatch_y, 1);
    LogDeviceState(device_, "pre-zf.6.dispatch-framepool1");
    XELOGI("NVOF: pre-zf.recorded: 2 zero-fill dispatches recorded "
           "(frame_pool_[0] and frame_pool_[1]).");
    conversion_command_list_->Close();
    XELOGI("NVOF: pre-zf.closed: command list closed.");
    // Submit on the caller's command queue.
    ID3D12CommandList* zf_list = conversion_command_list_.Get();
    command_queue->ExecuteCommandLists(1, &zf_list);
    XELOGI("NVOF: pre-zf.submitted: submitted on the caller's command queue.");
    // CPU-wait on a temporary fence.
    Microsoft::WRL::ComPtr<ID3D12Fence> zf_fence;
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&zf_fence)))) {
      XELOGE("NVOF: pre-zf: failed to create the zero-fill fence. Caching as "
             "unavailable.");
      DestroySession();
      state_ = State::kUnavailable;
      return BackendResult::kInitFailed;
    }
    command_queue->Signal(zf_fence.Get(), 1);
    HANDLE zf_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const HRESULT zf_set_hr = zf_fence->SetEventOnCompletion(1, zf_event);
    const DWORD zf_waited =
        SUCCEEDED(zf_set_hr) ? WaitForSingleObject(zf_event, 5000)
                             : WAIT_FAILED;
    const uint64_t zf_completed = zf_fence->GetCompletedValue();
    CloseHandle(zf_event);
    if (zf_waited != WAIT_OBJECT_0 || zf_completed < 1) {
      XELOGE("NVOF: pre-zf.sync-done FAIL: zero-fill CPU-wait timed out or "
             "failed (completed={}, wait={}).",
             zf_completed, zf_waited);
      DestroySession();
      state_ = State::kUnavailable;
      return BackendResult::kSyncConditionFailed;
    }
    XELOGI("NVOF: pre-zf.sync-done: zero-fill completed (CPU-wait done, "
           "fence completed={}).",
           zf_completed);
    LogDeviceState(device_, "pre-zf.sync-done");
  }

  XELOGI("NVOF: reg.start: beginning SDK resource registration (after "
         "pre-registration zero-fill).");
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
  {
    const HRESULT dr = device_->GetDeviceRemovedReason();
    if (FAILED(dr)) {
      XELOGE("NVOF: device removed right after the registration fence (0x{:08X}).",
             static_cast<uint32_t>(dr));
    }
  }

  state_ = State::kAvailable;
  XELOGI("NVOF: backend initialized ({}x{}, grid={}x{}, quality={}, "
         "frame_index={}).",
         input_width_, input_height_, grid_width_, grid_height_, quality_,
         frame_index);
  {
    const HRESULT dr = device_->GetDeviceRemovedReason();
    if (FAILED(dr)) {
      XELOGE("NVOF: device removed at the end of Initialize (0x{:08X}) -- "
             "not caused by any NVOF SDK call above.",
             static_cast<uint32_t>(dr));
    }
  }
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
    BackendResult init_result = Initialize(command_list, command_queue,
                                          frame_index);
    if (init_result != BackendResult::kSuccess) {
      return init_result;
    }
  }

  auto fn = static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(fn_list_);

  // Descriptor-handle helpers. The D3D12 API exposes only the HEAP-START
  // handle; a per-descriptor handle is the heap start advanced by
  // index * increment. cpu_view(i) is the CPU handle for CreateShaderResourceView
  // / CreateUnorderedAccessView; gpu_view(i) is the GPU handle for the root
  // descriptor table.
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

  // Re-bind the private view heap after the Reset (Reset clears all bound
  // descriptor heaps). Without this, the per-frame conversion's first
  // SetComputeRootDescriptorTable (line ~2443) crashes the NVIDIA driver — the
  // same root cause as the pre-zf.3 crash.
  BindViewHeap("per-frame-conversion");

  // --- 1a. ABGR8 conversion pass (compute shader, UAV write), recorded into
  //         the backend-owned conversion command list. ---
  //
  // The densify-pass descriptors (slots 3-8) are created NOW, before the
  // conversion's SRV table is bound. The conversion's SRV table (root
  // parameter [0]) is 4 descriptors wide (shared with the densify pass, which
  // needs t0-t3), so it spans heap slots 0-3. On the first frame, slot 3
  // (grid-flow SRV) was still zero-cleared (a null descriptor) when the
  // conversion dispatch bound the table - the NVIDIA driver read a null SRV in
  // the table range and crashed (access violation in nvwgf2umx.dll). Creating
  // all descriptors up front guarantees every table range is fully initialized
  // before any SetComputeRootDescriptorTable. The densify pass still records
  // its own resource barriers and dispatch; only the descriptor creation moves
  // here.
  //
  // The input SRV (slot 0), the frame-pool UAV (slot 1), and the spare UAV
  // (slot 2) are created every frame (the guest output and the frame-pool
  // current index change per frame). This SDK's descriptor-creation methods
  // (ID3D12Device::CreateShaderResourceView / CreateUnorderedAccessView)
  // return void (unlike CreateRootSignature, which returns HRESULT), so a
  // creation failure is not observable via a return code. A failed creation
  // would leave a NULL descriptor in the heap, and the NVIDIA driver
  // dereferences the whole table range on Dispatch - a null descriptor in
  // the range raises a GPU fault (access violation in nvwgf2umx.dll). The
  // fail-closed protection is therefore (a) the input-contract check below
  // (the input SRV is the only descriptor created from a non-backend-owned
  // resource) and (b) the up-front creation of all nine descriptors (so every
  // table range is fully initialized before any SetComputeRootDescriptorTable).
  //
  // Input-contract check (fail-closed): the input SRV (slot 0) is created from
  // input_frame, the only non-backend-owned resource. If it is not a 2D
  // texture, the SRV creation produces a null/garbage descriptor and the
  // driver faults on Dispatch. Verify the dimension and fail closed.
  const D3D12_RESOURCE_DESC input_desc = input_frame->GetDesc();
  if (input_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    XELOGE("NVOF: input frame is not a 2D texture (dimension={}); the input "
           "SRV would be a null descriptor and the NVIDIA driver would fault "
           "on Dispatch.",
           static_cast<int>(input_desc.Dimension));
    has_valid_flow_ = false;
    return BackendResult::kInputContractFailed;
  }
  // All ten descriptors, up front (input SRV slot 0, frame-pool UAV slot 1,
  // spare UAV slot 2, grid-flow SRV slot 3, grid-cost SRV slot 4, ABGR8-
  // previous SRV slot 5, ABGR8-current SRV slot 6, MV UAV slot 7, confidence
  // UAV slot 8, ABGR8-previous UAV slot 9). Slot 9 is the zero-fill pass's
  // destination: the ABGR8 previous frame as a UAV. It is distinct from slot 1
  // (the current-frame UAV) because, in the deferred zero-fill case (control
  // test kEmptyList, zero-fill moved to frame 1), previous_index !=
  // current_index, so the previous frame is NOT slot 1. This SDK's
  // descriptor-creation methods return void (no HRESULT), so they are called
  // as plain statements (the codebase idiom).
  device_->CreateShaderResourceView(input_frame, nullptr, cpu_view(0));
  LogDeviceState(device_, "A.desc0.srv-input");
  device_->CreateUnorderedAccessView(frame_pool_[current_index].Get(), nullptr,
                                     nullptr, cpu_view(1));
  LogDeviceState(device_, "A.desc1.uav-framepool-current");
  device_->CreateUnorderedAccessView(confidence_texture_.Get(), nullptr, nullptr,
                                     cpu_view(2));
  LogDeviceState(device_, "A.desc2.uav-confidence");
  device_->CreateShaderResourceView(grid_flow_texture_.Get(), nullptr,
                                    cpu_view(3));
  LogDeviceState(device_, "A.desc3.srv-gridflow");
  device_->CreateShaderResourceView(grid_cost_texture_.Get(), nullptr,
                                    cpu_view(4));
  LogDeviceState(device_, "A.desc4.srv-gridcost");
  device_->CreateShaderResourceView(frame_pool_[previous_index].Get(), nullptr,
                                    cpu_view(5));
  LogDeviceState(device_, "A.desc5.srv-framepool-prev");
  device_->CreateShaderResourceView(frame_pool_[current_index].Get(), nullptr,
                                    cpu_view(6));
  LogDeviceState(device_, "A.desc6.srv-framepool-current");
  device_->CreateUnorderedAccessView(motion_vector_texture_.Get(), nullptr,
                                     nullptr, cpu_view(7));
  LogDeviceState(device_, "A.desc7.uav-mv");
  device_->CreateUnorderedAccessView(confidence_texture_.Get(), nullptr, nullptr,
                                     cpu_view(8));
  LogDeviceState(device_, "A.desc8.uav-confidence");
  device_->CreateUnorderedAccessView(frame_pool_[previous_index].Get(), nullptr,
                                     nullptr, cpu_view(9));
  LogDeviceState(device_, "A.desc9.uav-framepool-prev");

  const uint32_t dispatch_x = (input_width_ + 7) / 8;
  const uint32_t dispatch_y = (input_height_ + 7) / 8;

  // The conversion pass (compute dispatch) writes the ABGR8 current frame.
  // On the first frame (frame_index == 0) current_index == previous_index, so
  // the zero-fill (below) immediately overwrites the just-written current
  // frame with zeros. The conversion dispatch on frame 0 is therefore
  // pointless GPU work: its output is discarded, and it is the most likely
  // source of the driver-level access violation (a compute dispatch reading
  // the guest output SRV and writing the frame-pool UAV). Skipping it on
  // frame 0 eliminates that GPU work while keeping the zero-fill (which makes
  // the ABGR8 reference valid for frame 1). From frame 1 onward the
  // conversion is needed (it writes the current frame that the NVOF execute
  // reads as input).
  if (frame_index > 0) {
    // The conversion PSO was created with conversion_rs_ (1-SRV + 1-UAV
    // table). The command-list root signature must match the PSO's root
    // signature, so use conversion_rs_ here (not the shared 4-wide
    // root_signature_).
    conversion_command_list_->SetComputeRootSignature(conversion_rs_.Get());
    conversion_command_list_->SetPipelineState(conversion_pso_.Get());
    conversion_command_list_->SetComputeRootDescriptorTable(0, gpu_view(0));
    // The UAV is now a descriptor table (root parameter [1]), not a root UAV
    // descriptor. The GPU handle points at the table's first descriptor
    // (slot 1).
    conversion_command_list_->SetComputeRootDescriptorTable(1, gpu_view(1));
    conversion_command_list_->Dispatch(dispatch_x, dispatch_y, 1);
  }

  // --- Stage A/B: Close the conversion command list, submit it on the direct
  //         queue, and signal the input fence (GPU) once the submitted work
  //         completes. The NVOF OF engine waits on this fence before running,
  //         so it cannot read an unwritten ABGR8 current frame. Note:
  //         ID3D12GraphicsCommandList has no Signal method in this SDK; the
  //         fence is signaled from the command queue (it is signaled when all
  //         previously submitted command lists on this queue - just this one -
  //         have completed).
  //
  //         Stage A (frame 0): the submitted command list is the ZERO-FILL
  //         only (a D2D copy from the upload buffer to frame_pool_[0]) -- or,
  //         in control-test mode (kEmptyList), an EMPTY list; in kNoSubmit
  //         mode the submission is skipped entirely (early return above).
  //         Stage B (frame 1+): the submitted command list is the ABGR8
  //         CONVERSION dispatch (the zero-fill was done on frame 0) -- or, in
  //         control-test mode on frame 1, the deferred zero-fill plus the
  //         conversion dispatch.
  //         The stage name is logged so the device-removed reason can be
  //         attributed to the exact submitted work. ---
  const char* submit_stage =
      (frame_index == 0) ? "A.empty-submit" : "B.conversion-submit";
  // Finer-grained log nodes (per the debugging directive): Close(),
  // ExecuteCommandLists(), and Signal() all return void, so the submit
  // success/failure CANNOT be read from a return code. We log the device
  // state immediately before AND after each call, so the removed reason can
  // be attributed to the exact operation. The zero-fill recording (barriers +
  // CopyTextureRegion) is done above; this is the point where the command list
  // is complete.
  XELOGI("NVOF: stage={}: recording complete (frame_index={}); about to "
         "Close().",
         submit_stage, frame_index);
  LogDeviceState(device_, submit_stage);
  conversion_command_list_->Close();
  XELOGI("NVOF: stage={}: Close() returned; about to ExecuteCommandLists().",
         submit_stage);
  LogDeviceState(device_, submit_stage);
  {
    ID3D12CommandList* conv_list = conversion_command_list_.Get();
    command_queue->ExecuteCommandLists(1, &conv_list);
    // The submit is asynchronous; query the device state immediately. A fault
    // in the submitted work may or may not be detected yet (the driver
    // removes the device only after it observes the fault), so S_OK here does
    // not prove the work succeeded. The authoritative check is after the
    // fence wait.
    LogDeviceState(device_, submit_stage);
    XELOGI("NVOF: stage={}: ExecuteCommandLists() returned; about to "
           "Signal(input_fence, {}).",
           submit_stage, frame_index + 1);
    // Signal the input fence to frame_index + 1 (NOT frame_index). The input
    // fence is created at value 0, so signaling it to frame_index == 0 on the
    // first frame is a no-op: the direct queue does not wait for the submitted
    // command list (the frame-0 zero-fill) before "signaling" an already-
    // reached fence value. Signaling to frame_index + 1 (strictly above the
    // fence's current value) forces the queue to wait for the zero-fill to
    // complete before signaling the fence. This makes the frame-0 zero-fill's
    // completion synchronous; otherwise the zero-fill runs unwaited and, if it
    // faults on the GPU, removes the device asynchronously (breaking the SR/FG
    // backends' CreateCommittedResource before the device-loss is detected).
    command_queue->Signal(input_fence_.Get(), frame_index + 1);
    XELOGI("NVOF: stage={}: Signal() returned; entering the fence wait.",
           submit_stage);
    LogDeviceState(device_, submit_stage);
  }

  // --- Stage A/B sync: CPU wait for the submitted work to complete (input
  //         fence). The NVOF execute must be submitted after the conversion
  //         pass has executed (otherwise the OF engine waits on the input
  //         fence, which is signaled by the conversion pass; submitting the
  //         execute before the conversion runs would serialize the two and
  //         risk a deadlock). The input fence is signaled by the submitted work
  //         to frame_index + 1. On the first frame (frame_index == 0) the fence
  //         is signaled to 1, which waits for the frame-0 zero-fill to complete
  //         (the zero-fill is the only GPU work on the first frame). From frame
  //         1 onward it waits for the conversion pass. ---
  {
    const char* sync_stage =
        (frame_index == 0) ? "A.empty-sync" : "B.conversion-sync";
    if (!WaitFenceAndLog(input_fence_.Get(), frame_index + 1, sync_stage,
                         device_)) {
      XELOGE("NVOF: stage={}: input fence wait failed (frame_index={}).",
             sync_stage, frame_index);
      has_valid_flow_ = false;
      return BackendResult::kSyncConditionFailed;
    }
  }

  // --- 2. NvOFExecuteD3D12 (through the function table). ---
  // The NVOF execute, the output-fence wait, and the densify pass are all
  // skipped on the first frame (frame_index == 0). On the first frame there is
  // no valid previous frame: current_index == previous_index, so the NVOF
  // execute would be called with inputFrame == referenceFrame (the same
  // texture). The SDK expects a "frame pair" (two distinct frames); passing
  // the same texture as both input and reference is not a valid pair and
  // raises a GPU fault in the NVIDIA driver (access violation in
  // nvwgf2umx.dll). The first frame's flow is invalid and discarded anyway, so
  // skipping the execute + densify on the first frame is both safe and
  // consistent with the existing semantics. From frame 1 onward, current_index
  // != previous_index, so the NVOF execute gets a valid frame pair.
  if (frame_index > 0) {
  NV_OF_FENCE_POINT in_fence_point{};
  in_fence_point.fence = input_fence_.Get();
  // Match the input fence's signaled value (frame_index + 1, see the Signal
  // call above). The NVOF execute waits for the conversion pass (the current
  // frame's ABGR8 write) to complete before reading it.
  in_fence_point.value = frame_index + 1;
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
    // Stage C: the NVOF execute is asynchronous. It submits to the OF
    // engine's own hardware queue at call time and returns immediately. A
    // device fault from the execute is therefore NOT observable here; the
    // authoritative check is after the output-fence wait below.
    LogDeviceState(device_, "C.nvof-execute");
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
  // Stage C sync: wait for the NVOF execute to complete (output fence). The
  // WaitFenceAndLog helper logs the fence target, completed value, and wait
  // result, and re-queries the device state AFTER the wait (a GPU fault during
  // the execute removes the device, and the wait may still "succeed").
  if (!WaitFenceAndLog(output_fence_.Get(), out_fence_point.value,
                       "C.nvof-execute-sync", device_)) {
    XELOGE("NVOF: stage=C.nvof-execute-sync: output fence wait failed "
           "(frame_index={}, output fence value {}).",
           frame_index, out_fence_point.value);
    has_valid_flow_ = false;
    return BackendResult::kSyncConditionFailed;
  }

  // --- 4. Densify pass (compute shader, UAV write). ---
  // The densify-pass descriptors (slots 3-8) are created earlier, in the
  // conversion section, so that the conversion's 4-wide SRV table (slots 0-3)
  // is fully initialized before it is bound. Creating them here (after the
  // conversion dispatch) left slot 3 null on the first frame and crashed the
  // NVIDIA driver. Only the resource barriers and the dispatch remain in this
  // pass.

  // The ABGR8 frame pool textures are in UAV state (the conversion pass wrote
  // them as unordered access, and the NVOF OF engine restores them to the
  // registered UAV state after execute). Transition them to
  // NON_PIXEL_SHADER_RESOURCE so the densify pass can read them as SRVs for
  // the photometric/bounds validation. On the first frame
  // (previous_index == current_index) both indices refer to the same texture,
  // so only one barrier is recorded (a double transition of the same resource
  // would be an invalid state change).
  // ISOLATION TEST A (session 3): gate the entire densify section. When
  // kSkipDensify is true the barrier-in/barrier-back pair is skipped, so the
  // framepool stays in the post-SDK-execute UAV state and the next frame's
  // conversion writes the UAV as usual. Flip to false to re-enable densify.
  constexpr bool kSkipDensify = false;  // ISOLATION TEST A1: re-enable densify section, gate only the Dispatch below.
  if (!kSkipDensify) {
  constexpr bool kSkipDensifyBarriers = false;  // ISOLATION TEST A2: skip barriers, keep bindings.
  if (!kSkipDensifyBarriers) {
  {
    D3D12_RESOURCE_BARRIER densify_barriers[2] = {};
    uint32_t num_barriers = 0;
    auto make_transition = [&](uint32_t pool_index) {
      D3D12_RESOURCE_BARRIER& b = densify_barriers[num_barriers];
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = frame_pool_[pool_index].Get();
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++num_barriers;
    };
    make_transition(previous_index);
    if (current_index != previous_index) make_transition(current_index);
    command_list->ResourceBarrier(num_barriers, densify_barriers);
  }
  LogDeviceState(device_, "D.barrier-in");
  }

  // ISOLATION TEST A5: bind the private view heap to the CALLER's command
  // list before recording the densify descriptor tables. The densify bindings
  // (gpu_view(3), gpu_view(7)) are recorded on command_list (the caller's list),
  // but BindViewHeap only binds view_heap_ to conversion_command_list_. Without
  // this bind the GPU descriptor handles are dangling and the NVIDIA driver AVs
  // (nvwgf2umx.dll, offset 0x6db7aa). SetDescriptorHeaps is additive (it appends
  // to the caller's bound-heap list, does not replace it), so the caller's own
  // heaps stay bound and the GPU handles resolve against their own heaps.
  {
    ID3D12DescriptorHeap* caller_heaps[] = {view_heap_.Get()};
    XELOGI("NVOF: stage=D.bind-view-heap: binding view_heap to caller's "
           "command_list (frame_index={}); command_list={:p}, view_heap={:p}, "
           "gpu_heap_start=0x{:016X}.",
           frame_index, static_cast<void*>(command_list),
           static_cast<void*>(view_heap_.Get()),
           static_cast<unsigned long long>(
               view_heap_->GetGPUDescriptorHandleForHeapStart().ptr));
    command_list->SetDescriptorHeaps(1, caller_heaps);
  }
  LogDeviceState(device_, "D.bind-view-heap");
  command_list->SetComputeRootSignature(root_signature_.Get());
  LogDeviceState(device_, "D.root-sig");
  command_list->SetPipelineState(densify_pso_.Get());
  LogDeviceState(device_, "D.pso");
  // ISOLATION TEST A5: restore BOTH the SRV table binding (step 3) and the
  // UAV table binding. The root cause of the A2/A3/A4 crashes was that the
  // view heap was never bound to the caller's command list (fixed in the
  // D.bind-view-heap block above); with that fixed, both bindings are safe.
  constexpr bool kSkipDensifySrvTable = false;
  if (!kSkipDensifySrvTable) {
    command_list->SetComputeRootDescriptorTable(0, gpu_view(3));
    // The UAVs are now a descriptor table (root parameter [1]), not root UAV
    // descriptors. The GPU handle points at the table's first descriptor (slot 7).
    // Stage D: the densify pass is recorded into the CALLER's command list
    // (not submitted by the backend). It runs at the end of the frame, after
    // the SR block. A device fault from the densify dispatch is therefore NOT
    // observable inside TryProcess; the device state is logged here so a
    // reader can confirm the device was alive when the densify was recorded.
    LogDeviceState(device_, "D.densify-record");
    command_list->SetComputeRootDescriptorTable(1, gpu_view(7));
  }

  LogDeviceState(device_, "D.uav-table");
  // Bind the densify cbuffer via root 32-bit constants (order matches the
  // HLSL cbuffer DensifyConstants: gridW, gridH, workingW, workingH,
  // gridSize, negate, costThreshold, validationFlags, discardSentinelBits).
  // validationFlags = 0x3 enables both bounds (bit0) and photometric (bit1)
  // validation, matching Veyra's desc_.validateMotion ? 3u : 0u (Veyra
  // defaults to validation on). discardSentinelBits carries the Stage-3
  // sentinel as IEEE-754 float bits (0.0f default = zero MV, current
  // behavior; hardware candidates: 65504.0 / -65504.0 / NaN).
  const uint32_t kValidationFlags = 0x3;
  const uint32_t densify_constants[9] = {grid_width_, grid_height_,
                                         input_width_, input_height_,
                                         grid_size_, kNegate, kCostThreshold,
                                         kValidationFlags,
                                         PackFloatBits(kDiscardSentinel)};
  // Root parameter [2] (was [3] before the UAVs moved into a descriptor table).
  command_list->SetComputeRoot32BitConstants(2, 9, densify_constants, 0);
  LogDeviceState(device_, "D.constants");
  // ISOLATION TEST A5: restore the densify dispatch. The view-heap bind
  // fix (above) and the restored table bindings make the dispatch safe.
  constexpr bool kSkipDensifyDispatch = false;
  if (!kSkipDensifyDispatch) {
    command_list->Dispatch(dispatch_x, dispatch_y, 1);
  }
  LogDeviceState(device_, "D.dispatch");

  // Transition the ABGR8 frame pool textures back to UAV state (the next
  // frame's conversion pass and NVOF execute will use them as UAVs). Same
  // first-frame handling as above (only one barrier when the indices coincide).
  if (!kSkipDensifyBarriers) {
  {
    D3D12_RESOURCE_BARRIER densify_barriers_back[2] = {};
    uint32_t num_barriers_back = 0;
    auto make_transition_back = [&](uint32_t pool_index) {
      D3D12_RESOURCE_BARRIER& b = densify_barriers_back[num_barriers_back];
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = frame_pool_[pool_index].Get();
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      ++num_barriers_back;
    };
    make_transition_back(previous_index);
    if (current_index != previous_index) make_transition_back(current_index);
    command_list->ResourceBarrier(num_barriers_back, densify_barriers_back);
  }
  LogDeviceState(device_, "D.barrier-back");
  }
  } else {
    // Isolation test A: the densify pass is skipped. Log so the gate is
    // visible in the log (a reader can confirm densify was not recorded).
    LogDeviceState(device_, "D.densify-skipped");
    XELOGI("NVOF: stage=D.densify-skipped: densify pass skipped "
           "(kSkipDensify=true, frame_index={}).",
           frame_index);
  }
  }

  // Update the valid-flow flag. The first frame (frame_index == 0) has an
  // invalid flow (no valid reference); subsequent frames are valid.
  has_valid_flow_ = (frame_index > 0);

  return BackendResult::kSuccess;
}

}  // namespace nvof
}  // namespace ui
}  // namespace xe
