/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_D3D12_D3D12_PRESENTER_H_
#define XENIA_UI_D3D12_D3D12_PRESENTER_H_

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <mutex>
#include <vector>

#include "xenia/base/math.h"
#include "xenia/ui/dlss/sr/dlss_super_sampling_backend.h"
#include "xenia/ui/d3d12/d3d12_gpu_completion_timeline.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/guest_output_enhancement.h"
#include "xenia/ui/dlss/ngx/ngx_core_host.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/surface.h"

namespace xe {
namespace ui {
namespace d3d12 {

class D3D12UIDrawContext final : public UIDrawContext {
 public:
  D3D12UIDrawContext(Presenter& presenter, uint32_t render_target_width,
                     uint32_t render_target_height,
                     ID3D12GraphicsCommandList* command_list,
                     uint64_t submission_index_current,
                     uint64_t submission_index_completed)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        command_list_(command_list),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed) {}

  ID3D12GraphicsCommandList* command_list() const {
    return command_list_.Get();
  }
  uint64_t submission_index_current() const {
    return submission_index_current_;
  }
  uint64_t submission_index_completed() const {
    return submission_index_completed_;
  }

 private:
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
  uint64_t submission_index_current_;
  uint64_t submission_index_completed_;
};

class D3D12Presenter final : public Presenter {
 public:
  static constexpr DXGI_FORMAT kGuestOutputFormat =
      DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr D3D12_RESOURCE_STATES kGuestOutputInternalState =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  static constexpr DXGI_FORMAT kGuestOutputIntermediateFormat =
      DXGI_FORMAT_R10G10B10A2_UNORM;

  // The format used internally by Windows composition.
  static constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

  // The callback must use the main direct queue of the provider.
  class D3D12GuestOutputRefreshContext final
      : public GuestOutputRefreshContext {
   public:
    D3D12GuestOutputRefreshContext(bool& is_8bpc_out_ref,
                                   ID3D12Resource* resource)
        : GuestOutputRefreshContext(is_8bpc_out_ref), resource_(resource) {}

    // kGuestOutputFormat, supports UAV. The initial state in the callback is
    // kGuestOutputInternalState, and the callback must also transition it back
    // to kGuestOutputInternalState before finishing.
    ID3D12Resource* resource_uav_capable() const { return resource_.Get(); }

   private:
    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
  };

  static std::unique_ptr<D3D12Presenter> Create(
      HostGpuLossCallback host_gpu_loss_callback,
      const D3D12Provider& provider) {
    auto presenter = std::unique_ptr<D3D12Presenter>(
        new D3D12Presenter(host_gpu_loss_callback, provider));
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~D3D12Presenter();

  const D3D12Provider& provider() const { return provider_; }

  // UI thread only. Mirrors the GuestOutputPaintConfig access pattern: the UI
  // thread writes the enhancement config, the paint thread reads it (under the
  // mutex) while painting.
  void SetGuestOutputEnhancementConfigFromUIThread(
      const GuestOutputEnhancementConfig& new_config);
  const GuestOutputEnhancementConfig& GetGuestOutputEnhancementConfigFromUIThread()
      const;

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;

  void AwaitUISubmissionCompletionFromUIThread(uint64_t submission_index) {
    ui_completion_timeline_->AwaitSubmissionAndUpdateCompleted(
        submission_index);
  }

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width,
      uint32_t new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(
      uint32_t mailbox_index, uint32_t frontbuffer_width,
      uint32_t frontbuffer_height,
      std::function<bool(GuestOutputRefreshContext& context)> refresher,
      bool& is_8bpc_out) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        float x;
        float y;
      };
      float offset[2];
    };
    union {
      struct {
        float width;
        float height;
      };
      float size[2];
    };
  };

  enum class GuestOutputPaintRootParameter : UINT {
    kSource,
    kRectangle,
    kEffectConstants,

    kCount,
  };

  enum GuestOutputPaintRootSignatureIndex : size_t {
    kGuestOutputPaintRootSignatureIndexBilinear,
    kGuestOutputPaintRootSignatureIndexCasSharpen,
    kGuestOutputPaintRootSignatureIndexCasResample,
    kGuestOutputPaintRootSignatureIndexFsrEasu,
    kGuestOutputPaintRootSignatureIndexFsrRcas,

    kGuestOutputPaintRootSignatureCount,
  };

  static constexpr GuestOutputPaintRootSignatureIndex
  GetGuestOutputPaintRootSignatureIndex(GuestOutputPaintEffect effect) {
    switch (effect) {
      case GuestOutputPaintEffect::kBilinear:
      case GuestOutputPaintEffect::kBilinearDither:
        return kGuestOutputPaintRootSignatureIndexBilinear;
      case GuestOutputPaintEffect::kCasSharpen:
      case GuestOutputPaintEffect::kCasSharpenDither:
        return kGuestOutputPaintRootSignatureIndexCasSharpen;
      case GuestOutputPaintEffect::kCasResample:
      case GuestOutputPaintEffect::kCasResampleDither:
        return kGuestOutputPaintRootSignatureIndexCasResample;
      case GuestOutputPaintEffect::kFsrEasu:
        return kGuestOutputPaintRootSignatureIndexFsrEasu;
      case GuestOutputPaintEffect::kFsrRcas:
      case GuestOutputPaintEffect::kFsrRcasDither:
        return kGuestOutputPaintRootSignatureIndexFsrRcas;
      default:
        assert_unhandled_case(effect);
        return kGuestOutputPaintRootSignatureCount;
    }
  }

  struct PaintContext {
    explicit PaintContext() = default;
    PaintContext(const PaintContext& paint_context) = delete;
    PaintContext& operator=(const PaintContext& paint_context) = delete;

    static constexpr uint32_t kSwapChainBufferCount = 3;

    enum RTVIndex : UINT {
      // Swap chain buffers - updated when creating the swap chain
      // (connection-specific).
      kRTVIndexSwapChainBuffer0,

      // Intermediate textures - the last usage is
      // guest_output_intermediate_texture_paint_last_usage_.
      kRTVIndexGuestOutputIntermediate0 =
          kRTVIndexSwapChainBuffer0 + kSwapChainBufferCount,

      kRTVCount =
          kRTVIndexGuestOutputIntermediate0 + kGuestOutputMailboxSize - 1,
    };

    enum ViewIndex : UINT {
      // Guest output textures - indices are the same as in
      // guest_output_resource_paint_refs, and the last usage is tied to them.
      kViewIndexGuestOutput0Srv,

      // Intermediate textures - the last usage is
      // guest_output_intermediate_texture_paint_last_usage_.
      kViewIndexGuestOutputIntermediate0Srv =
          kViewIndexGuestOutput0Srv + kGuestOutputMailboxSize,

      // SRV of the DLSS SR backend's output texture. Only written when the
      // backend is active and produced usable output; the first paint effect's
      // source is redirected to it.
      kViewIndexDLSSSrOutputSrv =
          kViewIndexGuestOutputIntermediate0Srv + kMaxGuestOutputPaintEffects - 1,

      // SRV of the DLSS FG backend's interpolated-output texture (the generated
      // frame N-0.5). Only written when the FG backend is active and produced
      // usable output; the FG paint pass reads from it (before presenting the
      // real frame).
      kViewIndexDLSSFgOutputSrv = kViewIndexDLSSSrOutputSrv + 1,

      // SRV of the DLSS FG backend's OutputReal texture (the SDK's copy of the
      // real frame N). Only written when the FG backend is active; the FG paint
      // pass reads from it when presenting the real frame (instead of the
      // backbuffer's own content).
      kViewIndexDLSSFgOutputRealSrv = kViewIndexDLSSFgOutputSrv + 1,

      kViewCount = kViewIndexDLSSFgOutputRealSrv + 1,
    };

    void AwaitSwapChainUsageCompletion() {
      // May be called during destruction.

      // Presentation engine usage.
      if (present_completion_timeline) {
        present_completion_timeline->AwaitAllSubmissions();
      }

      // Paint (render target) usage. While the presentation fence is signaled
      // on the same queue, and presentation happens after painting, awaiting
      // anyway for safety just to make less assumptions in the architecture.
      if (paint_completion_timeline) {
        paint_completion_timeline->AwaitAllSubmissions();
      }
    }

    void DestroySwapChain();

    // Connection-independent.

    // Signaled before presenting.
    std::unique_ptr<D3D12GPUCompletionTimeline> paint_completion_timeline;
    // Signaled after presenting.
    std::unique_ptr<D3D12GPUCompletionTimeline> present_completion_timeline;

    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,
               kSwapChainBufferCount>
        command_allocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    // Separate command list for the FG block (Milestone 5-C). The FG Evaluate
    // and the interp->backbuffer draw are recorded here (NOT into the main
    // command_list) so that an FG GPU-side failure discards only the FG
    // commands, leaving the effect chain (in command_list) intact. The main
    // command list is executed first (painting the real frame N into the
    // backbuffer), then this command list (the FG Evaluate reading the
    // backbuffer + the interp->backbuffer draw). The second present (the real
    // frame N from OutputReal) reuses command_list (re-recorded).
    // A ring of command allocators (one per in-flight frame), like
    // command_allocators, so the fg_command_list can be Reset every frame
    // without discarding the previous frame's in-flight FG commands. Indexed
    // by the current paint submission modulo the ring size.
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,
               kSwapChainBufferCount>
        fg_command_allocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> fg_command_list;

    // Descriptor heaps for views of the current resources related to the guest
    // output and to painting, updated either during painting or during
    // connection lifetime management if outdated after awaiting usage
    // completion.
    // RTV heap.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    // Shader-visible CBV/SRV/UAV heap.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> view_heap;

    // Refreshed and cleaned up during guest output painting. The first is the
    // paint submission index in which the guest output texture (and its
    // descriptors) was last used, the second is the reference to the texture,
    // which may be null. The indices are not mailbox indices here, rather, if
    // the reference is not in this array yet, the most outdated reference, if
    // needed, is replaced with the new one, awaiting the completion of the last
    // paint usage.
    std::array<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>,
               kGuestOutputMailboxSize>
        guest_output_resource_paint_refs;

    // Current intermediate textures for guest output painting, refreshed when
    // painting guest output. While not in use, they are in
    // D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,
               kMaxGuestOutputPaintEffects - 1>
        guest_output_intermediate_textures;
    uint64_t guest_output_intermediate_texture_last_usage = 0;

    // Connection-specific.

    uint32_t swap_chain_width = 0;
    uint32_t swap_chain_height = 0;
    bool swap_chain_allows_tearing = false;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kSwapChainBufferCount>
        swap_chain_buffers;
  };

  explicit D3D12Presenter(HostGpuLossCallback host_gpu_loss_callback,
                          const D3D12Provider& provider)
      : Presenter(host_gpu_loss_callback),
        provider_(provider),
        // The shared NGX core owner (Milestone 5-B, prerequisite P0-1). Created
        // always (the constructor is trivial - no DLL load, no GPU work); the
        // SDK Init happens only on the first Acquire (when SR or FG is
        // requested). Shared by the DLSS SR backend and (Milestone 5-C) the
        // DLSS FG backend; Shutdown1 runs only when the last consumer releases.
        core_host_(std::make_unique<NgxCoreHost>()) {}

  bool dxgi_supports_tearing() const { return dxgi_supports_tearing_; }

  bool InitializeSurfaceIndependent();

  // Resolves the runtime directories to search for the NGX runtime DLLs,
  // relative to the executable (per the design: the runtime is deployed
  // alongside the main program and referenced by relative path). Returns an
  // empty vector when the executable path cannot be determined.
  std::vector<std::wstring> GetDLSSRuntimeDirectories() const;

  // Resolves the absolute path to the ngx-local.json config file, relative to
  // the executable (runtime\config\ngx-local.json). Returns an empty string when
  // the executable path cannot be determined (the backend then fails to load the
  // config and falls back to the original path).
  std::wstring GetDLSSConfigPath() const;

  const D3D12Provider& provider_;

  // Whether DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is supported by DXGI (depends in
  // particular on the Windows 10 version and hardware support), primarily for
  // variable refresh rate support.
  bool dxgi_supports_tearing_ = false;

  // Static objects for guest output presentation, used only when painting the
  // main target (can be destroyed only after awaiting main target usage
  // completion).
  std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>,
             kGuestOutputPaintRootSignatureCount>
      guest_output_paint_root_signatures_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
             size_t(GuestOutputPaintEffect::kCount)>
      guest_output_paint_intermediate_pipelines_;
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,
             size_t(GuestOutputPaintEffect::kCount)>
      guest_output_paint_final_pipelines_;

  // The first is the refresher completion timeline submission index at which
  // the guest output texture was last refreshed, the second is the reference to
  // the texture, which may be null. The indices are the mailbox indices.
  std::array<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>,
             kGuestOutputMailboxSize>
      guest_output_resources_;
  // The guest output resources are protected by two completion timelines - the
  // refresher one (for writing to them via the guest_output_resources_
  // references) and the paint one (for presenting it via the
  // paint_context_.guest_output_resource_paint_refs references taken from
  // guest_output_resources_).
  std::unique_ptr<D3D12GPUCompletionTimeline>
      guest_output_resource_refresher_completion_timeline_;

  // UI completion timeline with the submission index that can be given to UI
  // drawers (accessible from the UI thread only, at any time).
  std::unique_ptr<D3D12GPUCompletionTimeline> ui_completion_timeline_;

  // The shared NGX core owner (Milestone 5-B, prerequisite P0-1). Non-owning
  // pointers to it are passed to the DLSS SR backend and (Milestone 5-C) the
  // DLSS FG backend. The core host is the ONLY place that calls the NGX core
  // lifecycle entry points (Init_with_ProjectID / Shutdown1 / AllocateParameters
  // / DestroyParameters). Reference-counted; Shutdown1 runs only when the last
  // consumer releases. Created in the constructor (trivial - no side effects).
  std::unique_ptr<NgxCoreHost> core_host_;

  // Accessible only by painting and by surface connection lifetime management
  // (ConnectOrReconnectPaintingToSurfaceFromUIThread,
  // DisconnectPaintingFromSurfaceFromUIThreadImpl) by the thread doing it, as
  // well as by presenter initialization and shutdown.
  PaintContext paint_context_;

  // DLSS SR backend (Milestone 3-B). Lazily created when SR is requested and
  // the guest output is not already at the target resolution (1:1 bypass).
  // Owns its output texture and the SDK feature handle; only borrows the guest
  // output (read-only). nullptr when SR has never been requested. The backend
  // never touches guest_output_resource_paint_refs.
  std::unique_ptr<IDLSSSuperSamplingBackend> dlss_sr_backend_;

  // DLSS Frame Generation (FG) backend (Milestone 5-C). Lazily created when FG
  // is requested (master && FG sub-switch). Owns its interpolated-output
  // texture (N-0.5), OutputReal texture (N), disable-interpolation status
  // buffer, and zero depth / motion-vector placeholders (guest resolution, read
  // from frame_params.render_width/height); only borrows the backbuffer
  // (read-only). nullptr when FG has never been requested. The backend never
  // touches guest_output_resource_paint_refs.
  std::unique_ptr<IFrameGenerationBackend> fg_backend_;
  // Monotonically increasing FG real-frame ID (the ID of the real guest frame N
  // being processed). Incremented on every processed guest frame.
  uint64_t fg_frame_counter_ = 0;

  // NVOF optical-flow backend (Milestone 4-E). Produces a full-resolution
  // motion vector (R16G16_FLOAT, current -> previous) that the SR backend
  // borrows as pInMotionVectors. Orthogonal to SR: NVOF failure does not force
  // SR failure (SR falls back to its zero placeholder MV). nullptr when NVOF
  // has never been requested. The backend never touches
  // guest_output_resource_paint_refs.
  std::unique_ptr<IOpticalFlowBackend> nvof_backend_;
  // Monotonically increasing NVOF frame index (distinguishes the first frame
  // from round-robin frame-pool slots). Incremented on every processed frame.
  uint64_t nvof_frame_counter_ = 0;

  // DLSS SR enhancement config. Writable only by the UI thread, read by the
  // paint thread (under guest_output_enhancement_config_mutex_). All switches
  // default OFF.
  GuestOutputEnhancementConfig guest_output_enhancement_config_;
  std::mutex guest_output_enhancement_config_mutex_;
};

}  // namespace d3d12
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_D3D12_D3D12_PRESENTER_H_
