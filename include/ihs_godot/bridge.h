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
 * ihs_godot bridge: the C ABI between the `ihs` Godot engine module (compiled
 * into libgodot.so) and the ivi-homescreen platform-view producer that drives
 * it (libihs_godot_view.so).
 *
 * Upstream LibGodot (4.6+) exposes exactly two entry points --
 * libgodot_create_godot_instance / libgodot_destroy_godot_instance -- and a
 * GodotInstance object with start()/iteration(). It has no notion of rendering
 * somewhere other than an OS window. The `ihs` module fills that gap from the
 * engine side:
 *
 *   - a DisplayServer ("ihs") with one virtual window and no OS surface;
 *   - a surfaceless RenderingDevice (Vulkan) or a surfaceless EGL context
 *     (OpenGL 3.3 / GLES 3.0) for Godot to render with;
 *   - a VulkanHooks implementation that puts Godot on the same GPU the shell
 *     composites on and enables the external-memory extensions the hand-off
 *     needs;
 *   - a capture step at the point Godot would blit its root viewport to the
 *     screen, which instead writes the frame into a *target* the producer
 *     registered: a dma-buf, a Vulkan OPAQUE_FD allocation, or CPU memory.
 *
 * The producer owns everything ihs-specific: negotiation, buffer allocation,
 * fences, submit. The module knows nothing about ivi-homescreen; anything that
 * can hand it a dma-buf can display Godot.
 *
 * The module exports one symbol, IHS_GODOT_BRIDGE_GET_API_SYMBOL, from
 * libgodot.so. The producer dlopen()s libgodot, resolves it, and calls through
 * the returned table. The table is versioned by struct_size: fields are only
 * ever appended.
 *
 * THREADING. Godot binds its "main thread" to the thread that creates the
 * instance. Every entry point below except the input/message ones must be
 * called on that thread. Input and message entry points are thread-safe: they
 * queue, and the queue is drained at the start of the next iteration.
 */

#ifndef IHS_GODOT_BRIDGE_H_
#define IHS_GODOT_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IHS_GODOT_BRIDGE_VERSION 1u
#define IHS_GODOT_BRIDGE_GET_API_SYMBOL "ihs_godot_bridge_get_api"

/* Name of the display driver the module registers; pass it to libgodot as
 * `--display-driver ihs`. */
#define IHS_GODOT_DISPLAY_DRIVER "ihs"

/* No target: Godot renders, but the frame is not captured anywhere. */
#define IHS_GODOT_NO_TARGET 0xffffffffu

typedef enum IhsGodotResult {
  IHS_GODOT_OK = 0,
  IHS_GODOT_ERR_INVALID = -1,     /* bad argument / struct_size */
  IHS_GODOT_ERR_STATE = -2,       /* wrong call order (e.g. before start) */
  IHS_GODOT_ERR_UNSUPPORTED = -3, /* the active renderer cannot do this */
  IHS_GODOT_ERR_IMPORT = -4       /* the driver refused the buffer */
} IhsGodotResult;

/* The renderer Godot ended up on (fallbacks included). */
typedef enum IhsGodotApi {
  IHS_GODOT_API_NONE = 0, /* not started, or the dummy renderer */
  IHS_GODOT_API_VULKAN = 1,
  IHS_GODOT_API_OPENGL = 2, /* desktop GL 3.3 core over EGL */
  IHS_GODOT_API_GLES = 3    /* GLES 3.0 over EGL */
} IhsGodotApi;

/* ------------------------------------------------------------------------ */
/* Targets                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * A buffer the captured frame is written into.
 *
 *   DMABUF        a dma-buf of @fourcc + @modifier with @plane_count planes.
 *                 Vulkan imports it with VK_EXT_external_memory_dma_buf (and
 *                 VK_EXT_image_drm_format_modifier for a non-linear modifier);
 *                 GL imports it as an EGLImage. The frame is written top row
 *                 first.
 *   VK_OPAQUE_FD  Vulkan only: memory exported as OPAQUE_FD from an image on
 *                 another VkDevice of the SAME physical device (@device_uuid
 *                 must match IhsGodotRendererInfo::device_uuid). Both sides
 *                 create the image with exactly the IHS_GODOT_OPAQUE_IMAGE_*
 *                 parameters below, so the two images alias the same layout.
 *                 This is the path for a VkImage handed to the shell when the
 *                 driver has no dma-buf export (proprietary stacks).
 *   CPU           host memory at @pixels with @cpu_stride: the frame is read
 *                 back after the GPU finishes. The SOFTWARE_SHM floor.
 *
 * @fourcc is the DRM fourcc of the pixel layout for every type: one of
 * ABGR8888/XBGR8888 (bytes R,G,B,A) or ARGB8888/XRGB8888 (bytes B,G,R,A).
 *
 * File descriptors are BORROWED: the module dup()s what it keeps. @pixels must
 * stay mapped until the target is removed.
 */
typedef enum IhsGodotTargetType {
  IHS_GODOT_TARGET_DMABUF = 1,
  IHS_GODOT_TARGET_VK_OPAQUE_FD = 2,
  IHS_GODOT_TARGET_CPU = 3
} IhsGodotTargetType;

/* VK_OPAQUE_FD image parameters (VkImageCreateInfo), fixed on both sides. */
#define IHS_GODOT_OPAQUE_IMAGE_USAGE                                       \
  (0x00000001u /* TRANSFER_SRC */ | 0x00000002u /* TRANSFER_DST */ |     \
   0x00000004u /* SAMPLED */ | 0x00000010u /* COLOR_ATTACHMENT */)
#define IHS_GODOT_OPAQUE_IMAGE_TILING 0u /* VK_IMAGE_TILING_OPTIMAL */
#define IHS_GODOT_OPAQUE_IMAGE_FLAGS 0u

typedef struct IhsGodotTarget {
  size_t struct_size;
  uint32_t type; /* IhsGodotTargetType */
  uint32_t width;
  uint32_t height;
  uint32_t fourcc;

  /* DMABUF */
  uint64_t modifier;
  uint32_t plane_count;
  int plane_fd[4];
  uint32_t plane_offset[4];
  uint32_t plane_stride[4];

  /* VK_OPAQUE_FD */
  int memory_fd;
  uint64_t allocation_size;
  uint8_t dedicated; /* the exporter used a dedicated allocation */
  uint8_t reserved[7];
  uint8_t device_uuid[16];

  /* CPU */
  void* pixels;
  uint32_t cpu_stride;
} IhsGodotTarget;

/* A (fourcc, modifier) the active renderer can render into as a DMABUF
 * target. */
typedef struct IhsGodotFormat {
  uint32_t fourcc;
  uint32_t reserved;
  uint64_t modifier;
} IhsGodotFormat;

/* ------------------------------------------------------------------------ */
/* Configuration, renderer info, frame info                                  */
/* ------------------------------------------------------------------------ */

typedef enum IhsGodotLogLevel {
  IHS_GODOT_LOG_ERROR = 0,
  IHS_GODOT_LOG_WARNING = 1,
  IHS_GODOT_LOG_INFO = 2,
  IHS_GODOT_LOG_DEBUG = 3
} IhsGodotLogLevel;

/*
 * Callbacks into the producer. All are invoked on the Godot main thread.
 *   log      a module diagnostic (not Godot's own print output).
 *   message  GDScript called IhsHost.send_message(text); @utf8 is valid for
 *            the call only.
 *   mouse_mode  see below.
 */
typedef struct IhsGodotHostCallbacks {
  size_t struct_size;
  void* user_data;
  void (*log)(void* user_data, int level, const char* utf8);
  void (*message)(void* user_data, const char* utf8);
  /* The game changed Input.mouse_mode (IhsGodotMouseMode): a host can hide
   * its cursor while the game captures the mouse. */
  void (*mouse_mode)(void* user_data, uint32_t mode);
} IhsGodotHostCallbacks;

/* Input.MouseMode values. */
typedef enum IhsGodotMouseMode {
  IHS_GODOT_MOUSE_VISIBLE = 0,
  IHS_GODOT_MOUSE_HIDDEN = 1,
  IHS_GODOT_MOUSE_CAPTURED = 2,
  IHS_GODOT_MOUSE_CONFINED = 3,
  IHS_GODOT_MOUSE_CONFINED_HIDDEN = 4
} IhsGodotMouseMode;

/*
 * Where Godot should render. Applied when the display server is created, so
 * configure() must run before create_instance().
 *
 * @render_device is the dev_t of the DRM render node the consumer imports on
 * (IhsPvCapabilities::render_device), 0 when unknown. @device_uuid, when
 * @has_device_uuid, is VkPhysicalDeviceIDProperties::deviceUUID of the
 * consumer's GPU. The Vulkan path picks the physical device matching either
 * (uuid first); the GL path picks the EGL device whose render node matches.
 * With neither, the first discrete-then-integrated GPU wins.
 *
 * @extra_device_extensions are Vulkan device extensions to enable when the
 * physical device supports them, on top of the external-memory set the module
 * always asks for.
 */
typedef struct IhsGodotConfig {
  size_t struct_size;
  uint64_t render_device;
  uint8_t has_device_uuid;
  uint8_t reserved[7];
  uint8_t device_uuid[16];
  const char* const* extra_device_extensions;
  size_t extra_device_extension_count;
} IhsGodotConfig;

typedef struct IhsGodotRendererInfo {
  size_t struct_size;
  uint32_t api; /* IhsGodotApi */
  /* Vulkan */
  void* vk_instance;
  void* vk_physical_device;
  void* vk_device;
  uint32_t vk_queue_family;
  uint8_t device_uuid[16];
  uint8_t driver_uuid[16];
  /* Target types the renderer can import (bit 1 << IhsGodotTargetType). */
  uint32_t target_types;
  /* 1 when DMABUF targets with a non-linear modifier can be imported. */
  uint8_t dmabuf_modifiers;
  /* 1 when IhsGodotFrameInfo::acquire_fence_fd can be a real sync_file. */
  uint8_t explicit_sync;
  uint8_t reserved[6];
  /* GL */
  void* egl_display;
  void* egl_context;
  /* dev_t of the render node Godot runs on, 0 when unknown. */
  uint64_t render_device;
} IhsGodotRendererInfo;

/*
 * The result of one iteration.
 *   target_id         the target the frame was captured into, or
 *                     IHS_GODOT_NO_TARGET when nothing was captured (no
 *                     target set, hidden, or the frame was skipped).
 *   acquire_fence_fd  -1 when the frame is already complete on the GPU (the
 *                     Vulkan path always: a surfaceless RenderingDevice runs
 *                     with a single frame in flight and waits for it inside
 *                     the iteration), or a sync_file the caller owns that
 *                     signals when the capture finished (the GL path with
 *                     EGL_ANDROID_native_fence_sync).
 *   width/height      the captured region inside the target; the viewport can
 *                     lag a resize by a frame.
 *   frame             Godot's frame counter.
 */
typedef struct IhsGodotFrameInfo {
  size_t struct_size;
  uint32_t target_id;
  int acquire_fence_fd;
  uint32_t width;
  uint32_t height;
  uint64_t frame;
} IhsGodotFrameInfo;

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

typedef enum IhsGodotTouchPhase {
  IHS_GODOT_TOUCH_DOWN = 0,
  IHS_GODOT_TOUCH_MOVE = 1,
  IHS_GODOT_TOUCH_UP = 2,
  IHS_GODOT_TOUCH_CANCEL = 3
} IhsGodotTouchPhase;

/* Mouse button bitmask, Godot's MouseButtonMask order. */
#define IHS_GODOT_BUTTON_LEFT 0x1u
#define IHS_GODOT_BUTTON_RIGHT 0x2u
#define IHS_GODOT_BUTTON_MIDDLE 0x4u
#define IHS_GODOT_BUTTON_BACK 0x80u
#define IHS_GODOT_BUTTON_FORWARD 0x100u

/* Modifier bitmask for key events. */
#define IHS_GODOT_MOD_SHIFT 0x1u
#define IHS_GODOT_MOD_CTRL 0x2u
#define IHS_GODOT_MOD_ALT 0x4u
#define IHS_GODOT_MOD_META 0x8u

typedef enum IhsGodotNotification {
  IHS_GODOT_FOCUS_IN = 0,
  IHS_GODOT_FOCUS_OUT = 1,
  IHS_GODOT_PAUSE = 2,
  IHS_GODOT_RESUME = 3
} IhsGodotNotification;

/* ------------------------------------------------------------------------ */
/* The table                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct IhsGodotApiV1 {
  size_t struct_size;
  uint32_t version;

  /* --- lifecycle (Godot main thread) --- */

  /* Register the "ihs" display driver and store @config/@callbacks. Call
   * before create_instance(). Both are copied. */
  int (*configure)(const IhsGodotConfig* config,
                   const IhsGodotHostCallbacks* callbacks);
  /* libgodot_create_godot_instance with the module's own GDExtension init.
   * @argv[0] is the program name. Returns the GodotInstance or NULL. Only one
   * instance can exist per process, and Godot cannot be set up twice: after
   * destroy_instance() the process cannot create another. */
  void* (*create_instance)(int argc, char** argv);
  int (*start)(void* instance);
  /* Run one main-loop iteration. Returns 1 when Godot asked to quit, else 0.
   * @out may be NULL. */
  int (*iteration)(void* instance, IhsGodotFrameInfo* out);
  void (*destroy_instance)(void* instance);
  void (*notify)(void* instance, uint32_t notification);

  /* --- renderer (Godot main thread, after start) --- */

  int (*get_renderer_info)(IhsGodotRendererInfo* out);
  /* DMABUF formats the renderer can render into. Writes up to @max to @out and
   * returns how many there are in total (which may exceed @max). */
  size_t (*query_dmabuf_formats)(IhsGodotFormat* out, size_t max);

  /* --- output (Godot main thread) --- */

  /* Import @target under @id. An existing @id is replaced. */
  int (*add_target)(uint32_t id, const IhsGodotTarget* target);
  void (*remove_target)(uint32_t id);
  /* The target the next iteration captures into (IHS_GODOT_NO_TARGET for
   * none). Sticky until changed. */
  void (*set_next_target)(uint32_t id);

  /* --- window (Godot main thread) --- */

  /* The virtual window's size in physical pixels and its content scale. */
  void (*set_window_size)(uint32_t width, uint32_t height, float scale);
  /* 0: the view is not on screen; Godot keeps processing but stops drawing. */
  void (*set_visible)(int visible);

  /* --- input (any thread) --- */

  /* Window-pixel coordinates. @index is the touch slot. */
  void (*touch)(int32_t index, uint32_t phase, float x, float y,
                float pressure);
  /* @dx/@dy is the motion since the last event in window pixels; a game
   * that captures the mouse reads only this (InputEventMouseMotion
   * .screen_relative), so pass the pointer's own delta rather than letting
   * it be derived from positions that stop at the view's edge. */
  void (*mouse_motion)(float x, float y, float dx, float dy,
                       uint32_t button_mask);
  void (*mouse_button)(float x, float y, uint32_t button, int pressed,
                       uint32_t button_mask, int double_click);
  void (*scroll)(float x, float y, float dx, float dy);
  /* @usb_hid_usage is the USB HID usage (page << 16 | id; Flutter's
   * PhysicalKeyboardKey.usbHidUsage), @unicode the produced character or 0. */
  void (*key)(uint32_t usb_hid_usage, uint32_t unicode, int pressed,
              int echo, uint32_t modifiers);

  /* Joypads (Godot's SDL-style layout: JoyButton / JoyAxis numbering). A
   * pad wired to the device is read by Godot itself; these are for pads the
   * host owns -- a Flutter on-screen stick, or a controller the shell
   * already reads. Connect before sending buttons/axes. @guid may be NULL. */
  void (*joy_connection)(int32_t device, int connected, const char* name,
                         const char* guid);
  void (*joy_button)(int32_t device, uint32_t button, int pressed);
  /* @value in [-1, 1] for sticks, [0, 1] for triggers. */
  void (*joy_axis)(int32_t device, uint32_t axis, float value);

  /* --- messaging (any thread) --- */

  /* Emit IhsHost.message_received(text) in GDScript. */
  void (*post_message)(const char* utf8);
} IhsGodotApiV1;

typedef const IhsGodotApiV1* (*IhsGodotBridgeGetApiFn)(uint32_t version);

#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const IhsGodotApiV1* ihs_godot_bridge_get_api(uint32_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_GODOT_BRIDGE_H_ */
