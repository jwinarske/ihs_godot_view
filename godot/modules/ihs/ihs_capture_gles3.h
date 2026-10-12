/**************************************************************************/
/*  ihs_capture_gles3.h                                                   */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)

#include "ihs_egl.h"
#include "ihs_state.h"

#include "core/templates/hash_map.h"
#include "drivers/gles3/rasterizer_gles3.h"

// Captures the root viewport on the compatibility (GL / GLES) path.
//
// A DMABUF target is imported as an EGLImage, bound to a texture and wrapped
// in a framebuffer; the capture is a glBlitFramebuffer from the render
// target. Godot renders render targets "upside down" (row 0 on top) so that
// the blit to a window can flip them, which means a straight blit writes the
// target top row first -- the order a dma-buf consumer expects -- and no
// flip flag is needed. A CPU target is read back through a pair of pixel
// pack buffers: each frame starts its own read and copies out the previous
// one, which has finished on the GPU by then, so the CPU never waits on the
// frame it just drew. A CPU target therefore shows the frame before.
//
// When EGL_ANDROID_native_fence_sync is there, the capture is fenced with a
// sync_file the producer passes on as the acquire fence; otherwise it is
// glFinish()ed and the frame is complete when the iteration returns.
class IhsCaptureGLES3 : public IhsCaptureBackend {
	struct Target {
		uint32_t type = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t fourcc = 0;
		IhsEGLImageKHR image = nullptr;
		GLuint texture = 0;
		GLuint fbo = 0;
		void *pixels = nullptr;
		uint32_t cpu_stride = 0;
	};

	// One CPU readback: a pixel pack buffer and the fence after its read.
	struct Readback {
		GLuint buffer = 0;
		size_t size = 0;
		GLsync fence = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
	};

	IhsEgl *egl = nullptr;
	HashMap<uint32_t, Target> targets;
	Readback readbacks[2];
	uint32_t readback_next = 0;

	void _release(Target &p_target);
	void _drop_readbacks();
	void _capture_cpu(GLuint p_fbo, const Size2i &p_size, uint32_t p_id, const Target &p_target);
	int _import_dmabuf(const IhsGodotTarget &p_target, Target &r_target);
	int _fence();

public:
	explicit IhsCaptureGLES3(IhsEgl *p_egl);
	~IhsCaptureGLES3() override;

	uint32_t get_api() const override;
	int add_target(uint32_t p_id, const IhsGodotTarget &p_target) override;
	void remove_target(uint32_t p_id) override;
	void remove_all_targets() override;
	size_t query_dmabuf_formats(IhsGodotFormat *r_out, size_t p_max) override;
	void fill_renderer_info(IhsGodotRendererInfo &r_info) override;

	void capture(const RenderingServerTypes::BlitToScreen *p_blits, int p_count);
};

class IhsRasterizerGLES3 : public RasterizerGLES3 {
public:
	static IhsCaptureGLES3 *capture_backend;

	static RendererCompositor *_create_current() {
		return memnew(IhsRasterizerGLES3);
	}
	static void make_current(bool p_gles_over_gl) {
		RasterizerGLES3::make_current(p_gles_over_gl);
		_create_func = _create_current;
	}

	void set_boot_image_with_stretch(const Ref<Image> &p_image, const Color &p_color, RSE::SplashStretchMode p_stretch_mode, bool p_use_filter = true) override {}
	void blit_render_targets_to_screen(DisplayServerEnums::WindowID p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) override;
};

#endif // GLES3_ENABLED && EGL_ENABLED
