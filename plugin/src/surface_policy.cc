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

#include "surface_policy.h"

#include <algorithm>

#include <ihs/platform_view.h>
#include <ihs_godot/bridge.h>

#include "log.h"

namespace ihs_godot {

namespace {

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) |
         (uint32_t(d) << 24);
}

constexpr uint32_t kARGB8888 = Fourcc('A', 'R', '2', '4');
constexpr uint32_t kXRGB8888 = Fourcc('X', 'R', '2', '4');
constexpr uint32_t kABGR8888 = Fourcc('A', 'B', '2', '4');
constexpr uint32_t kXBGR8888 = Fourcc('X', 'B', '2', '4');

bool IsRgb32(uint32_t f) {
  return f == kARGB8888 || f == kXRGB8888 || f == kABGR8888 || f == kXBGR8888;
}

bool HasAlpha(uint32_t f) {
  return f == kARGB8888 || f == kABGR8888;
}

bool Has(uint32_t bits, uint32_t kind) {
  return (bits & kind) != 0;
}

bool GodotCan(const SurfaceFacts& f, uint32_t target_type) {
  return (f.godot_target_types & (1u << target_type)) != 0;
}

}  // namespace

const char* SurfacePathName(SurfacePath path) {
  switch (path) {
    case SurfacePath::kNone:
      return "none";
    case SurfacePath::kDmabuf:
      return "dmabuf";
    case SurfacePath::kVkImage:
      return "vk_image";
    case SurfacePath::kEglImage:
      return "egl_image";
    case SurfacePath::kShm:
      return "shm";
  }
  return "none";
}

std::vector<FourccModifier> IntersectFormats(const SurfaceFacts& facts) {
  std::vector<FourccModifier> out;
  for (const FourccModifier& s : facts.shell_formats) {
    if (!IsRgb32(s.fourcc)) {
      continue;
    }
    if (std::find(facts.godot_formats.begin(), facts.godot_formats.end(), s) !=
        facts.godot_formats.end()) {
      out.push_back(s);
    }
  }
  if (!facts.needs_alpha) {
    // Opaque first, keeping the shell's order within each group.
    std::stable_partition(out.begin(), out.end(), [](const FourccModifier& f) {
      return !HasAlpha(f.fourcc);
    });
  }
  return out;
}

SurfacePlan PlanSurface(const SurfaceFacts& f, SurfacePref pref) {
  const std::vector<FourccModifier> dmabuf_formats = IntersectFormats(f);
  const bool dmabuf_ok = Has(f.shell_kinds, IHS_PV_KIND_TEXTURE_DMABUF_IMPORT) &&
                         GodotCan(f, IHS_GODOT_TARGET_DMABUF) &&
                         !dmabuf_formats.empty();
  const bool plane_ok = Has(f.shell_kinds, IHS_PV_KIND_DRM_PLANE) &&
                        GodotCan(f, IHS_GODOT_TARGET_DMABUF) &&
                        !dmabuf_formats.empty();
  const bool vk_ok = Has(f.shell_kinds, IHS_PV_KIND_TEXTURE_VK_IMAGE) &&
                     GodotCan(f, IHS_GODOT_TARGET_VK_OPAQUE_FD) &&
                     f.godot_vulkan && f.same_vk_device && f.shell_vk_export;
  const bool egl_ok = Has(f.shell_kinds, IHS_PV_KIND_TEXTURE_EGL_IMAGE) &&
                      GodotCan(f, IHS_GODOT_TARGET_DMABUF) &&
                      f.shell_egl_import && !dmabuf_formats.empty();
  const bool shm_ok = Has(f.shell_kinds, IHS_PV_KIND_SOFTWARE_SHM) &&
                      GodotCan(f, IHS_GODOT_TARGET_CPU);

  SurfacePlan plan;
  auto dmabuf = [&](bool plane) {
    plan.path = SurfacePath::kDmabuf;
    plan.negotiate = true;
    plan.kinds = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_SOFTWARE_SHM;
    plan.formats = dmabuf_formats;
    if (plane) {
      // A preference, not a demand: import and shm stay in the mask so a
      // frame with no plane free still composites (ivi-homescreen#673).
      plan.kinds |= IHS_PV_KIND_DRM_PLANE;
      plan.preferred_kind = IHS_PV_KIND_DRM_PLANE;
    }
  };
  auto shm = [&]() {
    plan.path = SurfacePath::kShm;
    plan.negotiate = true;
    plan.kinds = IHS_PV_KIND_SOFTWARE_SHM;
    plan.preferred_kind = IHS_PV_KIND_SOFTWARE_SHM;
    // Linear CPU layouts the readback can write; the host picks.
    plan.formats = {{kXRGB8888, 0}, {kARGB8888, 0}, {kXBGR8888, 0},
                    {kABGR8888, 0}};
    if (!f.needs_alpha) {
      plan.formats = {{kXRGB8888, 0}, {kXBGR8888, 0}, {kARGB8888, 0},
                      {kABGR8888, 0}};
    }
  };

  switch (pref) {
    case SurfacePref::kPlane:
      if (plane_ok) {
        dmabuf(true);
        plan.reason = "plane requested";
        return plan;
      }
      LOG_W("surface 'plane' unavailable (shell kinds 0x%x); choosing "
            "automatically",
            f.shell_kinds);
      break;
    case SurfacePref::kDmabuf:
      if (dmabuf_ok) {
        dmabuf(false);
        plan.reason = "dmabuf requested";
        return plan;
      }
      LOG_W("surface 'dmabuf' unavailable; choosing automatically");
      break;
    case SurfacePref::kVkImage:
      if (vk_ok) {
        plan.path = SurfacePath::kVkImage;
        plan.reason = "vk_image requested";
        return plan;
      }
      LOG_W("surface 'vk_image' unavailable (needs a Vulkan shell, Godot on "
            "Vulkan on the same GPU, and OPAQUE_FD export); choosing "
            "automatically");
      break;
    case SurfacePref::kEglImage:
      if (egl_ok) {
        plan.path = SurfacePath::kEglImage;
        plan.reason = "egl_image requested";
        return plan;
      }
      LOG_W("surface 'egl_image' unavailable; choosing automatically");
      break;
    case SurfacePref::kShm:
      if (shm_ok) {
        shm();
        plan.reason = "shm requested";
        return plan;
      }
      LOG_W("surface 'shm' unavailable; choosing automatically");
      break;
    case SurfacePref::kAuto:
      break;
  }

  // Automatic: the certain zero-copy path first, then the image kinds that
  // need no dma-buf export, then the floor.
  if (dmabuf_ok) {
    dmabuf(false);
    plan.reason = "dma-buf import";
  } else if (vk_ok) {
    plan.path = SurfacePath::kVkImage;
    plan.reason = "no dma-buf path; VkImage on the shell's device";
  } else if (egl_ok) {
    plan.path = SurfacePath::kEglImage;
    plan.reason = "EGLImage on the shell's display";
  } else if (shm_ok) {
    shm();
    plan.reason = "software floor";
  } else {
    plan.path = SurfacePath::kNone;
    plan.reason = "no surface path both sides support";
  }
  return plan;
}

SurfacePath PathForGrant(uint32_t granted_kind) {
  switch (granted_kind) {
    case IHS_PV_KIND_TEXTURE_DMABUF_IMPORT:
    case IHS_PV_KIND_DRM_PLANE:
      return SurfacePath::kDmabuf;
    case IHS_PV_KIND_SOFTWARE_SHM:
      return SurfacePath::kShm;
    default:
      return SurfacePath::kNone;
  }
}

}  // namespace ihs_godot
