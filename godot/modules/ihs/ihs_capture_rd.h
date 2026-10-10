/**************************************************************************/
/*  ihs_capture_rd.h                                                      */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#if defined(RD_ENABLED) && defined(VULKAN_ENABLED)

#include "ihs_state.h"
#include "ihs_vulkan_hooks.h"

#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"

namespace RendererRD {
class TextureStorage;
}

// Captures the root viewport on the RenderingDevice path.
//
// Every target becomes an RD texture plus framebuffer. DMABUF and
// VK_OPAQUE_FD targets are VkImages this backend creates over the imported
// memory and hands to RD with texture_create_from_extension (the OpenXR
// swapchain route); CPU targets are RD-owned textures read back with
// texture_get_data_async.
//
// The capture itself is Godot's own copy_to_fb_rect pass, recorded where the
// compositor would blit to the screen, so it lands in the frame's command
// buffer with the render graph's barriers. An external target is then moved to
// VK_IMAGE_LAYOUT_GENERAL through a driver callback, which is the layout a
// consumer on another device or API can make sense of.
//
// No fence crosses the bridge: the display server runs RD without a surface,
// which RD answers with a single frame in flight, so swap_buffers() returns
// only after the GPU finished the frame. The capture is complete by the time
// the iteration returns.
class IhsCaptureRD : public IhsCaptureBackend {
	struct Target {
		uint32_t type = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t fourcc = 0;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		RID texture;
		RID framebuffer;
		void *pixels = nullptr;
		uint32_t cpu_stride = 0;
	};

	struct Garbage {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint64_t frame = 0;
	};

	IhsVulkanHooks *hooks = nullptr;
	VkDevice device = VK_NULL_HANDLE;
	HashMap<uint32_t, Target> targets;
	LocalVector<Garbage> garbage;

	bool has_dmabuf = false;
	bool has_modifiers = false;
	bool has_opaque = false;
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
	PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties = nullptr;

	// CPU readback in flight: the callback fires inside the same iteration.
	static IhsCaptureRD *singleton;
	uint32_t pending_cpu_target = IHS_GODOT_NO_TARGET;
	static void _cpu_data_ready(const Vector<uint8_t> &p_data);

	int _import_dmabuf(const IhsGodotTarget &p_target, Target &r_target);
	int _import_opaque(const IhsGodotTarget &p_target, Target &r_target);
	int _create_cpu(const IhsGodotTarget &p_target, Target &r_target);
	bool _wrap(Target &r_target);
	int32_t _memory_type(uint32_t p_bits, bool p_device_local) const;
	void _release(Target &p_target, bool p_now);
	uint64_t _frame() const;

public:
	explicit IhsCaptureRD(IhsVulkanHooks *p_hooks);
	~IhsCaptureRD() override;

	uint32_t get_api() const override { return IHS_GODOT_API_VULKAN; }
	int add_target(uint32_t p_id, const IhsGodotTarget &p_target) override;
	void remove_target(uint32_t p_id) override;
	void remove_all_targets() override;
	size_t query_dmabuf_formats(IhsGodotFormat *r_out, size_t p_max) override;
	void fill_renderer_info(IhsGodotRendererInfo &r_info) override;
	void collect_garbage() override;

	// From the compositor, where the blit to screen would happen.
	void capture(RendererRD::TextureStorage *p_texture_storage, const RenderingServerTypes::BlitToScreen *p_blits, int p_count);
};

// RendererCompositorRD that captures instead of presenting. There is no
// screen behind the RenderingDevice, so the two methods that would draw to one
// are replaced.
class IhsRendererCompositorRD : public RendererCompositorRD {
public:
	static IhsCaptureRD *capture_backend;

	static RendererCompositor *_create_current() {
		return memnew(IhsRendererCompositorRD);
	}
	static void make_current() {
		RendererCompositorRD::make_current();
		_create_func = _create_current;
	}

	void set_boot_image_with_stretch(const Ref<Image> &p_image, const Color &p_color, RSE::SplashStretchMode p_stretch_mode, bool p_use_filter) override {}
	void blit_render_targets_to_screen(DisplayServerEnums::WindowID p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) override;
};

#endif // RD_ENABLED && VULKAN_ENABLED
