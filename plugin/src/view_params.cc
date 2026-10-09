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

#include "view_params.h"

#include <cstdlib>
#include <sstream>

#include "codec.h"
#include "log.h"

namespace ihs_godot {

namespace {

std::string Resolve(const std::string& path, const char* assets_path) {
  if (path.empty() || path[0] == '/' || assets_path == nullptr ||
      assets_path[0] == '\0') {
    return path;
  }
  std::string base(assets_path);
  if (base.back() != '/') {
    base.push_back('/');
  }
  return base + path;
}

const char* Env(const char* name) {
  const char* v = std::getenv(name);
  return (v != nullptr && v[0] != '\0') ? v : nullptr;
}

}  // namespace

const char* SurfacePrefName(SurfacePref pref) {
  switch (pref) {
    case SurfacePref::kAuto:
      return "auto";
    case SurfacePref::kDmabuf:
      return "dmabuf";
    case SurfacePref::kPlane:
      return "plane";
    case SurfacePref::kVkImage:
      return "vk_image";
    case SurfacePref::kEglImage:
      return "egl_image";
    case SurfacePref::kShm:
      return "shm";
  }
  return "auto";
}

bool ParseSurfacePref(const std::string& text, SurfacePref& out) {
  static const struct {
    const char* name;
    SurfacePref pref;
  } kNames[] = {
      {"auto", SurfacePref::kAuto},         {"dmabuf", SurfacePref::kDmabuf},
      {"plane", SurfacePref::kPlane},       {"drm_plane", SurfacePref::kPlane},
      {"vk_image", SurfacePref::kVkImage},  {"vkimage", SurfacePref::kVkImage},
      {"egl_image", SurfacePref::kEglImage}, {"shm", SurfacePref::kShm},
      {"software", SurfacePref::kShm},
  };
  for (const auto& n : kNames) {
    if (text == n.name) {
      out = n.pref;
      return true;
    }
  }
  return false;
}

ViewParams ParseViewParams(const uint8_t* data,
                           size_t size,
                           const char* assets_path) {
  ViewParams p;
  if (data != nullptr && size != 0) {
    const std::optional<CodecValue> root = DecodeStandardMessage(data, size);
    if (!root || root->type != CodecValue::Type::kMap) {
      LOG_W("creation params are not a StandardMessageCodec map; using "
            "defaults");
    } else {
      auto str = [&root](const char* key) -> std::string {
        const CodecValue* v = root->Find(key);
        return v ? v->AsString().value_or("") : "";
      };
      p.main_pack = str("main_pack");
      p.project_path = str("path");
      p.rendering_driver = str("rendering_driver");
      p.rendering_method = str("rendering_method");
      p.libgodot = str("libgodot");
      if (const std::string s = str("surface"); !s.empty()) {
        if (!ParseSurfacePref(s, p.surface)) {
          LOG_W("unknown surface '%s'; using auto", s.c_str());
        }
      }
      if (const CodecValue* v = root->Find("device_pixel_ratio")) {
        p.device_pixel_ratio = v->AsDouble().value_or(1.0);
      }
      if (const CodecValue* v = root->Find("max_fps")) {
        p.max_fps = static_cast<int>(v->AsInt().value_or(0));
      }
      if (const CodecValue* v = root->Find("buffers")) {
        p.buffers = static_cast<int>(v->AsInt().value_or(3));
      }
      if (const CodecValue* v = root->Find("args");
          v && v->type == CodecValue::Type::kList) {
        for (const CodecValue& a : v->list) {
          if (auto s = a.AsString()) {
            p.extra_args.push_back(*s);
          }
        }
      }
    }
  }
  if (p.device_pixel_ratio <= 0.0) {
    p.device_pixel_ratio = 1.0;
  }
  if (p.buffers < 2) {
    p.buffers = 2;
  } else if (p.buffers > 4) {
    p.buffers = 4;
  }
  p.main_pack = Resolve(p.main_pack, assets_path);
  p.project_path = Resolve(p.project_path, assets_path);
  return p;
}

void ApplyEnvironment(ViewParams& p) {
  if (const char* v = Env("IHS_GODOT_MAIN_PACK")) {
    p.main_pack = v;
  }
  if (const char* v = Env("IHS_GODOT_PATH")) {
    p.project_path = v;
  }
  if (const char* v = Env("IHS_GODOT_ARGS")) {
    std::istringstream in(v);
    std::string a;
    while (in >> a) {
      p.extra_args.push_back(a);
    }
  }
  if (const char* v = Env("IHS_GODOT_RENDERING_DRIVER")) {
    p.rendering_driver = v;
  }
  if (const char* v = Env("IHS_GODOT_RENDERING_METHOD")) {
    p.rendering_method = v;
  }
  if (const char* v = Env("IHS_GODOT_SURFACE")) {
    if (!ParseSurfacePref(v, p.surface)) {
      LOG_W("IHS_GODOT_SURFACE='%s' is not a surface; ignored", v);
    }
  }
  if (const char* v = Env("IHS_GODOT_LIBRARY")) {
    p.libgodot = v;
  }
  if (const char* v = Env("IHS_GODOT_MAX_FPS")) {
    p.max_fps = std::atoi(v);
  }
}

std::vector<std::string> BuildGodotArgs(const ViewParams& p) {
  std::vector<std::string> args{"ihs_godot_view"};
  if (!p.main_pack.empty()) {
    args.emplace_back("--main-pack");
    args.push_back(p.main_pack);
  }
  if (!p.project_path.empty()) {
    args.emplace_back("--path");
    args.push_back(p.project_path);
  }
  if (!p.rendering_driver.empty()) {
    args.emplace_back("--rendering-driver");
    args.push_back(p.rendering_driver);
  }
  if (!p.rendering_method.empty()) {
    args.emplace_back("--rendering-method");
    args.push_back(p.rendering_method);
  }
  if (p.max_fps > 0) {
    args.emplace_back("--max-fps");
    args.push_back(std::to_string(p.max_fps));
  }
  args.insert(args.end(), p.extra_args.begin(), p.extra_args.end());
  return args;
}

}  // namespace ihs_godot
