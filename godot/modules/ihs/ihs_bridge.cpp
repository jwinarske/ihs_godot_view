/**************************************************************************/
/*  ihs_bridge.cpp                                                        */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

// The exported C table the producer drives libgodot through. See
// include/ihs_godot/bridge.h for the contract.

#include "display_server_ihs.h"
#include "ihs_logger.h"
#include "ihs_state.h"

#include "core/config/engine.h"
#include "core/extension/godot_instance.h"
#include "core/extension/libgodot.h"
#include "core/io/resource_loader.h"

#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace {

void _gdext_initialize(void *p_userdata, GDExtensionInitializationLevel p_level) {}
void _gdext_deinitialize(void *p_userdata, GDExtensionInitializationLevel p_level) {}

// LibGodot loads the host as a GDExtension ("libgodot://main"). The producer
// registers no classes of its own -- everything it needs is in this table --
// so the extension is empty.
GDExtensionBool _gdext_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	r_initialization->minimum_initialization_level = GDEXTENSION_INITIALIZATION_CORE;
	r_initialization->userdata = nullptr;
	r_initialization->initialize = _gdext_initialize;
	r_initialization->deinitialize = _gdext_deinitialize;
	return 1;
}

GodotInstance *_instance(void *p_instance) {
	return static_cast<GodotInstance *>(reinterpret_cast<Object *>(p_instance));
}

int _configure(const IhsGodotConfig *p_config, const IhsGodotHostCallbacks *p_callbacks) {
	IhsState &state = IhsState::get();
	if (p_config) {
		if (p_config->struct_size < offsetof(IhsGodotConfig, extra_device_extensions)) {
			return IHS_GODOT_ERR_INVALID;
		}
		state.render_device = p_config->render_device;
		state.has_device_uuid = p_config->has_device_uuid != 0;
		memcpy(state.device_uuid, p_config->device_uuid, 16);
		state.extra_device_extensions.clear();
		if (p_config->struct_size >= sizeof(IhsGodotConfig)) {
			for (size_t i = 0; i < p_config->extra_device_extension_count; i++) {
				if (p_config->extra_device_extensions[i]) {
					state.extra_device_extensions.push_back(CharString(p_config->extra_device_extensions[i]));
				}
			}
		}
	}
	state.callbacks = IhsGodotHostCallbacks();
	if (p_callbacks) {
		// Copy what the caller's struct covers; older producers leave the
		// trailing callbacks NULL.
		const size_t n = MIN(p_callbacks->struct_size, sizeof(IhsGodotHostCallbacks));
		memcpy(&state.callbacks, p_callbacks, n);
		state.callbacks.struct_size = sizeof(IhsGodotHostCallbacks);
	}
	// From here on, even before the engine exists, what Godot would write to
	// stdout or stderr goes to the host.
	set_stdio_print_func(state.callbacks.engine_log ? &IhsLogger::stdio_print : nullptr);
	DisplayServerIhs::register_ihs_driver();
	state.configured = true;
	return IHS_GODOT_OK;
}

void *_create_instance(int p_argc, char **p_argv) {
	if (p_argc < 1 || !p_argv || !p_argv[0]) {
		return nullptr;
	}
	if (!IhsState::get().configured) {
		_configure(nullptr, nullptr);
	}
	// Force the two settings everything here depends on: our display driver,
	// and rendering on the main thread (the capture and the target imports
	// both touch the renderer from the thread that iterates).
	std::vector<std::string> args;
	args.emplace_back(p_argv[0]);
	for (int i = 1; i < p_argc; i++) {
		const std::string a = p_argv[i] ? p_argv[i] : "";
		if ((a == "--display-driver" || a == "--render-thread") && i + 1 < p_argc) {
			i++;
			continue;
		}
		if (a == "--headless") {
			continue;
		}
		args.push_back(a);
	}
	args.emplace_back("--display-driver");
	args.emplace_back(IHS_GODOT_DISPLAY_DRIVER);
	args.emplace_back("--render-thread");
	args.emplace_back("safe");

	std::vector<char *> argv;
	for (std::string &a : args) {
		argv.push_back(a.data());
	}
	argv.push_back(nullptr);
	return libgodot_create_godot_instance(int(args.size()), argv.data(), _gdext_init);
}

int _start(void *p_instance) {
	if (!p_instance) {
		return IHS_GODOT_ERR_INVALID;
	}
	return _instance(p_instance)->start() ? IHS_GODOT_OK : IHS_GODOT_ERR_STATE;
}

int _iteration(void *p_instance, IhsGodotFrameInfo *r_out) {
	if (!p_instance) {
		return 1;
	}
	IhsState &state = IhsState::get();
	IhsState::Capture stale = state.take_capture();
	if (stale.fence_fd >= 0) {
		close(stale.fence_fd);
	}

	GodotInstance *gi = _instance(p_instance);
	state.iterating = true;
	const bool quit = gi->is_started() ? gi->iteration() : true;
	state.iterating = false;

	const int fence = state.backend ? state.backend->end_iteration() : -1;
	if (state.backend) {
		state.backend->collect_garbage();
	}
	IhsState::Capture c = state.take_capture();
	if (fence >= 0) {
		if (c.target_id != IHS_GODOT_NO_TARGET && c.fence_fd < 0) {
			c.fence_fd = fence;
		} else {
			close(fence);
		}
	}
	if (r_out && r_out->struct_size >= sizeof(IhsGodotFrameInfo)) {
		r_out->target_id = c.target_id;
		r_out->acquire_fence_fd = c.fence_fd;
		r_out->width = c.width;
		r_out->height = c.height;
		r_out->frame = Engine::get_singleton()->get_frames_drawn();
	} else if (c.fence_fd >= 0) {
		close(c.fence_fd);
	}
	return quit ? 1 : 0;
}

void _destroy_instance(void *p_instance) {
	if (p_instance) {
		if (IhsCaptureBackend *backend = IhsState::get().backend) {
			backend->wait_idle();
		}
		libgodot_destroy_godot_instance(reinterpret_cast<GDExtensionObjectPtr>(p_instance));
	}
}

void _notify(void *p_instance, uint32_t p_what) {
	if (!p_instance) {
		return;
	}
	GodotInstance *gi = _instance(p_instance);
	DisplayServerIhs *ds = DisplayServerIhs::get_ihs_singleton();
	switch (p_what) {
		case IHS_GODOT_FOCUS_IN:
			gi->focus_in();
			if (ds) {
				ds->set_focused(true);
			}
			break;
		case IHS_GODOT_FOCUS_OUT:
			gi->focus_out();
			if (ds) {
				ds->set_focused(false);
			}
			break;
		case IHS_GODOT_PAUSE:
			gi->pause();
			break;
		case IHS_GODOT_RESUME:
			gi->resume();
			break;
		default:
			break;
	}
}

int _get_renderer_info(IhsGodotRendererInfo *r_info) {
	if (!r_info || r_info->struct_size < sizeof(IhsGodotRendererInfo)) {
		return IHS_GODOT_ERR_INVALID;
	}
	const size_t size = r_info->struct_size;
	memset(r_info, 0, sizeof(IhsGodotRendererInfo));
	r_info->struct_size = size;
	IhsCaptureBackend *backend = IhsState::get().backend;
	if (!backend) {
		r_info->api = IHS_GODOT_API_NONE;
		return IHS_GODOT_ERR_STATE;
	}
	backend->fill_renderer_info(*r_info);
	return IHS_GODOT_OK;
}

size_t _query_dmabuf_formats(IhsGodotFormat *r_out, size_t p_max) {
	IhsCaptureBackend *backend = IhsState::get().backend;
	return backend ? backend->query_dmabuf_formats(r_out, p_max) : 0;
}

int _add_target(uint32_t p_id, const IhsGodotTarget *p_target) {
	if (!p_target || p_target->struct_size < sizeof(IhsGodotTarget)) {
		return IHS_GODOT_ERR_INVALID;
	}
	IhsCaptureBackend *backend = IhsState::get().backend;
	if (!backend) {
		return IHS_GODOT_ERR_STATE;
	}
	return backend->add_target(p_id, *p_target);
}

void _remove_target(uint32_t p_id) {
	IhsState &state = IhsState::get();
	if (state.backend) {
		state.backend->remove_target(p_id);
	}
	if (state.next_target == p_id) {
		state.next_target = IHS_GODOT_NO_TARGET;
	}
}

void _set_next_target(uint32_t p_id) {
	IhsState::get().next_target = p_id;
}

void _set_window_size(uint32_t p_width, uint32_t p_height, float p_scale) {
	if (p_width == 0 || p_height == 0) {
		return;
	}
	const Size2i size{ int(p_width), int(p_height) };
	const float scale = p_scale > 0.0f ? p_scale : 1.0f;
	DisplayServerIhs *ds = DisplayServerIhs::get_ihs_singleton();
	if (ds) {
		ds->set_window_size(size, scale);
	} else {
		// Before the display server exists: it is created at this size.
		IhsState::get().window_size = size;
		IhsState::get().window_scale = scale;
	}
}

void _set_visible(int p_visible) {
	IhsState::get().visible = p_visible != 0;
}

void _touch(int32_t p_index, uint32_t p_phase, float p_x, float p_y, float p_pressure) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::TOUCH;
	c.index = p_index;
	c.phase = p_phase;
	c.x = p_x;
	c.y = p_y;
	c.pressure = p_pressure;
	IhsState::get().push_input(c);
}

void _mouse_motion(float p_x, float p_y, float p_dx, float p_dy, uint32_t p_mask) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::MOUSE_MOTION;
	c.x = p_x;
	c.y = p_y;
	c.dx = p_dx;
	c.dy = p_dy;
	c.mask = p_mask;
	IhsState::get().push_input(c);
}

void _mouse_button(float p_x, float p_y, uint32_t p_button, int p_pressed, uint32_t p_mask, int p_double_click) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::MOUSE_BUTTON;
	c.x = p_x;
	c.y = p_y;
	c.phase = p_button;
	c.pressed = p_pressed != 0;
	c.mask = p_mask;
	c.flag = p_double_click != 0;
	IhsState::get().push_input(c);
}

void _scroll(float p_x, float p_y, float p_dx, float p_dy) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::SCROLL;
	c.x = p_x;
	c.y = p_y;
	c.dx = p_dx;
	c.dy = p_dy;
	IhsState::get().push_input(c);
}

void _key(uint32_t p_usage, uint32_t p_unicode, int p_pressed, int p_echo, uint32_t p_modifiers) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::KEY;
	c.phase = p_usage;
	c.unicode = p_unicode;
	c.pressed = p_pressed != 0;
	c.flag = p_echo != 0;
	c.mask = p_modifiers;
	IhsState::get().push_input(c);
}

void _joy_connection(int32_t p_device, int p_connected, const char *p_name, const char *p_guid) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::JOY_CONNECTION;
	c.index = p_device;
	c.pressed = p_connected != 0;
	c.name = CharString(p_name ? p_name : "ihs joypad");
	c.guid = CharString(p_guid ? p_guid : "");
	IhsState::get().push_input(c);
}

void _joy_button(int32_t p_device, uint32_t p_button, int p_pressed) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::JOY_BUTTON;
	c.index = p_device;
	c.phase = p_button;
	c.pressed = p_pressed != 0;
	IhsState::get().push_input(c);
}

void _joy_axis(int32_t p_device, uint32_t p_axis, float p_value) {
	IhsState::InputCmd c;
	c.type = IhsState::InputCmd::JOY_AXIS;
	c.index = p_device;
	c.phase = p_axis;
	c.x = p_value;
	IhsState::get().push_input(c);
}

void _post_message(const char *p_utf8) {
	IhsState::get().push_message(p_utf8);
}

int _loads_in_progress() {
	return ResourceLoader::has_threaded_loads_in_progress() ? 1 : 0;
}

const IhsGodotApiV1 api = {
	sizeof(IhsGodotApiV1),
	IHS_GODOT_BRIDGE_VERSION,
	_configure,
	_create_instance,
	_start,
	_iteration,
	_destroy_instance,
	_notify,
	_get_renderer_info,
	_query_dmabuf_formats,
	_add_target,
	_remove_target,
	_set_next_target,
	_set_window_size,
	_set_visible,
	_touch,
	_mouse_motion,
	_mouse_button,
	_scroll,
	_key,
	_joy_connection,
	_joy_button,
	_joy_axis,
	_post_message,
	_loads_in_progress,
};

} // namespace

extern "C" const IhsGodotApiV1 *ihs_godot_bridge_get_api(uint32_t p_version) {
	if (p_version != IHS_GODOT_BRIDGE_VERSION) {
		return nullptr;
	}
	return &api;
}
