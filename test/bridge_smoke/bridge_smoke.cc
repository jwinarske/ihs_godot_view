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
//                [cpu|dmabuf]
//
// dmabuf mode allocates a linear dma-buf on a separate Vulkan device (the
// stand-in for a gbm ring), hands it to Godot as a DMABUF target and reads
// the pixels back from that device's mapping.
//
// Exit status 0 when every check passed.

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ihs_godot/bridge.h>
#include <vulkan/vulkan.h>

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

// A linear, host-visible image exported as a dma-buf.
struct DmabufImage {
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  int fd = -1;
  uint32_t stride = 0;
  uint8_t* map = nullptr;

  static bool Fail(const char* what) {
    std::printf("  dma-buf setup: %s failed\n", what);
    return false;
  }

  bool Create(uint32_t w, uint32_t h) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
      return Fail("vkCreateInstance");
    }
    VkPhysicalDevice pd;
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(instance, &n, &pd);
    if (n == 0) {
      return false;
    }
    const char* exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                          VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(pd, &dci, nullptr, &device) != VK_SUCCESS) {
      return Fail("vkCreateDevice with dma-buf export");
    }
    VkExternalMemoryImageCreateInfo ext{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &ext;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_LINEAR;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
      return Fail("vkCreateImage (linear, dma-buf exportable)");
    }
    VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(device, image, &sub, &layout);
    stride = uint32_t(layout.rowPitch);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      if ((req.memoryTypeBits & (1u << i)) &&
          (mp.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type = i;
        break;
      }
    }
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo ded{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = image;
    exp.pNext = &ded;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
      return Fail("dma-buf exportable allocation");
    }
    auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.memory = memory;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (!get_fd || get_fd(device, &gi, &fd) != VK_SUCCESS) {
      return Fail("vkGetMemoryFdKHR(DMA_BUF)");
    }
    void* p = nullptr;
    vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &p);
    map = static_cast<uint8_t*>(p) + layout.offset;
    return true;
  }

  void Destroy() {
    if (fd >= 0) {
      close(fd);
    }
    if (device) {
      if (map) {
        vkUnmapMemory(device, memory);
      }
      vkDestroyImage(device, image, nullptr);
      vkFreeMemory(device, memory, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance) {
      vkDestroyInstance(instance, nullptr);
    }
  }
};

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
  const bool dmabuf = argc > 4 && std::string(argv[4]) == "dmabuf";

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
  target.width = kW;
  target.height = kH;
  target.fourcc = 0x34324241;  // ABGR8888
  for (int& fd : target.plane_fd) {
    fd = -1;
  }
  target.memory_fd = -1;
  DmabufImage dma;
  uint32_t stride = kW * 4;
  if (dmabuf) {
    Check((info.target_types & (1u << IHS_GODOT_TARGET_DMABUF)) != 0,
          "dma-buf targets supported");
    if (!dma.Create(kW, kH)) {
      // lavapipe exports dma-bufs through /dev/udmabuf; without it (most
      // containers) there is nothing to import. Skip rather than fail.
      std::printf("SKIP no dma-buf exporter on this machine\n");
      dma.Destroy();
      api->destroy_instance(instance);
      return 77;
    }
    target.type = IHS_GODOT_TARGET_DMABUF;
    target.modifier = 0;  // LINEAR
    target.plane_count = 1;
    target.plane_fd[0] = dma.fd;
    target.plane_stride[0] = dma.stride;
    stride = dma.stride;
    std::vector<IhsGodotFormat> formats(api->query_dmabuf_formats(nullptr, 0));
    api->query_dmabuf_formats(formats.data(), formats.size());
    bool linear_abgr = false;
    for (const IhsGodotFormat& f : formats) {
      linear_abgr |= f.fourcc == target.fourcc && f.modifier == 0;
    }
    Check(linear_abgr, "renderer lists AB24/LINEAR as a render format");
    Check(api->add_target(7, &target) == IHS_GODOT_OK,
          "dma-buf imported as a target");
  } else {
    target.type = IHS_GODOT_TARGET_CPU;
    target.pixels = pixels.data();
    target.cpu_stride = kW * 4;
    Check(api->add_target(7, &target) == IHS_GODOT_OK, "add CPU target");
  }
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
      // The GL path may fence the capture; wait for it before reading.
      pollfd pfd{frame.acquire_fence_fd, POLLIN, 0};
      poll(&pfd, 1, 1000);
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

  const uint8_t* base = dmabuf ? dma.map : pixels.data();
  auto px = [&](uint32_t x, uint32_t y) {
    const uint8_t* p = base + size_t(y) * stride + size_t(x) * 4;
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
  if (dmabuf) {
    dma.Destroy();
  }
  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
  return g_failures == 0 ? 0 : 1;
}
