/**************************************************************************/
/*  display_server_ihs.h                                                  */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#include <ihs_godot/bridge.h>

#include "servers/display/display_server_headless.h"

class IhsCaptureBackend;
class IhsEgl;
class IhsVulkanHooks;
class RenderingContextDriver;
class RenderingDevice;

// The display driver Godot runs under inside an ivi-homescreen platform view.
//
// One virtual window (MAIN_WINDOW_ID) whose size is whatever the platform view
// is; no OS window, no swapchain, no input of its own. It differs from the
// headless driver it extends in exactly the ways that make Godot render:
//
//   - a real renderer: RenderingDevice on Vulkan with no surface, or the
//     compatibility renderer on a surfaceless EGL context;
//   - the window reports itself drawable while the view is visible, so the
//     main loop draws;
//   - the compositor's blit-to-screen is replaced by a capture into the
//     producer's target (see ihs_capture_rd.h / ihs_capture_gles3.h);
//   - input comes from the bridge's queue, drained in process_events().
class DisplayServerIhs : public DisplayServerHeadless {
	GDSOFTCLASS(DisplayServerIhs, DisplayServerHeadless);

	String rendering_driver;
	Callable rect_changed_callback;
	Callable window_event_callback;

	IhsCaptureBackend *capture_backend = nullptr;
#if defined(RD_ENABLED)
	RenderingContextDriver *rendering_context = nullptr;
	RenderingDevice *rendering_device = nullptr;
#endif
#if defined(VULKAN_ENABLED)
	IhsVulkanHooks *vulkan_hooks = nullptr;
#endif
#if defined(GLES3_ENABLED) && defined(EGL_ENABLED)
	IhsEgl *egl = nullptr;
#endif

	bool _init_vulkan();
	bool _init_gl(bool p_gles);

public:
	static DisplayServerIhs *get_ihs_singleton();

	static Vector<String> get_rendering_drivers_func();
	static DisplayServer *create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error);
	static void register_ihs_driver();

	// Producer-driven, main thread.
	void set_window_size(const Size2i &p_size, float p_scale);
	void set_focused(bool p_focused);

	bool has_feature(DisplayServerEnums::Feature p_feature) const override;
	String get_name() const override { return IHS_GODOT_DISPLAY_DRIVER; }

	int get_screen_count() const override { return 1; }
	Size2i screen_get_size(int p_screen = DisplayServerEnums::SCREEN_OF_MAIN_WINDOW) const override;
	Rect2i screen_get_usable_rect(int p_screen = DisplayServerEnums::SCREEN_OF_MAIN_WINDOW) const override;
	int screen_get_dpi(int p_screen = DisplayServerEnums::SCREEN_OF_MAIN_WINDOW) const override;
	float screen_get_scale(int p_screen = DisplayServerEnums::SCREEN_OF_MAIN_WINDOW) const override;
	float screen_get_max_scale() const override;

	Vector<DisplayServerEnums::WindowID> get_window_list() const override;
	int window_get_current_screen(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override { return 0; }

	void window_set_rect_changed_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) override;
	void window_set_window_event_callback(const Callable &p_callable, DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) override;

	Size2i window_get_size(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override;
	Size2i window_get_size_with_decorations(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override;
	DisplayServerEnums::WindowMode window_get_mode(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override;

	bool window_can_draw(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override;
	bool can_any_window_draw() const override;

	int64_t window_get_native_handle(DisplayServerEnums::HandleType p_handle_type, DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override;

	void mouse_set_mode(DisplayServerEnums::MouseMode p_mode) override;
	DisplayServerEnums::MouseMode mouse_get_mode() const override;

	void process_events() override;

	DisplayServerIhs(const String &p_rendering_driver, Error &r_error);
	~DisplayServerIhs();
};
