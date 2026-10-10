/**************************************************************************/
/*  ihs_egl.h                                                             */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)

#include "thirdparty/glad/glad/egl.h"

#include <cstdint>

// EGL extension entry points and tokens Godot's bundled glad does not carry.
// Resolved with eglGetProcAddress, so nothing links against libEGL directly.
typedef void *IhsEGLImageKHR;
typedef void *IhsEGLSyncKHR;
typedef void *IhsEGLDeviceEXT;

#define IHS_EGL_LINUX_DMA_BUF_EXT 0x3270
#define IHS_EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define IHS_EGL_PLATFORM_DEVICE_EXT 0x313F
#define IHS_EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define IHS_EGL_PLATFORM_GBM_KHR 0x31D7
#define IHS_EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#define IHS_EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define IHS_EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#define IHS_EGL_CONTEXT_MAJOR_VERSION 0x3098
#define IHS_EGL_CONTEXT_MINOR_VERSION 0x30FB
#define IHS_EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define IHS_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#define IHS_EGL_OPENGL_ES3_BIT 0x00000040

// Per-plane dma-buf import attributes, indexed by plane.
static const EGLint IHS_EGL_PLANE_FD[4] = { 0x3272, 0x3275, 0x3278, 0x3440 };
static const EGLint IHS_EGL_PLANE_OFFSET[4] = { 0x3273, 0x3276, 0x3279, 0x3441 };
static const EGLint IHS_EGL_PLANE_PITCH[4] = { 0x3274, 0x3277, 0x327A, 0x3442 };
static const EGLint IHS_EGL_PLANE_MOD_LO[4] = { 0x3443, 0x3445, 0x3447, 0x3449 };
static const EGLint IHS_EGL_PLANE_MOD_HI[4] = { 0x3444, 0x3446, 0x3448, 0x344A };

// A surfaceless EGL context on the GPU the shell imports on: Godot's
// compatibility renderer runs on it, and targets are imported into it.
class IhsEgl {
public:
	typedef IhsEGLImageKHR (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
	typedef EGLBoolean (*DestroyImageKHR)(EGLDisplay, IhsEGLImageKHR);
	typedef IhsEGLSyncKHR (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
	typedef EGLBoolean (*DestroySyncKHR)(EGLDisplay, IhsEGLSyncKHR);
	typedef EGLint (*DupNativeFenceFDANDROID)(EGLDisplay, IhsEGLSyncKHR);
	typedef EGLBoolean (*QueryDmaBufModifiersEXT)(EGLDisplay, EGLint, EGLint, uint64_t *, EGLBoolean *, EGLint *);
	typedef EGLBoolean (*QueryDevicesEXT)(EGLint, IhsEGLDeviceEXT *, EGLint *);
	typedef const char *(*QueryDeviceStringEXT)(IhsEGLDeviceEXT, EGLint);
	typedef EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
	typedef void (*ImageTargetTexture2DOES)(unsigned int, void *);

	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	EGLConfig config = nullptr;
	EGLSurface surface = EGL_NO_SURFACE;
	bool gles = false;
	uint64_t render_device = 0;
	// The GBM platform's device (Vivante, ...): the render node and the
	// gbm_device opened on it, kept for the display's life.
	int gbm_fd = -1;
	void *gbm_device = nullptr;

	CreateImageKHR create_image = nullptr;
	DestroyImageKHR destroy_image = nullptr;
	CreateSyncKHR create_sync = nullptr;
	DestroySyncKHR destroy_sync = nullptr;
	DupNativeFenceFDANDROID dup_native_fence = nullptr;
	QueryDmaBufModifiersEXT query_modifiers = nullptr;
	ImageTargetTexture2DOES image_target_texture = nullptr;

	bool has_dmabuf_import = false;
	bool has_dmabuf_modifiers = false;
	bool has_native_fence = false;

	// Create the display and context and make it current on this thread.
	// @p_gles selects GLES 3.0 over desktop GL 3.3 core.
	bool initialize(bool p_gles, uint64_t p_render_device);
	void finalize();
	bool make_current();
};

#endif // GLES3_ENABLED && EGL_ENABLED
