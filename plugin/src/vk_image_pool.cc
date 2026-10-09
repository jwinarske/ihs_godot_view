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

#include "vk_image_pool.h"

#include <cstring>

#include <ihs_godot/bridge.h>

#include "log.h"

namespace ihs_godot {

struct VkImagePool::Entry {
  Image image;
  std::weak_ptr<VkImagePool> pool;
  // Buffer ids the shell took this image under and has not released yet.
  std::atomic<int> outstanding{0};
  std::atomic<bool> dropped{false};
  std::atomic<bool> destroyed{false};
  // The release token of the buffer id currently submitted, reused for every
  // submit under that id so whichever submit's user data the shell keeps, it
  // is the same pointer.
  std::mutex token_mutex;
  Token* live_token = nullptr;
  uint32_t live_buffer_id = UINT32_MAX;
};

struct VkImagePool::Token {
  std::shared_ptr<Entry> entry;
  // Keeps the device-side function table alive until the shell lets go.
  std::shared_ptr<VkImagePool> pool;
};

std::shared_ptr<VkImagePool> VkImagePool::Create(const IhsVulkanContext& ctx) {
  if (ctx.device == nullptr || ctx.get_instance_proc_addr == nullptr ||
      ctx.queue == nullptr) {
    return nullptr;
  }
  bool has_fd = false;
  for (size_t i = 0; i < ctx.device_extension_count; i++) {
    if (std::strcmp(ctx.device_extensions[i],
                    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
      has_fd = true;
    }
  }
  if (!has_fd) {
    LOG_I("shell VkDevice has no VK_KHR_external_memory_fd; no VkImage path");
    return nullptr;
  }
  if (ctx.api_version < VK_API_VERSION_1_1) {
    LOG_I("shell VkDevice is Vulkan 1.0; no VkImage path");
    return nullptr;
  }

  auto gipa =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(ctx.get_instance_proc_addr);
  auto instance = static_cast<VkInstance>(ctx.instance);
  auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      gipa(instance, "vkGetDeviceProcAddr"));
  auto get_props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      gipa(instance, "vkGetPhysicalDeviceProperties2"));
  auto get_mem_props =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (gdpa == nullptr || get_props2 == nullptr || get_mem_props == nullptr) {
    return nullptr;
  }

  std::shared_ptr<VkImagePool> pool(new VkImagePool());
  pool->physical_device_ = static_cast<VkPhysicalDevice>(ctx.physical_device);
  pool->device_ = static_cast<VkDevice>(ctx.device);
  pool->queue_ = static_cast<VkQueue>(ctx.queue);
  pool->queue_family_ = ctx.queue_family_index;
  get_mem_props(pool->physical_device_, &pool->memory_props_);

  VkPhysicalDeviceIDProperties ids{};
  ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
  VkPhysicalDeviceProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props.pNext = &ids;
  get_props2(pool->physical_device_, &props);
  std::memcpy(pool->device_uuid_, ids.deviceUUID, 16);

  VkDevice d = pool->device_;
  Fns& f = pool->fn_;
#define LOAD(name) \
  f.name = reinterpret_cast<PFN_vk##name>(gdpa(d, "vk" #name))
  LOAD(CreateImage);
  LOAD(DestroyImage);
  LOAD(GetImageMemoryRequirements2);
  LOAD(AllocateMemory);
  LOAD(FreeMemory);
  LOAD(BindImageMemory);
  LOAD(GetMemoryFdKHR);
  LOAD(CreateCommandPool);
  LOAD(DestroyCommandPool);
  LOAD(AllocateCommandBuffers);
  LOAD(FreeCommandBuffers);
  LOAD(BeginCommandBuffer);
  LOAD(EndCommandBuffer);
  LOAD(CmdPipelineBarrier);
  LOAD(QueueSubmit);
  LOAD(CreateFence);
  LOAD(WaitForFences);
  LOAD(DestroyFence);
#undef LOAD
  const void* const* all = reinterpret_cast<const void* const*>(&f);
  for (size_t i = 0; i < sizeof(Fns) / sizeof(void*); i++) {
    if (all[i] == nullptr) {
      LOG_W("shell VkDevice lacks an entry point the VkImage path needs");
      return nullptr;
    }
  }
  return pool;
}

VkImagePool::~VkImagePool() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& e : entries_) {
    if (e && !e->destroyed) {
      // Only reached once every release token is gone (each holds the
      // pool), so nothing the shell took is still in use.
      DestroyEntry(*e);
    }
  }
}

int32_t VkImagePool::MemoryType(uint32_t bits) const {
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t i = 0; i < memory_props_.memoryTypeCount; i++) {
      if ((bits & (1u << i)) == 0) {
        continue;
      }
      if (pass == 0 && (memory_props_.memoryTypes[i].propertyFlags &
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
        continue;
      }
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

bool VkImagePool::TransitionToGeneral(VkImage image) {
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = queue_family_;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  if (fn_.CreateCommandPool(device_, &pool_info, nullptr, &cmd_pool) !=
      VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = cmd_pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  bool ok = fn_.AllocateCommandBuffers(device_, &alloc, &cmd) == VK_SUCCESS;

  VkFence fence = VK_NULL_HANDLE;
  if (ok) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    fn_.BeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fn_.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);
    fn_.EndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ok = fn_.CreateFence(device_, &fence_info, nullptr, &fence) == VK_SUCCESS;
  }
  if (ok) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    // The interposed vkQueueSubmit takes the shared-queue lock.
    ok = fn_.QueueSubmit(queue_, 1, &submit, fence) == VK_SUCCESS &&
         fn_.WaitForFences(device_, 1, &fence, VK_TRUE, 1000000000ull) ==
             VK_SUCCESS;
  }
  if (fence != VK_NULL_HANDLE) {
    fn_.DestroyFence(device_, fence, nullptr);
  }
  if (cmd != VK_NULL_HANDLE) {
    fn_.FreeCommandBuffers(device_, cmd_pool, 1, &cmd);
  }
  fn_.DestroyCommandPool(device_, cmd_pool, nullptr);
  return ok;
}

int VkImagePool::CreateImage(uint32_t width, uint32_t height, VkFormat format) {
  VkExternalMemoryImageCreateInfo external{};
  external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.pNext = &external;
  info.flags = IHS_GODOT_OPAQUE_IMAGE_FLAGS;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = static_cast<VkImageTiling>(IHS_GODOT_OPAQUE_IMAGE_TILING);
  info.usage = IHS_GODOT_OPAQUE_IMAGE_USAGE;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  auto entry = std::make_shared<Entry>();
  entry->pool = weak_from_this();
  Image& img = entry->image;
  img.width = width;
  img.height = height;
  img.format = format;
  if (fn_.CreateImage(device_, &info, nullptr, &img.image) != VK_SUCCESS) {
    LOG_E("shell vkCreateImage %ux%u failed", width, height);
    return -1;
  }

  VkMemoryDedicatedRequirements dedicated_reqs{};
  dedicated_reqs.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
  VkMemoryRequirements2 reqs{};
  reqs.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  reqs.pNext = &dedicated_reqs;
  VkImageMemoryRequirementsInfo2 req_info{};
  req_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
  req_info.image = img.image;
  fn_.GetImageMemoryRequirements2(device_, &req_info, &reqs);

  const int32_t type = MemoryType(reqs.memoryRequirements.memoryTypeBits);
  img.dedicated = dedicated_reqs.prefersDedicatedAllocation ||
                  dedicated_reqs.requiresDedicatedAllocation;
  img.size = reqs.memoryRequirements.size;

  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = img.image;
  VkExportMemoryAllocateInfo export_info{};
  export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  export_info.pNext = img.dedicated ? &dedicated : nullptr;

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &export_info;
  alloc.allocationSize = img.size;  // exact: a dedicated allocation is
  alloc.memoryTypeIndex = static_cast<uint32_t>(type);
  if (type < 0 ||
      fn_.AllocateMemory(device_, &alloc, nullptr, &img.memory) != VK_SUCCESS ||
      fn_.BindImageMemory(device_, img.image, img.memory, 0) != VK_SUCCESS ||
      !TransitionToGeneral(img.image)) {
    LOG_E("shell image memory allocation/export failed");
    DestroyEntry(*entry);
    return -1;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  entries_.push_back(std::move(entry));
  return static_cast<int>(entries_.size() - 1);
}

const VkImagePool::Image* VkImagePool::Get(int index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index < 0 || static_cast<size_t>(index) >= entries_.size() ||
      !entries_[index]) {
    return nullptr;
  }
  return &entries_[index]->image;
}

UniqueFd VkImagePool::ExportFd(int index) {
  const Image* img = Get(index);
  if (img == nullptr) {
    return UniqueFd();
  }
  VkMemoryGetFdInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  info.memory = img->memory;
  info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  if (fn_.GetMemoryFdKHR(device_, &info, &fd) != VK_SUCCESS) {
    return UniqueFd();
  }
  return UniqueFd(fd);
}

void VkImagePool::Describe(int index, uint32_t buffer_id, IhsVkImage& out) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    e = entries_.at(static_cast<size_t>(index));
  }
  out = IhsVkImage{};
  out.struct_size = sizeof(IhsVkImage);
  out.image = e->image.image;
  out.width = e->image.width;
  out.height = e->image.height;
  out.buffer_id = buffer_id;
  out.format = static_cast<uint32_t>(e->image.format);
  out.layout = VK_IMAGE_LAYOUT_GENERAL;
  out.release = &VkImagePool::OnRelease;
  std::lock_guard<std::mutex> lock(e->token_mutex);
  if (e->live_token == nullptr || e->live_buffer_id != buffer_id) {
    // First submit under this id: the shell releases it exactly once.
    e->live_token = new Token{e, shared_from_this()};
    e->live_buffer_id = buffer_id;
    e->outstanding.fetch_add(1);
  }
  out.release_user_data = e->live_token;
}

void VkImagePool::OnRelease(void* user_data, void* /*image*/,
                            uint32_t /*buffer_id*/) {
  // Any thread, must not block. Only the first-submit token owns anything.
  auto* token = static_cast<Token*>(user_data);
  std::shared_ptr<Entry> e = token->entry;
  std::shared_ptr<VkImagePool> pool = token->pool;
  {
    std::lock_guard<std::mutex> lock(e->token_mutex);
    if (e->live_token == token) {
      e->live_token = nullptr;
      e->live_buffer_id = UINT32_MAX;
    }
  }
  delete token;
  if (e->outstanding.fetch_sub(1) == 1 && e->dropped) {
    pool->DestroyEntry(*e);
  }
}

void VkImagePool::Drop(int index) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || static_cast<size_t>(index) >= entries_.size()) {
      return;
    }
    e = entries_[index];
  }
  if (!e) {
    return;
  }
  e->dropped = true;
  if (e->outstanding.load() == 0) {
    DestroyEntry(*e);
  }
}

void VkImagePool::DestroyEntry(Entry& e) {
  if (e.destroyed.exchange(true)) {
    return;
  }
  if (e.image.image != VK_NULL_HANDLE) {
    fn_.DestroyImage(device_, e.image.image, nullptr);
  }
  if (e.image.memory != VK_NULL_HANDLE) {
    fn_.FreeMemory(device_, e.image.memory, nullptr);
  }
  e.image.image = VK_NULL_HANDLE;
  e.image.memory = VK_NULL_HANDLE;
}

}  // namespace ihs_godot
