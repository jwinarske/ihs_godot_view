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
#include <vector>

#include "view_params.h"

namespace ihs_godot {

// How the frames of one view travel. Decided from what the shell offers, what
// Godot's renderer can render into, and the view's preference.
enum class SurfacePath {
  kNone,
  kDmabuf,    // TEXTURE_DMABUF_IMPORT or DRM_PLANE grant, producer gbm ring
  kVkImage,   // TEXTURE_VK_IMAGE layers, images on the shell's VkDevice
  kEglImage,  // TEXTURE_EGL_IMAGE layers over a producer gbm ring
  kShm,       // SOFTWARE_SHM grant, host buffers, CPU readback
};

const char* SurfacePathName(SurfacePath path);

struct FourccModifier {
  uint32_t fourcc = 0;
  uint64_t modifier = 0;
  bool operator==(const FourccModifier& o) const {
    return fourcc == o.fourcc && modifier == o.modifier;
  }
};

// The facts the decision is made from, gathered on the platform thread
// (shell) and the engine thread (Godot).
struct SurfaceFacts {
  uint32_t shell_kinds = 0;  // IhsPvCapabilities::kinds
  std::vector<FourccModifier> shell_formats;
  // Godot renderer (IhsGodotRendererInfo)
  uint32_t godot_target_types = 0;  // 1 << IhsGodotTargetType
  bool godot_vulkan = false;
  std::vector<FourccModifier> godot_formats;  // DMABUF render formats
  // Same physical device on both sides (deviceUUID match).
  bool same_vk_device = false;
  // The shell's VkDevice can export OPAQUE_FD memory.
  bool shell_vk_export = false;
  // The shell's EGL display can import dma-bufs.
  bool shell_egl_import = false;
  bool needs_alpha = false;
};

// What to ask ihs_pv_negotiate for, or that negotiation is skipped (the image
// kinds are not negotiated: any view may submit them where offered).
struct SurfacePlan {
  SurfacePath path = SurfacePath::kNone;
  bool negotiate = false;
  uint32_t kinds = 0;           // IhsPvRequirements::kinds
  uint8_t preferred_kind = 0;   // IhsPvRequirements::preferred_kind
  std::vector<FourccModifier> formats;  // IhsPvRequirements::formats
  const char* reason = "";
};

// The dma-buf formats both sides can do, in the shell's order of preference
// (it knows which of its modifiers sample well; LINEAR imports but is not
// sampleable on some GPUs). Opaque X formats are kept ahead of their alpha
// twins unless @needs_alpha.
std::vector<FourccModifier> IntersectFormats(const SurfaceFacts& facts);

SurfacePlan PlanSurface(const SurfaceFacts& facts, SurfacePref pref);

// The path a negotiated grant puts the view on.
SurfacePath PathForGrant(uint32_t granted_kind);

}  // namespace ihs_godot
