/**************************************************************************/
/*  ihs_capture_gles3.cpp                                                 */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_capture_gles3.h"

#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)

#include "ihs_fourcc.h"

#include "drivers/gles3/storage/texture_storage.h"

#include <unistd.h>
#include <cstring>

IhsCaptureGLES3 *IhsRasterizerGLES3::capture_backend = nullptr;

IhsCaptureGLES3::IhsCaptureGLES3(IhsEgl *p_egl) :
		egl(p_egl) {
	IhsRasterizerGLES3::capture_backend = this;
}

IhsCaptureGLES3::~IhsCaptureGLES3() {
	remove_all_targets();
	_drop_readbacks();
	if (IhsRasterizerGLES3::capture_backend == this) {
		IhsRasterizerGLES3::capture_backend = nullptr;
	}
}

// On panfrost (RK3566, Linux 6.1 BSP) GL work still queued when the context
// and its buffers go away faults the GPU MMU at exit.
void IhsCaptureGLES3::wait_idle() {
	glFinish();
}

uint32_t IhsCaptureGLES3::get_api() const {
	return egl->gles ? IHS_GODOT_API_GLES : IHS_GODOT_API_OPENGL;
}

void IhsCaptureGLES3::_release(Target &p_target) {
	if (p_target.fbo) {
		glDeleteFramebuffers(1, &p_target.fbo);
		p_target.fbo = 0;
	}
	if (p_target.texture) {
		glDeleteTextures(1, &p_target.texture);
		p_target.texture = 0;
	}
	// GL keeps the texture's storage alive while anything still uses it, so
	// the image can go right away.
	if (p_target.image) {
		egl->destroy_image(egl->display, p_target.image);
		p_target.image = nullptr;
	}
}

void IhsCaptureGLES3::_drop_readbacks() {
	for (Readback &r : readbacks) {
		if (r.fence) {
			glDeleteSync(r.fence);
		}
		if (r.buffer) {
			glDeleteBuffers(1, &r.buffer);
		}
		r = Readback();
	}
}

int IhsCaptureGLES3::_import_dmabuf(const IhsGodotTarget &p_target, Target &r_target) {
	if (!egl->has_dmabuf_import) {
		return IHS_GODOT_ERR_UNSUPPORTED;
	}
	if (p_target.plane_count < 1 || p_target.plane_count > 4) {
		return IHS_GODOT_ERR_INVALID;
	}
	const bool explicit_modifier = p_target.modifier != IHS_MOD_INVALID;
	if (explicit_modifier && p_target.modifier != IHS_MOD_LINEAR && !egl->has_dmabuf_modifiers) {
		IHS_LOG_ERROR("dma-buf modifier needs EGL_EXT_image_dma_buf_import_modifiers");
		return IHS_GODOT_ERR_UNSUPPORTED;
	}

	EGLint attribs[64];
	int n = 0;
	attribs[n++] = EGL_WIDTH;
	attribs[n++] = EGLint(p_target.width);
	attribs[n++] = EGL_HEIGHT;
	attribs[n++] = EGLint(p_target.height);
	attribs[n++] = IHS_EGL_LINUX_DRM_FOURCC_EXT;
	attribs[n++] = EGLint(p_target.fourcc);
	for (uint32_t i = 0; i < p_target.plane_count; i++) {
		attribs[n++] = IHS_EGL_PLANE_FD[i];
		attribs[n++] = p_target.plane_fd[i];
		attribs[n++] = IHS_EGL_PLANE_OFFSET[i];
		attribs[n++] = EGLint(p_target.plane_offset[i]);
		attribs[n++] = IHS_EGL_PLANE_PITCH[i];
		attribs[n++] = EGLint(p_target.plane_stride[i]);
		if (explicit_modifier && egl->has_dmabuf_modifiers) {
			attribs[n++] = IHS_EGL_PLANE_MOD_LO[i];
			attribs[n++] = EGLint(p_target.modifier & 0xffffffff);
			attribs[n++] = IHS_EGL_PLANE_MOD_HI[i];
			attribs[n++] = EGLint(p_target.modifier >> 32);
		}
	}
	attribs[n++] = EGL_NONE;

	// EGL dup()s the fds it keeps; ours stay the caller's.
	r_target.image = egl->create_image(egl->display, EGL_NO_CONTEXT, IHS_EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
	if (!r_target.image) {
		IHS_LOG_ERROR(vformat("eglCreateImageKHR for dma-buf %dx%d failed (0x%x)", p_target.width, p_target.height, eglGetError()));
		return IHS_GODOT_ERR_IMPORT;
	}
	glGenTextures(1, &r_target.texture);
	glBindTexture(GL_TEXTURE_2D, r_target.texture);
	egl->image_target_texture(GL_TEXTURE_2D, r_target.image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &r_target.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, r_target.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r_target.texture, 0);
	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, GLES3::TextureStorage::system_fbo);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		// Importable is not renderable: some drivers sample a modifier they
		// cannot draw to.
		IHS_LOG_ERROR(vformat("dma-buf framebuffer incomplete (0x%x)", status));
		_release(r_target);
		return IHS_GODOT_ERR_IMPORT;
	}
	return IHS_GODOT_OK;
}

int IhsCaptureGLES3::add_target(uint32_t p_id, const IhsGodotTarget &p_target) {
	if (p_id == IHS_GODOT_NO_TARGET || p_target.width == 0 || p_target.height == 0 || !ihs_fourcc_supported(p_target.fourcc)) {
		return IHS_GODOT_ERR_INVALID;
	}
	remove_target(p_id);

	Target t;
	t.type = p_target.type;
	t.width = p_target.width;
	t.height = p_target.height;
	t.fourcc = p_target.fourcc;

	switch (p_target.type) {
		case IHS_GODOT_TARGET_DMABUF: {
			const int rc = _import_dmabuf(p_target, t);
			if (rc != IHS_GODOT_OK) {
				return rc;
			}
		} break;
		case IHS_GODOT_TARGET_CPU:
			if (!p_target.pixels || p_target.cpu_stride < p_target.width * 4 || (p_target.cpu_stride % 4) != 0) {
				return IHS_GODOT_ERR_INVALID;
			}
			t.pixels = p_target.pixels;
			t.cpu_stride = p_target.cpu_stride;
			break;
		default:
			// OPAQUE_FD would need GL_EXT_memory_object_fd; the producer uses a
			// dma-buf with a GL renderer instead.
			return IHS_GODOT_ERR_UNSUPPORTED;
	}
	targets.insert(p_id, t);
	return IHS_GODOT_OK;
}

void IhsCaptureGLES3::remove_target(uint32_t p_id) {
	HashMap<uint32_t, Target>::Iterator it = targets.find(p_id);
	if (!it) {
		return;
	}
	_release(it->value);
	targets.remove(it);
}

void IhsCaptureGLES3::remove_all_targets() {
	for (KeyValue<uint32_t, Target> &kv : targets) {
		_release(kv.value);
	}
	targets.clear();
}

size_t IhsCaptureGLES3::query_dmabuf_formats(IhsGodotFormat *r_out, size_t p_max) {
	if (!egl->has_dmabuf_import) {
		return 0;
	}
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
		if (!egl->has_dmabuf_modifiers) {
			emit(fourcc, IHS_MOD_LINEAR);
			continue;
		}
		EGLint num = 0;
		if (!egl->query_modifiers(egl->display, EGLint(fourcc), 0, nullptr, nullptr, &num) || num <= 0) {
			continue;
		}
		LocalVector<uint64_t> mods;
		LocalVector<EGLBoolean> external_only;
		mods.resize(num);
		external_only.resize(num);
		if (!egl->query_modifiers(egl->display, EGLint(fourcc), num, mods.ptr(), external_only.ptr(), &num)) {
			continue;
		}
		for (EGLint i = 0; i < num; i++) {
			// External-only images can be sampled but not rendered to.
			if (!external_only[i]) {
				emit(fourcc, mods[i]);
			}
		}
	}
	return count;
}

void IhsCaptureGLES3::fill_renderer_info(IhsGodotRendererInfo &r_info) {
	r_info.api = get_api();
	r_info.egl_display = egl->display;
	r_info.egl_context = egl->context;
	r_info.target_types = 1u << IHS_GODOT_TARGET_CPU;
	if (egl->has_dmabuf_import) {
		r_info.target_types |= 1u << IHS_GODOT_TARGET_DMABUF;
	}
	r_info.dmabuf_modifiers = egl->has_dmabuf_modifiers ? 1 : 0;
	r_info.explicit_sync = egl->has_native_fence ? 1 : 0;
	r_info.render_device = egl->render_device;
}

int IhsCaptureGLES3::_fence() {
	if (egl->has_native_fence) {
		const EGLint attribs[] = { EGL_NONE };
		IhsEGLSyncKHR sync = egl->create_sync(egl->display, IHS_EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
		if (sync) {
			// The fence fd exists only once the sync is flushed.
			glFlush();
			const int fd = egl->dup_native_fence(egl->display, sync);
			egl->destroy_sync(egl->display, sync);
			if (fd >= 0) {
				return fd;
			}
		}
	}
	glFinish();
	return -1;
}

void IhsCaptureGLES3::capture(const RenderingServerTypes::BlitToScreen *p_blits, int p_count) {
	IhsState &state = IhsState::get();
	if (p_count < 1 || state.next_target == IHS_GODOT_NO_TARGET) {
		return;
	}
	Target *t = targets.getptr(state.next_target);
	if (!t) {
		return;
	}
	GLES3::RenderTarget *rt = GLES3::TextureStorage::get_singleton()->get_render_target(p_blits[0].render_target);
	if (!rt || !rt->fbo) {
		return;
	}
	const Size2i src = rt->size;

	if (t->type == IHS_GODOT_TARGET_CPU) {
		_capture_cpu(rt->fbo, src, state.next_target, *t);
		return;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
	glDisable(GL_SCISSOR_TEST);
	glBlitFramebuffer(0, 0, src.width, src.height, 0, 0, GLint(t->width), GLint(t->height), GL_COLOR_BUFFER_BIT, GL_LINEAR);
	glBindFramebuffer(GL_FRAMEBUFFER, GLES3::TextureStorage::system_fbo);

	if (state.capture.fence_fd >= 0) {
		close(state.capture.fence_fd); // An earlier capture this iteration nobody took.
	}
	state.capture.target_id = state.next_target;
	state.capture.fence_fd = _fence();
	state.capture.width = t->width;
	state.capture.height = t->height;
}

void IhsCaptureGLES3::_capture_cpu(GLuint p_fbo, const Size2i &p_size, uint32_t p_id, const Target &p_target) {
	// Start this frame's read into a pack buffer; it completes on the GPU.
	Readback &cur = readbacks[readback_next];
	Readback &prev = readbacks[readback_next ^ 1];
	readback_next ^= 1;

	const uint32_t w = MIN(uint32_t(p_size.width), p_target.width);
	const uint32_t h = MIN(uint32_t(p_size.height), p_target.height);
	const size_t size = size_t(w) * h * 4;
	if (cur.fence) {
		glDeleteSync(cur.fence); // Never collected (a frame without a CPU target).
		cur.fence = nullptr;
	}
	if (!cur.buffer) {
		glGenBuffers(1, &cur.buffer);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, cur.buffer);
	if (cur.size < size) {
		glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(size), nullptr, GL_STREAM_READ);
		cur.size = size;
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, p_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, GLES3::TextureStorage::system_fbo);
	cur.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	cur.width = w;
	cur.height = h;
	// Submit it now, so it runs while the CPU copies out the previous one.
	glFlush();

	// Copy out the previous frame's read: a frame old, so normally done.
	if (!prev.fence) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		return;
	}
	const GLenum waited = glClientWaitSync(prev.fence, GL_SYNC_FLUSH_COMMANDS_BIT, GLuint64(1000000000));
	glDeleteSync(prev.fence);
	prev.fence = nullptr;
	if (waited == GL_TIMEOUT_EXPIRED || waited == GL_WAIT_FAILED) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		return;
	}
	// A resize since then crops it.
	const uint32_t cw = MIN(prev.width, p_target.width);
	const uint32_t ch = MIN(prev.height, p_target.height);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, prev.buffer);
	const uint8_t *src = static_cast<const uint8_t *>(
			glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(size_t(prev.width) * prev.height * 4), GL_MAP_READ_BIT));
	if (!src) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		return;
	}
	const size_t src_row = size_t(prev.width) * 4;
	const bool swap_rb = !ihs_fourcc_is_rgba(p_target.fourcc);
	for (uint32_t y = 0; y < ch; y++) {
		const uint8_t *s = src + y * src_row;
		uint8_t *d = static_cast<uint8_t *>(p_target.pixels) + size_t(y) * p_target.cpu_stride;
		if (!swap_rb) {
			memcpy(d, s, size_t(cw) * 4);
			continue;
		}
		// B,G,R,A target.
		for (uint32_t x = 0; x < cw; x++) {
			d[x * 4 + 0] = s[x * 4 + 2];
			d[x * 4 + 1] = s[x * 4 + 1];
			d[x * 4 + 2] = s[x * 4 + 0];
			d[x * 4 + 3] = s[x * 4 + 3];
		}
	}
	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	IhsState &state = IhsState::get();
	state.capture.target_id = p_id;
	state.capture.fence_fd = -1; // Copied on the CPU: complete.
	state.capture.width = cw;
	state.capture.height = ch;
}

void IhsRasterizerGLES3::blit_render_targets_to_screen(DisplayServerEnums::WindowID p_screen, const RenderingServerTypes::BlitToScreen *p_render_targets, int p_amount) {
	if (p_screen != DisplayServerEnums::MAIN_WINDOW_ID || !capture_backend) {
		return;
	}
	capture_backend->capture(p_render_targets, p_amount);
}

#endif // GLES3_ENABLED && EGL_ENABLED
