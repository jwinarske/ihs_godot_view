/**************************************************************************/
/*  ihs_egl.cpp                                                           */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_egl.h"

#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)

#include "ihs_state.h"

#include "core/variant/variant.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace {

bool _has(const char *p_list, const char *p_name) {
	if (!p_list) {
		return false;
	}
	const size_t len = strlen(p_name);
	for (const char *s = strstr(p_list, p_name); s; s = strstr(s + 1, p_name)) {
		const bool start = s == p_list || s[-1] == ' ';
		const bool end = s[len] == '\0' || s[len] == ' ';
		if (start && end) {
			return true;
		}
	}
	return false;
}

template <typename T>
T _proc(const char *p_name) {
	return reinterpret_cast<T>(eglGetProcAddress(p_name));
}

uint64_t _rdev(const char *p_path) {
	struct stat st;
	if (!p_path || stat(p_path, &st) != 0) {
		return 0;
	}
	return uint64_t(st.st_rdev);
}

// The /dev/dri/renderD* whose device number is @p_rdev, opened read-write.
int _open_render_node(uint64_t p_rdev) {
	DIR *dir = opendir("/dev/dri");
	if (!dir) {
		return -1;
	}
	int fd = -1;
	while (const dirent *e = readdir(dir)) {
		if (strncmp(e->d_name, "renderD", 7) != 0) {
			continue;
		}
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
		if (_rdev(path) == p_rdev) {
			fd = open(path, O_RDWR | O_CLOEXEC);
			break;
		}
	}
	closedir(dir);
	return fd;
}

} // namespace

bool IhsEgl::initialize(bool p_gles, uint64_t p_render_device) {
	gles = p_gles;
	if (!gladLoaderLoadEGL(EGL_NO_DISPLAY)) {
		IHS_LOG_ERROR("cannot load libEGL");
		return false;
	}
	const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	auto get_platform_display = _proc<GetPlatformDisplayEXT>("eglGetPlatformDisplayEXT");

	// 1. The EGL device whose render node is the shell's.
	if (p_render_device != 0 && get_platform_display && _has(client, "EGL_EXT_device_enumeration") &&
			_has(client, "EGL_EXT_platform_device")) {
		auto query_devices = _proc<QueryDevicesEXT>("eglQueryDevicesEXT");
		auto query_string = _proc<QueryDeviceStringEXT>("eglQueryDeviceStringEXT");
		IhsEGLDeviceEXT devices[16];
		EGLint count = 0;
		if (query_devices && query_string && query_devices(16, devices, &count)) {
			for (EGLint i = 0; i < count && display == EGL_NO_DISPLAY; i++) {
				const char *exts = query_string(devices[i], EGL_EXTENSIONS);
				if (!_has(exts, "EGL_EXT_device_drm_render_node")) {
					continue;
				}
				const char *node = query_string(devices[i], IHS_EGL_DRM_RENDER_NODE_FILE_EXT);
				if (_rdev(node) == p_render_device) {
					display = get_platform_display(IHS_EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
					render_device = p_render_device;
					IHS_LOG_INFO(vformat("EGL display on %s (the shell's render node)", node));
				}
			}
		}
	}
	// 2. Mesa's surfaceless platform: the default GPU, no window system.
	if (display == EGL_NO_DISPLAY && get_platform_display && _has(client, "EGL_MESA_platform_surfaceless")) {
		display = get_platform_display(IHS_EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
	}
	// 3. The GBM platform on the shell's render node, for an EGL with neither
	// of the above (Vivante). libgbm is loaded here, not linked.
	if (display == EGL_NO_DISPLAY && p_render_device != 0 && get_platform_display &&
			(_has(client, "EGL_KHR_platform_gbm") || _has(client, "EGL_MESA_platform_gbm"))) {
		void *libgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
		typedef void *(*GbmCreateDevice)(int);
		auto gbm_create = libgbm ? reinterpret_cast<GbmCreateDevice>(dlsym(libgbm, "gbm_create_device")) : nullptr;
		gbm_fd = gbm_create ? _open_render_node(p_render_device) : -1;
		gbm_device = gbm_fd >= 0 ? gbm_create(gbm_fd) : nullptr;
		if (!gbm_device && gbm_fd >= 0) {
			close(gbm_fd);
			gbm_fd = -1;
		}
		if (gbm_device) {
			display = get_platform_display(IHS_EGL_PLATFORM_GBM_KHR, gbm_device, nullptr);
			render_device = p_render_device;
			IHS_LOG_INFO("EGL display on the GBM platform (the shell's render node)");
		}
	}
	// 4. Whatever the default display is.
	if (display == EGL_NO_DISPLAY) {
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}
	if (display == EGL_NO_DISPLAY) {
		IHS_LOG_ERROR("no EGL display");
		return false;
	}

	EGLint major = 0, minor = 0;
	if (!eglInitialize(display, &major, &minor)) {
		IHS_LOG_ERROR("eglInitialize failed");
		display = EGL_NO_DISPLAY;
		return false;
	}
	// Reload with the display so the version-dependent entry points resolve.
	gladLoaderLoadEGL(display);
	const char *exts = eglQueryString(display, EGL_EXTENSIONS);

	if (!eglBindAPI(gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API)) {
		IHS_LOG_ERROR(gles ? "EGL has no OpenGL ES API" : "EGL has no desktop OpenGL API");
		finalize();
		return false;
	}

	const bool surfaceless = _has(exts, "EGL_KHR_surfaceless_context");
	const bool no_config = _has(exts, "EGL_KHR_no_config_context") || _has(exts, "EGL_MESA_configless_context");
	if (!surfaceless || !no_config) {
		const EGLint attribs[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, gles ? IHS_EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_NONE
		};
		EGLint n = 0;
		if (!eglChooseConfig(display, attribs, &config, 1, &n) || n < 1) {
			IHS_LOG_ERROR("no EGL config");
			finalize();
			return false;
		}
	}

	const EGLint gl_attribs[] = {
		IHS_EGL_CONTEXT_MAJOR_VERSION, 3,
		IHS_EGL_CONTEXT_MINOR_VERSION, 3,
		IHS_EGL_CONTEXT_OPENGL_PROFILE_MASK, IHS_EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE
	};
	const EGLint gles_attribs[] = {
		IHS_EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_NONE
	};
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, gles ? gles_attribs : gl_attribs);
	if (context == EGL_NO_CONTEXT) {
		IHS_LOG_ERROR(vformat("eglCreateContext failed (0x%x)", eglGetError()));
		finalize();
		return false;
	}
	if (!surfaceless) {
		const EGLint pbuffer[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
		surface = eglCreatePbufferSurface(display, config, pbuffer);
		if (surface == EGL_NO_SURFACE) {
			IHS_LOG_ERROR("eglCreatePbufferSurface failed");
			finalize();
			return false;
		}
	}
	if (!make_current()) {
		IHS_LOG_ERROR("eglMakeCurrent failed");
		finalize();
		return false;
	}

	create_image = _proc<CreateImageKHR>("eglCreateImageKHR");
	destroy_image = _proc<DestroyImageKHR>("eglDestroyImageKHR");
	image_target_texture = _proc<ImageTargetTexture2DOES>("glEGLImageTargetTexture2DOES");
	has_dmabuf_import = _has(exts, "EGL_EXT_image_dma_buf_import") && create_image && destroy_image && image_target_texture;
	if (has_dmabuf_import && _has(exts, "EGL_EXT_image_dma_buf_import_modifiers")) {
		query_modifiers = _proc<QueryDmaBufModifiersEXT>("eglQueryDmaBufModifiersEXT");
		has_dmabuf_modifiers = query_modifiers != nullptr;
	}
	if (_has(exts, "EGL_ANDROID_native_fence_sync") && _has(exts, "EGL_KHR_fence_sync")) {
		create_sync = _proc<CreateSyncKHR>("eglCreateSyncKHR");
		destroy_sync = _proc<DestroySyncKHR>("eglDestroySyncKHR");
		dup_native_fence = _proc<DupNativeFenceFDANDROID>("eglDupNativeFenceFDANDROID");
		has_native_fence = create_sync && destroy_sync && dup_native_fence;
	}
	IHS_LOG_INFO(vformat("EGL %d.%d %s context: dma-buf import %s, modifiers %s, native fence %s", major, minor,
			gles ? "GLES 3.0" : "GL 3.3 core", has_dmabuf_import ? "yes" : "no",
			has_dmabuf_modifiers ? "yes" : "no", has_native_fence ? "yes" : "no"));
	return true;
}

bool IhsEgl::make_current() {
	return eglMakeCurrent(display, surface, surface, context) == EGL_TRUE;
}

void IhsEgl::finalize() {
	if (display == EGL_NO_DISPLAY) {
		return;
	}
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (surface != EGL_NO_SURFACE) {
		eglDestroySurface(display, surface);
		surface = EGL_NO_SURFACE;
	}
	if (context != EGL_NO_CONTEXT) {
		eglDestroyContext(display, context);
		context = EGL_NO_CONTEXT;
	}
	eglTerminate(display);
	display = EGL_NO_DISPLAY;
	// The gbm_device and its fd are left open: Vivante's libEGL keeps every
	// device handed to eglGetPlatformDisplay and destroys it itself at exit.
}

#endif // GLES3_ENABLED && EGL_ENABLED
