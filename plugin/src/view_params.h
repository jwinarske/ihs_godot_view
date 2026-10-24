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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ihs_godot {

// Which surface path a view should take. kAuto picks the best one the shell
// and the renderer can both do (see surface_policy.h).
enum class SurfacePref { kAuto, kDmabuf, kPlane, kVkImage, kEglImage, kShm };

const char* SurfacePrefName(SurfacePref pref);
bool ParseSurfacePref(const std::string& text, SurfacePref& out);

// What a GodotView was created with: the platform view's creationParams (a
// StandardMessageCodec map), then IHS_GODOT_* environment overrides.
//
//   main_pack           .pck to run (--main-pack); relative to the bundle's
//                       flutter_assets when not absolute
//   path                project directory (--path), same resolution
//   args                extra Godot command-line arguments
//   rendering_driver    vulkan | opengl3 | opengl3_es; empty: the project's
//   rendering_method    forward_plus | mobile | gl_compatibility; empty: the
//                       project's
//   surface             auto | dmabuf | plane | vk_image | egl_image | shm
//   device_pixel_ratio  logical -> physical scale of pointer coordinates
//   libgodot            path to libgodot.so
//   max_fps             0 = follow the display
//   buffers             ring depth for producer-allocated buffers (2..4)
struct ViewParams {
  std::string main_pack;
  std::string project_path;
  std::vector<std::string> extra_args;
  std::string rendering_driver;
  std::string rendering_method;
  SurfacePref surface = SurfacePref::kAuto;
  double device_pixel_ratio = 1.0;
  std::string libgodot;
  int max_fps = 0;
  int buffers = 3;
  // Log how long frames take to reach the screen (FrameLatency), a report
  // every latency_window frames shown; 0 is off.
  int latency_window = 0;
};

// @assets_path may be null. Unknown keys are ignored; malformed params
// yield defaults (and a log line).
ViewParams ParseViewParams(const uint8_t* data,
                           size_t size,
                           const char* assets_path);

// IHS_GODOT_MAIN_PACK, IHS_GODOT_PATH, IHS_GODOT_ARGS (space separated),
// IHS_GODOT_RENDERING_DRIVER, IHS_GODOT_RENDERING_METHOD, IHS_GODOT_SURFACE,
// IHS_GODOT_LIBRARY, IHS_GODOT_MAX_FPS, IHS_GODOT_VIEW_LATENCY (frames per
// report, 2 to 100000; any other value but 0 means 300). A variable that is set
// wins.
void ApplyEnvironment(ViewParams& params);

// argv for libgodot, argv[0] included.
std::vector<std::string> BuildGodotArgs(const ViewParams& params);

}  // namespace ihs_godot
