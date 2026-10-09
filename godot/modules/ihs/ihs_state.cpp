/**************************************************************************/
/*  ihs_state.cpp                                                         */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_state.h"

#include "ihs_host.h"
#include "ihs_keymap.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "core/string/print_string.h"
#include "servers/display/display_server.h"

IhsState &IhsState::get() {
	static IhsState state;
	return state;
}

void IhsState::log(int p_level, const String &p_message) {
	if (callbacks.log) {
		callbacks.log(callbacks.user_data, p_level, p_message.utf8().get_data());
		return;
	}
	switch (p_level) {
		case IHS_GODOT_LOG_ERROR:
			ERR_PRINT("[ihs] " + p_message);
			break;
		case IHS_GODOT_LOG_WARNING:
			WARN_PRINT("[ihs] " + p_message);
			break;
		case IHS_GODOT_LOG_INFO:
			print_line("[ihs] " + p_message);
			break;
		default:
			print_verbose("[ihs] " + p_message);
			break;
	}
}

void IhsState::push_input(const InputCmd &p_cmd) {
	MutexLock lock(queue_mutex);
	// Bounded: a producer feeding input to an engine that is not iterating
	// (suspended view) must not grow this forever. Old motion is the least
	// valuable thing to keep, but dropping arbitrary events could strand a
	// button down, so drop the newest instead and let the producer resend.
	if (input_queue.size() < 4096) {
		input_queue.push_back(p_cmd);
	}
}

void IhsState::push_message(const char *p_utf8) {
	if (!p_utf8) {
		return;
	}
	MutexLock lock(queue_mutex);
	if (message_queue.size() < 1024) {
		message_queue.push_back(CharString(p_utf8));
	}
}

static BitField<MouseButtonMask> _to_mask(uint32_t p_mask) {
	BitField<MouseButtonMask> mask;
	mask.set_flag(MouseButtonMask(p_mask & (IHS_GODOT_BUTTON_LEFT | IHS_GODOT_BUTTON_RIGHT | IHS_GODOT_BUTTON_MIDDLE | IHS_GODOT_BUTTON_BACK | IHS_GODOT_BUTTON_FORWARD)));
	return mask;
}

static MouseButton _to_button(uint32_t p_bit) {
	switch (p_bit) {
		case IHS_GODOT_BUTTON_LEFT:
			return MouseButton::LEFT;
		case IHS_GODOT_BUTTON_RIGHT:
			return MouseButton::RIGHT;
		case IHS_GODOT_BUTTON_MIDDLE:
			return MouseButton::MIDDLE;
		case IHS_GODOT_BUTTON_BACK:
			return MouseButton::MB_XBUTTON1;
		case IHS_GODOT_BUTTON_FORWARD:
			return MouseButton::MB_XBUTTON2;
		default:
			return MouseButton::NONE;
	}
}

static void _set_modifiers(const Ref<InputEventWithModifiers> &p_ev, uint32_t p_mods) {
	p_ev->set_shift_pressed(p_mods & IHS_GODOT_MOD_SHIFT);
	p_ev->set_ctrl_pressed(p_mods & IHS_GODOT_MOD_CTRL);
	p_ev->set_alt_pressed(p_mods & IHS_GODOT_MOD_ALT);
	p_ev->set_meta_pressed(p_mods & IHS_GODOT_MOD_META);
}

void IhsState::flush_input() {
	LocalVector<InputCmd> pending;
	{
		MutexLock lock(queue_mutex);
		pending = input_queue;
		input_queue.clear();
	}
	Input *input = Input::get_singleton();
	if (!input) {
		return;
	}
	const int64_t window = DisplayServerEnums::MAIN_WINDOW_ID;

	for (const InputCmd &c : pending) {
		switch (c.type) {
			case InputCmd::TOUCH: {
				const int idx = CLAMP(c.index, 0, 31);
				const Vector2 pos(c.x, c.y);
				if (c.phase == IHS_GODOT_TOUCH_MOVE) {
					Ref<InputEventScreenDrag> ev;
					ev.instantiate();
					ev->set_window_id(window);
					ev->set_index(idx);
					ev->set_position(pos);
					ev->set_relative(pos - last_touch[idx]);
					ev->set_relative_screen_position(pos - last_touch[idx]);
					ev->set_pressure(c.pressure);
					input->parse_input_event(ev);
				} else {
					Ref<InputEventScreenTouch> ev;
					ev.instantiate();
					ev->set_window_id(window);
					ev->set_index(idx);
					ev->set_position(pos);
					ev->set_pressed(c.phase == IHS_GODOT_TOUCH_DOWN);
					ev->set_canceled(c.phase == IHS_GODOT_TOUCH_CANCEL);
					input->parse_input_event(ev);
				}
				last_touch[idx] = pos;
			} break;

			case InputCmd::MOUSE_MOTION: {
				const Vector2 pos(c.x, c.y);
				Ref<InputEventMouseMotion> ev;
				ev.instantiate();
				ev->set_window_id(window);
				ev->set_position(pos);
				ev->set_global_position(pos);
				ev->set_button_mask(_to_mask(c.mask));
				// The producer's own delta: a captured mouse is read through
				// screen_relative only, and positions stop at the view's edge.
				ev->set_relative(Vector2(c.dx, c.dy));
				ev->set_relative_screen_position(Vector2(c.dx, c.dy));
				input->parse_input_event(ev);
				last_mouse = pos;
			} break;

			case InputCmd::MOUSE_BUTTON: {
				const Vector2 pos(c.x, c.y);
				Ref<InputEventMouseButton> ev;
				ev.instantiate();
				ev->set_window_id(window);
				ev->set_position(pos);
				ev->set_global_position(pos);
				ev->set_button_index(_to_button(c.phase));
				ev->set_pressed(c.pressed);
				ev->set_double_click(c.flag);
				ev->set_button_mask(_to_mask(c.mask));
				input->parse_input_event(ev);
				last_mouse = pos;
			} break;

			case InputCmd::SCROLL: {
				// One wheel event per axis, with the magnitude in factor; a
				// trackpad delivers fractional deltas, a wheel ~1 per notch.
				const Vector2 pos(c.x, c.y);
				auto wheel = [&](MouseButton p_button, float p_factor) {
					for (int pressed = 1; pressed >= 0; --pressed) {
						Ref<InputEventMouseButton> ev;
						ev.instantiate();
						ev->set_window_id(window);
						ev->set_position(pos);
						ev->set_global_position(pos);
						ev->set_button_index(p_button);
						ev->set_factor(p_factor);
						ev->set_pressed(pressed);
						input->parse_input_event(ev);
					}
				};
				if (c.dy != 0.0f) {
					wheel(c.dy > 0 ? MouseButton::WHEEL_DOWN : MouseButton::WHEEL_UP, Math::abs(c.dy));
				}
				if (c.dx != 0.0f) {
					wheel(c.dx > 0 ? MouseButton::WHEEL_RIGHT : MouseButton::WHEEL_LEFT, Math::abs(c.dx));
				}
			} break;

			case InputCmd::JOY_CONNECTION:
				input->joy_connection_changed(c.index, c.pressed, String::utf8(c.name.get_data()), String::utf8(c.guid.get_data()));
				break;

			case InputCmd::JOY_BUTTON:
				if (c.phase < uint32_t(JoyButton::SDL_MAX)) {
					input->joy_button(c.index, JoyButton(c.phase), c.pressed);
				}
				break;

			case InputCmd::JOY_AXIS:
				if (c.phase < uint32_t(JoyAxis::SDL_MAX)) {
					input->joy_axis(c.index, JoyAxis(c.phase), CLAMP(c.x, -1.0f, 1.0f));
				}
				break;

			case InputCmd::KEY: {
				KeyLocation location = KeyLocation::UNSPECIFIED;
				const Key physical = ihs_hid_to_godot_key(c.phase, &location);
				Key keycode = physical;
				Key label = physical;
				// Letters follow the active layout, so take them from the
				// character rather than the key position.
				char32_t ch = char32_t(c.unicode);
				if (ch >= 'a' && ch <= 'z') {
					keycode = label = Key(ch - 'a' + 'A');
				} else if (ch >= 'A' && ch <= 'Z') {
					keycode = label = Key(ch);
				} else if (ch > 0x20 && ch < 0x7f && physical == Key::NONE) {
					keycode = label = Key(ch);
				}
				if (keycode == Key::NONE && ch == 0) {
					break;
				}
				Ref<InputEventKey> ev;
				ev.instantiate();
				ev->set_window_id(window);
				ev->set_keycode(keycode);
				ev->set_physical_keycode(physical);
				ev->set_key_label(label);
				ev->set_location(location);
				ev->set_unicode(c.pressed && ch >= 0x20 && ch != 0x7f ? ch : 0);
				ev->set_pressed(c.pressed);
				ev->set_echo(c.flag);
				_set_modifiers(ev, c.mask);
				input->parse_input_event(ev);
			} break;
		}
	}
}

void IhsState::flush_messages() {
	LocalVector<CharString> pending;
	{
		MutexLock lock(queue_mutex);
		pending = message_queue;
		message_queue.clear();
	}
	IhsHost *host = IhsHost::get_singleton();
	if (!host) {
		return;
	}
	for (const CharString &m : pending) {
		host->deliver(String::utf8(m.get_data()));
	}
}

IhsState::Capture IhsState::take_capture() {
	Capture c = capture;
	capture = Capture();
	return c;
}
