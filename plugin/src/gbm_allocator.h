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

#include <cstdint>
#include <memory>
#include <string>

#include "unique_fd.h"

struct gbm_device;
struct gbm_bo;

namespace ihs_godot {

// One dma-buf from gbm: what a DMABUF target and an IhsFrame both need.
struct DmabufBuffer {
  gbm_bo* bo = nullptr;
  UniqueFd fd;  // plane 0's buffer; every plane lives in it
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fourcc = 0;
  uint64_t modifier = 0;
  uint32_t plane_count = 1;
  uint32_t offsets[4] = {};
  uint32_t strides[4] = {};
};

// gbm, loaded at run time so the plugin still loads (and the SOFTWARE_SHM
// floor still works) on a system without libgbm.
class GbmAllocator {
 public:
  ~GbmAllocator();

  // Open the render node whose dev_t is @render_device (the shell's import
  // GPU), or the first render node when 0. False without libgbm or a node.
  bool Open(uint64_t render_device);
  [[nodiscard]] bool is_open() const { return device_ != nullptr; }
  [[nodiscard]] const std::string& node() const { return node_; }

  // Allocate exactly @modifier (the granted one; the import fails on any
  // other). @scanout adds GBM_BO_USE_SCANOUT for a plane grant. A tiled
  // modifier gets a tile of extra rows: the importer computes padded sizes a
  // producer cannot, and refuses a buffer that is short (ivi-homescreen#598).
  std::unique_ptr<DmabufBuffer> Allocate(uint32_t width,
                                         uint32_t height,
                                         uint32_t fourcc,
                                         uint64_t modifier,
                                         bool scanout);
  void Free(std::unique_ptr<DmabufBuffer> buffer);

 private:
  struct Api;
  static const Api* LoadApi();

  const Api* api_ = nullptr;
  UniqueFd drm_fd_;
  gbm_device* device_ = nullptr;
  std::string node_;
};

// The /dev/dri/renderD* node with @dev (or the first when 0); empty if none.
std::string FindRenderNode(uint64_t dev);

}  // namespace ihs_godot
