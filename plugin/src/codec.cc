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

#include "codec.h"

#include <cstring>

namespace ihs_godot {

namespace {

enum : uint8_t {
  kNull = 0,
  kTrue = 1,
  kFalse = 2,
  kInt32 = 3,
  kInt64 = 4,
  kLargeInt = 5,
  kFloat64 = 6,
  kString = 7,
  kUint8List = 8,
  kInt32List = 9,
  kInt64List = 10,
  kFloat64List = 11,
  kList = 12,
  kMap = 13,
  kFloat32List = 14,
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool Byte(uint8_t& out) {
    if (pos_ + 1 > size_) {
      return false;
    }
    out = data_[pos_++];
    return true;
  }
  bool Bytes(void* out, size_t n) {
    if (n > size_ - pos_ || pos_ > size_) {
      return false;
    }
    std::memcpy(out, data_ + pos_, n);
    pos_ += n;
    return true;
  }
  bool Align(size_t a) {
    const size_t mod = pos_ % a;
    if (mod != 0) {
      const size_t pad = a - mod;
      if (pad > size_ - pos_) {
        return false;
      }
      pos_ += pad;
    }
    return true;
  }
  bool Size(uint32_t& out) {
    uint8_t b = 0;
    if (!Byte(b)) {
      return false;
    }
    if (b < 254) {
      out = b;
      return true;
    }
    if (b == 254) {
      uint16_t v = 0;
      if (!Bytes(&v, 2)) {
        return false;
      }
      out = v;
      return true;
    }
    return Bytes(&out, 4);
  }
  [[nodiscard]] size_t remaining() const { return size_ - pos_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

bool Decode(Reader& r, CodecValue& out, int depth) {
  if (depth > 64) {
    return false;
  }
  uint8_t type = 0;
  if (!r.Byte(type)) {
    return false;
  }
  switch (type) {
    case kNull:
      out.type = CodecValue::Type::kNull;
      return true;
    case kTrue:
    case kFalse:
      out.type = CodecValue::Type::kBool;
      out.b = type == kTrue;
      return true;
    case kInt32: {
      int32_t v = 0;
      if (!r.Bytes(&v, 4)) {
        return false;
      }
      out.type = CodecValue::Type::kInt;
      out.i = v;
      return true;
    }
    case kInt64: {
      int64_t v = 0;
      if (!r.Bytes(&v, 8)) {
        return false;
      }
      out.type = CodecValue::Type::kInt;
      out.i = v;
      return true;
    }
    case kFloat64: {
      double v = 0;
      if (!r.Align(8) || !r.Bytes(&v, 8)) {
        return false;
      }
      out.type = CodecValue::Type::kDouble;
      out.d = v;
      return true;
    }
    case kLargeInt:
    case kString:
    case kUint8List: {
      uint32_t n = 0;
      if (!r.Size(n) || n > r.remaining()) {
        return false;
      }
      out.s.resize(n);
      if (n != 0 && !r.Bytes(out.s.data(), n)) {
        return false;
      }
      out.type = type == kUint8List ? CodecValue::Type::kBytes
                                    : CodecValue::Type::kString;
      return true;
    }
    case kInt32List:
    case kInt64List:
    case kFloat64List:
    case kFloat32List: {
      uint32_t n = 0;
      const size_t elem =
          (type == kInt64List || type == kFloat64List) ? 8 : 4;
      if (!r.Size(n) || !r.Align(elem) || size_t(n) * elem > r.remaining()) {
        return false;
      }
      out.type = CodecValue::Type::kList;
      out.list.resize(n);
      for (uint32_t k = 0; k < n; k++) {
        CodecValue& v = out.list[k];
        if (type == kInt32List) {
          int32_t x = 0;
          r.Bytes(&x, 4);
          v.type = CodecValue::Type::kInt;
          v.i = x;
        } else if (type == kInt64List) {
          int64_t x = 0;
          r.Bytes(&x, 8);
          v.type = CodecValue::Type::kInt;
          v.i = x;
        } else if (type == kFloat32List) {
          float x = 0;
          r.Bytes(&x, 4);
          v.type = CodecValue::Type::kDouble;
          v.d = x;
        } else {
          double x = 0;
          r.Bytes(&x, 8);
          v.type = CodecValue::Type::kDouble;
          v.d = x;
        }
      }
      return true;
    }
    case kList: {
      uint32_t n = 0;
      if (!r.Size(n) || n > r.remaining()) {
        return false;
      }
      out.type = CodecValue::Type::kList;
      out.list.resize(n);
      for (uint32_t k = 0; k < n; k++) {
        if (!Decode(r, out.list[k], depth + 1)) {
          return false;
        }
      }
      return true;
    }
    case kMap: {
      uint32_t n = 0;
      if (!r.Size(n) || n > r.remaining()) {
        return false;
      }
      out.type = CodecValue::Type::kMap;
      out.map.resize(n);
      for (uint32_t k = 0; k < n; k++) {
        if (!Decode(r, out.map[k].first, depth + 1) ||
            !Decode(r, out.map[k].second, depth + 1)) {
          return false;
        }
      }
      return true;
    }
    default:
      return false;
  }
}

void WriteSize(uint32_t n, std::vector<uint8_t>& out) {
  if (n < 254) {
    out.push_back(static_cast<uint8_t>(n));
  } else if (n <= 0xffff) {
    out.push_back(254);
    out.push_back(static_cast<uint8_t>(n & 0xff));
    out.push_back(static_cast<uint8_t>(n >> 8));
  } else {
    out.push_back(255);
    for (int k = 0; k < 4; k++) {
      out.push_back(static_cast<uint8_t>(n >> (8 * k)));
    }
  }
}

template <typename T>
void WriteRaw(const T& v, std::vector<uint8_t>& out) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

}  // namespace

const CodecValue* CodecValue::Find(const std::string& key) const {
  if (type != Type::kMap) {
    return nullptr;
  }
  for (const auto& kv : map) {
    if (kv.first.type == Type::kString && kv.first.s == key) {
      return &kv.second;
    }
  }
  return nullptr;
}

std::optional<std::string> CodecValue::AsString() const {
  if (type == Type::kString) {
    return s;
  }
  return std::nullopt;
}

std::optional<double> CodecValue::AsDouble() const {
  if (type == Type::kDouble) {
    return d;
  }
  if (type == Type::kInt) {
    return static_cast<double>(i);
  }
  return std::nullopt;
}

std::optional<int64_t> CodecValue::AsInt() const {
  if (type == Type::kInt) {
    return i;
  }
  if (type == Type::kDouble) {
    return static_cast<int64_t>(d);
  }
  return std::nullopt;
}

std::optional<bool> CodecValue::AsBool() const {
  if (type == Type::kBool) {
    return b;
  }
  return std::nullopt;
}

std::optional<CodecValue> DecodeStandardMessage(const uint8_t* data,
                                                size_t size) {
  if (data == nullptr || size == 0) {
    return std::nullopt;
  }
  Reader r(data, size);
  CodecValue v;
  if (!Decode(r, v, 0)) {
    return std::nullopt;
  }
  return v;
}

void EncodeStandardMessage(const CodecValue& v, std::vector<uint8_t>& out) {
  switch (v.type) {
    case CodecValue::Type::kNull:
      out.push_back(kNull);
      break;
    case CodecValue::Type::kBool:
      out.push_back(v.b ? kTrue : kFalse);
      break;
    case CodecValue::Type::kInt:
      if (v.i >= INT32_MIN && v.i <= INT32_MAX) {
        out.push_back(kInt32);
        WriteRaw(static_cast<int32_t>(v.i), out);
      } else {
        out.push_back(kInt64);
        WriteRaw(v.i, out);
      }
      break;
    case CodecValue::Type::kDouble:
      out.push_back(kFloat64);
      while (out.size() % 8 != 0) {
        out.push_back(0);
      }
      WriteRaw(v.d, out);
      break;
    case CodecValue::Type::kString:
    case CodecValue::Type::kBytes:
      out.push_back(v.type == CodecValue::Type::kString ? kString : kUint8List);
      WriteSize(static_cast<uint32_t>(v.s.size()), out);
      out.insert(out.end(), v.s.begin(), v.s.end());
      break;
    case CodecValue::Type::kList:
      out.push_back(kList);
      WriteSize(static_cast<uint32_t>(v.list.size()), out);
      for (const CodecValue& e : v.list) {
        EncodeStandardMessage(e, out);
      }
      break;
    case CodecValue::Type::kMap:
      out.push_back(kMap);
      WriteSize(static_cast<uint32_t>(v.map.size()), out);
      for (const auto& kv : v.map) {
        EncodeStandardMessage(kv.first, out);
        EncodeStandardMessage(kv.second, out);
      }
      break;
  }
}

}  // namespace ihs_godot
