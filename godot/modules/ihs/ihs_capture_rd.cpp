/**************************************************************************/
/*  ihs_capture_rd.cpp                                                    */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_capture_rd.h"

#if defined(RD_ENABLED) && defined(VULKAN_ENABLED)

#include "ihs_fourcc.h"

#include "core/object/callable_mp.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_device.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

IhsCaptureRD *IhsCaptureRD::singleton = nullptr;
IhsCaptureRD *IhsRendererCompositorRD::capture_backend = nullptr;

namespace {

constexpr VkImageUsageFlags kImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

VkFormat _vk_format(uint32_t p_fourcc) {
	return ihs_fourcc_is_rgba(p_fourcc) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
}

RD::DataFormat _rd_format(uint32_t p_fourcc) {
	return ihs_fourcc_is_rgba(p_fourcc) ? RD::DATA_FORMAT_R8G8B8A8_UNORM : RD::DATA_FORMAT_B8G8R8A8_UNORM;
}

// The driver callback exists only for its declared resource usage: it makes
// the render graph leave the target in VK_IMAGE_LAYOUT_GENERAL.
void _noop_callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, void *p_userdata) {}

bool _same_buffer(int p_a, int p_b) {
	if (p_a == p_b) {
		return true;
	}
	struct stat a, b;
	if (fstat(p_a, &a) != 0 || fstat(p_b, &b) != 0) {
		return false;
	}
	return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

} // namespace

IhsCaptureRD::IhsCaptureRD(IhsVulkanHooks *p_hooks) :
		hooks(p_hooks) {
	singleton = this;
	IhsRendererCompositorRD::capture_backend = this;
	if (hooks) {
		device = hooks->get_device();
		has_opaque = hooks->has_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
		has_dmabuf = has_opaque && hooks->has_extension(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
		has_modifiers = has_dmabuf && hooks->has_extension(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
	}
	if (device != VK_NULL_HANDLE) {
		get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");
		get_modifier_properties = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT");
	}
	if (has_dmabuf && !get_memory_fd_properties) {
		has_dmabuf = false;
		has_modifiers = false;
	}
	if (device != VK_NULL_HANDLE && _frame_delay() > 1) {
		get_semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
		VkExportSemaphoreCreateInfo export_info = {};
		export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
		export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		VkSemaphoreCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		info.pNext = &export_info;
		explicit_sync = get_semaphore_fd && vkCreateSemaphore(device, &info, nullptr, &frame_semaphore) == VK_SUCCESS;
		if (!explicit_sync) {
			IHS_LOG_WARN("no exportable semaphore; waiting for every frame instead");
		}
	}
	IHS_LOG_INFO(vformat("RD capture: dma-buf %s, modifiers %s, opaque-fd %s, %d frames in flight%s",
			has_dmabuf ? "yes" : "no", has_modifiers ? "yes" : "no", has_opaque ? "yes" : "no",
			_frame_delay(), explicit_sync ? " (sync_file)" : ""));
}

IhsCaptureRD::~IhsCaptureRD() {
	remove_all_targets();
	// Nothing can still be reading these: the display server flushed the
	// RenderingDevice before destroying the backend.
	for (const Garbage &g : garbage) {
		if (g.image != VK_NULL_HANDLE) {
			vkDestroyImage(device, g.image, nullptr);
		}
		if (g.memory != VK_NULL_HANDLE) {
			vkFreeMemory(device, g.memory, nullptr);
		}
	}
	garbage.clear();
	if (frame_semaphore != VK_NULL_HANDLE) {
		vkDestroySemaphore(device, frame_semaphore, nullptr);
	}
	if (IhsRendererCompositorRD::capture_backend == this) {
		IhsRendererCompositorRD::capture_backend = nullptr;
	}
	if (singleton == this) {
		singleton = nullptr;
	}
}

uint64_t IhsCaptureRD::_frame() const {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	return rd ? rd->get_frames_drawn() : 0;
}

uint32_t IhsCaptureRD::_frame_delay() const {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	return rd ? rd->get_frame_delay() : 1;
}

int32_t IhsCaptureRD::_memory_type(uint32_t p_bits, bool p_device_local) const {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(hooks->get_physical_device_handle(), &props);
	// Prefer device-local, then anything allowed.
	for (int pass = p_device_local ? 0 : 1; pass < 2; pass++) {
		for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
			if (!(p_bits & (1u << i))) {
				continue;
			}
			if (pass == 0 && !(props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				continue;
			}
			return int32_t(i);
		}
	}
	return -1;
}

int IhsCaptureRD::_import_dmabuf(const IhsGodotTarget &p_target, Target &r_target) {
	if (!has_dmabuf) {
		return IHS_GODOT_ERR_UNSUPPORTED;
	}
	if (p_target.plane_count < 1 || p_target.plane_count > 4 || p_target.plane_fd[0] < 0) {
		return IHS_GODOT_ERR_INVALID;
	}
	// One allocation backs every plane (a compression/metadata plane rides in
	// the same buffer); disjoint multi-buffer images are not supported.
	for (uint32_t i = 1; i < p_target.plane_count; i++) {
		if (!_same_buffer(p_target.plane_fd[0], p_target.plane_fd[i])) {
			IHS_LOG_ERROR("dma-buf target with planes in separate buffers is not supported");
			return IHS_GODOT_ERR_UNSUPPORTED;
		}
	}
	const bool linear = p_target.modifier == IHS_MOD_LINEAR;
	if (!has_modifiers && !linear) {
		IHS_LOG_ERROR(vformat("dma-buf modifier 0x%x needs VK_EXT_image_drm_format_modifier", p_target.modifier));
		return IHS_GODOT_ERR_UNSUPPORTED;
	}

	VkSubresourceLayout planes[4] = {};
	for (uint32_t i = 0; i < p_target.plane_count; i++) {
		planes[i].offset = p_target.plane_offset[i];
		planes[i].rowPitch = p_target.plane_stride[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {};
	modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	modifier_info.drmFormatModifier = p_target.modifier;
	modifier_info.drmFormatModifierPlaneCount = p_target.plane_count;
	modifier_info.pPlaneLayouts = planes;

	VkExternalMemoryImageCreateInfo external = {};
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	external.pNext = has_modifiers ? &modifier_info : nullptr;

	VkImageCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.pNext = &external;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = _vk_format(p_target.fourcc);
	info.extent = { p_target.width, p_target.height, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = has_modifiers ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
	info.usage = kImageUsage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkResult err = vkCreateImage(device, &info, nullptr, &r_target.image);
	if (err != VK_SUCCESS) {
		IHS_LOG_ERROR(vformat("vkCreateImage for dma-buf %dx%d modifier 0x%x failed (%d)", p_target.width, p_target.height, p_target.modifier, err));
		return IHS_GODOT_ERR_IMPORT;
	}

	if (!has_modifiers) {
		// Without explicit plane layouts the driver picks the linear pitch;
		// it has to agree with the buffer's.
		VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
		VkSubresourceLayout layout;
		vkGetImageSubresourceLayout(device, r_target.image, &sub, &layout);
		if (layout.rowPitch != p_target.plane_stride[0] || layout.offset != p_target.plane_offset[0]) {
			IHS_LOG_ERROR(vformat("linear dma-buf stride %d does not match the driver's %d", p_target.plane_stride[0], (int64_t)layout.rowPitch));
			vkDestroyImage(device, r_target.image, nullptr);
			r_target.image = VK_NULL_HANDLE;
			return IHS_GODOT_ERR_IMPORT;
		}
	}

	VkMemoryFdPropertiesKHR fd_props = {};
	fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
	err = get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, p_target.plane_fd[0], &fd_props);
	if (err != VK_SUCCESS) {
		IHS_LOG_ERROR(vformat("vkGetMemoryFdPropertiesKHR failed (%d)", err));
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(device, r_target.image, &reqs);
	const int32_t type = _memory_type(reqs.memoryTypeBits & fd_props.memoryTypeBits, false);
	if (type < 0) {
		IHS_LOG_ERROR("no memory type can import the dma-buf");
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}

	// The buffer must be at least what the image needs; a tiled modifier
	// pads to whole tiles, which a producer cannot always predict.
	const off_t size = lseek(p_target.plane_fd[0], 0, SEEK_END);
	if (size > 0 && uint64_t(size) < reqs.size) {
		IHS_LOG_ERROR(vformat("dma-buf holds %d bytes, the image needs %d", (int64_t)size, (int64_t)reqs.size));
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}

	const int fd = dup(p_target.plane_fd[0]);
	if (fd < 0) {
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}

	VkImportMemoryFdInfoKHR import = {};
	import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	import.fd = fd;

	VkMemoryDedicatedAllocateInfo dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.image = r_target.image;
	dedicated.pNext = &import;

	// A dedicated allocation is exactly the image's size: padding it is
	// invalid usage (ivi-homescreen#598).
	VkMemoryAllocateInfo alloc = {};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.pNext = &dedicated;
	alloc.allocationSize = reqs.size;
	alloc.memoryTypeIndex = uint32_t(type);

	err = vkAllocateMemory(device, &alloc, nullptr, &r_target.memory);
	if (err != VK_SUCCESS) {
		close(fd); // Ownership transfers only on success.
		IHS_LOG_ERROR(vformat("dma-buf import failed (%d)", err));
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}
	err = vkBindImageMemory(device, r_target.image, r_target.memory, 0);
	if (err != VK_SUCCESS) {
		IHS_LOG_ERROR(vformat("vkBindImageMemory failed (%d)", err));
		_release(r_target, true);
		return IHS_GODOT_ERR_IMPORT;
	}
	return IHS_GODOT_OK;
}

int IhsCaptureRD::_import_opaque(const IhsGodotTarget &p_target, Target &r_target) {
	if (!has_opaque) {
		return IHS_GODOT_ERR_UNSUPPORTED;
	}
	uint8_t device_uuid[16], driver_uuid[16];
	if (!hooks->get_device_uuids(device_uuid, driver_uuid) || memcmp(device_uuid, p_target.device_uuid, 16) != 0) {
		IHS_LOG_ERROR("opaque-fd target comes from a different physical device");
		return IHS_GODOT_ERR_UNSUPPORTED;
	}
	if (p_target.memory_fd < 0 || p_target.allocation_size == 0) {
		return IHS_GODOT_ERR_INVALID;
	}

	VkExternalMemoryImageCreateInfo external = {};
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	VkImageCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.pNext = &external;
	info.flags = IHS_GODOT_OPAQUE_IMAGE_FLAGS;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = _vk_format(p_target.fourcc);
	info.extent = { p_target.width, p_target.height, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VkImageTiling(IHS_GODOT_OPAQUE_IMAGE_TILING);
	info.usage = IHS_GODOT_OPAQUE_IMAGE_USAGE;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkResult err = vkCreateImage(device, &info, nullptr, &r_target.image);
	if (err != VK_SUCCESS) {
		IHS_LOG_ERROR(vformat("vkCreateImage for opaque-fd target failed (%d)", err));
		return IHS_GODOT_ERR_IMPORT;
	}
	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(device, r_target.image, &reqs);
	const int32_t type = _memory_type(reqs.memoryTypeBits, true);
	if (type < 0 || reqs.size > p_target.allocation_size) {
		IHS_LOG_ERROR("opaque-fd allocation does not fit the image");
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}

	const int fd = dup(p_target.memory_fd);
	if (fd < 0) {
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}
	VkImportMemoryFdInfoKHR import = {};
	import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	import.fd = fd;

	VkMemoryDedicatedAllocateInfo dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.image = r_target.image;
	dedicated.pNext = &import;

	// OPAQUE_FD imports must match the exporter's allocation exactly.
	VkMemoryAllocateInfo alloc = {};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.pNext = p_target.dedicated ? static_cast<const void *>(&dedicated) : static_cast<const void *>(&import);
	alloc.allocationSize = p_target.allocation_size;
	alloc.memoryTypeIndex = uint32_t(type);

	err = vkAllocateMemory(device, &alloc, nullptr, &r_target.memory);
	if (err != VK_SUCCESS) {
		close(fd);
		IHS_LOG_ERROR(vformat("opaque-fd import failed (%d)", err));
		vkDestroyImage(device, r_target.image, nullptr);
		r_target.image = VK_NULL_HANDLE;
		return IHS_GODOT_ERR_IMPORT;
	}
	err = vkBindImageMemory(device, r_target.image, r_target.memory, 0);
	if (err != VK_SUCCESS) {
		_release(r_target, true);
		return IHS_GODOT_ERR_IMPORT;
	}
	return IHS_GODOT_OK;
}

int IhsCaptureRD::_create_cpu(const IhsGodotTarget &p_target, Target &r_target) {
	if (!p_target.pixels || p_target.cpu_stride < p_target.width * 4) {
		return IHS_GODOT_ERR_INVALID;
	}
	RD::TextureFormat format;
	format.format = _rd_format(p_target.fourcc);
	format.width = p_target.width;
	format.height = p_target.height;
	format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	r_target.texture = RD::get_singleton()->texture_create(format, RD::TextureView());
	if (r_target.texture.is_null()) {
		return IHS_GODOT_ERR_IMPORT;
	}
	Vector<RID> attachments;
	attachments.push_back(r_target.texture);
	r_target.framebuffer = RD::get_singleton()->framebuffer_create(attachments);
	if (r_target.framebuffer.is_null()) {
		RD::get_singleton()->free_rid(r_target.texture);
		r_target.texture = RID();
		return IHS_GODOT_ERR_IMPORT;
	}
	r_target.pixels = p_target.pixels;
	r_target.cpu_stride = p_target.cpu_stride;
	return IHS_GODOT_OK;
}

bool IhsCaptureRD::_wrap(Target &r_target) {
	RD *rd = RD::get_singleton();
	r_target.texture = rd->texture_create_from_extension(RD::TEXTURE_TYPE_2D, _rd_format(r_target.fourcc), RD::TEXTURE_SAMPLES_1,
			RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT,
			uint64_t(r_target.image), r_target.width, r_target.height, 1, 1, 1);
	if (r_target.texture.is_null()) {
		return false;
	}
	Vector<RID> attachments;
	attachments.push_back(r_target.texture);
	r_target.framebuffer = rd->framebuffer_create(attachments);
	return r_target.framebuffer.is_valid();
}

void IhsCaptureRD::_release(Target &p_target, bool p_now) {
	RD *rd = RD::get_singleton();
	if (rd) {
		if (p_target.framebuffer.is_valid() && rd->framebuffer_is_valid(p_target.framebuffer)) {
			rd->free_rid(p_target.framebuffer);
		}
		if (p_target.texture.is_valid() && rd->texture_is_valid(p_target.texture)) {
			rd->free_rid(p_target.texture);
		}
	}
	p_target.framebuffer = RID();
	p_target.texture = RID();
	if (p_target.image == VK_NULL_HANDLE && p_target.memory == VK_NULL_HANDLE) {
		return;
	}
	if (p_now) {
		if (p_target.image != VK_NULL_HANDLE) {
			vkDestroyImage(device, p_target.image, nullptr);
		}
		if (p_target.memory != VK_NULL_HANDLE) {
			vkFreeMemory(device, p_target.memory, nullptr);
		}
	} else {
		// RD frees the texture's view at the end of a later frame, and the
		// image may still be in this frame's command buffer: keep it two
		// frames past now.
		Garbage g;
		g.image = p_target.image;
		g.memory = p_target.memory;
		g.frame = _frame() + _frame_delay() + 1;
		garbage.push_back(g);
	}
	p_target.image = VK_NULL_HANDLE;
	p_target.memory = VK_NULL_HANDLE;
}

int IhsCaptureRD::add_target(uint32_t p_id, const IhsGodotTarget &p_target) {
	if (p_id == IHS_GODOT_NO_TARGET || p_target.width == 0 || p_target.height == 0 || !ihs_fourcc_supported(p_target.fourcc)) {
		return IHS_GODOT_ERR_INVALID;
	}
	remove_target(p_id);

	Target t;
	t.type = p_target.type;
	t.width = p_target.width;
	t.height = p_target.height;
	t.fourcc = p_target.fourcc;

	int rc = IHS_GODOT_ERR_INVALID;
	switch (p_target.type) {
		case IHS_GODOT_TARGET_DMABUF:
			rc = _import_dmabuf(p_target, t);
			break;
		case IHS_GODOT_TARGET_VK_OPAQUE_FD:
			rc = _import_opaque(p_target, t);
			break;
		case IHS_GODOT_TARGET_CPU:
			rc = _create_cpu(p_target, t);
			break;
		default:
			break;
	}
	if (rc != IHS_GODOT_OK) {
		return rc;
	}
	if (p_target.type != IHS_GODOT_TARGET_CPU && !_wrap(t)) {
		IHS_LOG_ERROR("RenderingDevice refused the imported image");
		_release(t, true);
		return IHS_GODOT_ERR_IMPORT;
	}
	targets.insert(p_id, t);
	return IHS_GODOT_OK;
}

void IhsCaptureRD::remove_target(uint32_t p_id) {
	HashMap<uint32_t, Target>::Iterator it = targets.find(p_id);
	if (!it) {
		return;
	}
	_release(it->value, false);
	targets.remove(it);
}

void IhsCaptureRD::remove_all_targets() {
	for (KeyValue<uint32_t, Target> &kv : targets) {
		_release(kv.value, false);
	}
	targets.clear();
}

void IhsCaptureRD::collect_garbage() {
	const uint64_t now = _frame();
	for (uint32_t i = 0; i < garbage.size();) {
		if (garbage[i].frame <= now) {
			if (garbage[i].image != VK_NULL_HANDLE) {
				vkDestroyImage(device, garbage[i].image, nullptr);
			}
			if (garbage[i].memory != VK_NULL_HANDLE) {
				vkFreeMemory(device, garbage[i].memory, nullptr);
			}
			garbage.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

int IhsCaptureRD::end_iteration() {
	const bool had_capture = captured;
	captured = false;
	if (fence_pending) {
		// The frame was submitted at the end of the iteration, so the signal is
		// pending; the export leaves the semaphore unsignaled for the next one.
		fence_pending = false;
		VkSemaphoreGetFdInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
		info.semaphore = frame_semaphore;
		info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		int fd = -1;
		const VkResult err = get_semaphore_fd(device, &info, &fd);
		if (err == VK_SUCCESS) {
			return fd;
		}
		// The semaphore may still be signaled; it cannot be used again.
		IHS_LOG_ERROR(vformat("vkGetSemaphoreFdKHR failed (VkResult %d); waiting for every frame instead", err));
		explicit_sync = false;
	}
	if (had_capture && _frame_delay() > 1) {
		// No fence to hand over: drain the queue so the capture is complete.
		RD *rd = RD::get_singleton();
		rd->submit();
		rd->sync();
	}
	return -1;
}

size_t IhsCaptureRD::query_dmabuf_formats(IhsGodotFormat *r_out, size_t p_max) {
	if (!has_dmabuf) {
		return 0;
	}
	VkPhysicalDevice pd = hooks->get_physical_device_handle();
	size_t count = 0;
	auto emit = [&](uint32_t p_fourcc, uint64_t p_modifier) {
		if (r_out && count < p_max) {
			r_out[count].fourcc = p_fourcc;
			r_out[count].reserved = 0;
			r_out[count].modifier = p_modifier;
		}
		count++;
	};

	for (uint32_t fourcc : IHS_SUPPORTED_FOURCCS) {
		const VkFormat format = _vk_format(fourcc);
		if (!has_modifiers) {
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties(pd, format, &props);
			if (props.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) {
				emit(fourcc, IHS_MOD_LINEAR);
			}
			continue;
		}

		VkDrmFormatModifierPropertiesListEXT list = {};
		list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
		VkFormatProperties2 props = {};
		props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
		props.pNext = &list;
		vkGetPhysicalDeviceFormatProperties2(pd, format, &props);
		if (list.drmFormatModifierCount == 0) {
			continue;
		}
		LocalVector<VkDrmFormatModifierPropertiesEXT> mods;
		mods.resize(list.drmFormatModifierCount);
		list.pDrmFormatModifierProperties = mods.ptr();
		vkGetPhysicalDeviceFormatProperties2(pd, format, &props);

		for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
			const VkDrmFormatModifierPropertiesEXT &m = mods[i];
			if (!(m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
				continue;
			}
			// Renderable is not importable: ask with the external-memory
			// handle type and the exact usage the import will carry.
			VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info = {};
			mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
			mod_info.drmFormatModifier = m.drmFormatModifier;
			mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			VkPhysicalDeviceExternalImageFormatInfo ext_info = {};
			ext_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
			ext_info.pNext = &mod_info;
			ext_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

			VkPhysicalDeviceImageFormatInfo2 fmt_info = {};
			fmt_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
			fmt_info.pNext = &ext_info;
			fmt_info.format = format;
			fmt_info.type = VK_IMAGE_TYPE_2D;
			fmt_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
			fmt_info.usage = kImageUsage;

			VkExternalImageFormatProperties ext_props = {};
			ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
			VkImageFormatProperties2 fmt_props = {};
			fmt_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
			fmt_props.pNext = &ext_props;

			if (vkGetPhysicalDeviceImageFormatProperties2(pd, &fmt_info, &fmt_props) != VK_SUCCESS) {
				continue;
			}
			if (!(ext_props.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
				continue;
			}
			emit(fourcc, m.drmFormatModifier);
		}
	}
	return count;
}

void IhsCaptureRD::fill_renderer_info(IhsGodotRendererInfo &r_info) {
	r_info.api = IHS_GODOT_API_VULKAN;
	if (!hooks) {
		r_info.target_types = 1u << IHS_GODOT_TARGET_CPU;
		return;
	}
	r_info.vk_instance = hooks->get_instance();
	r_info.vk_physical_device = hooks->get_physical_device_handle();
	r_info.vk_device = hooks->get_device();
	r_info.vk_queue_family = hooks->get_queue_family();
	hooks->get_device_uuids(r_info.device_uuid, r_info.driver_uuid);
	r_info.target_types = 1u << IHS_GODOT_TARGET_CPU;
	if (has_dmabuf) {
		r_info.target_types |= 1u << IHS_GODOT_TARGET_DMABUF;
	}
	if (has_opaque) {
		r_info.target_types |= 1u << IHS_GODOT_TARGET_VK_OPAQUE_FD;
	}
	r_info.dmabuf_modifiers = has_modifiers ? 1 : 0;
	r_info.explicit_sync = explicit_sync ? 1 : 0;
	r_info.render_device = hooks->get_render_device();
}

void IhsCaptureRD::_cpu_data_ready(const Vector<uint8_t> &p_data, uint32_t p_width, uint32_t p_height) {
	IhsCaptureRD *self = singleton;
	IhsState &state = IhsState::get();
	// Outside an iteration (a teardown sync) no buffer is the producer's to
	// write; that frame is dropped.
	if (!self || !state.iterating || state.next_target == IHS_GODOT_NO_TARGET) {
		return;
	}
	const uint32_t id = state.next_target;
	const Target *t = self->targets.getptr(id);
	if (!t || t->type != IHS_GODOT_TARGET_CPU || !t->pixels) {
		return;
	}
	// The frame was read back at its own size; a resize since then crops it.
	const size_t src_row = size_t(p_width) * 4;
	const size_t row = size_t(MIN(p_width, t->width)) * 4;
	const size_t rows = MIN(size_t(MIN(p_height, t->height)), src_row ? size_t(p_data.size()) / src_row : 0);
	const uint8_t *src = p_data.ptr();
	uint8_t *dst = static_cast<uint8_t *>(t->pixels);
	for (size_t y = 0; y < rows; y++) {
		memcpy(dst + y * t->cpu_stride, src + y * src_row, row);
	}
	IhsState::Capture &c = state.capture;
	c.target_id = id;
	c.fence_fd = -1;
	c.width = uint32_t(row / 4);
	c.height = uint32_t(rows);
}

void IhsCaptureRD::capture(RendererRD::TextureStorage *p_texture_storage, const RenderingServerTypes::BlitToScreen *p_blits, int p_count) {
	IhsState &state = IhsState::get();
	if (p_count < 1 || state.next_target == IHS_GODOT_NO_TARGET) {
		return;
	}
	Target *t = targets.getptr(state.next_target);
	if (!t) {
		return;
	}
	const RID render_target = p_blits[0].render_target;
	const RID source = p_texture_storage->render_target_get_rd_texture(render_target);
	if (source.is_null()) {
		return;
	}
	// An HDR render target holds linear values; the target is 8-bit sRGB.
	const bool to_srgb = p_texture_storage->render_target_is_using_hdr(render_target);
	RendererRD::CopyEffects::get_singleton()->copy_to_fb_rect(source, t->framebuffer, Rect2i(0, 0, t->width, t->height), false, false, false, to_srgb);

	if (t->type == IHS_GODOT_TARGET_CPU) {
		// texture_get_data() would flush and wait for every frame in flight.
		// The async copy lands frame_delay - 1 iterations later, in whichever
		// CPU target that iteration draws into: one frame more latency, no
		// stall.
		RD::get_singleton()->texture_get_data_async(t->texture, 0,
				callable_mp_static(&IhsCaptureRD::_cpu_data_ready).bind(t->width, t->height));
		return;
	}

	RD::CallbackResource resource;
	resource.rid = t->texture;
	resource.type = RD::CALLBACK_RESOURCE_TYPE_TEXTURE;
	resource.usage = RD::CALLBACK_RESOURCE_USAGE_GENERAL;
	RD::get_singleton()->driver_callback_add(_noop_callback, nullptr, VectorView<RD::CallbackResource>(&resource, 1));
	if (explicit_sync && !fence_pending) {
		RD::get_singleton()->frame_signal_semaphore_add(RDD::SemaphoreID(frame_semaphore));
		fence_pending = true;
	}
	captured = true;

	state.capture.target_id = state.next_target;
	state.capture.fence_fd = -1;
	state.capture.width = t->width;
	state.capture.height = t->height;
}

void IhsRendererCompositorRD::blit_render_targets_to_screen(DisplayServerEnums::WindowID p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) {
	if (p_screen != DisplayServerEnums::MAIN_WINDOW_ID || !capture_backend) {
		return;
	}
	capture_backend->capture(texture_storage, p_render_targets, p_amount);
}

#endif // RD_ENABLED && VULKAN_ENABLED
