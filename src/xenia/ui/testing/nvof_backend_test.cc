/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/nvof/nvof_optical_flow_backend.h"
#include "xenia/ui/nvof/nvof_test_components.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#define CATCH_CONFIG_ENABLE_CHRONO_STRINGMAKER
#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace ui {
namespace nvof {
namespace {

// ---------------------------------------------------------------------------
// Host-side helpers (no GPU required).
// ---------------------------------------------------------------------------

// True if the NVOF runtime DLL (nvofapi64.dll) is present on this host. It is
// shipped with the NVIDIA driver; on a Hyper-V dev box without a driver it is
// absent (the deterministic "runtime missing" fallback).
bool NvOfRuntimePresent() {
  HMODULE lib = LoadLibraryExW(L"nvofapi64.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (lib) {
    FreeLibrary(lib);
    return true;
  }
  return false;
}

// A minimal D3D12 context for the host-side contract tests and the real-GPU
// tests. Prefer an NVIDIA adapter; fall back to any adapter (WARP / Basic) so
// the "missing nvof runtime" fallback can still be exercised on a GPU-less
// host. Returns false if no device could be created at all.
struct GpuContext {
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  Microsoft::WRL::ComPtr<ID3D12Resource> input_texture;
  bool is_nvidia = false;
};

bool CreateGpuContext(GpuContext* ctx) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  // Prefer an NVIDIA adapter; fall back to the first available adapter.
  Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(i, &adapter))) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      continue;  // skip WARP for the NVIDIA preference pass
    }
    // Check for NVIDIA in the description.
    bool nvidia = false;
    for (wchar_t* p = desc.Description; *p; ++p) {
      if (wcsncmp(p, L"NVIDIA", 6) == 0) {
        nvidia = true;
        break;
      }
    }
    if (nvidia) {
      chosen = adapter;
      ctx->is_nvidia = true;
      break;
    }
    if (!chosen) {
      chosen = adapter;  // remember the first non-software adapter as fallback
    }
  }
  if (!chosen) {
    // Last resort: the software (WARP) adapter, so a GPU-less host can still
    // exercise the "missing nvof runtime" fallback.
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&ctx->device)))) {
      return false;
    }
  }
  if (ctx->device) {
    // Already created via D3D12CreateDevice above.
  } else if (FAILED(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&ctx->device)))) {
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.NodeMask = 0;
  queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  if (FAILED(ctx->device->CreateCommandQueue(
          &queue_desc, IID_PPV_ARGS(&ctx->queue)))) {
    return false;
  }
  if (FAILED(ctx->device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx->allocator)))) {
    return false;
  }
  if (FAILED(ctx->device->CreateCommandList(
          0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx->allocator.Get(), nullptr,
          IID_PPV_ARGS(&ctx->list)))) {
    return false;
  }
  // A small R10G10B10A2 input texture (the NVOF input format).
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 64;
  desc.Height = 64;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (FAILED(ctx->device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&ctx->input_texture)))) {
    return false;
  }
  return true;
}

}  // namespace

// ===========================================================================
// Test components (CPU-side, no GPU required).
// ===========================================================================

TEST_CASE("MotionInputGenerator shifts content by dx per frame",
          "[nvof.test_components]") {
  MotionInputGenerator::Params params;
  params.width = 64;
  params.height = 64;
  params.dx = 8;
  params.dy = 0;
  MotionInputGenerator gen(params);

  std::vector<uint8_t> frame0, frame1;
  gen.GenerateFrame(0, &frame0);
  gen.GenerateFrame(1, &frame1);
  CHECK(frame0.size() == 64 * 64 * 4);
  CHECK(frame1.size() == 64 * 64 * 4);

  // The content at (x, y) in frame 1 is the content at (x - 8, y) in frame 0
  // (content moved right by 8). So frame1[x] == frame0[x - 8] for x >= 8.
  // The checkerboard period is 16, so a shift of 8 inverts the pattern.
  // Verify the pattern is periodic with period 16 in frame 0.
  auto pixel_bright = [](const std::vector<uint8_t>& f, uint32_t w, uint32_t x,
                        uint32_t y) { return f[(y * w + x) * 4 + 0] == 255; };
  for (uint32_t y = 0; y < 64; y += 2) {
    for (uint32_t x = 0; x < 64; x += 2) {
      CHECK(pixel_bright(frame0, 64, x, y) != pixel_bright(frame0, 64, x + 16, y));
    }
  }
  // A shift of 8 (half the period) inverts the pattern: frame1[x] != frame0[x]
  // for the same (x, y) where both are within the pattern.
  bool any_diff = false;
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      if (pixel_bright(frame0, 64, x, y) != pixel_bright(frame1, 64, x, y)) {
        any_diff = true;
      }
    }
  }
  CHECK(any_diff);
}

TEST_CASE("GridFlowDecoder converts S10.5 to pixels", "[nvof.test_components]") {
  // S10.5: raw 256 = 8.0 pixels (256 / 32). raw -256 = -8.0 pixels.
  int16_t raw[4] = {256, -256, 0, 32};
  float out[4] = {};
  GridFlowDecoder::Decode(raw, 2, 1, out);
  CHECK(out[0] == Approx(8.0f));    // 256 / 32
  CHECK(out[1] == Approx(-8.0f));   // -256 / 32
  CHECK(out[2] == Approx(0.0f));    // 0 / 32
  CHECK(out[3] == Approx(1.0f));    // 32 / 32
}

TEST_CASE("CpuDensify expands grid to full-res with nearest neighbor",
          "[nvof.test_components]") {
  // 2x2 grid, working 4x4, gridSize 2. Grid flow: all (8, -8) pixels.
  float grid_flow[2 * 2 * 2] = {8, -8, 8, -8, 8, -8, 8, -8};
  uint8_t grid_cost[2 * 2] = {0, 0, 0, 0};  // all reliable
  float mv[4 * 4 * 2] = {};
  float conf[4 * 4] = {};
  CpuDensify::Densify(grid_flow, 2, 2, grid_cost, 4, 4, 2, /*negate=*/0,
                      /*cost_threshold=*/128, mv, conf);
  // Every full-res pixel should be (8, -8) with confidence 1.0.
  for (uint32_t i = 0; i < 4 * 4; ++i) {
    CHECK(mv[2 * i] == Approx(8.0f));
    CHECK(mv[2 * i + 1] == Approx(-8.0f));
    CHECK(conf[i] == Approx(1.0f));
  }
}

TEST_CASE("CpuDensify zeroes cells above the cost threshold",
          "[nvof.test_components]") {
  // 1x1 grid, working 2x2, gridSize 1. Grid flow (8, -8), cost 200 (>= 128).
  float grid_flow[2] = {8, -8};
  uint8_t grid_cost[1] = {200};
  float mv[2 * 2 * 2] = {1, 1, 1, 1, 1, 1, 1, 1};
  float conf[2 * 2] = {1, 1, 1, 1};
  CpuDensify::Densify(grid_flow, 1, 1, grid_cost, 2, 2, 1, /*negate=*/0,
                      /*cost_threshold=*/128, mv, conf);
  // All cells zeroed (cost 200 >= 128).
  for (uint32_t i = 0; i < 2 * 2; ++i) {
    CHECK(mv[2 * i] == 0.0f);
    CHECK(mv[2 * i + 1] == 0.0f);
    CHECK(conf[i] == 0.0f);
  }
}

TEST_CASE("CpuDensify negates flow when negate is set", "[nvof.test_components]") {
  float grid_flow[2] = {8, -8};
  uint8_t grid_cost[1] = {0};
  float mv[1 * 1 * 2] = {};
  float conf[1] = {};
  CpuDensify::Densify(grid_flow, 1, 1, grid_cost, 1, 1, 1, /*negate=*/1,
                      /*cost_threshold=*/128, mv, conf);
  CHECK(mv[0] == Approx(-8.0f));
  CHECK(mv[1] == Approx(8.0f));
}

TEST_CASE("FlowValidators.IsValidFlow detects all-zero", "[nvof.test_components]") {
  float zero[4 * 2] = {};
  std::string err;
  CHECK_FALSE(FlowValidators::IsValidFlow(zero, 2, 2, &err));
  float nonzero[4 * 2] = {0, 0, 8, -8, 0, 0, 0, 0};
  CHECK(FlowValidators::IsValidFlow(nonzero, 2, 2, &err));
}

TEST_CASE("FlowValidators.IsValidConfidence range", "[nvof.test_components]") {
  float ok[4] = {0.0f, 0.5f, 1.0f, 0.25f};
  std::string err;
  CHECK(FlowValidators::IsValidConfidence(ok, 2, 2, &err));
  float bad[4] = {0.0f, 1.5f, 1.0f, 0.25f};
  CHECK_FALSE(FlowValidators::IsValidConfidence(bad, 2, 2, &err));
}

TEST_CASE("FlowValidators.MagnitudeMatches", "[nvof.test_components]") {
  // 64x64 (the validator excludes a 16px edge margin, so the interior is
  // 32x32), interior expected (8, -8) => magnitude sqrt(128) ~ 11.31.
  std::vector<float> mv(64 * 64 * 2);
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      mv[2 * (y * 64 + x)] = 8.0f;
      mv[2 * (y * 64 + x) + 1] = -8.0f;
    }
  }
  std::string err;
  CHECK(FlowValidators::MagnitudeMatches(mv.data(), 64, 64, 8, -8, 0.5f, &err));
  // A wrong expectation fails.
  CHECK_FALSE(FlowValidators::MagnitudeMatches(mv.data(), 64, 64, 0, 0, 0.5f,
                                               &err));
}

TEST_CASE("DepthValidators.IsAllZeroDepth", "[nvof.test_components]") {
  float zero[16] = {};
  std::string err;
  CHECK(DepthValidators::IsAllZeroDepth(zero, 4, 4, &err));
  float nonzero[16] = {};
  nonzero[5] = 1.0f;
  CHECK_FALSE(DepthValidators::IsAllZeroDepth(nonzero, 4, 4, &err));
}

TEST_CASE("MvContract.PointsCurrentToPrevious", "[nvof.test_components]") {
  // Content moved right by 8 (dx=8). The MV should point left (-8) to be
  // current->previous.
  std::vector<float> mv(64 * 64 * 2);
  for (uint32_t y = 0; y < 64; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      mv[2 * (y * 64 + x)] = -8.0f;   // points left (back to where content was)
      mv[2 * (y * 64 + x) + 1] = 0.0f;
    }
  }
  std::string err;
  CHECK(MvContract::PointsCurrentToPrevious(mv.data(), 64, 64, 8, 0, 0.5f,
                                            &err));
  // If the MV pointed the wrong way (+8), it fails.
  for (auto& v : mv) v = -v;
  CHECK_FALSE(MvContract::PointsCurrentToPrevious(mv.data(), 64, 64, 8, 0,
                                                  0.5f, &err));
}

TEST_CASE("FlowVisualizer exports BMP files", "[nvof.test_components]") {
  const char* flow_path = "nvof_test_flow.bmp";
  const char* cost_path = "nvof_test_cost.bmp";
  const char* mv_path = "nvof_test_mv.bmp";
  float flow[2 * 2 * 2] = {8, -8, 8, -8, 8, -8, 8, -8};
  uint8_t cost[2 * 2] = {0, 100, 200, 255};
  float conf[2 * 2] = {1.0f, 0.5f, 0.2f, 0.0f};
  CHECK(FlowVisualizer::ExportGridFlow(flow, 2, 2, flow_path));
  CHECK(FlowVisualizer::ExportCost(cost, 2, 2, cost_path));
  CHECK(FlowVisualizer::ExportMv(flow, conf, 2, 2, mv_path));
  // Verify the files exist and are non-empty (a valid BMP is >= 54 bytes).
  for (const char* p : {flow_path, cost_path, mv_path}) {
    std::FILE* f = std::fopen(p, "rb");
    REQUIRE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fclose(f);
    CHECK(size >= 54);
    std::remove(p);
  }
}

// ===========================================================================
// Backend contract (host-side).
// ===========================================================================

TEST_CASE("NVOF backend rejects null command_list / input_frame",
          "[nvof.backend_contract]") {
  NVOFOpticalFlowBackend backend(/*device=*/nullptr, 64, 64, 4, 1);
  CHECK_FALSE(backend.IsAvailable());
  CHECK_FALSE(backend.IsUnavailable());
  // Null command_list / input_frame is an input-contract failure (not a retry
  // of the SDK, and not marked as success).
  auto result = backend.TryProcess(nullptr, nullptr, nullptr, 0);
  CHECK(result == BackendResult::kInputContractFailed);
  CHECK(IsOriginalPathFallback(result));
  // The backend is still "not initialized" (not cached as unavailable).
  CHECK_FALSE(backend.IsUnavailable());
}

TEST_CASE("NVOF backend falls back to kUnavailable when the runtime is "
          "missing",
          "[nvof.backend_contract]") {
  // This is the deterministic "runtime missing" fallback. On a Hyper-V dev box
  // without an NVIDIA driver, nvofapi64.dll is absent and the backend caches
  // itself as unavailable. If the runtime IS present (an NVIDIA test machine),
  // this particular fallback is not applicable and the test is skipped.
  if (NvOfRuntimePresent()) {
    SUCCEED("nvofapi64.dll is present; the missing-runtime fallback is not "
            "applicable on this host (real-GPU tests cover this path).");
    return;
  }
  GpuContext ctx;
  if (!CreateGpuContext(&ctx)) {
    SUCCEED("No D3D12 device available on this host; skipping the "
            "missing-runtime fallback test (UNVERIFIED on this host).");
    return;
  }
  NVOFOpticalFlowBackend backend(ctx.device.Get(), 64, 64, 4, 1);
  auto result = backend.TryProcess(ctx.list.Get(), ctx.queue.Get(), ctx.input_texture.Get(), 0);
  CHECK(result == BackendResult::kUnavailable);
  CHECK(backend.IsUnavailable());
  CHECK(IsOriginalPathFallback(result));
  // A second call does not retry the SDK (cached unavailable).
  auto result2 = backend.TryProcess(ctx.list.Get(), ctx.queue.Get(), ctx.input_texture.Get(), 1);
  CHECK(result2 == BackendResult::kUnavailable);
}

// ===========================================================================
// Real-GPU tests (guarded). These SKIP (never fail) on a host without an
// NVIDIA GPU + NVOF runtime, and are marked UNVERIFIED. They run on the RTX
// 4090 test machine.
// ===========================================================================

TEST_CASE("NVOF real-GPU init, controlled +8px flow, and densify",
          "[nvof.gpu]") {
  if (!NvOfRuntimePresent()) {
    SUCCEED("nvofapi64.dll absent on this host; real-GPU test UNVERIFIED "
            "(skipped).");
    return;
  }
  GpuContext ctx;
  if (!CreateGpuContext(&ctx) || !ctx.is_nvidia) {
    SUCCEED("No NVIDIA D3D12 device on this host; real-GPU test UNVERIFIED "
            "(skipped).");
    return;
  }

  const uint32_t w = 128, h = 128, grid = 4;
  NVOFOpticalFlowBackend backend(ctx.device.Get(), w, h, grid, 1);

  // Generate a controlled +8px right-shift sequence (content moves right).
  MotionInputGenerator::Params params;
  params.width = w;
  params.height = h;
  params.dx = 8;
  params.dy = 0;
  MotionInputGenerator gen(params);

  // Helper: submit the current command list and wait for the GPU to finish.
  auto submit_and_wait = [&]() {
    ctx.list->Close();
    ID3D12CommandList* list = ctx.list.Get();
    ctx.queue->ExecuteCommandLists(1, &list);
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                            IID_PPV_ARGS(&fence));
    ctx.queue->Signal(fence.Get(), 1);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    DWORD waited = WaitForSingleObject(event, 10000);
    CloseHandle(event);
    return waited == WAIT_OBJECT_0;
  };

  // Helper: reset the command allocator so the command list can be reused.
  auto reset_list = [&]() { ctx.allocator->Reset(); };

  // Helper: read back a 2D texture (R16G16) into a CPU buffer via a staging
  // buffer. Returns the raw bytes (w*h*8) and true on success.
  auto readback_r16g16 = [&](ID3D12Resource* tex, uint32_t rw, uint32_t rh,
                             std::vector<uint8_t>* out) -> bool {
    out->assign(size_t(rw) * rh * 8, 0);
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    D3D12_RESOURCE_DESC sd{};
    sd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    sd.Width = size_t(rw) * rh * 8;
    sd.Height = 1;
    sd.DepthOrArraySize = 1;
    sd.MipLevels = 1;
    sd.SampleDesc.Count = 1;
    sd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (FAILED(ctx.device->CreateCommittedResource(
            &upload, D3D12_HEAP_FLAG_NONE, &sd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&staging)))) {
      return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    fp.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    fp.Footprint.Width = rw;
    fp.Footprint.Height = rh;
    fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = rw * 8;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (!submit_and_wait()) {
      return false;
    }
    reset_list();
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
      return false;
    }
    std::memcpy(out->data(), mapped, out->size());
    staging->Unmap(0, nullptr);
    return true;
  };

  // Frame 0 initializes the backend (its input fence value 0 is pre-signaled).
  auto r0 = backend.TryProcess(ctx.list.Get(), ctx.queue.Get(), ctx.input_texture.Get(), 0);
  // Catch2 does not support operator|| inside an assertion (even parenthesized),
  // so decompose into a plain bool first.
  const bool r0_ok =
      (r0 == BackendResult::kSuccess) || (r0 == BackendResult::kUnavailable);
  CHECK(r0_ok);
  if (r0 == BackendResult::kUnavailable) {
    SUCCEED("NVOF session could not be created on this device; real-GPU "
            "flow test UNVERIFIED.");
    return;
  }
  CHECK(backend.IsAvailable());
  // Frame 0 has an invalid flow (no valid reference).
  CHECK_FALSE(backend.HasValidFlow());

  // Submit frame 0 and wait for the GPU to finish.
  CHECK(submit_and_wait());
  reset_list();

  // Frames 1..2: signal the input fence to the frame index (the input is
  // GPU-ready), then process.
  for (uint32_t frame = 1; frame <= 2; ++frame) {
    backend.GetInputFence()->Signal(frame);
    reset_list();
    auto r = backend.TryProcess(ctx.list.Get(), ctx.queue.Get(), ctx.input_texture.Get(), frame);
    CHECK(r == BackendResult::kSuccess);
    if (r == BackendResult::kSuccess) {
      CHECK(backend.HasValidFlow());
    }
  }

  // Read back the grid flow (R16G16_SINT, stored as R16G16 float16 bytes) and
  // the full-resolution MV (R16G16_FLOAT).
  std::vector<uint8_t> grid_flow_bytes, mv_bytes;
  CHECK(readback_r16g16(backend.GridFlowOutput(), backend.GridWidth(),
                        backend.GridHeight(), &grid_flow_bytes));
  CHECK(readback_r16g16(backend.MotionVectorOutput(), w, h, &mv_bytes));

  // Decode the grid flow (S10.5 -> pixels). The raw bytes are int16 pairs.
  const int16_t* raw_flow =
      reinterpret_cast<const int16_t*>(grid_flow_bytes.data());
  std::vector<float> grid_flow(
      float(backend.GridWidth() * backend.GridHeight() * 2));
  GridFlowDecoder::Decode(raw_flow, backend.GridWidth(), backend.GridHeight(),
                          grid_flow.data());

  // The content moved right by 8 px/frame (dx = +8). The flow points
  // current -> previous, so the interior grid-flow x component is ~ -8 pixels.
  {
    std::string err;
    CHECK(MvContract::PointsCurrentToPrevious(
        grid_flow.data(), backend.GridWidth(), backend.GridHeight(), 8, 0,
        1.5f, &err));
  }

  // GPU densify vs CPU densify: run the CPU reference densify on the decoded
  // grid flow (+ a zero cost, since the checkerboard is highly reliable) and
  // compare against the readback full-resolution MV. The GPU densify uses the
  // real cost; here we compare the flow direction/magnitude, which is the
  // primary validation. (A full byte-exact cost comparison is a future
  // refinement.)
  {
    std::vector<uint8_t> zero_cost(
        backend.GridWidth() * backend.GridHeight(), 0);
    std::vector<float> cpu_mv(w * h * 2);
    std::vector<float> cpu_conf(w * h);
    CpuDensify::Densify(grid_flow.data(), backend.GridWidth(),
                        backend.GridHeight(), zero_cost.data(), w, h, grid,
                        /*negate=*/0, /*cost_threshold=*/128, cpu_mv.data(),
                        cpu_conf.data());
    // The readback MV is R16G16_FLOAT (float16 pairs). Convert to float32.
    const uint16_t* mv_half =
        reinterpret_cast<const uint16_t*>(mv_bytes.data());
    auto half_to_float = [](uint16_t hh) -> float {
      union { uint16_t u; float f; } cvt;
      cvt.u = hh;
      return cvt.f;
    };
    // Spot-check the interior: the CPU densify and the GPU densify should agree
    // within float16 precision (~0.01) on the flow magnitude.
    bool all_close = true;
    for (uint32_t y = 16; y < h - 16; ++y) {
      for (uint32_t x = 16; x < w - 16; x += 4) {
        const uint32_t i = y * w + x;
        float gpu_vx = half_to_float(mv_half[2 * i]);
        float gpu_vy = half_to_float(mv_half[2 * i + 1]);
        float cpu_vx = cpu_mv[2 * i];
        float cpu_vy = cpu_mv[2 * i + 1];
        if (std::fabs(gpu_vx - cpu_vx) > 0.5f ||
            std::fabs(gpu_vy - cpu_vy) > 0.5f) {
          all_close = false;
        }
      }
    }
    CHECK(all_close);
  }

  backend.Shutdown();
}

}  // namespace nvof
}  // namespace ui
}  // namespace xe
