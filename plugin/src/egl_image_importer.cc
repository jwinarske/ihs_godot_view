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

#include "egl_image_importer.h"

#include <dlfcn.h>

#include <cstring>

#include "log.h"

namespace ihs_godot {

namespace {

constexpr int kNone = 0x3038;
constexpr int kWidth = 0x3057;
constexpr int kHeight = 0x3056;
constexpr int kExtensions = 0x3055;
constexpr unsigned int kLinuxDmaBuf = 0x3270;
constexpr int kFourcc = 0x3271;
constexpr int kPlaneFd[4] = {0x3272, 0x3275, 0x3278, 0x3440};
constexpr int kPlaneOffset[4] = {0x3273, 0x3276, 0x3279, 0x3441};
constexpr int kPlanePitch[4] = {0x3274, 0x3277, 0x327A, 0x3442};
constexpr int kModLo[4] = {0x3443, 0x3445, 0x3447, 0x3449};
constexpr int kModHi[4] = {0x3444, 0x3446, 0x3448, 0x344A};

bool HasExt(const char* list, const char* name) {
  if (list == nullptr) {
    return false;
  }
  const size_t len = std::strlen(name);
  for (const char* s = std::strstr(list, name); s != nullptr;
       s = std::strstr(s + 1, name)) {
    if ((s == list || s[-1] == ' ') && (s[len] == '\0' || s[len] == ' ')) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool EglImageImporter::Init(void* display) {
  void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr || display == nullptr) {
    return false;
  }
  using GetProc = void* (*)(const char*);
  using QueryString = const char* (*)(void*, int);
  auto get_proc = reinterpret_cast<GetProc>(dlsym(lib, "eglGetProcAddress"));
  auto query = reinterpret_cast<QueryString>(dlsym(lib, "eglQueryString"));
  if (get_proc == nullptr || query == nullptr) {
    return false;
  }
  const char* exts = query(display, kExtensions);
  if (!HasExt(exts, "EGL_EXT_image_dma_buf_import")) {
    LOG_I("shell EGL display cannot import dma-bufs; no EGLImage path");
    return false;
  }
  modifiers_ = HasExt(exts, "EGL_EXT_image_dma_buf_import_modifiers");
  create_ = reinterpret_cast<CreateImage>(get_proc("eglCreateImageKHR"));
  destroy_ = reinterpret_cast<DestroyImage>(get_proc("eglDestroyImageKHR"));
  if (create_ == nullptr || destroy_ == nullptr) {
    create_ = nullptr;
    return false;
  }
  display_ = display;
  return true;
}

void* EglImageImporter::Import(const DmabufBuffer& b) {
  if (create_ == nullptr) {
    return nullptr;
  }
  int attribs[64];
  int n = 0;
  attribs[n++] = kWidth;
  attribs[n++] = static_cast<int>(b.width);
  attribs[n++] = kHeight;
  attribs[n++] = static_cast<int>(b.height);
  attribs[n++] = kFourcc;
  attribs[n++] = static_cast<int>(b.fourcc);
  for (uint32_t i = 0; i < b.plane_count; i++) {
    attribs[n++] = kPlaneFd[i];
    attribs[n++] = b.fd.get();
    attribs[n++] = kPlaneOffset[i];
    attribs[n++] = static_cast<int>(b.offsets[i]);
    attribs[n++] = kPlanePitch[i];
    attribs[n++] = static_cast<int>(b.strides[i]);
    if (modifiers_) {
      attribs[n++] = kModLo[i];
      attribs[n++] = static_cast<int>(b.modifier & 0xffffffffu);
      attribs[n++] = kModHi[i];
      attribs[n++] = static_cast<int>(b.modifier >> 32);
    }
  }
  attribs[n++] = kNone;
  void* image = create_(display_, nullptr, kLinuxDmaBuf, nullptr, attribs);
  if (image == nullptr) {
    LOG_E("eglCreateImageKHR on the shell display failed");
  }
  return image;
}

void EglImageImporter::Destroy(void* image) {
  if (image != nullptr && destroy_ != nullptr) {
    destroy_(display_, image);
  }
}

}  // namespace ihs_godot
