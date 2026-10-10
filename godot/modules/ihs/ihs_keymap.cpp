/**************************************************************************/
/*  ihs_keymap.cpp                                                        */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_keymap.h"

// USB HID usage page 0x07 (keyboard/keypad) to Godot keys. Flutter reports
// PhysicalKeyboardKey.usbHidUsage as (page << 16) | usage, which is what the
// producer forwards; the physical key is the same on every layout, so it maps
// to Godot's physical_keycode, and the keycode is derived from the produced
// character where there is one.

Key ihs_hid_to_godot_key(uint32_t p_usb_hid_usage, KeyLocation *r_location) {
	if (r_location) {
		*r_location = KeyLocation::UNSPECIFIED;
	}
	if ((p_usb_hid_usage >> 16) != 0x07) {
		return Key::NONE;
	}
	const uint32_t u = p_usb_hid_usage & 0xffff;

	if (u >= 0x04 && u <= 0x1d) {
		return Key(uint32_t(Key::A) + (u - 0x04));
	}
	if (u >= 0x1e && u <= 0x26) {
		return Key(uint32_t(Key::KEY_1) + (u - 0x1e));
	}
	if (u >= 0x3a && u <= 0x45) {
		return Key(uint32_t(Key::F1) + (u - 0x3a));
	}
	if (u >= 0x68 && u <= 0x73) {
		return Key(uint32_t(Key::F13) + (u - 0x68));
	}
	if (u >= 0x59 && u <= 0x61) {
		return Key(uint32_t(Key::KP_1) + (u - 0x59));
	}

	auto left = [r_location]() {
		if (r_location) {
			*r_location = KeyLocation::LEFT;
		}
	};
	auto right = [r_location]() {
		if (r_location) {
			*r_location = KeyLocation::RIGHT;
		}
	};

	switch (u) {
		case 0x27:
			return Key::KEY_0;
		case 0x28:
			return Key::ENTER;
		case 0x29:
			return Key::ESCAPE;
		case 0x2a:
			return Key::BACKSPACE;
		case 0x2b:
			return Key::TAB;
		case 0x2c:
			return Key::SPACE;
		case 0x2d:
			return Key::MINUS;
		case 0x2e:
			return Key::EQUAL;
		case 0x2f:
			return Key::BRACKETLEFT;
		case 0x30:
			return Key::BRACKETRIGHT;
		case 0x31:
		case 0x32:
			return Key::BACKSLASH;
		case 0x33:
			return Key::SEMICOLON;
		case 0x34:
			return Key::APOSTROPHE;
		case 0x35:
			return Key::QUOTELEFT;
		case 0x36:
			return Key::COMMA;
		case 0x37:
			return Key::PERIOD;
		case 0x38:
			return Key::SLASH;
		case 0x39:
			return Key::CAPSLOCK;
		case 0x46:
			return Key::PRINT;
		case 0x47:
			return Key::SCROLLLOCK;
		case 0x48:
			return Key::PAUSE;
		case 0x49:
			return Key::INSERT;
		case 0x4a:
			return Key::HOME;
		case 0x4b:
			return Key::PAGEUP;
		case 0x4c:
			return Key::KEY_DELETE;
		case 0x4d:
			return Key::END;
		case 0x4e:
			return Key::PAGEDOWN;
		case 0x4f:
			return Key::RIGHT;
		case 0x50:
			return Key::LEFT;
		case 0x51:
			return Key::DOWN;
		case 0x52:
			return Key::UP;
		case 0x53:
			return Key::NUMLOCK;
		case 0x54:
			return Key::KP_DIVIDE;
		case 0x55:
			return Key::KP_MULTIPLY;
		case 0x56:
			return Key::KP_SUBTRACT;
		case 0x57:
			return Key::KP_ADD;
		case 0x58:
			return Key::KP_ENTER;
		case 0x62:
			return Key::KP_0;
		case 0x63:
			return Key::KP_PERIOD;
		case 0x65:
			return Key::MENU;
		case 0x75:
			return Key::HELP;
		case 0x7f:
			return Key::VOLUMEMUTE;
		case 0x80:
			return Key::VOLUMEUP;
		case 0x81:
			return Key::VOLUMEDOWN;
		case 0xe0:
			left();
			return Key::CTRL;
		case 0xe1:
			left();
			return Key::SHIFT;
		case 0xe2:
			left();
			return Key::ALT;
		case 0xe3:
			left();
			return Key::META;
		case 0xe4:
			right();
			return Key::CTRL;
		case 0xe5:
			right();
			return Key::SHIFT;
		case 0xe6:
			right();
			return Key::ALT;
		case 0xe7:
			right();
			return Key::META;
		default:
			return Key::NONE;
	}
}
