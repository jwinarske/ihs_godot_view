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
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ihs_godot {

// A decoded value of Flutter's StandardMessageCodec -- what a platform view's
// creationParams arrive as. Only what the codec can carry; typed lists other
// than bytes decode to plain lists.
struct CodecValue {
  enum class Type { kNull, kBool, kInt, kDouble, kString, kBytes, kList, kMap };

  Type type = Type::kNull;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  std::string s;  // kString, also the bytes of kBytes
  std::vector<CodecValue> list;
  std::vector<std::pair<CodecValue, CodecValue>> map;

  [[nodiscard]] bool is_null() const { return type == Type::kNull; }
  // Map lookup by string key; nullptr when absent or not a map.
  [[nodiscard]] const CodecValue* Find(const std::string& key) const;

  [[nodiscard]] std::optional<std::string> AsString() const;
  [[nodiscard]] std::optional<double> AsDouble() const;  // int or double
  [[nodiscard]] std::optional<int64_t> AsInt() const;
  [[nodiscard]] std::optional<bool> AsBool() const;
};

// Decode one message. Returns nullopt on malformed input (truncated,
// unknown type, nesting deeper than 64).
std::optional<CodecValue> DecodeStandardMessage(const uint8_t* data,
                                                size_t size);

// Encoder, for tests and for symmetry.
void EncodeStandardMessage(const CodecValue& value, std::vector<uint8_t>& out);

}  // namespace ihs_godot
