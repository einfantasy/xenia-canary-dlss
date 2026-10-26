/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_NVOF_NVOF_TEST_COMPONENTS_H_
#define XENIA_UI_NVOF_NVOF_TEST_COMPONENTS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace xe {
namespace ui {
namespace nvof {

// ---------------------------------------------------------------------------
// MotionInputGenerator
//   A CPU-side controlled motion-input generator. It produces synthetic
//   ABGR8 (B8G8R8A8) frames with a KNOWN per-frame translation, so the
//   optical-flow output can be validated against an expected shift (the
//   Veyra-style "+8px/frame right-shift" pattern). This is the ground-truth
//   input for the controlled-input flow tests; it needs no GPU.
// ---------------------------------------------------------------------------
class MotionInputGenerator {
 public:
  struct Params {
    uint32_t width = 256;
    uint32_t height = 256;
    // Per-frame content translation (pixels). Positive dx = content moves
    // right from the previous frame to the current frame.
    int32_t dx = 8;
    int32_t dy = 0;
  };

  explicit MotionInputGenerator(Params params) : params_(params) {}

  // Fills `out` (width*height*4 bytes, B8G8R8A8) with the synthetic frame at
  // `frame_index`. The pattern is shifted by frame_index * (dx, dy). Frame 0 is
  // the unshifted reference; frame N is shifted by N*(dx,dy).
  void GenerateFrame(uint32_t frame_index, std::vector<uint8_t>* out) const;

  const Params& params() const { return params_; }

 private:
  Params params_;
};

// ---------------------------------------------------------------------------
// GridFlowDecoder
//   Decodes the raw NVOF grid flow (R16G16_SINT; each texel is an
//   NV_OF_FLOW_VECTOR with S10.5 flowx/flowy) to float2 motion vectors in
//   PIXEL units. The S10.5 -> pixel conversion is /32. Matches the densify
//   shader's `float2(raw) / 32.0`.
// ---------------------------------------------------------------------------
struct GridFlowDecoder {
  // `raw` is gridW*gridH texels of (int16 flowx, int16 flowy). `out` is
  // gridW*gridH texels of (float x, float y) in pixel units.
  static void Decode(const int16_t* raw, uint32_t gridW, uint32_t gridH,
                     float* out);
};

// ---------------------------------------------------------------------------
// CpuDensify
//   CPU reference densify: expands the grid flow (pixel units) and cost to a
//   full-resolution MV (pixel units) + confidence [0,1], EXACTLY matching the
//   GPU densify shader. Used as the reference for the GPU-vs-CPU densify
//   comparison test. `negate` / `cost_threshold` mirror the backend's
//   kNegate / kCostThreshold.
// ---------------------------------------------------------------------------
struct CpuDensify {
  static void Densify(const float* grid_flow, uint32_t gridW, uint32_t gridH,
                      const uint8_t* grid_cost, uint32_t workingW,
                      uint32_t workingH, uint32_t gridSize, uint32_t negate,
                      uint32_t cost_threshold, float* out_mv,
                      float* out_conf);
};

// ---------------------------------------------------------------------------
// FlowValidators / DepthValidators / MvContract
//   CPU-side validity and contract checks. Each returns true on pass and
//   fills `error` with a diagnostic on failure. No GPU required.
// ---------------------------------------------------------------------------
struct FlowValidators {
  // True if the full-resolution MV buffer is non-empty and has at least some
  // non-zero vectors (i.e., not all-zero where motion is expected).
  static bool IsValidFlow(const float* mv, uint32_t w, uint32_t h,
                          std::string* error);
  // True if all confidence values lie within [0,1].
  static bool IsValidConfidence(const float* conf, uint32_t w, uint32_t h,
                                std::string* error);
  // True if the MV magnitude at the interior texels is within `tolerance`
  // pixels of the expected shift (expected_dx, expected_dy). Edge texels are
  // excluded (they may be occluded / undefined).
  static bool MagnitudeMatches(const float* mv, uint32_t w, uint32_t h,
                               int32_t expected_dx, int32_t expected_dy,
                               float tolerance, std::string* error);
};

struct DepthValidators {
  // The SR consumer contract is a SYNTHETIC ZERO depth (R32F, all-zero). True
  // if the depth buffer is all-zero.
  static bool IsAllZeroDepth(const float* depth, uint32_t w, uint32_t h,
                             std::string* error);
};

struct MvContract {
  // True if the MV points current -> previous. For content that moved by
  // (content_dx, content_dy) from prev to current, the flow at a pixel in the
  // CURRENT frame points back to where that feature was in the PREVIOUS frame,
  // i.e. the MV x component is ~ -content_dx. Matches the verified Veyra
  // +8px test (content moves right -> MV points left).
  static bool PointsCurrentToPrevious(const float* mv, uint32_t w, uint32_t h,
                                      int32_t content_dx, int32_t content_dy,
                                      float tolerance, std::string* error);
};

// ---------------------------------------------------------------------------
// FlowVisualizer
//   Exports the grid flow / cost / full-res MV / confidence to 24-bit BMP
//   files (CPU-side, no GPU required). Returns true on success. The files are
//   for human inspection / regression comparison.
// ---------------------------------------------------------------------------
struct FlowVisualizer {
  // `flow` is w*h texels of (float x, float y) in pixel units.
  static bool ExportGridFlow(const float* flow, uint32_t w, uint32_t h,
                             const char* path);
  // `cost` is w*h bytes (0 = reliable, 255 = unreliable).
  static bool ExportCost(const uint8_t* cost, uint32_t w, uint32_t h,
                         const char* path);
  // `mv` is w*h texels of (float x, float y); `conf` is w*h floats in [0,1].
  static bool ExportMv(const float* mv, const float* conf, uint32_t w, uint32_t h,
                       const char* path);
};

}  // namespace nvof
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_NVOF_NVOF_TEST_COMPONENTS_H_
