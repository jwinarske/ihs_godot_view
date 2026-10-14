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

#include "godot_view.h"

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

#include "fence.h"
#include "godot_runtime.h"
#include "input_map.h"
#include "log.h"

namespace ihs_godot {

namespace {

constexpr uint32_t kABGR8888 = 0x34324241;  // 'AB24': R,G,B,A bytes
constexpr int kReleaseWaitMs = 100;
constexpr uint32_t kMinSize = 16;

const char* KindName(uint32_t kind, SurfacePath path) {
  switch (kind) {
    case IHS_PV_KIND_TEXTURE_DMABUF_IMPORT:
      return "dmabuf";
    case IHS_PV_KIND_DRM_PLANE:
      return "plane";
    case IHS_PV_KIND_SOFTWARE_SHM:
      return "shm";
    default:
      break;
  }
  return SurfacePathName(path);
}

const char* ApiName(uint32_t api) {
  switch (api) {
    case IHS_GODOT_API_VULKAN:
      return "vulkan";
    case IHS_GODOT_API_OPENGL:
      return "opengl";
    case IHS_GODOT_API_GLES:
      return "gles";
    default:
      return "none";
  }
}

void PlatformNegotiate(void* user_data) {
  const auto id = static_cast<int32_t>(reinterpret_cast<intptr_t>(user_data));
  if (auto view = GodotRuntime::Get().Find(id); view && !view->detached()) {
    view->NegotiateOnPlatform();
  }
}

// Ask for a negotiation from a thread that is not the platform thread.
void PostNegotiate(int32_t id) {
  if (ihs_pv_post_platform_task == nullptr ||
      ihs_pv_post_platform_task(&PlatformNegotiate,
                                reinterpret_cast<void*>(intptr_t{id})) !=
          IHS_PV_OK) {
    LOG_E("view %d: cannot reach the platform thread to negotiate (shell "
          "older than ihs_pv_post_platform_task?)",
          id);
  }
}

}  // namespace

std::shared_ptr<GodotView> GodotView::Create(const IhsPvCreateInfo* info,
                                             IhsPlatformView* view) {
  std::shared_ptr<GodotView> v(new GodotView());
  v->id_ = info->id;
  v->pv_ = view;

  const char* assets =
      ihs_pv_assets_path != nullptr ? ihs_pv_assets_path() : nullptr;
  v->params_ = ParseViewParams(info->params, info->params_size, assets);
  ApplyEnvironment(v->params_);

  // The shell may create at 1x1 and send the real size with the first
  // resize; anything that small is "not known yet".
  if (info->width >= kMinSize && info->height >= kMinSize) {
    v->width_ = static_cast<uint32_t>(info->width);
    v->height_ = static_cast<uint32_t>(info->height);
  }

  v->RefreshCaps();
  if (v->backend_key_.empty() && v->shell_kinds_ == 0) {
    LOG_E("view %d: capability query failed; no backend to render for",
          v->id_);
    return nullptr;
  }

  IhsVulkanContext vk{};
  vk.struct_size = sizeof(vk);
  if (ihs_pv_vulkan_context(&vk) == IHS_PV_OK && vk.device != nullptr) {
    v->has_vk_ = true;
    v->vk_ = vk;
    auto gipa =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk.get_instance_proc_addr);
    auto props2 = gipa ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                             gipa(static_cast<VkInstance>(vk.instance),
                                  "vkGetPhysicalDeviceProperties2"))
                       : nullptr;
    if (props2 != nullptr && vk.api_version >= VK_API_VERSION_1_1) {
      VkPhysicalDeviceIDProperties ids{};
      ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
      VkPhysicalDeviceProperties2 p{};
      p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      p.pNext = &ids;
      props2(static_cast<VkPhysicalDevice>(vk.physical_device), &p);
      std::memcpy(v->shell_uuid_, ids.deviceUUID, 16);
    }
  }
  IhsEglContext egl{};
  egl.struct_size = sizeof(egl);
  if (ihs_pv_egl_context(&egl) == IHS_PV_OK && egl.egl_display != nullptr) {
    v->has_egl_ = true;
    v->egl_ = egl;
  }

  LOG_I("view %d: backend %s kinds 0x%x formats %zu vulkan %s egl %s, "
        "surface=%s",
        v->id_, v->backend_key_.c_str(), v->shell_kinds_,
        v->shell_formats_.size(), v->has_vk_ ? "yes" : "no",
        v->has_egl_ ? "yes" : "no", SurfacePrefName(v->params_.surface));
  return v;
}

GodotView::~GodotView() {
  // Teardown() normally ran on the engine thread already; this only catches a
  // view that never reached it (the engine never started).
  for (Slot& s : slots_) {
    if (s.shm_map != nullptr) {
      ::munmap(s.shm_map, s.shm_map_size);
    }
    gbm_.Free(std::move(s.dmabuf));
  }
}

void GodotView::RefreshCaps() {
  IhsPvCapabilities caps{};
  caps.struct_size = sizeof(caps);
  if (ihs_pv_query_capabilities(&caps) != IHS_PV_OK) {
    return;
  }
  std::lock_guard<std::mutex> lock(facts_mutex_);
  backend_key_ = caps.backend_key ? caps.backend_key : "";
  shell_kinds_ = caps.kinds;
  render_device_ = caps.render_device;
  shell_formats_.clear();
  for (size_t i = 0; i < caps.format_count; i++) {
    shell_formats_.push_back({caps.formats[i].fourcc, caps.formats[i].modifier});
  }
}

uint64_t GodotView::shell_render_device() const {
  std::lock_guard<std::mutex> lock(facts_mutex_);
  return render_device_;
}

bool GodotView::shell_device_uuid(uint8_t out[16]) const {
  std::lock_guard<std::mutex> lock(facts_mutex_);
  if (!has_vk_) {
    return false;
  }
  std::memcpy(out, shell_uuid_, 16);
  return true;
}

void GodotView::size(uint32_t& width, uint32_t& height) const {
  width = width_.load();
  height = height_.load();
}

// --- platform thread ------------------------------------------------------

void GodotView::OnResize(double width, double height) {
  // Already physical pixels (DPR-scaled by the shell).
  const auto w = static_cast<uint32_t>(width);
  const auto h = static_cast<uint32_t>(height);
  if (w < kMinSize || h < kMinSize) {
    return;
  }
  width_ = w;
  height_ = h;
  std::weak_ptr<GodotView> weak = weak_from_this();
  GodotRuntime::Get().Post([weak] {
    if (auto self = weak.lock()) {
      self->ApplyResize();
    }
  });
}

void GodotView::OnTouch(int32_t action,
                        int32_t point_count,
                        size_t size,
                        const double* data) {
  const IhsGodotApiV1* api = GodotRuntime::Get().api();
  if (api == nullptr || GodotRuntime::Get().active_view_id() != id_) {
    return;
  }
  for (const TouchEvent& e : DecodeAndroidTouch(action, point_count, size, data,
                                                params_.device_pixel_ratio)) {
    api->touch(e.index, e.phase, e.x, e.y, e.pressure);
  }
}

void GodotView::OnSuspended(bool suspended) {
  suspended_ = suspended;
  std::weak_ptr<GodotView> weak = weak_from_this();
  GodotRuntime::Get().Post([weak, suspended] {
    auto self = weak.lock();
    if (!self || self->api_ == nullptr) {
      return;
    }
    // Off screen: keep the game alive but stop drawing it.
    self->api_->set_visible(suspended ? 0 : 1);
  });
}

void GodotView::OnRenegotiate() {
  NegotiateOnPlatform();
}

void GodotView::Dispose() {
  {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    detached_ = true;
    pv_ = nullptr;
  }
  GodotRuntime::Get().Detach(id_);
}

void GodotView::OnPresented(uint32_t refresh_ns) {
  if (refresh_ns != 0) {
    refresh_ns_ = refresh_ns;
  }
}

void GodotView::NegotiateOnPlatform() {
  if (detached_) {
    return;
  }
  RefreshCaps();

  SurfaceFacts f;
  SurfacePref pref = params_.surface;
  {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    if (!godot_known_) {
      return;  // OnEngineReady posts again once it is.
    }
    f.shell_kinds = shell_kinds_;
    f.shell_formats = shell_formats_;
    f.godot_target_types = renderer_.target_types;
    f.godot_vulkan = renderer_.api == IHS_GODOT_API_VULKAN;
    f.godot_formats = godot_formats_;
    if (linear_only_) {
      auto& fs = f.godot_formats;
      fs.erase(std::remove_if(fs.begin(), fs.end(),
                              [](const FourccModifier& fm) {
                                return fm.modifier != 0;  // MOD_LINEAR
                              }),
               fs.end());
    }
    f.same_vk_device =
        has_vk_ && f.godot_vulkan &&
        std::memcmp(shell_uuid_, renderer_.device_uuid, 16) == 0;
    f.shell_vk_export = false;
    if (has_vk_) {
      for (size_t i = 0; i < vk_.device_extension_count; i++) {
        if (std::strcmp(vk_.device_extensions[i],
                        "VK_KHR_external_memory_fd") == 0) {
          f.shell_vk_export = vk_.api_version >= VK_API_VERSION_1_1;
        }
      }
    }
    f.shell_egl_import = has_egl_;
    if (force_shm_) {
      pref = SurfacePref::kShm;
    }
  }

  SurfacePlan plan = PlanSurface(f, pref);
  auto n = std::make_shared<Negotiated>();
  n->serial = ++negotiate_serial_;
  n->path = plan.path;
  n->width = width_.load();
  n->height = height_.load();

  if (plan.path == SurfacePath::kNone) {
    LOG_E("view %d: %s", id_, plan.reason);
    GodotRuntime::Get().Emit(id_, IHS_GODOT_VIEW_EVENT_ERROR, plan.reason);
    return;
  }

  if (plan.negotiate) {
    std::vector<IhsFormatModifier> formats;
    for (const FourccModifier& fm : plan.formats) {
      formats.push_back({fm.fourcc, 0, fm.modifier});
    }
    IhsPvRequirements req{};
    req.struct_size = sizeof(req);
    req.kinds = plan.kinds;
    req.formats = formats.data();
    req.format_count = formats.size();
    req.needs_alpha = 0;
    req.sync = IHS_PV_SYNC_EXPLICIT_PREFERRED;
    req.z_order = IHS_PV_Z_INLINE;
    req.preferred_kind = plan.preferred_kind;
    IhsPvGrant grant{};
    grant.struct_size = sizeof(grant);
    const int rc = ihs_pv_negotiate(pv_, &req, &grant);
    if (rc != IHS_PV_OK) {
      LOG_E("view %d: negotiate (%s) failed: %d", id_, plan.reason, rc);
      if (plan.path != SurfacePath::kShm) {
        {
          std::lock_guard<std::mutex> lock(facts_mutex_);
          force_shm_ = true;
        }
        NegotiateOnPlatform();
      } else {
        GodotRuntime::Get().Emit(id_, IHS_GODOT_VIEW_EVENT_ERROR,
                                 "negotiation failed");
      }
      return;
    }
    n->path = PathForGrant(grant.granted_kind);
    n->granted_kind = grant.granted_kind;
    n->format = {grant.format.fourcc, grant.format.modifier};

    // A dma-buf grant may come back with a modifier a plane can scan out
    // rather than one asked for (ivi-homescreen picks it from its own list);
    // Godot must be able to render it. Ask again for LINEAR only, which
    // planes scan out widely, then fall to the software floor.
    if (n->path == SurfacePath::kDmabuf &&
        std::find(f.godot_formats.begin(), f.godot_formats.end(), n->format) ==
            f.godot_formats.end()) {
      bool retry_linear = false;
      {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        retry_linear = !linear_only_;
        linear_only_ = true;
        force_shm_ = !retry_linear;
      }
      LOG_W("view %d: granted format %.4s/0x%" PRIx64
            " is not one Godot renders into; %s",
            id_, reinterpret_cast<const char*>(&n->format.fourcc),
            n->format.modifier,
            retry_linear ? "asking for LINEAR" : "using the software floor");
      NegotiateOnPlatform();
      return;
    }
    if (n->path == SurfacePath::kShm) {
      int fds[4] = {-1, -1, -1, -1};
      size_t stride = 0;
      const size_t count = ihs_pv_grant_shm_slots(pv_, fds, 4, &stride);
      // Borrowed until the grant is revoked; keep dups.
      for (size_t i = 0; i < count && i < 4; i++) {
        n->shm_fds.emplace_back(::dup(fds[i]));
      }
      n->shm_stride = static_cast<uint32_t>(stride);
      if (count == 0) {
        LOG_I("view %d: SOFTWARE_SHM granted without buffers yet (no size); "
              "waiting for resize",
              id_);
      }
    }
  } else if (plan.path == SurfacePath::kEglImage) {
    n->format = IntersectFormats(f).front();
  } else if (plan.path == SurfacePath::kVkImage) {
    n->format = {kABGR8888, 0};
  }

  LOG_I("view %d: surface %s (%s), format %.4s/0x%" PRIx64, id_,
        KindName(n->granted_kind, n->path), plan.reason,
        reinterpret_cast<const char*>(&n->format.fourcc), n->format.modifier);

  std::weak_ptr<GodotView> weak = weak_from_this();
  GodotRuntime::Get().Post([weak, n] {
    if (auto self = weak.lock()) {
      self->ApplyNegotiated(n);
    }
  });
}

// --- engine thread ----------------------------------------------------------

void GodotView::OnEngineReady(const IhsGodotApiV1* api) {
  api_ = api;
  IhsGodotRendererInfo info{};
  info.struct_size = sizeof(info);
  api->get_renderer_info(&info);

  std::vector<FourccModifier> formats;
  const size_t count = api->query_dmabuf_formats(nullptr, 0);
  if (count != 0) {
    std::vector<IhsGodotFormat> raw(count);
    const size_t got = api->query_dmabuf_formats(raw.data(), raw.size());
    for (size_t i = 0; i < got && i < raw.size(); i++) {
      formats.push_back({raw[i].fourcc, raw[i].modifier});
    }
  }
  LOG_I("view %d: Godot on %s, targets 0x%x, %zu dma-buf formats", id_,
        ApiName(info.api), info.target_types, formats.size());
  {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    renderer_ = info;
    godot_formats_ = std::move(formats);
    godot_known_ = true;
  }

  uint32_t w = 0, h = 0;
  size(w, h);
  if (w != 0 && h != 0) {
    api_->set_window_size(w, h, static_cast<float>(params_.device_pixel_ratio));
  }
  api_->set_visible(suspended_ ? 0 : 1);
  PostNegotiate(id_);
}

void GodotView::ApplyNegotiated(const std::shared_ptr<Negotiated>& n) {
  if (api_ == nullptr || n->serial <= applied_serial_) {
    return;
  }
  applied_serial_ = n->serial;
  ReleaseRing(/*retire=*/true);
  const bool first = !announced_;
  current_ = n;
  uint32_t w = 0, h = 0;
  size(w, h);
  if (n->path == SurfacePath::kShm) {
    // The host's buffers are the size they were granted at.
    w = n->width;
    h = n->height;
  }
  if (BuildRing(w, h)) {
    EmitSurface(*n, first);
  }
}

void GodotView::ApplyResize() {
  if (api_ == nullptr) {
    return;
  }
  uint32_t w = 0, h = 0;
  size(w, h);
  api_->set_window_size(w, h, static_cast<float>(params_.device_pixel_ratio));
  if (!current_ || current_->path == SurfacePath::kNone) {
    return;
  }
  if (current_->path == SurfacePath::kShm) {
    // A resize revokes the shm grant; the host follows with renegotiate.
    ReleaseRing(/*retire=*/false);
    return;
  }
  if (w == ring_width_ && h == ring_height_ && !slots_.empty()) {
    return;
  }
  ReleaseRing(/*retire=*/true);
  BuildRing(w, h);
}

bool GodotView::BuildRing(uint32_t width, uint32_t height) {
  if (!current_ || width < kMinSize || height < kMinSize) {
    return false;
  }
  const Negotiated& n = *current_;
  const auto count = static_cast<size_t>(params_.buffers);

  auto add = [&](Slot& slot, const IhsGodotTarget& target) {
    slot.target_id = GodotRuntime::Get().NewTargetId();
    const int rc = api_->add_target(slot.target_id, &target);
    if (rc != IHS_GODOT_OK) {
      LOG_E("view %d: Godot refused the %s buffer (%d)", id_,
            SurfacePathName(n.path), rc);
      slot.target_id = IHS_GODOT_NO_TARGET;
      return false;
    }
    return true;
  };

  switch (n.path) {
    case SurfacePath::kDmabuf:
    case SurfacePath::kEglImage: {
      if (!gbm_.Open(shell_render_device())) {
        FallBackToShm("no gbm to allocate dma-bufs");
        return false;
      }
      if (n.path == SurfacePath::kEglImage && !egl_importer_.ready() &&
          !egl_importer_.Init(egl_.egl_display)) {
        FallBackToShm("shell EGL display cannot import");
        return false;
      }
      const bool scanout = n.granted_kind == IHS_PV_KIND_DRM_PLANE;
      for (size_t i = 0; i < count; i++) {
        Slot slot;
        slot.buffer_id = next_buffer_id_++;
        slot.dmabuf = gbm_.Allocate(width, height, n.format.fourcc,
                                    n.format.modifier, scanout);
        if (!slot.dmabuf) {
          slots_.push_back(std::move(slot));
          FallBackToShm("dma-buf allocation failed");
          return false;
        }
        const DmabufBuffer& b = *slot.dmabuf;
        IhsGodotTarget t{};
        t.struct_size = sizeof(t);
        t.type = IHS_GODOT_TARGET_DMABUF;
        t.width = b.width;
        t.height = b.height;
        t.fourcc = b.fourcc;
        t.modifier = b.modifier;
        t.plane_count = b.plane_count;
        for (uint32_t p = 0; p < b.plane_count; p++) {
          t.plane_fd[p] = b.fd.get();
          t.plane_offset[p] = b.offsets[p];
          t.plane_stride[p] = b.strides[p];
        }
        t.memory_fd = -1;
        if (n.path == SurfacePath::kEglImage) {
          slot.egl_image = egl_importer_.Import(b);
          if (slot.egl_image == nullptr) {
            slots_.push_back(std::move(slot));
            FallBackToShm("shell EGLImage import failed");
            return false;
          }
        }
        const bool ok = add(slot, t);
        slots_.push_back(std::move(slot));
        if (!ok) {
          FallBackToShm("Godot cannot render into the dma-buf");
          return false;
        }
      }
      break;
    }

    case SurfacePath::kVkImage: {
      if (!vk_pool_) {
        vk_pool_ = VkImagePool::Create(vk_);
      }
      if (!vk_pool_) {
        FallBackToShm("shell VkDevice cannot export images");
        return false;
      }
      for (size_t i = 0; i < count; i++) {
        Slot slot;
        slot.buffer_id = next_buffer_id_++;
        slot.vk_index =
            vk_pool_->CreateImage(width, height, VK_FORMAT_R8G8B8A8_UNORM);
        const VkImagePool::Image* img = vk_pool_->Get(slot.vk_index);
        UniqueFd fd = vk_pool_->ExportFd(slot.vk_index);
        if (img == nullptr || !fd) {
          slots_.push_back(std::move(slot));
          FallBackToShm("shell image export failed");
          return false;
        }
        IhsGodotTarget t{};
        t.struct_size = sizeof(t);
        t.type = IHS_GODOT_TARGET_VK_OPAQUE_FD;
        t.width = width;
        t.height = height;
        t.fourcc = kABGR8888;
        t.memory_fd = fd.get();  // borrowed by the bridge; ours closes below
        t.allocation_size = img->size;
        t.dedicated = img->dedicated ? 1 : 0;
        std::memcpy(t.device_uuid, vk_pool_->device_uuid(), 16);
        for (int& pfd : t.plane_fd) {
          pfd = -1;
        }
        const bool ok = add(slot, t);
        slots_.push_back(std::move(slot));
        if (!ok) {
          FallBackToShm("Godot cannot import the shell's image memory");
          return false;
        }
      }
      break;
    }

    case SurfacePath::kShm: {
      if (n.shm_fds.empty()) {
        return false;  // granted before the view had a size
      }
      for (size_t i = 0; i < n.shm_fds.size(); i++) {
        Slot slot;
        slot.buffer_id = static_cast<uint32_t>(i);  // must be its index
        slot.shm_fd = n.shm_fds[i].Dup();
        slot.shm_stride = n.shm_stride;
        slot.shm_map_size = size_t(n.shm_stride) * (height + 1);
        void* map = slot.shm_fd ? ::mmap(nullptr, slot.shm_map_size,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         slot.shm_fd.get(), 0)
                                : MAP_FAILED;
        if (map == MAP_FAILED) {
          LOG_E("view %d: mmap of SOFTWARE_SHM buffer %zu failed: %s", id_, i,
                std::strerror(errno));
          slots_.push_back(std::move(slot));
          ReleaseRing(false);
          return false;
        }
        slot.shm_map = map;
        IhsGodotTarget t{};
        t.struct_size = sizeof(t);
        t.type = IHS_GODOT_TARGET_CPU;
        t.width = width;
        t.height = height;
        t.fourcc = n.format.fourcc;
        t.pixels = map;
        t.cpu_stride = n.shm_stride;
        t.memory_fd = -1;
        for (int& pfd : t.plane_fd) {
          pfd = -1;
        }
        const bool ok = add(slot, t);
        slots_.push_back(std::move(slot));
        if (!ok) {
          ReleaseRing(false);
          GodotRuntime::Get().Emit(id_, IHS_GODOT_VIEW_EVENT_ERROR,
                                   "Godot cannot read back into CPU memory");
          return false;
        }
      }
      break;
    }

    case SurfacePath::kNone:
      return false;
  }

  ring_width_ = width;
  ring_height_ = height;
  next_slot_ = 0;
  LOG_I("view %d: %zu %s buffers %ux%u", id_, slots_.size(),
        SurfacePathName(n.path), width, height);
  return true;
}

void GodotView::ReleaseRing(bool retire) {
  for (Slot& s : slots_) {
    if (s.target_id != IHS_GODOT_NO_TARGET && api_ != nullptr) {
      api_->remove_target(s.target_id);
    }
    if (retire && s.submitted && current_ &&
        current_->path != SurfacePath::kShm) {
      // The import cache never forgets an id on its own.
      std::lock_guard<std::mutex> lock(submit_mutex_);
      if (!detached_ && pv_ != nullptr && ihs_pv_retire_buffer != nullptr) {
        ihs_pv_retire_buffer(pv_, s.buffer_id);
      }
    }
    if (s.egl_image != nullptr) {
      egl_importer_.Destroy(s.egl_image);
      s.egl_image = nullptr;
    }
    gbm_.Free(std::move(s.dmabuf));
    if (s.vk_index >= 0 && vk_pool_) {
      vk_pool_->Drop(s.vk_index);
    }
    if (s.shm_map != nullptr) {
      ::munmap(s.shm_map, s.shm_map_size);
      s.shm_map = nullptr;
    }
  }
  slots_.clear();
  active_slot_ = -1;
  next_slot_ = 0;
  ring_width_ = ring_height_ = 0;
}

uint32_t GodotView::BeginFrame() {
  active_slot_ = -1;
  if (slots_.empty() || suspended_ || detached_) {
    return IHS_GODOT_NO_TARGET;
  }
  Slot& s = slots_[next_slot_];
  if (s.submitted && !WaitFence(s.release_fence, kReleaseWaitMs)) {
    // Draw over it anyway: a torn frame beats a frozen game.
    if (fence_timeouts_++ % 60 == 0) {
      LOG_W("view %d: release fence timed out (%" PRIu64 " so far)", id_,
            fence_timeouts_);
    }
  }
  if (s.shm_map != nullptr) {
    DmabufSyncBegin(s.shm_fd.get(), true);
  }
  active_slot_ = static_cast<int>(next_slot_);
  return s.target_id;
}

void GodotView::EndFrame(const IhsGodotFrameInfo& info) {
  UniqueFd acquire(info.acquire_fence_fd);
  if (active_slot_ < 0) {
    return;
  }
  Slot& s = slots_[static_cast<size_t>(active_slot_)];
  active_slot_ = -1;
  if (s.shm_map != nullptr) {
    DmabufSyncEnd(s.shm_fd.get(), true);
  }
  if (info.target_id != s.target_id) {
    return;  // Godot did not draw this iteration (hidden, loading).
  }
  if (Submit(s, acquire.release())) {
    s.submitted = true;
    next_slot_ = (next_slot_ + 1) % slots_.size();
  }
}

bool GodotView::Submit(Slot& slot, int acquire_fence_fd) {
  UniqueFd acquire(acquire_fence_fd);
  std::lock_guard<std::mutex> lock(submit_mutex_);
  if (detached_ || pv_ == nullptr || !current_) {
    return false;
  }
  int release = -1;
  int rc = IHS_PV_ERR_INVALID;

  switch (current_->path) {
    case SurfacePath::kDmabuf:
    case SurfacePath::kShm: {
      IhsFrame f{};
      f.struct_size = sizeof(f);
      f.color_space = IHS_COLOR_SPACE_DEFAULT;
      f.color_range = IHS_COLOR_RANGE_DEFAULT;
      f.hdr = nullptr;
      f.buffer_id = slot.buffer_id;
      if (slot.dmabuf) {
        const DmabufBuffer& b = *slot.dmabuf;
        f.format = {b.fourcc, 0, b.modifier};
        f.width = b.width;
        f.height = b.height;
        f.plane_count = b.plane_count;
        for (uint32_t p = 0; p < b.plane_count; p++) {
          // The registry consumes these on every path: hand it dups and keep
          // the ring's own fd for the life of the slot.
          f.plane_fd[p] = ::dup(b.fd.get());
          f.plane_offset[p] = b.offsets[p];
          f.plane_stride[p] = b.strides[p];
          if (f.plane_fd[p] < 0) {
            for (uint32_t q = 0; q < p; q++) {
              ::close(f.plane_fd[q]);
            }
            return false;
          }
        }
      } else {
        f.format = {current_->format.fourcc, 0, 0};
        f.width = ring_width_;
        f.height = ring_height_;
        f.plane_count = 1;
        f.plane_fd[0] = ::dup(slot.shm_fd.get());
        f.plane_offset[0] = 0;
        f.plane_stride[0] = slot.shm_stride;
        if (f.plane_fd[0] < 0) {
          return false;
        }
        acquire.reset();  // CPU writes are done; shm takes no fence.
      }
      rc = ihs_pv_submit(pv_, &f, acquire.release(), &release);
      break;
    }

    case SurfacePath::kVkImage:
    case SurfacePath::kEglImage: {
      if (ihs_pv_submit_layers == nullptr) {
        LOG_E("view %d: shell has no ihs_pv_submit_layers", id_);
        return false;
      }
      IhsLayer layer{};
      layer.struct_size = sizeof(layer);
      layer.layer_id = 0;
      layer.transform = IHS_TRANSFORM_NORMAL;
      layer.opaque = 1;
      layer.content_type = IHS_CONTENT_TYPE_GAME;
      IhsVkImage vk_image{};
      IhsImage egl_image{};
      if (current_->path == SurfacePath::kVkImage) {
        vk_pool_->Describe(slot.vk_index, slot.buffer_id, vk_image);
        layer.vk_image = &vk_image;
      } else {
        egl_image.struct_size = sizeof(egl_image);
        egl_image.egl_image = slot.egl_image;
        egl_image.width = ring_width_;
        egl_image.height = ring_height_;
        egl_image.buffer_id = slot.buffer_id;
        egl_image.external_oes = 0;
        layer.image = &egl_image;
      }
      layer.acquire_fence_fd = acquire.release();
      int fences[1] = {-1};
      rc = ihs_pv_submit_layers(pv_, &layer, 1, ++seq_, fences);
      release = fences[0];
      break;
    }

    case SurfacePath::kNone:
      return false;
  }

  if (rc != IHS_PV_OK) {
    // Consumed either way: never close what was handed over.
    if (submit_errors_++ % 60 == 0) {
      LOG_W("view %d: submit failed: %d", id_, rc);
    }
    if (submit_errors_ == 120) {
      // A grant that keeps failing is dead; nothing else would revive it.
      ihs_pv_request_renegotiate(pv_);
    }
    return false;
  }
  submit_errors_ = 0;
  slot.release_fence.reset(release);
  return true;
}

void GodotView::FallBackToShm(const char* why) {
  LOG_W("view %d: %s; falling back to the software floor", id_, why);
  ReleaseRing(/*retire=*/true);
  {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    if (force_shm_) {
      GodotRuntime::Get().Emit(id_, IHS_GODOT_VIEW_EVENT_ERROR, why);
      return;  // Already on the floor; nowhere left to go.
    }
    force_shm_ = true;
  }
  PostNegotiate(id_);
}

void GodotView::EmitSurface(const Negotiated& n, bool first) {
  std::string backend;
  uint32_t api = 0;
  {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    backend = backend_key_;
    api = renderer_.api;
  }
  char json[256];
  std::snprintf(json, sizeof(json),
                "{\"kind\":\"%s\",\"api\":\"%s\",\"backend\":\"%s\"}",
                KindName(n.granted_kind, n.path), ApiName(api),
                backend.c_str());
  GodotRuntime::Get().Emit(
      id_, first ? IHS_GODOT_VIEW_EVENT_READY : IHS_GODOT_VIEW_EVENT_SURFACE,
      json);
  announced_ = true;
}

void GodotView::Teardown() {
  ReleaseRing(/*retire=*/false);
  vk_pool_.reset();
  current_.reset();
  api_ = nullptr;
}

}  // namespace ihs_godot
