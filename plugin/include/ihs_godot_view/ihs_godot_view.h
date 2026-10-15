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

/*
 * libihs_godot_view: an ivi-homescreen platform view that runs a Godot game
 * (LibGodot + the `ihs` engine module) and hands its frames to the shell on
 * whatever surface path the active backend offers:
 *
 *   TEXTURE_DMABUF_IMPORT  dma-buf ring (gbm), Godot renders into it
 *   DRM_PLANE              same ring, allocated for scanout, plane preferred
 *   TEXTURE_VK_IMAGE       VkImages on the shell's device, Godot renders into
 *                          their memory through an OPAQUE_FD alias
 *   TEXTURE_EGL_IMAGE      dma-buf ring shown as EGLImages on the shell's
 *                          display
 *   SOFTWARE_SHM           the host's CPU buffers, filled by readback
 *
 * This header is the Dart FFI surface (package:ihs_godot_view). Every entry
 * point is C, callable from any thread unless noted.
 */

#ifndef IHS_GODOT_VIEW_H_
#define IHS_GODOT_VIEW_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define IHS_GODOT_VIEW_EXPORT __attribute__((visibility("default")))
#else
#define IHS_GODOT_VIEW_EXPORT
#endif

/* The platform-view type the factory registers. */
#define IHS_GODOT_VIEW_TYPE "ihs_godot_view"

/* Events delivered to the listener. */
typedef enum IhsGodotViewEvent {
  /* The engine is up and the view negotiated a surface path. @payload is a
   * JSON object: {"kind": "...", "api": "...", "backend": "..."}. */
  IHS_GODOT_VIEW_EVENT_READY = 0,
  /* IhsHost.send_message(text) from the game. @payload is the text. */
  IHS_GODOT_VIEW_EVENT_MESSAGE = 1,
  /* The game changed the mouse mode. @payload is the IhsGodotMouseMode
   * number as text ("2" = captured). */
  IHS_GODOT_VIEW_EVENT_MOUSE_MODE = 2,
  /* The game quit (get_tree().quit()). Godot cannot be started again in this
   * process; @payload is empty. */
  IHS_GODOT_VIEW_EVENT_QUIT = 3,
  /* Something failed; @payload says what. */
  IHS_GODOT_VIEW_EVENT_ERROR = 4,
  /* The surface path changed after a renegotiation. Same payload as READY. */
  IHS_GODOT_VIEW_EVENT_SURFACE = 5
} IhsGodotViewEvent;

/*
 * Event listener, for a C host. Called from the plugin's threads. @payload is
 * heap-allocated and owned by the receiver, which releases it with
 * ihs_godot_view_free_string(). Dart uses ihs_godot_view_set_dart_port
 * instead: a NativeCallable.listener aborts the process when it is called
 * after its isolate has gone, which an engine thread cannot rule out.
 */
typedef void (*IhsGodotViewListener)(int32_t view_id,
                                     int32_t event,
                                     char* payload);

/* Install the platform-view factory. Platform thread (it posts itself there
 * when called from elsewhere and a host is installed). Returns 0 on success. */
IHS_GODOT_VIEW_EXPORT int32_t ihs_godot_view_register(void);
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_unregister(void);

IHS_GODOT_VIEW_EXPORT void ihs_godot_view_set_listener(
    IhsGodotViewListener listener);

/* Deliver events to a Dart ReceivePort: @port is its SendPort.nativePort and
 * @post_cobject is NativeApi.postCObject. Each event arrives as the list
 * [view_id, event, payload]. A port whose isolate has gone (a hot restart, the
 * engine shutting down) drops the event. Takes precedence over a listener. */
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_set_dart_port(int64_t port,
                                                        void* post_cobject);
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_free_string(char* payload);

/* Pointer kinds and phases for ihs_godot_view_pointer. */
typedef enum IhsGodotViewPointerKind {
  IHS_GODOT_VIEW_POINTER_TOUCH = 0,
  IHS_GODOT_VIEW_POINTER_MOUSE = 1,
  IHS_GODOT_VIEW_POINTER_STYLUS = 2,
  IHS_GODOT_VIEW_POINTER_TRACKPAD = 3
} IhsGodotViewPointerKind;

typedef enum IhsGodotViewPointerPhase {
  IHS_GODOT_VIEW_POINTER_DOWN = 0,
  IHS_GODOT_VIEW_POINTER_MOVE = 1, /* also hover */
  IHS_GODOT_VIEW_POINTER_UP = 2,
  IHS_GODOT_VIEW_POINTER_CANCEL = 3,
  IHS_GODOT_VIEW_POINTER_SCROLL = 4
} IhsGodotViewPointerPhase;

/*
 * A Flutter pointer event, in view-local LOGICAL pixels (PointerEvent
 * .localPosition / .localDelta); the plugin scales by the view's device pixel
 * ratio. @buttons is Flutter's button bitmask (kPrimaryButton = 1,
 * kSecondaryButton = 2, kMiddleMouseButton = 4, kBackMouseButton = 8,
 * kForwardMouseButton = 16). @scroll_dx/@scroll_dy are a
 * PointerScrollEvent's scrollDelta.
 *
 * This is richer than the platform-view "touch" channel, which carries one
 * contact's coordinates and no buttons; the Dart widget uses this instead.
 */
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_pointer(int32_t view_id,
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
                                                  double scroll_dy);

/* A key event. @usb_hid_usage is PhysicalKeyboardKey.usbHidUsage, @unicode
 * the character it produced (0 for none), @modifiers IHS_GODOT_MOD_* bits
 * (shift 1, ctrl 2, alt 4, meta 8). */
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_key(uint32_t usb_hid_usage,
                                              uint32_t unicode,
                                              int32_t pressed,
                                              int32_t repeat,
                                              uint32_t modifiers);

/* Host-owned joypads (an on-screen stick, a pad the shell reads). Godot's
 * JoyButton / JoyAxis numbering. */
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_joy_connection(int32_t device,
                                                         int32_t connected,
                                                         const char* name);
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_joy_button(int32_t device,
                                                     uint32_t button,
                                                     int32_t pressed);
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_joy_axis(int32_t device,
                                                   uint32_t axis,
                                                   double value);

/* Emit IhsHost.message_received(text) in the game. */
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_post_message(const char* utf8);

/* Pause/resume the game (focus and NOTIFICATION_APPLICATION_PAUSED). */
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_set_paused(int32_t paused);

/* Stop the engine and join its thread. Godot cannot be restarted in the same
 * process afterwards. Never call from the platform thread while a view is
 * live. Not needed at process exit: the plugin stops the engine itself from
 * an exit handler. */
IHS_GODOT_VIEW_EXPORT void ihs_godot_view_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_GODOT_VIEW_H_ */
