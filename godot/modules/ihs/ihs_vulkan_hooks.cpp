/**************************************************************************/
/*  ihs_vulkan_hooks.cpp                                                  */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_vulkan_hooks.h"

#if defined(VULKAN_ENABLED)

#include "ihs_state.h"

#include "core/string/print_string.h"
#include "core/variant/variant.h"

#include <sys/sysmacros.h>
#include <cstring>

namespace {

LocalVector<VkExtensionProperties> _device_extensions(VkPhysicalDevice p_device) {
	LocalVector<VkExtensionProperties> props;
	uint32_t count = 0;
	if (vkEnumerateDeviceExtensionProperties(p_device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
		return props;
	}
	props.resize(count);
	if (vkEnumerateDeviceExtensionProperties(p_device, nullptr, &count, props.ptr()) != VK_SUCCESS) {
		props.clear();
	}
	return props;
}

bool _supports(const LocalVector<VkExtensionProperties> &p_props, const char *p_name) {
	for (const VkExtensionProperties &p : p_props) {
		if (strcmp(p.extensionName, p_name) == 0) {
			return true;
		}
	}
	return false;
}

// vkGetPhysicalDeviceProperties2 is core in 1.1; volk also loads the KHR
// alias when the instance enabled VK_KHR_get_physical_device_properties2.
PFN_vkGetPhysicalDeviceProperties2 _props2() {
	if (vkGetPhysicalDeviceProperties2) {
		return vkGetPhysicalDeviceProperties2;
	}
	return reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(vkGetPhysicalDeviceProperties2KHR);
}

struct DeviceIdentity {
	uint8_t device_uuid[16] = {};
	uint8_t driver_uuid[16] = {};
	bool has_uuid = false;
	uint64_t render_device = 0;
};

DeviceIdentity _identify(VkPhysicalDevice p_device) {
	DeviceIdentity id;
	PFN_vkGetPhysicalDeviceProperties2 get_props2 = _props2();
	if (!get_props2) {
		return id;
	}
	const bool has_drm = _supports(_device_extensions(p_device), VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);

	VkPhysicalDeviceDrmPropertiesEXT drm = {};
	drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;

	VkPhysicalDeviceIDProperties ids = {};
	ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	ids.pNext = has_drm ? &drm : nullptr;

	VkPhysicalDeviceProperties2 props = {};
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props.pNext = &ids;
	get_props2(p_device, &props);

	memcpy(id.device_uuid, ids.deviceUUID, 16);
	memcpy(id.driver_uuid, ids.driverUUID, 16);
	id.has_uuid = true;
	if (has_drm && drm.hasRender) {
		id.render_device = makedev(uint32_t(drm.renderMajor), uint32_t(drm.renderMinor));
	}
	return id;
}

int _type_rank(VkPhysicalDeviceType p_type) {
	switch (p_type) {
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			return 0;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			return 1;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			return 2;
		case VK_PHYSICAL_DEVICE_TYPE_CPU:
			return 3;
		default:
			return 4;
	}
}

} // namespace

bool IhsVulkanHooks::create_vulkan_instance(const VkInstanceCreateInfo *p_vulkan_create_info, VkInstance *r_instance) {
	if (p_vulkan_create_info->pApplicationInfo) {
		api_version = p_vulkan_create_info->pApplicationInfo->apiVersion;
	}
	const VkResult err = vkCreateInstance(p_vulkan_create_info, nullptr, r_instance);
	if (err != VK_SUCCESS) {
		IHS_LOG_ERROR(vformat("vkCreateInstance failed (VkResult %d)", err));
		return false;
	}
	instance = *r_instance;
	return true;
}

VkPhysicalDevice IhsVulkanHooks::_pick_physical_device() const {
	uint32_t count = 0;
	if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
		return VK_NULL_HANDLE;
	}
	LocalVector<VkPhysicalDevice> devices;
	devices.resize(count);
	if (vkEnumeratePhysicalDevices(instance, &count, devices.ptr()) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}

	const IhsState &state = IhsState::get();
	VkPhysicalDevice by_node = VK_NULL_HANDLE;
	VkPhysicalDevice by_type = VK_NULL_HANDLE;
	int best_rank = 1 << 30;

	for (VkPhysicalDevice dev : devices) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(dev, &props);
		const DeviceIdentity id = _identify(dev);

		if (state.has_device_uuid && id.has_uuid && memcmp(id.device_uuid, state.device_uuid, 16) == 0) {
			IHS_LOG_INFO(vformat("Vulkan device '%s' matches the shell's device UUID", String::utf8(props.deviceName)));
			return dev;
		}
		if (state.render_device != 0 && id.render_device == state.render_device && by_node == VK_NULL_HANDLE) {
			by_node = dev;
		}
		const int rank = _type_rank(props.deviceType);
		if (rank < best_rank) {
			best_rank = rank;
			by_type = dev;
		}
	}
	if (by_node != VK_NULL_HANDLE) {
		IHS_LOG_INFO("Vulkan device picked by the shell's DRM render node");
		return by_node;
	}
	if (state.has_device_uuid || state.render_device != 0) {
		IHS_LOG_WARN("no Vulkan device matches the shell's GPU; dma-buf/opaque-fd hand-off may fail and fall back to CPU readback");
	}
	return by_type;
}

bool IhsVulkanHooks::get_physical_device(VkPhysicalDevice *r_device) {
	if (physical_device == VK_NULL_HANDLE) {
		physical_device = _pick_physical_device();
	}
	if (physical_device == VK_NULL_HANDLE) {
		IHS_LOG_ERROR("no Vulkan physical device");
		return false;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(physical_device, &props);
	// What can actually be used on the device is the lower of the two.
	api_version = MIN(api_version, props.apiVersion);
	*r_device = physical_device;
	return true;
}

bool IhsVulkanHooks::create_vulkan_device(const VkDeviceCreateInfo *p_device_create_info, VkDevice *r_device) {
	const LocalVector<VkExtensionProperties> available = _device_extensions(physical_device);

	LocalVector<const char *> names;
	for (uint32_t i = 0; i < p_device_create_info->enabledExtensionCount; i++) {
		names.push_back(p_device_create_info->ppEnabledExtensionNames[i]);
	}
	auto enabled = [&names](const char *p_name) {
		for (const char *n : names) {
			if (strcmp(n, p_name) == 0) {
				return true;
			}
		}
		return false;
	};
	auto add = [&](const char *p_name) {
		if (!enabled(p_name) && _supports(available, p_name)) {
			names.push_back(p_name);
		}
	};

	const bool vk11 = api_version >= VK_API_VERSION_1_1;
	add(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	add(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	add(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
	add(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
	if (enabled(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
		add(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
	}
	// VK_EXT_image_drm_format_modifier needs bind_memory2, image_format_list,
	// sampler_ycbcr_conversion and get_physical_device_properties2; all but
	// image_format_list are 1.1 core. On a 1.0 device, linear dma-bufs still
	// import through LINEAR tiling without it.
	if (vk11 && enabled(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
		if (api_version < VK_API_VERSION_1_2) {
			add(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
		}
		if (api_version >= VK_API_VERSION_1_2 || enabled(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) {
			add(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
		}
	}
	for (const CharString &extra : IhsState::get().extra_device_extensions) {
		add(extra.get_data());
	}

	VkDeviceCreateInfo create_info = *p_device_create_info;
	create_info.enabledExtensionCount = names.size();
	create_info.ppEnabledExtensionNames = names.ptr();

	const VkResult err = vkCreateDevice(physical_device, &create_info, nullptr, r_device);
	if (err != VK_SUCCESS) {
		IHS_LOG_ERROR(vformat("vkCreateDevice failed (VkResult %d)", err));
		return false;
	}
	device = *r_device;

	enabled_extensions.clear();
	String list;
	for (const char *n : names) {
		enabled_extensions.push_back(String(n));
		list += String(" ") + n;
	}
	IHS_LOG_DEBUG("Vulkan device extensions:" + list);
	return true;
}

void IhsVulkanHooks::set_direct_queue_family_and_index(uint32_t p_queue_family_index, uint32_t p_queue_index) {
	queue_family = p_queue_family_index;
}

bool IhsVulkanHooks::has_extension(const char *p_name) const {
	for (const String &e : enabled_extensions) {
		if (e == p_name) {
			return true;
		}
	}
	return false;
}

bool IhsVulkanHooks::get_device_uuids(uint8_t r_device_uuid[16], uint8_t r_driver_uuid[16]) const {
	if (physical_device == VK_NULL_HANDLE) {
		return false;
	}
	const DeviceIdentity id = _identify(physical_device);
	if (!id.has_uuid) {
		return false;
	}
	memcpy(r_device_uuid, id.device_uuid, 16);
	memcpy(r_driver_uuid, id.driver_uuid, 16);
	return true;
}

uint64_t IhsVulkanHooks::get_render_device() const {
	if (physical_device == VK_NULL_HANDLE) {
		return 0;
	}
	return _identify(physical_device).render_device;
}

#endif // VULKAN_ENABLED
