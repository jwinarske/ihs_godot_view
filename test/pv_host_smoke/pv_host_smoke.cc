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

// The producer end to end against a mock shell.
//
// libihs_shared's shell seam (ihs_pv_set_host) is installed with a host that
// behaves like a software-only ivi-homescreen backend: it offers
// SOFTWARE_SHM, allocates the two host buffers on grant (memfds stand in for
// the dma-bufs), runs a platform-thread task queue on this thread, and
// records what is submitted. libihs_godot_view.so is then loaded and driven
// the way the shell's registry drives it -- register, factory, resize,
// frames, dispose -- with libgodot rendering a two-color test scene.
//
//   pv_host_smoke <libihs_godot_view.so> <libgodot.so> <project dir>
//                 [vulkan|opengl3|opengl3_es]

#include <dlfcn.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ihs/platform_view.h>
#include <vulkan/vulkan.h>

#include "../../plugin/include/ihs_godot_view/ihs_godot_view.h"
#include "../../plugin/src/codec.h"

// Shell-only seam, not in the installed headers.
extern "C" {
typedef struct IhsPvHost {
  size_t struct_size;
  void* user_data;
  int (*register_factory)(void*, const char*, IhsPvFactory, void*);
  void (*unregister_factory)(void*, const char*);
  int (*query_capabilities)(void*, IhsPvCapabilities*);
  int (*vulkan_context)(void*, IhsVulkanContext*);
  int (*egl_context)(void*, IhsEglContext*);
  int (*grant)(void*, IhsPlatformView*, uint32_t, IhsFormatModifier*,
               uint32_t*, int*, size_t*);
  void (*revoke)(void*, IhsPlatformView*);
  uint32_t (*grant_drm_plane_id)(void*, IhsPlatformView*);
  int (*grant_shm_fd)(void*, IhsPlatformView*, size_t*);
  int (*submit)(void*, IhsPlatformView*, const IhsFrame*, int, int*);
  const char* (*assets_path)(void*);
  int (*post_platform_task)(void*, IhsPvTaskFn, void*);
  int (*is_platform_thread)(void*);
  int (*retire_buffer)(void*, IhsPlatformView*, uint32_t);
  int (*submit_layers)(void*, IhsPlatformView*, const IhsLayer*, size_t,
                       uint64_t, int*);
  int (*request_renegotiate)(void*, IhsPlatformView*);
  size_t (*grant_shm_slots)(void*, IhsPlatformView*, int*, size_t, size_t*);
} IhsPvHost;
void ihs_pv_set_host(const IhsPvHost* host);
}

namespace {

struct Mock {
  std::thread::id platform = std::this_thread::get_id();
  std::mutex mutex;
  std::deque<std::pair<IhsPvTaskFn, void*>> tasks;
  // Posts to answer as a shell whose platform task runner is not up yet.
  int posts_not_up = 0;

  IhsPvFactory factory = nullptr;
  uint32_t width = 0, height = 0;
  int shm[2] = {-1, -1};
  size_t stride = 0;
  uint32_t fourcc = 0;

  std::mutex frames_mutex;
  int frames = 0;
  int bad_frames = 0;
  uint8_t left[4] = {}, right[4] = {};
  std::vector<std::string> events;

  // TEXTURE_VK_IMAGE mode: the "shell's" Vulkan device.
  bool vk_mode = false;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  const char* device_exts[1] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
  int layer_frames = 0;
  // Images the shell took, to release once.
  std::vector<std::pair<IhsVkImage, bool>> taken;
} g;

int g_failures = 0;
void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

int RegisterFactory(void*, const char* type, IhsPvFactory f, void*) {
  if (std::strcmp(type, IHS_GODOT_VIEW_TYPE) == 0) {
    g.factory = f;
  }
  return IHS_PV_OK;
}
void UnregisterFactory(void*, const char*) { g.factory = nullptr; }

int QueryCaps(void*, IhsPvCapabilities* out) {
  out->backend_key = "mock-software";
  out->kinds = IHS_PV_KIND_SOFTWARE_SHM |
               (g.vk_mode ? IHS_PV_KIND_TEXTURE_VK_IMAGE : 0u);
  out->explicit_sync = 0;
  out->formats = nullptr;  // software-only backends advertise none
  out->format_count = 0;
  if (out->struct_size >= offsetof(IhsPvCapabilities, render_device) + 8) {
    out->render_device = 0;
  }
  return IHS_PV_OK;
}

int Unsupported(void*, void*) { return IHS_PV_ERR_UNSUPPORTED; }

void Revoke(void*, IhsPlatformView*) {
  for (int& fd : g.shm) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }
}

int Grant(void* ud, IhsPlatformView* view, uint32_t kind, IhsFormatModifier* fmt,
          uint32_t*, int* out_fd, size_t* out_stride) {
  if (kind != IHS_PV_KIND_SOFTWARE_SHM || g.width == 0) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  Revoke(ud, view);
  g.stride = size_t(g.width) * 4;
  g.fourcc = fmt->fourcc;
  for (int& fd : g.shm) {
    fd = memfd_create("mock-shm", MFD_CLOEXEC);
    if (ftruncate(fd, off_t(g.stride * (g.height + 1))) != 0) {
      return IHS_PV_ERR_UNSUPPORTED;
    }
  }
  *out_fd = g.shm[0];
  *out_stride = g.stride;
  std::printf("  [host] SOFTWARE_SHM grant %ux%u fourcc %.4s\n", g.width,
              g.height, reinterpret_cast<const char*>(&g.fourcc));
  return IHS_PV_OK;
}

size_t ShmSlots(void*, IhsPlatformView*, int* fds, size_t max, size_t* stride) {
  if (g.shm[0] < 0) {
    return 0;
  }
  for (size_t i = 0; i < 2 && i < max; i++) {
    fds[i] = g.shm[i];
  }
  if (stride) {
    *stride = g.stride;
  }
  return 2;
}

int Submit(void*, IhsPlatformView*, const IhsFrame* f, int acquire, int* release) {
  if (acquire >= 0) {
    close(acquire);
  }
  std::lock_guard<std::mutex> lock(g.frames_mutex);
  const bool ok = f->plane_count == 1 && f->buffer_id < 2 &&
                  f->plane_stride[0] == g.stride && f->width == g.width &&
                  f->height == g.height;
  if (!ok) {
    g.bad_frames++;
  } else {
    auto* p = static_cast<uint8_t*>(mmap(nullptr, g.stride * g.height,
                                         PROT_READ, MAP_SHARED,
                                         f->plane_fd[0], 0));
    if (p != MAP_FAILED) {
      const size_t y = g.height / 2;
      memcpy(g.left, p + y * g.stride + (g.width / 8) * 4, 4);
      memcpy(g.right, p + y * g.stride + (g.width * 7 / 8) * 4, 4);
      munmap(p, g.stride * g.height);
    }
    g.frames++;
  }
  close(f->plane_fd[0]);
  // SOFTWARE_SHM always returns a release fence; this one has fired.
  *release = eventfd(1, EFD_CLOEXEC);
  return ok ? IHS_PV_OK : IHS_PV_ERR_INVALID;
}

const char* Assets(void*) { return nullptr; }

bool InitVulkan() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (vkCreateInstance(&ici, nullptr, &g.instance) != VK_SUCCESS) {
    return false;
  }
  uint32_t n = 1;
  vkEnumeratePhysicalDevices(g.instance, &n, &g.physical);
  if (n == 0) {
    return false;
  }
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(g.physical, &qn, qf.data());
  for (uint32_t i = 0; i < qn; i++) {
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      g.family = i;
      break;
    }
  }
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = g.family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = g.device_exts;
  if (vkCreateDevice(g.physical, &dci, nullptr, &g.device) != VK_SUCCESS) {
    return false;
  }
  vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
  return true;
}

int VulkanContext(void*, IhsVulkanContext* out) {
  if (!g.vk_mode) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  out->instance = g.instance;
  out->physical_device = g.physical;
  out->device = g.device;
  out->queue = g.queue;
  out->queue_family_index = g.family;
  out->api_version = VK_API_VERSION_1_1;
  out->get_instance_proc_addr = reinterpret_cast<void*>(&vkGetInstanceProcAddr);
  out->device_extensions = g.device_exts;
  out->device_extension_count = 1;
  return IHS_PV_OK;
}

// Read one pixel back from a submitted VkImage (GENERAL layout) on the
// shell's device, the way a compositor would sample it.
bool ReadPixel(VkImage image, int32_t x, int32_t y, uint8_t out[4]) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buf;
  vkCreateBuffer(g.device, &bci, nullptr, &buf);
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(g.device, buf, &req);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(g.physical, &mp);
  uint32_t type = 0;
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
    if ((req.memoryTypeBits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      type = i;
      break;
    }
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VkDeviceMemory mem;
  vkAllocateMemory(g.device, &mai, nullptr, &mem);
  vkBindBufferMemory(g.device, buf, mem, 0);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.queueFamilyIndex = g.family;
  VkCommandPool pool;
  vkCreateCommandPool(g.device, &pci, nullptr, &pool);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  vkAllocateCommandBuffers(g.device, &cai, &cmd);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cmd, &bi);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {x, y, 0};
  region.imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(g.queue);
  void* p = nullptr;
  vkMapMemory(g.device, mem, 0, 4, 0, &p);
  memcpy(out, p, 4);
  vkUnmapMemory(g.device, mem);
  vkDestroyCommandPool(g.device, pool, nullptr);
  vkDestroyBuffer(g.device, buf, nullptr);
  vkFreeMemory(g.device, mem, nullptr);
  return true;
}

int SubmitLayers(void*, IhsPlatformView*, const IhsLayer* layers, size_t count,
                 uint64_t, int* release) {
  std::lock_guard<std::mutex> lock(g.frames_mutex);
  for (size_t i = 0; i < count; i++) {
    if (layers[i].acquire_fence_fd >= 0) {
      close(layers[i].acquire_fence_fd);
    }
    if (release) {
      release[i] = -1;
    }
  }
  if (count != 1 || layers[0].vk_image == nullptr) {
    g.bad_frames++;
    return IHS_PV_ERR_INVALID;
  }
  const IhsVkImage& vi = *layers[0].vk_image;
  if (vi.layout != VK_IMAGE_LAYOUT_GENERAL || vi.width != g.width ||
      vi.height != g.height) {
    g.bad_frames++;
  }
  bool seen = false;
  for (auto& t : g.taken) {
    seen |= t.first.buffer_id == vi.buffer_id;
  }
  if (!seen) {
    g.taken.emplace_back(vi, false);
  }
  g.fourcc = 0x34324241;  // R,G,B,A bytes (VK_FORMAT_R8G8B8A8_UNORM)
  ReadPixel(static_cast<VkImage>(vi.image), int32_t(g.width / 8),
            int32_t(g.height / 2), g.left);
  ReadPixel(static_cast<VkImage>(vi.image), int32_t(g.width * 7 / 8),
            int32_t(g.height / 2), g.right);
  g.frames++;
  g.layer_frames++;
  return IHS_PV_OK;
}

int PostTask(void*, IhsPvTaskFn fn, void* data) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (g.posts_not_up > 0) {
    --g.posts_not_up;
    return IHS_PV_ERR_NO_BACKEND;
  }
  g.tasks.emplace_back(fn, data);
  return IHS_PV_OK;
}

int IsPlatform(void*) {
  return std::this_thread::get_id() == g.platform ? 1 : 0;
}

void RunTasks() {
  std::deque<std::pair<IhsPvTaskFn, void*>> tasks;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    tasks.swap(g.tasks);
  }
  for (auto& t : tasks) {
    t.first(t.second);
  }
}

void OnEvent(int32_t id, int32_t event, char* payload) {
  std::printf("  [event] view %d type %d: %s\n", id, event, payload);
  std::lock_guard<std::mutex> lock(g.frames_mutex);
  g.events.emplace_back(std::to_string(event) + ":" + payload);
  free(payload);
}

using ihs_godot::CodecValue;
CodecValue Str(const std::string& s) {
  CodecValue v;
  v.type = CodecValue::Type::kString;
  v.s = s;
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <libihs_godot_view.so> <libgodot.so> "
                 "<project> [driver]\n", argv[0]);
    return 2;
  }
  IhsPvHost host{};
  host.struct_size = sizeof(host);
  host.register_factory = RegisterFactory;
  host.unregister_factory = UnregisterFactory;
  host.query_capabilities = QueryCaps;
  g.vk_mode = argc > 5 && std::string(argv[5]) == "vk_image";
  // "exit": leave the engine running and return from main, as a host process
  // does; the plugin's exit handler has to stop it.
  const bool at_exit = argc > 5 && std::string(argv[5]) == "exit";
  if (g.vk_mode && !InitVulkan()) {
    std::fprintf(stderr, "no Vulkan device for the mock shell\n");
    return 1;
  }
  host.vulkan_context = VulkanContext;
  host.submit_layers = SubmitLayers;
  host.egl_context =
      reinterpret_cast<int (*)(void*, IhsEglContext*)>(&Unsupported);
  host.grant = Grant;
  host.revoke = Revoke;
  host.submit = Submit;
  host.assets_path = Assets;
  host.post_platform_task = PostTask;
  host.is_platform_thread = IsPlatform;
  host.grant_shm_slots = ShmSlots;
  ihs_pv_set_host(&host);

  void* plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!plugin) {
    std::fprintf(stderr, "%s\n", dlerror());
    return 1;
  }
#define SYM(name) reinterpret_cast<decltype(&name)>(dlsym(plugin, #name))
  auto reg = SYM(ihs_godot_view_register);
  auto set_listener = SYM(ihs_godot_view_set_listener);
  auto shutdown = SYM(ihs_godot_view_shutdown);
  auto post = SYM(ihs_godot_view_post_message);
#undef SYM
  set_listener(OnEvent);
  // Dart registers from the UI thread, which can run before the shell has
  // its platform task runner: posting says it is not up. The plugin waits
  // for it rather than failing.
  g.posts_not_up = 3;
  int off_thread = -1;
  std::thread([&] { off_thread = reg(); }).join();
  RunTasks();
  Check(off_thread == 0 && g.posts_not_up == 0 && g.factory != nullptr,
        "registered from another thread once the runner was up");
  Check(reg() == 0 && g.factory != nullptr, "factory registered");

  // creationParams as the Dart widget sends them.
  CodecValue params;
  params.type = CodecValue::Type::kMap;
  params.map.emplace_back(Str("path"), Str(argv[3]));
  params.map.emplace_back(Str("libgodot"), Str(argv[2]));
  params.map.emplace_back(Str("surface"),
                          Str(g.vk_mode ? "vk_image" : "auto"));
  if (argc > 4) {
    params.map.emplace_back(Str("rendering_driver"), Str(argv[4]));
    if (std::string(argv[4]) != "vulkan") {
      params.map.emplace_back(Str("rendering_method"),
                              Str("gl_compatibility"));
    }
  }
  std::vector<uint8_t> encoded;
  ihs_godot::EncodeStandardMessage(params, encoded);

  // The shell creates at 1x1 and sends the size with the first resize.
  IhsPvCreateInfo info{};
  info.struct_size = sizeof(info);
  info.id = 3;
  info.view_type = IHS_GODOT_VIEW_TYPE;
  info.width = 1;
  info.height = 1;
  info.params = encoded.data();
  info.params_size = encoded.size();
  static int view_token;
  IhsPvCallbacks cb{};
  void* ud = nullptr;
  Check(g.factory(&info, nullptr,
                  reinterpret_cast<IhsPlatformView*>(&view_token), &cb,
                  &ud) == IHS_PV_OK,
        "factory created the view");
  g.width = 320;
  g.height = 240;
  cb.resize(ud, 320, 240);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool posted = false;
  while (std::chrono::steady_clock::now() < deadline) {
    RunTasks();
    {
      std::lock_guard<std::mutex> lock(g.frames_mutex);
      if (g.frames >= 20) {
        break;
      }
      if (g.frames >= 5 && !posted) {
        posted = true;
        post("hello from the shell");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  std::printf("  frames %d (bad %d), left %u %u %u %u, right %u %u %u %u\n",
              g.frames, g.bad_frames, g.left[0], g.left[1], g.left[2],
              g.left[3], g.right[0], g.right[1], g.right[2], g.right[3]);
  // A GL Godot cannot feed a VkImage; the producer falls back to shm.
  const bool expect_vk =
      g.vk_mode && (argc <= 4 || std::string(argv[4]) == "vulkan");
  Check(g.frames >= 20, "frames submitted");
  Check(g.layer_frames == (expect_vk ? g.frames : 0),
        expect_vk ? "every frame was a VkImage layer"
                  : "every frame went through ihs_pv_submit");
  Check(g.bad_frames == 0, "every frame matched the grant");
  // B,G,R,A for XRGB/ARGB, R,G,B,A for XBGR/ABGR.
  const bool bgra = g.fourcc == 0x34325258 || g.fourcc == 0x34325241;
  const uint8_t* l = g.left;
  const uint8_t* r = g.right;
  Check(bgra ? (l[2] > 200 && l[0] < 40) : (l[0] > 200 && l[2] < 40),
        "left half red as the shell reads it");
  Check(bgra ? (r[0] > 200 && r[2] < 40) : (r[2] > 200 && r[0] < 40),
        "right half blue as the shell reads it");

  bool ready = false, echoed = false;
  {
    std::lock_guard<std::mutex> lock(g.frames_mutex);
    for (const std::string& e : g.events) {
      ready |= e.rfind(expect_vk ? "0:{\"kind\":\"vk_image\""
                                 : "0:{\"kind\":\"shm\"",
                       0) == 0;
      echoed |= e == "1:echo:hello from the shell";
    }
  }
  Check(ready, "READY event names the negotiated path");
  Check(echoed, "message round trip through the plugin");

  cb.dispose(ud);
  for (int i = 0; i < 50; i++) {
    RunTasks();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const int after = g.frames;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Check(g.frames == after, "no submit after dispose");
  // The shell lets go of the images it took after the view is gone; the
  // producer destroys them only then.
  for (auto& t : g.taken) {
    t.first.release(t.first.release_user_data, t.first.image,
                    t.first.buffer_id);
  }
  if (!at_exit) {
    shutdown();
  }
  Revoke(nullptr, nullptr);
  if (g.vk_mode) {
    vkDeviceWaitIdle(g.device);
    vkDestroyDevice(g.device, nullptr);
    vkDestroyInstance(g.instance, nullptr);
  }
  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
  return g_failures ? 1 : 0;
}
