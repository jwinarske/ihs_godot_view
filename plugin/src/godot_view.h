/*
 * Copyright 2026 Joel Winarske
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ihs/platform_view.h>
#include <ihs_godot/bridge.h>

#include "egl_image_importer.h"
#include "gbm_allocator.h"
#include "surface_policy.h"
#include "unique_fd.h"
#include "view_params.h"
#include "vk_image_pool.h"

namespace ihs_godot {

// One ihs_godot_view platform view: the producer side of the frame hand-off.
//
// Threads, and what each may touch:
//   platform  factory, every IhsPvCallbacks entry but presented, and the
//             negotiation (ihs_pv_negotiate is platform-thread only). Shell
//             facts and the negotiation result are written here.
//   engine    the Godot thread (GodotRuntime). The buffer ring lives here
//             exclusively: allocation, target registration, fence waits and
//             submits.
//   display   presented(): only atomics.
//
// The dispose guarantee (no ihs_pv_submit in flight once dispose returns, and
// none can start) is kept with submit_mutex_: every ihs_pv_* call for the
// view's handle is made under it after checking detached_, and Dispose() sets
// detached_ under it. Dispose never waits on the engine thread, which may be
// deep in a long Godot iteration (a scene load) at the time.
class GodotView : public std::enable_shared_from_this<GodotView> {
 public:
  // Platform thread. Gathers the shell's facts; returns null when the view
  // cannot exist (no capability query).
  static std::shared_ptr<GodotView> Create(const IhsPvCreateInfo* info,
                                           IhsPlatformView* view);
  ~GodotView();

  [[nodiscard]] int32_t id() const { return id_; }
  [[nodiscard]] const ViewParams& params() const { return params_; }
  [[nodiscard]] bool detached() const { return detached_.load(); }
  [[nodiscard]] uint64_t shell_render_device() const;
  [[nodiscard]] bool shell_device_uuid(uint8_t out[16]) const;
  // Physical size from the latest resize (or create), 0x0 until known.
  void size(uint32_t& width, uint32_t& height) const;
  [[nodiscard]] double device_pixel_ratio() const {
    return params_.device_pixel_ratio;
  }
  // The display's refresh period when the shell reported one, else 0.
  [[nodiscard]] uint32_t refresh_ns() const { return refresh_ns_.load(); }
  [[nodiscard]] bool suspended() const { return suspended_.load(); }

  // --- platform thread (IhsPvCallbacks) ---
  void OnResize(double width, double height);
  void OnTouch(int32_t action,
               int32_t point_count,
               size_t size,
               const double* data);
  void OnSuspended(bool suspended);
  void OnRenegotiate();
  void Dispose();
  // --- display thread ---
  void OnPresented(uint32_t refresh_ns);

  // --- engine thread ---
  // Godot is running: record what it can render into and negotiate.
  void OnEngineReady(const IhsGodotApiV1* api);
  // Pick the buffer this iteration renders into, after its release fence.
  uint32_t BeginFrame();
  // Hand the captured frame to the shell.
  void EndFrame(const IhsGodotFrameInfo& info);
  // The window resized: rebuild the ring at the new size.
  void ApplyResize();
  // Release everything; the view is gone or being replaced.
  void Teardown();

  // Platform-thread negotiation, posted with ihs_pv_post_platform_task from
  // the engine thread, or run directly from the renegotiate callback.
  void NegotiateOnPlatform();

 private:
  struct Slot {
    uint32_t target_id = IHS_GODOT_NO_TARGET;
    uint32_t buffer_id = 0;
    bool submitted = false;
    UniqueFd release_fence;
    // kDmabuf / kEglImage
    std::unique_ptr<DmabufBuffer> dmabuf;
    void* egl_image = nullptr;
    // kVkImage
    int vk_index = -1;
    // kShm
    UniqueFd shm_fd;
    void* shm_map = nullptr;
    size_t shm_map_size = 0;
    uint32_t shm_stride = 0;
  };

  // What negotiation decided, handed from the platform to the engine thread.
  struct Negotiated {
    uint64_t serial = 0;
    SurfacePath path = SurfacePath::kNone;
    uint32_t granted_kind = 0;
    FourccModifier format;
    // SOFTWARE_SHM: dup'd host buffers, their stride and the size they are.
    std::vector<UniqueFd> shm_fds;
    uint32_t shm_stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  GodotView() = default;

  void RefreshCaps();  // platform thread
  void ApplyNegotiated(const std::shared_ptr<Negotiated>& n);  // engine
  bool BuildRing(uint32_t width, uint32_t height);
  void ReleaseRing(bool retire);
  bool Submit(Slot& slot, int acquire_fence_fd);
  void FallBackToShm(const char* why);
  void EmitSurface(const Negotiated& n, bool first);

  // Immutable after Create.
  int32_t id_ = -1;
  ViewParams params_;

  // Shell facts, platform thread.
  mutable std::mutex facts_mutex_;
  std::string backend_key_;
  uint32_t shell_kinds_ = 0;
  uint64_t render_device_ = 0;
  std::vector<FourccModifier> shell_formats_;
  bool has_vk_ = false;
  IhsVulkanContext vk_{};
  uint8_t shell_uuid_[16] = {};
  bool has_egl_ = false;
  IhsEglContext egl_{};
  // Godot facts, written on the engine thread once.
  bool godot_known_ = false;
  IhsGodotRendererInfo renderer_{};
  std::vector<FourccModifier> godot_formats_;
  // A preference forced after a path failed (shm floor).
  bool force_shm_ = false;
  uint64_t negotiate_serial_ = 0;

  // Handle and dispose guarantee.
  std::mutex submit_mutex_;
  IhsPlatformView* pv_ = nullptr;
  std::atomic<bool> detached_{false};

  // Size, platform -> engine.
  std::atomic<uint32_t> width_{0};
  std::atomic<uint32_t> height_{0};
  std::atomic<bool> suspended_{false};
  std::atomic<uint32_t> refresh_ns_{0};

  // Engine thread only.
  const IhsGodotApiV1* api_ = nullptr;
  std::shared_ptr<Negotiated> current_;
  uint64_t applied_serial_ = 0;
  std::vector<Slot> slots_;
  size_t next_slot_ = 0;
  int active_slot_ = -1;
  uint32_t ring_width_ = 0;
  uint32_t ring_height_ = 0;
  uint32_t next_buffer_id_ = 1;
  uint64_t seq_ = 0;
  uint64_t submit_errors_ = 0;
  uint64_t fence_timeouts_ = 0;
  GbmAllocator gbm_;
  std::shared_ptr<VkImagePool> vk_pool_;
  EglImageImporter egl_importer_;
  bool announced_ = false;
};

}  // namespace ihs_godot
