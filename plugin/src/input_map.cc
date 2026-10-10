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

#include "input_map.h"

#include <cmath>
#include <set>

#include <ihs_godot/bridge.h>

namespace ihs_godot {

namespace {

constexpr int kActionDown = 0;
constexpr int kActionUp = 1;
constexpr int kActionMove = 2;
constexpr int kActionCancel = 3;
constexpr int kActionPointerDown = 5;
constexpr int kActionPointerUp = 6;
constexpr size_t kCoords = 9;

}  // namespace

std::vector<TouchEvent> DecodeAndroidTouch(int32_t action,
                                           int32_t point_count,
                                           size_t size,
                                           const double* data,
                                           double scale) {
  std::vector<TouchEvent> out;
  if (data == nullptr || size < kCoords) {
    return out;
  }
  size_t count = point_count > 0 ? static_cast<size_t>(point_count) : 1;
  size_t stride = size / count;
  if (stride < kCoords || size % count != 0) {
    count = 1;
    stride = size;
  }

  // Ids from slot 0, if they are usable as ids.
  std::vector<int32_t> ids(count);
  std::set<int32_t> seen;
  bool distinct = true;
  for (size_t i = 0; i < count; i++) {
    const double v = data[i * stride];
    const auto id = static_cast<int32_t>(std::lround(v));
    if (v < 0 || v > 1e6 || std::fabs(v - id) > 1e-6 || !seen.insert(id).second) {
      distinct = false;
    }
    ids[i] = id;
  }
  if (!distinct || count == 1) {
    for (size_t i = 0; i < count; i++) {
      ids[i] = static_cast<int32_t>(i);
    }
  }

  const int masked = action & 0xff;
  const auto action_index = static_cast<size_t>((action >> 8) & 0xff);
  auto contact = [&](size_t i, uint32_t phase) {
    TouchEvent e;
    e.index = ids[i] % TouchSlots::kMax;
    e.phase = phase;
    e.x = static_cast<float>(data[i * stride + 7] * scale);
    e.y = static_cast<float>(data[i * stride + 8] * scale);
    e.pressure = static_cast<float>(data[i * stride + 1]);
    out.push_back(e);
  };

  switch (masked) {
    case kActionDown:
      contact(0, IHS_GODOT_TOUCH_DOWN);
      break;
    case kActionPointerDown:
      if (action_index < count) {
        contact(action_index, IHS_GODOT_TOUCH_DOWN);
      }
      break;
    case kActionMove:
      for (size_t i = 0; i < count; i++) {
        contact(i, IHS_GODOT_TOUCH_MOVE);
      }
      break;
    case kActionUp:
      contact(count == 1 ? 0 : std::min(action_index, count - 1),
              IHS_GODOT_TOUCH_UP);
      break;
    case kActionPointerUp:
      if (action_index < count) {
        contact(action_index, IHS_GODOT_TOUCH_UP);
      }
      break;
    case kActionCancel:
      for (size_t i = 0; i < count; i++) {
        contact(i, IHS_GODOT_TOUCH_CANCEL);
      }
      break;
    default:
      break;  // hover/scroll/buttons: not touch
  }
  return out;
}

int TouchSlots::Acquire(int32_t pointer) {
  if (const int existing = Find(pointer); existing >= 0) {
    return existing;
  }
  std::set<int> used;
  for (const auto& kv : slots_) {
    used.insert(kv.second);
  }
  for (int i = 0; i < kMax; i++) {
    if (used.count(i) == 0) {
      slots_[pointer] = i;
      return i;
    }
  }
  return -1;
}

int TouchSlots::Find(int32_t pointer) const {
  const auto it = slots_.find(pointer);
  return it == slots_.end() ? -1 : it->second;
}

void TouchSlots::Release(int32_t pointer) {
  slots_.erase(pointer);
}

uint32_t FlutterButtonsToGodot(int32_t b) {
  uint32_t mask = 0;
  if (b & 0x01) {
    mask |= IHS_GODOT_BUTTON_LEFT;
  }
  if (b & 0x02) {
    mask |= IHS_GODOT_BUTTON_RIGHT;
  }
  if (b & 0x04) {
    mask |= IHS_GODOT_BUTTON_MIDDLE;
  }
  if (b & 0x08) {
    mask |= IHS_GODOT_BUTTON_BACK;
  }
  if (b & 0x10) {
    mask |= IHS_GODOT_BUTTON_FORWARD;
  }
  return mask;
}

std::vector<ButtonChange> DiffButtons(uint32_t before, uint32_t after) {
  static const uint32_t kOrder[] = {
      IHS_GODOT_BUTTON_LEFT, IHS_GODOT_BUTTON_RIGHT, IHS_GODOT_BUTTON_MIDDLE,
      IHS_GODOT_BUTTON_BACK, IHS_GODOT_BUTTON_FORWARD};
  std::vector<ButtonChange> out;
  for (const uint32_t bit : kOrder) {
    const bool was = (before & bit) != 0;
    const bool is = (after & bit) != 0;
    if (was != is) {
      out.push_back({bit, is});
    }
  }
  return out;
}

}  // namespace ihs_godot
