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

#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ihs_godot {

namespace {

LogLevel Floor() {
  static const LogLevel floor = [] {
    const char* env = std::getenv("IHS_GODOT_VIEW_LOG");
    if (env == nullptr) {
      return LogLevel::kInfo;
    }
    if (std::strcmp(env, "error") == 0) {
      return LogLevel::kError;
    }
    if (std::strcmp(env, "warning") == 0) {
      return LogLevel::kWarning;
    }
    if (std::strcmp(env, "debug") == 0) {
      return LogLevel::kDebug;
    }
    return LogLevel::kInfo;
  }();
  return floor;
}

}  // namespace

void Log(LogLevel level, const char* fmt, ...) {
  if (static_cast<int>(level) > static_cast<int>(Floor())) {
    return;
  }
  static const char* const kTags[] = {"E", "W", "I", "D"};
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  // Leaked: the engine thread may still log while exit() runs destructors.
  static std::mutex* mutex = new std::mutex();
  std::lock_guard<std::mutex> lock(*mutex);
  std::fprintf(stderr, "[ihs_godot_view] %s %s\n",
               kTags[static_cast<int>(level)], line);
  std::fflush(stderr);
}

}  // namespace ihs_godot
