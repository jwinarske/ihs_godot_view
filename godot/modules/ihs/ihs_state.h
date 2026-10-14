/**************************************************************************/
/*  ihs_state.h                                                           */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#include <ihs_godot/bridge.h>

#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

// What the display server's renderer can do with targets. One implementation
// per rendering API (ihs_capture_rd.cpp, ihs_capture_gles3.cpp), created by
// the display server once its renderer is up.
//
// All methods run on Godot's main thread, which in the single-threaded render
// model the display server forces is also the render thread.
class IhsCaptureBackend {
public:
	virtual ~IhsCaptureBackend() {}

	virtual uint32_t get_api() const = 0;
	virtual int add_target(uint32_t p_id, const IhsGodotTarget &p_target) = 0;
	virtual void remove_target(uint32_t p_id) = 0;
	virtual void remove_all_targets() = 0;
	virtual size_t query_dmabuf_formats(IhsGodotFormat *r_out, size_t p_max) = 0;
	virtual void fill_renderer_info(IhsGodotRendererInfo &r_info) = 0;
	// Release whatever removed targets left behind once the GPU is done.
	virtual void collect_garbage() {}
	// After each iteration: a sync_file the caller owns that signals when this
	// iteration's capture is complete, or -1 when it already is.
	virtual int end_iteration() { return -1; }
	// Before the engine is torn down: wait for the GPU to finish this
	// renderer's work, so none is still reading what the teardown frees.
	virtual void wait_idle() {}
};

// Process-global module state shared by the bridge, the display server and
// the capture backends. Godot allows one instance per process, so this is a
// plain singleton.
class IhsState {
public:
	struct InputCmd {
		enum Type {
			TOUCH,
			MOUSE_MOTION,
			MOUSE_BUTTON,
			SCROLL,
			KEY,
			JOY_CONNECTION,
			JOY_BUTTON,
			JOY_AXIS,
		};
		Type type = TOUCH;
		int32_t index = 0;
		uint32_t phase = 0; // touch phase / button index / hid usage
		uint32_t mask = 0; // button mask / modifiers
		uint32_t unicode = 0;
		bool pressed = false;
		bool flag = false; // double click / echo
		float x = 0, y = 0, dx = 0, dy = 0, pressure = 0;
		CharString name; // joypad name
		CharString guid; // joypad GUID
	};

	struct Capture {
		uint32_t target_id = IHS_GODOT_NO_TARGET;
		int fence_fd = -1;
		uint32_t width = 0;
		uint32_t height = 0;
	};

private:
	Mutex queue_mutex;
	LocalVector<InputCmd> input_queue;
	LocalVector<CharString> message_queue;

	// Last known pointer state, for relative motion.
	Vector2 last_mouse;
	Vector2 last_touch[32];

public:
	static IhsState &get();

	// --- configuration (copied from configure()) ---
	bool configured = false;
	uint64_t render_device = 0;
	bool has_device_uuid = false;
	uint8_t device_uuid[16] = {};
	Vector<CharString> extra_device_extensions;
	IhsGodotHostCallbacks callbacks = {};

	// --- window ---
	Size2i window_size = Size2i(1280, 720);
	float window_scale = 1.0f;
	bool visible = true;
	uint32_t mouse_mode = IHS_GODOT_MOUSE_VISIBLE;

	// --- capture ---
	IhsCaptureBackend *backend = nullptr;
	uint32_t next_target = IHS_GODOT_NO_TARGET;
	// Inside IhsGodotApiV1::iteration: next_target is the producer's buffer
	// for this frame, and it may be written.
	bool iterating = false;
	// Written by the backend during the iteration that captured, read and
	// cleared by the bridge after it.
	Capture capture;

	void log(int p_level, const String &p_message);

	// Producer-facing, any thread.
	void push_input(const InputCmd &p_cmd);
	void push_message(const char *p_utf8);

	// Main thread, from DisplayServerIhs::process_events().
	void flush_input();
	void flush_messages();

	// Take the capture result of the iteration that just ran.
	Capture take_capture();
};

#define IHS_LOG_ERROR(m) IhsState::get().log(IHS_GODOT_LOG_ERROR, m)
#define IHS_LOG_WARN(m) IhsState::get().log(IHS_GODOT_LOG_WARNING, m)
#define IHS_LOG_INFO(m) IhsState::get().log(IHS_GODOT_LOG_INFO, m)
#define IHS_LOG_DEBUG(m) IhsState::get().log(IHS_GODOT_LOG_DEBUG, m)
