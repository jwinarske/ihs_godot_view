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

#include "gbm_allocator.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "log.h"

namespace ihs_godot {

namespace {

constexpr uint32_t kGbmBoUseScanout = 1u << 0;
constexpr uint32_t kGbmBoUseRendering = 1u << 2;
constexpr uint32_t kGbmBoUseLinear = 1u << 4;
constexpr uint64_t kModLinear = 0;
constexpr uint64_t kModInvalid = 0x00ffffffffffffffULL;

}  // namespace

struct GbmAllocator::Api {
  gbm_device* (*create_device)(int fd);
  void (*device_destroy)(gbm_device*);
  gbm_bo* (*bo_create)(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t);
  gbm_bo* (*bo_create_with_modifiers)(gbm_device*,
                                      uint32_t,
                                      uint32_t,
                                      uint32_t,
                                      const uint64_t*,
                                      unsigned int);
  gbm_bo* (*bo_create_with_modifiers2)(gbm_device*,
                                       uint32_t,
                                       uint32_t,
                                       uint32_t,
                                       const uint64_t*,
                                       unsigned int,
                                       uint32_t);
  int (*bo_get_fd)(gbm_bo*);
  uint64_t (*bo_get_modifier)(gbm_bo*);
  int (*bo_get_plane_count)(gbm_bo*);
  uint32_t (*bo_get_stride_for_plane)(gbm_bo*, int);
  uint32_t (*bo_get_offset)(gbm_bo*, int);
  void (*bo_destroy)(gbm_bo*);
};

const GbmAllocator::Api* GbmAllocator::LoadApi() {
  static const Api* api = []() -> const Api* {
    void* lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
      lib = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (lib == nullptr) {
      LOG_W("libgbm not found; producer-allocated dma-buf paths unavailable");
      return nullptr;
    }
    static Api a{};
#define LOAD(field, name) \
  a.field = reinterpret_cast<decltype(a.field)>(dlsym(lib, name))
    LOAD(create_device, "gbm_create_device");
    LOAD(device_destroy, "gbm_device_destroy");
    LOAD(bo_create, "gbm_bo_create");
    LOAD(bo_create_with_modifiers, "gbm_bo_create_with_modifiers");
    LOAD(bo_create_with_modifiers2, "gbm_bo_create_with_modifiers2");
    LOAD(bo_get_fd, "gbm_bo_get_fd");
    LOAD(bo_get_modifier, "gbm_bo_get_modifier");
    LOAD(bo_get_plane_count, "gbm_bo_get_plane_count");
    LOAD(bo_get_stride_for_plane, "gbm_bo_get_stride_for_plane");
    LOAD(bo_get_offset, "gbm_bo_get_offset");
    LOAD(bo_destroy, "gbm_bo_destroy");
#undef LOAD
    if (!a.create_device || !a.device_destroy || !a.bo_create ||
        !a.bo_get_fd || !a.bo_get_stride_for_plane || !a.bo_destroy) {
      LOG_W("libgbm is missing required entry points");
      return nullptr;
    }
    return &a;
  }();
  return api;
}

std::string FindRenderNode(uint64_t dev) {
  DIR* dir = opendir("/dev/dri");
  if (dir == nullptr) {
    return {};
  }
  std::vector<std::string> nodes;
  while (dirent* e = readdir(dir)) {
    if (std::strncmp(e->d_name, "renderD", 7) == 0) {
      nodes.push_back(std::string("/dev/dri/") + e->d_name);
    }
  }
  closedir(dir);
  std::sort(nodes.begin(), nodes.end());
  for (const std::string& n : nodes) {
    struct stat st{};
    if (stat(n.c_str(), &st) == 0 && (dev == 0 || st.st_rdev == dev)) {
      return n;
    }
  }
  return {};
}

GbmAllocator::~GbmAllocator() {
  if (device_ != nullptr && api_ != nullptr) {
    api_->device_destroy(device_);
  }
}

bool GbmAllocator::Open(uint64_t render_device) {
  if (device_ != nullptr) {
    return true;
  }
  api_ = LoadApi();
  if (api_ == nullptr) {
    return false;
  }
  // A render node, never the card node: opening the card while the shell
  // owns it is how a producer takes the display away from the compositor.
  node_ = FindRenderNode(render_device);
  if (node_.empty() && render_device != 0) {
    LOG_W("no render node matches the shell's device; using the first");
    node_ = FindRenderNode(0);
  }
  if (node_.empty()) {
    LOG_W("no DRM render node");
    return false;
  }
  drm_fd_.reset(::open(node_.c_str(), O_RDWR | O_CLOEXEC));
  if (!drm_fd_) {
    LOG_W("cannot open %s", node_.c_str());
    return false;
  }
  device_ = api_->create_device(drm_fd_.get());
  if (device_ == nullptr) {
    LOG_W("gbm_create_device(%s) failed", node_.c_str());
    drm_fd_.reset();
    return false;
  }
  LOG_I("allocating dma-bufs on %s", node_.c_str());
  return true;
}

std::unique_ptr<DmabufBuffer> GbmAllocator::Allocate(uint32_t width,
                                                     uint32_t height,
                                                     uint32_t fourcc,
                                                     uint64_t modifier,
                                                     bool scanout) {
  if (device_ == nullptr || width == 0 || height == 0) {
    return nullptr;
  }
  const bool linear = modifier == kModLinear;
  constexpr uint32_t kTileRows = 64;
  const uint32_t padded_height =
      linear ? height : ((height + kTileRows - 1) / kTileRows) * kTileRows +
                            kTileRows;
  const uint32_t base_usage =
      kGbmBoUseRendering | (scanout ? kGbmBoUseScanout : 0u);

  auto create = [&](uint32_t alloc_height) -> gbm_bo* {
    gbm_bo* bo = nullptr;
    if (modifier != kModInvalid) {
      // Which usage a driver accepts alongside a modifier varies, so try the
      // narrow one before the broad one.
      if (api_->bo_create_with_modifiers2 != nullptr) {
        for (const uint32_t usage : {base_usage, kGbmBoUseRendering, 0u}) {
          bo = api_->bo_create_with_modifiers2(device_, width, alloc_height,
                                               fourcc, &modifier, 1, usage);
          if (bo != nullptr) {
            break;
          }
        }
      }
      if (bo == nullptr && api_->bo_create_with_modifiers != nullptr) {
        bo = api_->bo_create_with_modifiers(device_, width, alloc_height,
                                            fourcc, &modifier, 1);
      }
      if (bo == nullptr && linear) {
        bo = api_->bo_create(device_, width, alloc_height, fourcc,
                             base_usage | kGbmBoUseLinear);
      }
    } else {
      bo = api_->bo_create(device_, width, alloc_height, fourcc, base_usage);
    }
    return bo;
  };

  gbm_bo* bo = create(padded_height);
  // A modifier with metadata planes (AMD DCC, ...) places them after the
  // padded rows, and the importer, which lays them out for @height, refuses
  // the offsets. Those layouts are self-describing, so allocate them exact.
  if (bo != nullptr && padded_height != height &&
      api_->bo_get_plane_count != nullptr && api_->bo_get_plane_count(bo) > 1) {
    api_->bo_destroy(bo);
    bo = create(height);
  }
  if (bo == nullptr) {
    LOG_E("gbm allocation %ux%u fourcc %.4s modifier 0x%llx failed", width,
          height, reinterpret_cast<const char*>(&fourcc),
          static_cast<unsigned long long>(modifier));
    return nullptr;
  }

  auto buf = std::make_unique<DmabufBuffer>();
  buf->bo = bo;
  buf->width = width;
  buf->height = height;
  buf->fourcc = fourcc;
  buf->modifier =
      api_->bo_get_modifier != nullptr ? api_->bo_get_modifier(bo) : modifier;
  if (buf->modifier == kModInvalid) {
    buf->modifier = modifier;
  }
  buf->plane_count = api_->bo_get_plane_count != nullptr
                         ? static_cast<uint32_t>(api_->bo_get_plane_count(bo))
                         : 1u;
  buf->plane_count = std::clamp(buf->plane_count, 1u, 4u);
  for (uint32_t i = 0; i < buf->plane_count; i++) {
    buf->strides[i] = api_->bo_get_stride_for_plane(bo, static_cast<int>(i));
    buf->offsets[i] = api_->bo_get_offset != nullptr
                          ? api_->bo_get_offset(bo, static_cast<int>(i))
                          : 0;
  }
  buf->fd.reset(api_->bo_get_fd(bo));
  if (!buf->fd) {
    LOG_E("gbm_bo_get_fd failed");
    api_->bo_destroy(bo);
    return nullptr;
  }
  // Telling the shell LINEAR for a buffer the driver laid out tiled is a lie
  // it cannot detect; the import just fails.
  if (modifier != kModInvalid && buf->modifier != modifier) {
    LOG_E("allocator chose modifier 0x%llx, not the granted 0x%llx",
          static_cast<unsigned long long>(buf->modifier),
          static_cast<unsigned long long>(modifier));
    Free(std::move(buf));
    return nullptr;
  }
  return buf;
}

void GbmAllocator::Free(std::unique_ptr<DmabufBuffer> buffer) {
  if (!buffer) {
    return;
  }
  buffer->fd.reset();
  if (buffer->bo != nullptr && api_ != nullptr) {
    api_->bo_destroy(buffer->bo);
  }
}

}  // namespace ihs_godot
