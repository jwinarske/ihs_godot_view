/**************************************************************************/
/*  display_server_ihs.cpp                                                */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "display_server_ihs.h"

#include "ihs_capture_gles3.h"
#include "ihs_capture_rd.h"
#include "ihs_egl.h"
#include "ihs_state.h"
#include "ihs_vulkan_hooks.h"

#include "core/config/project_settings.h"
#include "core/input/input.h"
#include "core/os/os.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"

#include <unistd.h>

#if defined(RD_ENABLED)
#include "servers/rendering/rendering_device.h"
#endif
#if defined(VULKAN_ENABLED)
#include "drivers/vulkan/rendering_context_driver_vulkan.h"
#endif

DisplayServerIhs *DisplayServerIhs::get_ihs_singleton() {
	DisplayServer *ds = DisplayServer::get_singleton();
	if (!ds || ds->get_name() != IHS_GODOT_DISPLAY_DRIVER) {
		return nullptr;
	}
	return static_cast<DisplayServerIhs *>(ds);
}

Vector<String> DisplayServerIhs::get_rendering_drivers_func() {
	Vector<String> drivers;
#if defined(VULKAN_ENABLED)
	drivers.push_back("vulkan");
#endif
#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)
	drivers.push_back("opengl3");
	drivers.push_back("opengl3_es");
#endif
	drivers.push_back("dummy");
	return drivers;
}

DisplayServer *DisplayServerIhs::create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	DisplayServerIhs *ds = memnew(DisplayServerIhs(p_rendering_driver, r_error));
	if (r_error != OK) {
		ERR_PRINT("Can't create the ihs display server.");
		memdelete(ds);
		return nullptr;
	}
	return ds;
}

void DisplayServerIhs::register_ihs_driver() {
	static bool registered = false;
	if (registered) {
		return;
	}
	registered = true;
	DisplayServer::register_create_function(IHS_GODOT_DISPLAY_DRIVER, create_func, get_rendering_drivers_func);
}

bool DisplayServerIhs::_init_vulkan() {
#if defined(RD_ENABLED) && defined(VULKAN_ENABLED)
	vulkan_hooks = memnew(IhsVulkanHooks);
	rendering_context = memnew(RenderingContextDriverVulkan);
	if (rendering_context->initialize() != OK) {
		memdelete(rendering_context);
		rendering_context = nullptr;
		memdelete(vulkan_hooks);
		vulkan_hooks = nullptr;
		IHS_LOG_WARN("Vulkan context initialization failed");
		return false;
	}
	// No window: RenderingDevice then runs without a surface, with a single
	// frame in flight, which is what lets the capture skip fences.
	rendering_device = memnew(RenderingDevice);
	if (rendering_device->initialize(rendering_context, DisplayServerEnums::INVALID_WINDOW_ID) != OK) {
		memdelete(rendering_device);
		rendering_device = nullptr;
		memdelete(rendering_context);
		rendering_context = nullptr;
		memdelete(vulkan_hooks);
		vulkan_hooks = nullptr;
		IHS_LOG_WARN("RenderingDevice initialization failed");
		return false;
	}
	IhsRendererCompositorRD::make_current();
	capture_backend = memnew(IhsCaptureRD(vulkan_hooks));
	return true;
#else
	return false;
#endif
}

bool DisplayServerIhs::_init_gl(bool p_gles) {
#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)
	egl = memnew(IhsEgl);
	if (!egl->initialize(p_gles, IhsState::get().render_device)) {
		memdelete(egl);
		egl = nullptr;
		return false;
	}
	IhsRasterizerGLES3::make_current(!p_gles);
	capture_backend = memnew(IhsCaptureGLES3(egl));
	return true;
#else
	return false;
#endif
}

DisplayServerIhs::DisplayServerIhs(const String &p_rendering_driver, Error &r_error) {
	r_error = OK;
	rendering_driver = p_rendering_driver;
	bool ok = false;

	if (rendering_driver == "vulkan") {
		ok = _init_vulkan();
		if (!ok) {
#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)
			if (GLOBAL_GET("rendering/rendering_device/fallback_to_opengl3")) {
				IHS_LOG_WARN("Vulkan unavailable, falling back to OpenGL 3");
				rendering_driver = "opengl3";
				OS::get_singleton()->set_current_rendering_method("gl_compatibility", OS::RENDERING_SOURCE_FALLBACK);
				OS::get_singleton()->set_current_rendering_driver_name(rendering_driver, OS::RENDERING_SOURCE_FALLBACK);
			}
#endif
		}
	}
	if (!ok && rendering_driver == "opengl3") {
		ok = _init_gl(false);
		if (!ok && GLOBAL_GET("rendering/gl_compatibility/fallback_to_gles")) {
			IHS_LOG_WARN("desktop OpenGL unavailable, falling back to OpenGL ES");
			rendering_driver = "opengl3_es";
			OS::get_singleton()->set_current_rendering_driver_name(rendering_driver, OS::RENDERING_SOURCE_FALLBACK);
		}
	}
	if (!ok && rendering_driver == "opengl3_es") {
		ok = _init_gl(true);
	}
	if (!ok && rendering_driver == "dummy") {
		RasterizerDummy::make_current();
		ok = true;
	}
	if (!ok) {
		IHS_LOG_ERROR("no renderer could be created for driver '" + p_rendering_driver + "'");
		r_error = ERR_UNAVAILABLE;
		return;
	}
	IhsState::get().backend = capture_backend;
	IHS_LOG_INFO("display server up, rendering driver " + rendering_driver);
}

DisplayServerIhs::~DisplayServerIhs() {
	IhsState &state = IhsState::get();
	if (state.backend == capture_backend) {
		state.backend = nullptr;
	}
	if (state.capture.fence_fd >= 0) {
		::close(state.capture.fence_fd);
		state.capture = IhsState::Capture();
	}

#if defined(RD_ENABLED)
	if (rendering_device) {
		if (capture_backend) {
			capture_backend->remove_all_targets();
			// Run one frame boundary so RD disposes of the texture wrappers
			// before the backend destroys the images under them.
			rendering_device->submit();
			rendering_device->sync();
			memdelete(capture_backend);
			capture_backend = nullptr;
		}
		memdelete(rendering_device); // Its destructor finalizes.
		rendering_device = nullptr;
	}
	if (rendering_context) {
		memdelete(rendering_context);
		rendering_context = nullptr;
	}
#endif
#if defined(VULKAN_ENABLED)
	if (vulkan_hooks) {
		memdelete(vulkan_hooks);
		vulkan_hooks = nullptr;
	}
#endif
#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)
	if (egl) {
		egl->make_current();
		if (capture_backend) {
			memdelete(capture_backend);
			capture_backend = nullptr;
		}
		egl->finalize();
		memdelete(egl);
		egl = nullptr;
	}
#endif
	if (capture_backend) {
		memdelete(capture_backend);
		capture_backend = nullptr;
	}
}

void DisplayServerIhs::set_window_size(const Size2i &p_size, float p_scale) {
	IhsState &state = IhsState::get();
	const bool resized = state.window_size != p_size;
	const bool rescaled = state.window_scale != p_scale;
	state.window_size = p_size;
	state.window_scale = p_scale;
	if (resized && rect_changed_callback.is_valid()) {
		rect_changed_callback.call(Rect2i(Point2i(), p_size));
	}
	if (rescaled && window_event_callback.is_valid()) {
		window_event_callback.call(DisplayServerEnums::WINDOW_EVENT_DPI_CHANGE);
	}
}

void DisplayServerIhs::set_focused(bool p_focused) {
	if (window_event_callback.is_valid()) {
		window_event_callback.call(p_focused ? DisplayServerEnums::WINDOW_EVENT_FOCUS_IN : DisplayServerEnums::WINDOW_EVENT_FOCUS_OUT);
	}
}

bool DisplayServerIhs::has_feature(DisplayServerEnums::Feature p_feature) const {
	switch (p_feature) {
		case DisplayServerEnums::FEATURE_TOUCHSCREEN:
		case DisplayServerEnums::FEATURE_MOUSE:
		case DisplayServerEnums::FEATURE_HIDPI:
		case DisplayServerEnums::FEATURE_EMBEDDED:
			return true;
		default:
			return false;
	}
}

Size2i DisplayServerIhs::screen_get_size(int p_screen) const {
	return IhsState::get().window_size;
}

Rect2i DisplayServerIhs::screen_get_usable_rect(int p_screen) const {
	return Rect2i(Point2i(), IhsState::get().window_size);
}

int DisplayServerIhs::screen_get_dpi(int p_screen) const {
	return int(96.0f * IhsState::get().window_scale);
}

float DisplayServerIhs::screen_get_scale(int p_screen) const {
	return IhsState::get().window_scale;
}

float DisplayServerIhs::screen_get_max_scale() const {
	return IhsState::get().window_scale;
}

Vector<DisplayServerEnums::WindowID> DisplayServerIhs::get_window_list() const {
	Vector<DisplayServerEnums::WindowID> list;
	list.push_back(DisplayServerEnums::MAIN_WINDOW_ID);
	return list;
}

void DisplayServerIhs::window_set_rect_changed_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window) {
	if (p_window == DisplayServerEnums::MAIN_WINDOW_ID) {
		rect_changed_callback = p_callable;
	}
}

void DisplayServerIhs::window_set_window_event_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window) {
	if (p_window == DisplayServerEnums::MAIN_WINDOW_ID) {
		window_event_callback = p_callable;
	}
}

Size2i DisplayServerIhs::window_get_size(DisplayServerEnums::WindowID p_window) const {
	return IhsState::get().window_size;
}

Size2i DisplayServerIhs::window_get_size_with_decorations(DisplayServerEnums::WindowID p_window) const {
	return IhsState::get().window_size;
}

DisplayServerEnums::WindowMode DisplayServerIhs::window_get_mode(DisplayServerEnums::WindowID p_window) const {
	return IhsState::get().visible ? DisplayServerEnums::WINDOW_MODE_WINDOWED : DisplayServerEnums::WINDOW_MODE_MINIMIZED;
}

bool DisplayServerIhs::window_can_draw(DisplayServerEnums::WindowID p_window) const {
	return capture_backend != nullptr && IhsState::get().visible;
}

bool DisplayServerIhs::can_any_window_draw() const {
	return window_can_draw(DisplayServerEnums::MAIN_WINDOW_ID);
}

int64_t DisplayServerIhs::window_get_native_handle(DisplayServerEnums::HandleType p_handle_type, DisplayServerEnums::WindowID p_window) const {
#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)
	if (egl) {
		switch (p_handle_type) {
			case DisplayServerEnums::OPENGL_CONTEXT:
				return int64_t(egl->context);
			case DisplayServerEnums::EGL_DISPLAY:
				return int64_t(egl->display);
			case DisplayServerEnums::EGL_CONFIG:
				return int64_t(egl->config);
			default:
				break;
		}
	}
#endif
	return 0;
}

void DisplayServerIhs::mouse_set_mode(DisplayServerEnums::MouseMode p_mode) {
	IhsState &state = IhsState::get();
	if (state.mouse_mode == uint32_t(p_mode)) {
		return;
	}
	// There is no OS pointer to capture; the host decides what capture means
	// (hide its cursor, keep feeding deltas).
	state.mouse_mode = uint32_t(p_mode);
	if (state.callbacks.mouse_mode) {
		state.callbacks.mouse_mode(state.callbacks.user_data, state.mouse_mode);
	}
}

DisplayServerEnums::MouseMode DisplayServerIhs::mouse_get_mode() const {
	return DisplayServerEnums::MouseMode(IhsState::get().mouse_mode);
}

void DisplayServerIhs::process_events() {
	IhsState &state = IhsState::get();
	state.flush_messages();
	state.flush_input();
	Input::get_singleton()->flush_buffered_events();
}
