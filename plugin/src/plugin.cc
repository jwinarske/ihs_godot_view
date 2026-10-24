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

// The ihs_pv factory and the Dart FFI entry points.

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <ihs/platform_view.h>
#include <ihs_godot/bridge.h>

#include "../include/ihs_godot_view/ihs_godot_view.h"
#include "godot_runtime.h"
#include "godot_view.h"
#include "input_map.h"
#include "log.h"

namespace ihs_godot {
namespace {

// What the registry stores as user_data: a handle that keeps the view alive
// for as long as the registry may call into it. The view itself outlives it
// when the engine thread still holds it.
struct Handle {
  std::shared_ptr<GodotView> view;
};

GodotView* ViewOf(void* user_data) {
  return static_cast<Handle*>(user_data)->view.get();
}

void OnResize(void* user_data, double width, double height) {
  ViewOf(user_data)->OnResize(width, height);
}

void OnTouch(void* user_data,
             int32_t action,
             int32_t point_count,
             size_t size,
             const double* data) {
  ViewOf(user_data)->OnTouch(action, point_count, size, data);
}

void OnSuspended(void* user_data, uint8_t suspended) {
  ViewOf(user_data)->OnSuspended(suspended != 0);
}

void OnRenegotiate(void* user_data) {
  ViewOf(user_data)->OnRenegotiate();
}

void OnPresented(void* user_data,
                 uint64_t seq,
                 uint64_t ust_ns,
                 uint32_t refresh_ns,
                 uint64_t /*msc*/,
                 uint32_t flags) {
  ViewOf(user_data)->OnPresented(seq, ust_ns, refresh_ns, flags);
}

// The last call for this view. Once it returns no submit for it is in
// flight and none can start (GodotView::Dispose takes the submit lock); the
// engine thread tears the ring down on its own time.
void OnDispose(void* user_data) {
  auto* handle = static_cast<Handle*>(user_data);
  handle->view->Dispose();
  delete handle;
}

int Factory(const IhsPvCreateInfo* info,
            void* /*factory_user_data*/,
            IhsPlatformView* view,
            IhsPvCallbacks* out_callbacks,
            void** out_user_data) {
  if (info == nullptr || info->struct_size < sizeof(IhsPvCreateInfo)) {
    return IHS_PV_ERR_INVALID;
  }
  std::shared_ptr<GodotView> v = GodotView::Create(info, view);
  if (!v) {
    return IHS_PV_ERR_NO_BACKEND;
  }
  if (!GodotRuntime::Get().Attach(v)) {
    GodotRuntime::Get().Emit(info->id, IHS_GODOT_VIEW_EVENT_ERROR,
                             "another ihs_godot_view is showing the game");
    return IHS_PV_ERR_UNSUPPORTED;
  }
  *out_callbacks = IhsPvCallbacks{};
  out_callbacks->struct_size = sizeof(IhsPvCallbacks);
  out_callbacks->resize = OnResize;
  out_callbacks->on_touch = OnTouch;
  out_callbacks->set_suspended = OnSuspended;
  out_callbacks->renegotiate = OnRenegotiate;
  out_callbacks->dispose = OnDispose;
  out_callbacks->presented = OnPresented;
  *out_user_data = new Handle{std::move(v)};
  return IHS_PV_OK;
}

void RegisterOnPlatform(void* /*user_data*/) {
  const int rc =
      ihs_pv_register_factory(IHS_GODOT_VIEW_TYPE, Factory, nullptr);
  LOG_I("register_factory('%s') = %d", IHS_GODOT_VIEW_TYPE, rc);
}

void UnregisterOnPlatform(void* /*user_data*/) {
  ihs_pv_unregister_factory(IHS_GODOT_VIEW_TYPE);
}

// Pointer bookkeeping for ihs_godot_view_pointer (Dart UI thread, but keep
// it safe for any).
struct PointerState {
  std::mutex mutex;
  TouchSlots touch;
  std::map<int32_t, uint32_t> buttons;  // mouse pointer -> IHS_GODOT_BUTTON_*
};

PointerState& Pointers() {
  static PointerState* state = new PointerState();
  return *state;
}

}  // namespace
}  // namespace ihs_godot

using namespace ihs_godot;

extern "C" {

int32_t ihs_godot_view_register(void) {
  // Registration is platform-thread only. A Dart entry point runs on the
  // platform thread on this embedder, but a plugin loaded from elsewhere may
  // not.
  if (ihs_pv_is_platform_thread == nullptr || ihs_pv_is_platform_thread()) {
    RegisterOnPlatform(nullptr);
    return 0;
  }
  if (ihs_pv_post_platform_task == nullptr) {
    LOG_E("cannot reach the platform thread to register the factory");
    return -1;
  }
  // The shell installs its platform-view host before the engine runs, but
  // hands it the platform task runner only once the engine is running, and
  // the first build -- which calls this -- can land in between. Posting then
  // fails as the runner not being up, and a moment later it is.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  int rc = IHS_PV_ERR_NO_BACKEND;
  while ((rc = ihs_pv_post_platform_task(&RegisterOnPlatform, nullptr)) ==
             IHS_PV_ERR_NO_BACKEND &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (rc == IHS_PV_OK) {
    return 0;
  }
  LOG_E("cannot reach the platform thread to register the factory (%d)", rc);
  return -1;
}

void ihs_godot_view_unregister(void) {
  if (ihs_pv_is_platform_thread == nullptr || ihs_pv_is_platform_thread()) {
    UnregisterOnPlatform(nullptr);
  } else if (ihs_pv_post_platform_task != nullptr) {
    ihs_pv_post_platform_task(&UnregisterOnPlatform, nullptr);
  }
}

void ihs_godot_view_set_listener(IhsGodotViewListener listener) {
  GodotRuntime::Get().SetListener(listener);
}

void ihs_godot_view_set_dart_port(int64_t port, void* post_cobject) {
  GodotRuntime::Get().SetDartPort(port, post_cobject);
}

void ihs_godot_view_free_string(char* payload) {
  std::free(payload);
}

void ihs_godot_view_pointer(int32_t view_id,
                            int32_t kind,
                            int32_t phase,
                            int32_t pointer,
                            double x,
                            double y,
                            double dx,
                            double dy,
                            int32_t buttons,
                            double pressure,
                            double scroll_dx,
                            double scroll_dy) {
  GodotRuntime& rt = GodotRuntime::Get();
  const IhsGodotApiV1* api = rt.api();
  if (api == nullptr || rt.active_view_id() != view_id) {
    return;
  }
  const double s = rt.ActiveDevicePixelRatio();
  const auto px = static_cast<float>(x * s);
  const auto py = static_cast<float>(y * s);
  PointerState& st = Pointers();
  std::lock_guard<std::mutex> lock(st.mutex);

  if (kind == IHS_GODOT_VIEW_POINTER_TOUCH ||
      kind == IHS_GODOT_VIEW_POINTER_STYLUS) {
    const auto p = static_cast<float>(pressure);
    switch (phase) {
      case IHS_GODOT_VIEW_POINTER_DOWN:
        if (const int idx = st.touch.Acquire(pointer); idx >= 0) {
          api->touch(idx, IHS_GODOT_TOUCH_DOWN, px, py, p);
        }
        break;
      case IHS_GODOT_VIEW_POINTER_MOVE:
        if (const int idx = st.touch.Find(pointer); idx >= 0) {
          api->touch(idx, IHS_GODOT_TOUCH_MOVE, px, py, p);
        }
        break;
      case IHS_GODOT_VIEW_POINTER_UP:
      case IHS_GODOT_VIEW_POINTER_CANCEL:
        if (const int idx = st.touch.Find(pointer); idx >= 0) {
          api->touch(idx,
                     phase == IHS_GODOT_VIEW_POINTER_UP ? IHS_GODOT_TOUCH_UP
                                                        : IHS_GODOT_TOUCH_CANCEL,
                     px, py, p);
          st.touch.Release(pointer);
        }
        break;
      default:
        break;
    }
    return;
  }

  // Mouse and trackpad.
  if (phase == IHS_GODOT_VIEW_POINTER_SCROLL) {
    // Flutter scroll deltas are in logical pixels; Godot's wheel factor is
    // in notches. ~40 px a notch is the usual wheel step.
    api->scroll(px, py, static_cast<float>(scroll_dx / 40.0),
                static_cast<float>(scroll_dy / 40.0));
    return;
  }
  const uint32_t before = st.buttons[pointer];
  const uint32_t after = (phase == IHS_GODOT_VIEW_POINTER_UP ||
                          phase == IHS_GODOT_VIEW_POINTER_CANCEL)
                             ? 0u
                             : FlutterButtonsToGodot(buttons);
  if (dx != 0.0 || dy != 0.0 || phase == IHS_GODOT_VIEW_POINTER_MOVE) {
    api->mouse_motion(px, py, static_cast<float>(dx * s),
                      static_cast<float>(dy * s), before);
  }
  for (const ButtonChange& c : DiffButtons(before, after)) {
    api->mouse_button(px, py, c.button, c.pressed ? 1 : 0, after, 0);
  }
  if (after == 0 && phase != IHS_GODOT_VIEW_POINTER_MOVE) {
    st.buttons.erase(pointer);
  } else {
    st.buttons[pointer] = after;
  }
}

void ihs_godot_view_key(uint32_t usb_hid_usage,
                        uint32_t unicode,
                        int32_t pressed,
                        int32_t repeat,
                        uint32_t modifiers) {
  if (const IhsGodotApiV1* api = GodotRuntime::Get().api()) {
    api->key(usb_hid_usage, unicode, pressed, repeat, modifiers);
  }
}

void ihs_godot_view_joy_connection(int32_t device,
                                   int32_t connected,
                                   const char* name) {
  if (const IhsGodotApiV1* api = GodotRuntime::Get().api()) {
    api->joy_connection(device, connected, name, nullptr);
  }
}

void ihs_godot_view_joy_button(int32_t device, uint32_t button, int32_t pressed) {
  if (const IhsGodotApiV1* api = GodotRuntime::Get().api()) {
    api->joy_button(device, button, pressed);
  }
}

void ihs_godot_view_joy_axis(int32_t device, uint32_t axis, double value) {
  if (const IhsGodotApiV1* api = GodotRuntime::Get().api()) {
    api->joy_axis(device, axis, static_cast<float>(value));
  }
}

void ihs_godot_view_post_message(const char* utf8) {
  if (const IhsGodotApiV1* api = GodotRuntime::Get().api()) {
    api->post_message(utf8);
  }
}

void ihs_godot_view_set_paused(int32_t paused) {
  GodotRuntime::Get().SetPaused(paused != 0);
}

void ihs_godot_view_shutdown(void) {
  GodotRuntime::Get().Shutdown();
}

}  // extern "C"
