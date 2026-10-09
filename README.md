# ihs_godot_view

Run a Godot game inside a Flutter app on
[ivi-homescreen](https://github.com/toyota-connected/ivi-homescreen), as a
platform view backed by LibGodot (Godot as a library, upstream since 4.6),
on every surface path the shell offers:

| Path | ihs_pv kind | How the frame travels |
| --- | --- | --- |
| dma-buf | `TEXTURE_DMABUF_IMPORT` | gbm ring on the shell's render node; Godot renders into it (Vulkan import with DRM format modifiers, or EGLImage); the shell imports it |
| DRM plane | `DRM_PLANE` (preferred, import + shm as fallbacks) | same ring allocated for scanout; the shell puts it on a KMS plane when one is free |
| VkImage | `TEXTURE_VK_IMAGE` | images created on the **shell's** VkDevice, exported `OPAQUE_FD`; Godot renders into their memory on its own device (same GPU, matched by UUID). No dma-buf needed: the path for stacks without dma-buf export |
| EGLImage | `TEXTURE_EGL_IMAGE` | gbm ring imported as EGLImages on the shell's EGLDisplay |
| shared memory | `SOFTWARE_SHM` | the host's CPU buffers, filled by GPU readback. The universal floor |

`auto` picks dma-buf where both sides can do it, then VkImage, then EGLImage,
then the floor, and falls back on its own when an import is refused.

The acceptance target is the Godot third-person shooter demo
([godotengine/tps-demo](https://github.com/godotengine/tps-demo) and forks),
running with the Forward+ / Mobile renderer on Vulkan.

## How it fits together

```
 Flutter app (Dart)                    ivi-homescreen (shell)
 ┌──────────────────────────┐          ┌───────────────────────────────┐
 │ GodotView widget         │ platform │ PlatformViewRegistry          │
 │  PlatformViewLink ───────┼─ views ─▶│  ihs_pv host (libihs_shared)  │
 │  Listener/Focus ──┐      │ channel  │  compositor: import / plane / │
 │ VirtualGamepad ───┤ FFI  │          │  sample VkImage / EGLImage    │
 └───────────────────┼──────┘          └──────────────▲────────────────┘
                     ▼                                │ ihs_pv_submit /
 ┌──────────────────────────────────────────────┐     │ submit_layers
 │ libihs_godot_view.so  (plugin/)              │─────┘
 │  factory · negotiation · buffer rings ·      │
 │  fences · engine thread                      │
 └──────────────────────┬───────────────────────┘
                        │ include/ihs_godot/bridge.h (C ABI, dlsym)
 ┌──────────────────────▼───────────────────────┐
 │ libgodot.so + `ihs` engine module (godot/)   │
 │  DisplayServerIhs: one virtual window        │
 │  Vulkan: surfaceless RenderingDevice,        │
 │          VulkanHooks (GPU pick, ext enable)  │
 │  GL/GLES: surfaceless EGL context            │
 │  blit-to-screen  ──▶  capture into target    │
 └──────────────────────────────────────────────┘
```

* **`godot/modules/ihs`** is a Godot engine module, compiled into libgodot
  (`custom_modules=`). Upstream LibGodot has only
  `libgodot_create_godot_instance` + `GodotInstance::iteration()`; it cannot
  render anywhere but an OS window. The module adds:
  * the `ihs` display driver: no OS window, size and input from the host;
  * a renderer with no surface: `RenderingDevice` on Vulkan with no main
    window (one frame in flight, so a frame is complete on the GPU when
    `iteration()` returns), or Godot's compatibility renderer on a surfaceless
    EGL context (desktop GL 3.3 or GLES 3.0);
  * `VulkanHooks` that put Godot on the shell's GPU (device UUID, then DRM
    render node) and enable `VK_EXT_external_memory_dma_buf`,
    `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_semaphore_fd`, ...;
  * subclasses of `RendererCompositorRD` / `RasterizerGLES3` whose
    blit-to-screen captures the root viewport into the producer's target
    (dma-buf, Vulkan OPAQUE_FD memory, or CPU memory) instead;
  * `IhsHost`, a script singleton for app ⇄ game messages.
* **`include/ihs_godot/bridge.h`** is the C ABI between the module and any
  host. It knows nothing about ivi-homescreen.
* **`plugin/`** is the ivi-homescreen platform-view producer
  (`ihs_pv_register_factory`): it negotiates the path, allocates and imports
  buffers, waits release fences, submits frames, and runs Godot on its own
  thread. It links only `libihs_shared`; gbm, EGL and Vulkan are reached at
  run time.
* **`dart/ihs_godot_view`** is the Flutter package: `GodotView`,
  `GodotController`, `VirtualGamepad`, and an example app that runs the
  tps-demo.

## Build with emb

[emb_cli](https://github.com/toyota-connected/emb_cli) builds ivi-homescreen,
the two native libraries and the Flutter bundle in one go. The example app
carries an emb **app layer**
(`dart/ihs_godot_view/example/.emb/`) that adds two `modules` to
ivi-homescreen's own manifest:

* `libgodot`, built through `godot/CMakeLists.txt`, a CMake front that maps
  emb's cross toolchain onto Godot's SCons (emb has no SCons step);
* `ihs-godot-view`, the producer, which finds `libihs_shared` in the overlay
  that ivi-homescreen's `embedder_exports: [shared]` stages first.

```sh
. "$FLUTTER_WORKSPACE/setup_env.sh"

# This machine, Wayland + Vulkan:
emb cross /path/to/ivi-homescreen --target local --build \
    --backend wayland-vulkan --define BUILD_COMPOSITOR=ON \
    --app /path/to/ihs_godot_view/dart/ihs_godot_view/example

# Raspberry Pi 5, DRM/KMS + Vulkan, deploy and run:
emb cross /path/to/ivi-homescreen --target rpi5-trixie --build \
    --backend drm-kms-vulkan --define BUILD_COMPOSITOR=ON \
    --app /path/to/ihs_godot_view/dart/ihs_godot_view/example \
    --deploy pi@rpi5.local --run
```

`BUILD_COMPOSITOR=ON` is required: the platform-view host exists only in the
compositor build, and without it registration fails with `-4`. Host tools for
the libgodot module: `scons`, `python3` (`emb deps` installs them from the
manifest's `deps:`).

## Build by hand

```sh
# 1. libgodot with the ihs module (fetches the pinned Godot revision unless
#    GODOT_SOURCE_DIR points at a checkout). Cross builds: pass a CMake
#    toolchain file; its compilers, sysroot and flags are handed to SCons.
cmake -G Ninja -S godot -B build/godot -DGODOT_TARGET=template_release
cmake --build build/godot               # -> build/godot/libgodot.so

#    Equivalent direct SCons call from a Godot checkout:
#    scons platform=linuxbsd target=template_release library_type=shared_library \
#          custom_modules=/path/to/ihs_godot_view/godot/modules x11=no wayland=no

# 2. The producer, against an installed ihs_shared (or -DIHS_BUILD_DIR=... with
#    -DIHS_SOURCE_DIR=... for a shell build tree).
cmake -G Ninja -S plugin -B build/plugin -DCMAKE_PREFIX_PATH=<ihs prefix>
cmake --build build/plugin && ctest --test-dir build/plugin

# 3. Put both next to the bundle's engine:
cp build/godot/libgodot.so build/plugin/libihs_godot_view.so <bundle>/lib/
```

The Godot revision is pinned in `godot/CMakeLists.txt` (`GODOT_GIT_REF`). The
module uses engine-internal C++ API (display server, compositor, Vulkan
hooks), so it tracks Godot master; move the pin deliberately.

## Running the third-person shooter demo

1. Export the game as a `.pck` with the Godot **editor of the same revision**
   as libgodot (Project → Export → Linux → Export PCK/ZIP). For ARM targets
   enable ETC2/ASTC texture import
   (`rendering/textures/vram_compression/import_etc2_astc=true`).
2. Put it at `<bundle>/data/flutter_assets/godot/tps-demo.pck`, or point
   `IHS_GODOT_MAIN_PACK` at it.
3. Run the bundle: `./homescreen -b .` (`--backend drm-kms-vulkan` etc.).

The demo needs Vulkan (Forward+ or Mobile). On a modest GPU (Raspberry Pi 5)
use the Mobile renderer: `IHS_GODOT_RENDERING_METHOD=mobile`. Its controls
are keyboard/mouse and gamepad; on a touch-only head unit the example's
`VirtualGamepad` overlay drives move/look/jump/aim/fire, and a pad plugged
into the device is read by Godot itself (SDL). Mouse capture is reported to
the app (`GodotController.mouseCaptured`) and look input uses the pointer's
own deltas, so camera control keeps working at the view's edges.

## Using it from Flutter

```dart
final godot = GodotController();

GodotView(
  mainPack: 'godot/tps-demo.pck',   // under flutter_assets, or absolute
  surface: GodotSurface.auto,       // dmabuf | plane | vkImage | eglImage | shm
  controller: godot,
);

godot.messages.listen((m) => print('game says $m'));
godot.sendMessage('{"cmd":"pause"}');
```

From GDScript:

```gdscript
func _ready():
    IhsHost.message_received.connect(func(m): print("app says ", m))
    if IhsHost.is_embedded():
        IhsHost.send_message("ready")
```

## Configuration

Creation params (the `GodotView` fields) can be overridden from the
environment, which is convenient on a target:

| Variable | Meaning |
| --- | --- |
| `IHS_GODOT_MAIN_PACK` | `.pck` to run |
| `IHS_GODOT_PATH` | project directory (needs libgodot built with `GODOT_PATH_OVERRIDES=ON`) |
| `IHS_GODOT_SURFACE` | `auto`, `dmabuf`, `plane`, `vk_image`, `egl_image`, `shm` |
| `IHS_GODOT_RENDERING_DRIVER` | `vulkan`, `opengl3`, `opengl3_es` |
| `IHS_GODOT_RENDERING_METHOD` | `forward_plus`, `mobile`, `gl_compatibility` |
| `IHS_GODOT_ARGS` | extra Godot arguments, space separated |
| `IHS_GODOT_LIBRARY` | path to `libgodot.so` (default: beside the plugin, then the bundle's `lib/`) |
| `IHS_GODOT_MAX_FPS` | cap the frame rate (default: the display's refresh) |
| `IHS_GODOT_VIEW_LIB` | Dart side: path to `libihs_godot_view.so` |
| `IHS_GODOT_VIEW_LOG` | `error`, `warning`, `info` (default), `debug` |

## Tests

* `plugin/test`: unit tests for creation-param decoding, touch decoding and
  the surface-selection policy (`ctest --test-dir build/plugin`).
* `test/`: end-to-end smoke tests that need no GPU and no shell; Mesa's
  lavapipe (Vulkan) and llvmpipe (EGL) render.
  * `bridge_smoke` runs libgodot under the `ihs` driver on Vulkan, GL 3.3 and
    GLES 3.0, captures frames into a CPU target and checks the pixels, and
    round-trips messages, keys, touch and joypad input.
  * `pv_host_smoke` installs a mock shell through libihs_shared's host seam
    and drives the real producer: factory, 1×1 create then resize,
    negotiation on the platform thread, `SOFTWARE_SHM` grant and submit, and
    `TEXTURE_VK_IMAGE` across two VkDevices (the mock shell's and Godot's)
    with pixels read back on the shell's side; then dispose (no submit after
    it) and shutdown.

```sh
cmake -G Ninja -S test -B build/test -DCMAKE_PREFIX_PATH=<ihs prefix> \
    -DLIBGODOT=<libgodot.so built with GODOT_PATH_OVERRIDES=ON> \
    -DIHS_GODOT_VIEW=build/plugin/libihs_godot_view.so
cmake --build build/test && ctest --test-dir build/test --output-on-failure
```

## Limits and notes

* **One Godot per process.** LibGodot creates one instance and cannot set
  Godot up twice. The engine starts with the first view and outlives it: a
  disposed view (or a hot restart) detaches and the next view re-attaches to
  the same game. Two simultaneous views of the game are refused. When the
  game quits (`get_tree().quit()`) the app gets a `quit` event and Godot
  cannot be started again in that process.
* **Vulkan runs one frame in flight.** A RenderingDevice without a window
  uses a single frame, which is what makes the capture fence-free, at the
  cost of CPU/GPU overlap. It also skips Godot's pipeline cache (a main-window
  feature), so shaders compile at each launch.
* **Layouts and ownership.** Exported Vulkan images are left in
  `VK_IMAGE_LAYOUT_GENERAL` after each frame; queue-family ownership is not
  transferred to `VK_QUEUE_FAMILY_FOREIGN_EXT`, which current Mesa drivers do
  not need.
* **HDR 2D on GL.** With `rendering/viewport/hdr_2d` on the compatibility
  renderer the capture blits linear values without sRGB encoding; the
  Vulkan path converts.
* **Release templates** refuse `--path`; ship a `.pck` (or build with
  `GODOT_PATH_OVERRIDES=ON` for development).

## License

Apache License 2.0; see [LICENSE](LICENSE). Godot is MIT licensed.
