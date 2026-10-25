/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/nvof/nvof_test_components.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xe {
namespace ui {
namespace nvof {

namespace {

constexpr uint32_t kCheckerPeriod = 16;
constexpr uint32_t kEdgeMargin = 16;

// Writes a 24-bit, bottom-up, uncompressed BMP. Returns true on success.
bool WriteBmp(const char* path, uint32_t w, uint32_t h,
              const uint8_t* rgb) {
  if (!path || !rgb || w == 0 || h == 0) {
    return false;
  }
  const uint32_t row_bytes = w * 3;
  const uint32_t row_padded = (row_bytes + 3) & ~3u;
  const uint32_t pixel_size = row_padded * h;
  const uint32_t file_size = 14 + 40 + pixel_size;
  std::FILE* f = std::fopen(path, "wb");
  if (!f) {
    return false;
  }
  // 14-byte file header.
  uint8_t header[14] = {'B', 'M'};
  auto put32 = [&](uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
    p[2] = uint8_t((v >> 16) & 0xFF);
    p[3] = uint8_t((v >> 24) & 0xFF);
  };
  auto put16 = [&](uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
  };
  put32(header + 2, file_size);
  put32(header + 10, 14 + 40);  // offset to pixel data
  // 40-byte DIB header.
  uint8_t dib[40] = {};
  put32(dib, 40);                        // biSize
  put32(dib + 4, w);                     // biWidth
  put32(dib + 8, h);                     // biHeight (positive = bottom-up)
  put16(dib + 12, 1);                    // biPlanes
  put16(dib + 14, 24);                   // biBitCount
  // biCompression = 0 (BI_RGB), already zero.
  put32(dib + 20, pixel_size);           // biSizeImage
  std::fwrite(header, 1, 14, f);
  std::fwrite(dib, 1, 40, f);
  // Pixel data, bottom-up, rows padded to 4 bytes.
  std::vector<uint8_t> row(row_padded, 0);
  for (int32_t y = int32_t(h) - 1; y >= 0; --y) {
    std::memcpy(row.data(), rgb + size_t(y) * row_bytes, row_bytes);
    std::fwrite(row.data(), 1, row_padded, f);
  }
  std::fclose(f);
  return true;
}

uint8_t ClampByte(float v) {
  if (v < 0.0f) return 0;
  if (v > 255.0f) return 255;
  return uint8_t(std::lround(v));
}

}  // namespace

void MotionInputGenerator::GenerateFrame(
    uint32_t frame_index, std::vector<uint8_t>* out) const {
  const uint32_t n = params_.width * params_.height;
  out->assign(n * 4, 0);
  const int64_t shift_x = int64_t(frame_index) * params_.dx;
  const int64_t shift_y = int64_t(frame_index) * params_.dy;
  for (uint32_t y = 0; y < params_.height; ++y) {
    for (uint32_t x = 0; x < params_.width; ++x) {
      // The content at position (x, y) in frame N is the content that was at
      // (x - N*dx, y - N*dy) in frame 0, i.e. the content moved by (dx, dy)
      // from the previous frame to the current frame.
      const int64_t sx = int64_t(x) - shift_x;
      const int64_t sy = int64_t(y) - shift_y;
      const bool bright =
          (((sx / kCheckerPeriod) + (sy / kCheckerPeriod)) % 2) == 0;
      uint8_t* px = out->data() + size_t(y) * params_.width * 4 + size_t(x) * 4;
      // B8G8R8A8.
      px[0] = bright ? 255 : 0;  // B
      px[1] = bright ? 255 : 0;  // G
      px[2] = bright ? 255 : 0;  // R
      px[3] = 255;               // A
    }
  }
}

void GridFlowDecoder::Decode(const int16_t* raw, uint32_t gridW, uint32_t gridH,
                             float* out) {
  const uint32_t n = gridW * gridH;
  for (uint32_t i = 0; i < n; ++i) {
    out[2 * i] = float(raw[2 * i]) / 32.0f;      // S10.5 -> pixels
    out[2 * i + 1] = float(raw[2 * i + 1]) / 32.0f;
  }
}

void CpuDensify::Densify(const float* grid_flow, uint32_t gridW,
                         uint32_t gridH, const uint8_t* grid_cost,
                         uint32_t workingW, uint32_t workingH,
                         uint32_t gridSize, uint32_t negate,
                         uint32_t cost_threshold, float* out_mv,
                         float* out_conf) {
  for (uint32_t y = 0; y < workingH; ++y) {
    for (uint32_t x = 0; x < workingW; ++x) {
      const uint32_t gx = std::min(x / gridSize, gridW - 1);
      const uint32_t gy = std::min(y / gridSize, gridH - 1);
      const uint32_t gi = gy * gridW + gx;
      float vx = grid_flow[2 * gi];
      float vy = grid_flow[2 * gi + 1];
      if (negate != 0) {
        vx = -vx;
        vy = -vy;
      }
      const uint8_t cost = grid_cost[gi];
      float confidence = float(255u - std::min<uint32_t>(cost, 255u)) / 255.0f;
      if (cost >= cost_threshold) {
        vx = 0.0f;
        vy = 0.0f;
        confidence = 0.0f;
      }
      const uint32_t fi = y * workingW + x;
      out_mv[2 * fi] = vx;
      out_mv[2 * fi + 1] = vy;
      out_conf[fi] = confidence;
    }
  }
}

bool FlowValidators::IsValidFlow(const float* mv, uint32_t w, uint32_t h,
                                 std::string* error) {
  if (!mv) {
    if (error) *error = "null mv buffer";
    return false;
  }
  if (w == 0 || h == 0) {
    if (error) *error = "empty mv buffer";
    return false;
  }
  bool any_nonzero = false;
  for (uint32_t i = 0; i < w * h; ++i) {
    if (mv[2 * i] != 0.0f || mv[2 * i + 1] != 0.0f) {
      any_nonzero = true;
      break;
    }
  }
  if (!any_nonzero) {
    if (error) *error = "mv buffer is all-zero (no motion detected)";
    return false;
  }
  return true;
}

bool FlowValidators::IsValidConfidence(const float* conf, uint32_t w, uint32_t h,
                                       std::string* error) {
  if (!conf) {
    if (error) *error = "null confidence buffer";
    return false;
  }
  for (uint32_t i = 0; i < w * h; ++i) {
    if (conf[i] < 0.0f || conf[i] > 1.0f) {
      if (error) {
        *error = "confidence out of [0,1] at index " + std::to_string(i) +
                 " value " + std::to_string(conf[i]);
      }
      return false;
    }
  }
  return true;
}

bool FlowValidators::MagnitudeMatches(const float* mv, uint32_t w, uint32_t h,
                                      int32_t expected_dx, int32_t expected_dy,
                                      float tolerance, std::string* error) {
  if (!mv) {
    if (error) *error = "null mv buffer";
    return false;
  }
  if (w <= 2 * kEdgeMargin || h <= 2 * kEdgeMargin) {
    if (error) *error = "buffer too small to have an interior region";
    return false;
  }
  const float expected_magnitude =
      std::sqrt(float(expected_dx) * float(expected_dx) +
                float(expected_dy) * float(expected_dy));
  for (uint32_t y = kEdgeMargin; y < h - kEdgeMargin; ++y) {
    for (uint32_t x = kEdgeMargin; x < w - kEdgeMargin; ++x) {
      const float vx = mv[2 * (y * w + x)];
      const float vy = mv[2 * (y * w + x) + 1];
      const float magnitude = std::sqrt(vx * vx + vy * vy);
      if (std::fabs(magnitude - expected_magnitude) > tolerance) {
        if (error) {
          *error = "mv magnitude " + std::to_string(magnitude) +
                   " at (" + std::to_string(x) + "," + std::to_string(y) +
                   ") not within " + std::to_string(tolerance) + " of expected " +
                   std::to_string(expected_magnitude);
        }
        return false;
      }
    }
  }
  return true;
}

bool DepthValidators::IsAllZeroDepth(const float* depth, uint32_t w, uint32_t h,
                                     std::string* error) {
  if (!depth) {
    if (error) *error = "null depth buffer";
    return false;
  }
  for (uint32_t i = 0; i < w * h; ++i) {
    if (depth[i] != 0.0f) {
      if (error) {
        *error = "depth not all-zero at index " + std::to_string(i) + " value " +
                 std::to_string(depth[i]);
      }
      return false;
    }
  }
  return true;
}

bool MvContract::PointsCurrentToPrevious(const float* mv, uint32_t w, uint32_t h,
                                         int32_t content_dx, int32_t content_dy,
                                         float tolerance, std::string* error) {
  // For content that moved by (content_dx, content_dy) from prev to current,
  // the flow at a pixel in the CURRENT frame points back to where that feature
  // was in the PREVIOUS frame, i.e. the expected MV is (-content_dx,
  // -content_dy). Verify the interior MV matches that.
  const int32_t expected_flow_dx = -content_dx;
  const int32_t expected_flow_dy = -content_dy;
  if (!mv) {
    if (error) *error = "null mv buffer";
    return false;
  }
  if (w <= 2 * kEdgeMargin || h <= 2 * kEdgeMargin) {
    if (error) *error = "buffer too small to have an interior region";
    return false;
  }
  for (uint32_t y = kEdgeMargin; y < h - kEdgeMargin; ++y) {
    for (uint32_t x = kEdgeMargin; x < w - kEdgeMargin; ++x) {
      const float vx = mv[2 * (y * w + x)];
      const float vy = mv[2 * (y * w + x) + 1];
      if (std::fabs(vx - float(expected_flow_dx)) > tolerance ||
          std::fabs(vy - float(expected_flow_dy)) > tolerance) {
        if (error) {
          *error = "mv (" + std::to_string(vx) + "," + std::to_string(vy) +
                   ") at (" + std::to_string(x) + "," + std::to_string(y) +
                   ") does not point current->previous (expected " +
                   std::to_string(expected_flow_dx) + "," +
                   std::to_string(expected_flow_dy) + ")";
        }
        return false;
      }
    }
  }
  return true;
}

bool FlowVisualizer::ExportGridFlow(const float* flow, uint32_t w, uint32_t h,
                                    const char* path) {
  if (!flow) return false;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (uint32_t i = 0; i < w * h; ++i) {
    // Flow (pixel units) -> RGB. Zero flow = gray (128,128,0); positive x =
    // redder, positive y = greener. Scale 16 maps a ~8px flow to ~128.
    rgb[3 * i] = ClampByte(128.0f + flow[2 * i] * 16.0f);
    rgb[3 * i + 1] = ClampByte(128.0f + flow[2 * i + 1] * 16.0f);
    rgb[3 * i + 2] = 0;
  }
  return WriteBmp(path, w, h, rgb.data());
}

bool FlowVisualizer::ExportCost(const uint8_t* cost, uint32_t w, uint32_t h,
                                const char* path) {
  if (!cost) return false;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (uint32_t i = 0; i < w * h; ++i) {
    // cost 0 = reliable (white), 255 = unreliable (black).
    const uint8_t v = uint8_t(255u - cost[i]);
    rgb[3 * i] = v;
    rgb[3 * i + 1] = v;
    rgb[3 * i + 2] = v;
  }
  return WriteBmp(path, w, h, rgb.data());
}

bool FlowVisualizer::ExportMv(const float* mv, const float* conf, uint32_t w,
                              uint32_t h, const char* path) {
  if (!mv || !conf) return false;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (uint32_t i = 0; i < w * h; ++i) {
    rgb[3 * i] = ClampByte(128.0f + mv[2 * i] * 16.0f);
    rgb[3 * i + 1] = ClampByte(128.0f + mv[2 * i + 1] * 16.0f);
    rgb[3 * i + 2] = ClampByte(conf[i] * 255.0f);  // B = confidence
  }
  return WriteBmp(path, w, h, rgb.data());
}

}  // namespace nvof
}  // namespace ui
}  // namespace xe
