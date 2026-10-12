/**************************************************************************/
/*  ihs_vulkan_hooks.h                                                    */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#if defined(VULKAN_ENABLED)

#include "drivers/vulkan/vulkan_hooks.h"

#include "core/templates/local_vector.h"
#include "core/templates/vector.h"
#include "core/string/ustring.h"

// Takes over instance/physical-device/device creation for Godot's Vulkan
// driver (the same seam OpenXR uses) so that:
//
//   - Godot renders on the GPU the shell composites on. A dma-buf imports on
//     another GPU only through a copy, if at all, and an OPAQUE_FD handle is
//     meaningless across physical devices. The device is picked by
//     VkPhysicalDeviceIDProperties::deviceUUID, then by DRM render node
//     (VK_EXT_physical_device_drm), then by type.
//   - the device has the external-memory extensions a hand-off needs.
//     Godot asks only for VK_KHR_external_memory_fd ("we don't actually use
//     this extension"), so dma-buf import, DRM format modifiers and
//     sync_file export are added here when the device supports them.
//
// Only device creation is augmented; Godot's own extension and feature
// choices pass through untouched.
class IhsVulkanHooks : public VulkanHooks {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	uint32_t api_version = VK_API_VERSION_1_0;

	LocalVector<String> enabled_extensions;

	VkPhysicalDevice _pick_physical_device() const;
	void _hint_v3dv(VkPhysicalDevice p_device) const;

public:
	bool create_vulkan_instance(const VkInstanceCreateInfo *p_vulkan_create_info, VkInstance *r_instance) override;
	bool get_physical_device(VkPhysicalDevice *r_device) override;
	bool create_vulkan_device(const VkDeviceCreateInfo *p_device_create_info, VkDevice *r_device) override;
	void set_direct_queue_family_and_index(uint32_t p_queue_family_index, uint32_t p_queue_index) override;
	bool use_fragment_density_offsets() override { return false; }
	void get_fragment_density_offsets(LocalVector<VkOffset2D> &r_offsets, const Vector2i &p_granularity) override {}
	bool use_subsampled_images() override { return false; }

	bool has_extension(const char *p_name) const;

	// Whether the device Godot will pick can export a semaphore as a
	// sync_file. Usable once the instance exists, before the device does.
	bool can_export_sync_file();

	VkInstance get_instance() const { return instance; }
	VkPhysicalDevice get_physical_device_handle() const { return physical_device; }
	VkDevice get_device() const { return device; }
	uint32_t get_queue_family() const { return queue_family; }
	uint32_t get_api_version() const { return api_version; }

	// 1.1 core or via VK_KHR_get_physical_device_properties2.
	bool get_device_uuids(uint8_t r_device_uuid[16], uint8_t r_driver_uuid[16]) const;
	// dev_t of the device's render node, 0 when unknown.
	uint64_t get_render_device() const;
};

#endif // VULKAN_ENABLED
