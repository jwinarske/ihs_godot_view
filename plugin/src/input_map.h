/*
 * Copyright 2026 Joel Winarske
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace ihs_godot {

// One touch contact change, in window (physical) pixels, ready for the
// bridge's touch().
struct TouchEvent {
  int32_t index = 0;
  uint32_t phase = 0;  // IhsGodotTouchPhase
  float x = 0;
  float y = 0;
  float pressure = 0;
};

// Decode IhsPvCallbacks::on_touch: an Android MotionEvent (action with the
// pointer index in bits 8..15) and @size doubles of per-contact coordinates,
// x at offset 7 and y at 8 of each contact. The shell forwards one contact's
// coordinates today, so a short packet is read as a single contact.
//
// The Godot touch index is the contact's position in the packet; slot 0 of a
// contact, which Android uses for orientation, carries a pointer id when the
// sender is this package's Dart controller -- used when every contact has a
// distinct one. Coordinates are logical pixels, scaled by @scale.
std::vector<TouchEvent> DecodeAndroidTouch(int32_t action,
                                           int32_t point_count,
                                           size_t size,
                                           const double* data,
                                           double scale);

// Flutter pointer ids grow without bound; Godot touch indices are small and
// reused. A pointer takes the lowest free index on down and frees it on up.
class TouchSlots {
 public:
  static constexpr int kMax = 32;
  int Acquire(int32_t pointer);   // -1 when full
  int Find(int32_t pointer) const;  // -1 when unknown
  void Release(int32_t pointer);
  void Clear() { slots_.clear(); }

 private:
  std::map<int32_t, int> slots_;
};

// Flutter's button bitmask (kPrimaryButton...) to the bridge's
// IHS_GODOT_BUTTON_* mask.
uint32_t FlutterButtonsToGodot(int32_t flutter_buttons);

// The individual IHS_GODOT_BUTTON_* bits that changed from @before to
// @after, presses and releases, in a stable order.
struct ButtonChange {
  uint32_t button = 0;
  bool pressed = false;
};
std::vector<ButtonChange> DiffButtons(uint32_t before, uint32_t after);

}  // namespace ihs_godot
