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

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <ihs/platform_view.h>

#include "unique_fd.h"

namespace ihs_godot {

// Images on the SHELL's VkDevice for the TEXTURE_VK_IMAGE path.
//
// The shell samples them as they are; Godot renders into their memory through
// an alias it imports from an OPAQUE_FD export (same physical device, same
// image parameters on both sides -- bridge.h IHS_GODOT_OPAQUE_IMAGE_*). No
// dma-buf, no modifiers: this is the path for a driver stack without dma-buf
// export.
//
// Lifetime: the shell keeps reading an image until its IhsVkImage::release
// fires, which can be after the view is disposed. Each image is therefore
// owned by a shared Entry that both the pool and the release token hold; the
// VkImage and its memory go when the last one lets go.
class VkImagePool : public std::enable_shared_from_this<VkImagePool> {
 public:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool dedicated = false;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  };

  // Resolves everything through the context's interposed loader, which is
  // what serializes queue access with the engine (#208). Null when the
  // context cannot export OPAQUE_FD memory.
  static std::shared_ptr<VkImagePool> Create(const IhsVulkanContext& ctx);
  ~VkImagePool();

  [[nodiscard]] const uint8_t* device_uuid() const { return device_uuid_; }

  // Create an image and move it to VK_IMAGE_LAYOUT_GENERAL, the layout the
  // shell is told it is in and Godot leaves it in. Returns an index.
  int CreateImage(uint32_t width, uint32_t height, VkFormat format);
  [[nodiscard]] const Image* Get(int index) const;
  // A fresh OPAQUE_FD for the image's memory (the caller owns it).
  UniqueFd ExportFd(int index);

  // Fill @out for a layer. The first time a buffer id is used, a release
  // token is attached; the shell calls it once per buffer id.
  void Describe(int index, uint32_t buffer_id, IhsVkImage& out);

  // Drop the pool's hold on an image. It is destroyed now if the shell never
  // took it, else once its release has fired.
  void Drop(int index);

 private:
  struct Entry;
  struct Token;
  static void OnRelease(void* user_data, void* image, uint32_t buffer_id);
  void DestroyEntry(Entry& e);
  bool TransitionToGeneral(VkImage image);
  int32_t MemoryType(uint32_t bits) const;

  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  uint8_t device_uuid_[16] = {};
  VkPhysicalDeviceMemoryProperties memory_props_{};

  struct Fns {
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDestroyFence DestroyFence;
  } fn_{};

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Entry>> entries_;
};

}  // namespace ihs_godot
