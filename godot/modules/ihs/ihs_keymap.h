/**************************************************************************/
/*  ihs_keymap.h                                                          */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#include "core/os/keyboard.h"

#include <cstdint>

// Map a USB HID usage ((page << 16) | id, page 0x07) to a Godot key. Returns
// Key::NONE for anything unmapped. @r_location, when not null, receives the
// left/right side of a modifier.
Key ihs_hid_to_godot_key(uint32_t p_usb_hid_usage, KeyLocation *r_location);
