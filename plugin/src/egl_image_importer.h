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

#include "gbm_allocator.h"

namespace ihs_godot {

// EGLImages on the SHELL's EGLDisplay for the TEXTURE_EGL_IMAGE path. An
// EGLImage from a dma-buf needs no context, so the producer can create them
// from its own thread; libEGL is loaded at run time.
class EglImageImporter {
 public:
  // @display is IhsEglContext::egl_display. False when the display cannot
  // import dma-bufs.
  bool Init(void* display);
  [[nodiscard]] bool ready() const { return create_ != nullptr; }

  // Returns the EGLImage, or nullptr. The buffer's fds stay the caller's.
  void* Import(const DmabufBuffer& buffer);
  void Destroy(void* image);

 private:
  using CreateImage = void* (*)(void*, void*, unsigned int, void*, const int*);
  using DestroyImage = unsigned int (*)(void*, void*);
  void* display_ = nullptr;
  bool modifiers_ = false;
  CreateImage create_ = nullptr;
  DestroyImage destroy_ = nullptr;
};

}  // namespace ihs_godot
