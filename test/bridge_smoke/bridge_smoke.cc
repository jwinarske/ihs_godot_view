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

// End-to-end check of a libgodot built with the ihs module, without a shell
// or a GPU: Mesa's lavapipe / llvmpipe are enough. It loads libgodot, runs the
// project in ./project under the ihs display driver, captures frames into a
// CPU target, checks the pixels, and round-trips messages and input through
// the bridge.
//
//   bridge_smoke <libgodot.so> <project dir> [vulkan|opengl3|opengl3_es]
//
// Exit status 0 when every check passed.

#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ihs_godot/bridge.h>

namespace {

std::vector<std::string> g_messages;
int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

void OnLog(void*, int level, const char* text) {
  std::printf("  [module %d] %s\n", level, text);
}

void OnMessage(void*, const char* text) {
  std::printf("  [game] %s\n", text);
  g_messages.emplace_back(text);
}

bool Received(const std::string& prefix) {
  for (const std::string& m : g_messages) {
    if (m.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <libgodot.so> <project> [driver]\n",
                 argv[0]);
    return 2;
  }
  const std::string driver = argc > 3 ? argv[3] : "vulkan";

  void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) {
    std::fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  auto get_api = reinterpret_cast<IhsGodotBridgeGetApiFn>(
      dlsym(lib, IHS_GODOT_BRIDGE_GET_API_SYMBOL));
  Check(get_api != nullptr, "libgodot exports the ihs bridge");
  if (get_api == nullptr) {
    return 1;
  }
  const IhsGodotApiV1* api = get_api(IHS_GODOT_BRIDGE_VERSION);
  Check(api != nullptr && api->struct_size >= sizeof(IhsGodotApiV1),
        "bridge v1 table");
  Check(get_api(IHS_GODOT_BRIDGE_VERSION + 1) == nullptr,
        "unknown bridge version refused");

  IhsGodotConfig config{};
  config.struct_size = sizeof(config);
  IhsGodotHostCallbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.log = OnLog;
  callbacks.message = OnMessage;
  Check(api->configure(&config, &callbacks) == IHS_GODOT_OK, "configure");

  constexpr uint32_t kW = 320, kH = 240;
  api->set_window_size(kW, kH, 1.0f);

  std::vector<std::string> args = {"bridge_smoke", "--path", argv[2],
                                    "--rendering-driver", driver};
  if (driver != "vulkan") {
    args.emplace_back("--rendering-method");
    args.emplace_back("gl_compatibility");
  }
  std::vector<char*> cargs;
  for (std::string& a : args) {
    cargs.push_back(a.data());
  }
  cargs.push_back(nullptr);
  void* instance = api->create_instance(int(args.size()), cargs.data());
  Check(instance != nullptr, "create_instance");
  if (instance == nullptr) {
    return 1;
  }
  Check(api->start(instance) == IHS_GODOT_OK, "start");

  IhsGodotRendererInfo info{};
  info.struct_size = sizeof(info);
  Check(api->get_renderer_info(&info) == IHS_GODOT_OK, "renderer info");
  std::printf("  api=%u targets=0x%x modifiers=%u explicit_sync=%u\n",
              info.api, info.target_types, info.dmabuf_modifiers,
              info.explicit_sync);
  Check(info.api == (driver == "vulkan" ? IHS_GODOT_API_VULKAN
                     : driver == "opengl3" ? IHS_GODOT_API_OPENGL
                                           : IHS_GODOT_API_GLES),
        "renderer is the one asked for");
  Check((info.target_types & (1u << IHS_GODOT_TARGET_CPU)) != 0,
        "CPU targets supported");

  // R,G,B,A bytes.
  std::vector<uint8_t> pixels(size_t(kW) * kH * 4, 0x55);
  IhsGodotTarget target{};
  target.struct_size = sizeof(target);
  target.type = IHS_GODOT_TARGET_CPU;
  target.width = kW;
  target.height = kH;
  target.fourcc = 0x34324241;  // ABGR8888
  target.pixels = pixels.data();
  target.cpu_stride = kW * 4;
  for (int& fd : target.plane_fd) {
    fd = -1;
  }
  target.memory_fd = -1;
  Check(api->add_target(7, &target) == IHS_GODOT_OK, "add CPU target");
  api->set_next_target(7);

  int captured = 0;
  IhsGodotFrameInfo frame{};
  for (int i = 0; i < 30; i++) {
    frame = IhsGodotFrameInfo{};
    frame.struct_size = sizeof(frame);
    if (api->iteration(instance, &frame) != 0) {
      break;
    }
    if (frame.target_id == 7) {
      captured++;
    }
    if (frame.acquire_fence_fd >= 0) {
      close(frame.acquire_fence_fd);
    }
    if (i == 5) {
      api->post_message("ping");
      api->key(0x00070004 /* A */, 'a', 1, 0, 0);
      api->key(0x00070004, 'a', 0, 0, 0);
      api->touch(0, IHS_GODOT_TOUCH_DOWN, 10, 20, 1.0f);
      api->touch(0, IHS_GODOT_TOUCH_UP, 10, 20, 0.0f);
      api->joy_connection(0, 1, "smoke pad", nullptr);
      api->joy_axis(0, 0 /* LEFT_X */, 1.0f);
    }
  }
  Check(captured > 0, "frames captured into the target");
  std::printf("  captured %d frames, last %ux%u frame %llu\n", captured,
              frame.width, frame.height,
              static_cast<unsigned long long>(frame.frame));

  auto px = [&](uint32_t x, uint32_t y) {
    const uint8_t* p = &pixels[(size_t(y) * kW + x) * 4];
    std::printf("  pixel(%u,%u) = %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]);
    return p;
  };
  const uint8_t* left = px(40, 120);
  const uint8_t* right = px(280, 120);
  Check(left[0] > 200 && left[1] < 40 && left[2] < 40, "left half is red");
  Check(right[2] > 200 && right[0] < 40 && right[1] < 40, "right half is blue");

  Check(Received("ready embedded=true display=ihs"), "game saw the ihs driver");
  Check(Received("echo:ping"), "message round trip");
  Check(Received("key:A"), "key event delivered");
  Check(Received("touch:10,20"), "touch event delivered");
  Check(Received("joy:0=1.0"), "joypad axis delivered");

  api->remove_target(7);
  for (int i = 0; i < 3; i++) {
    api->iteration(instance, nullptr);
  }
  api->destroy_instance(instance);
  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
  return g_failures == 0 ? 0 : 1;
}
