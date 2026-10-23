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

// Sends the plugin's log lines to the shell's logging service (ihs_shared),
// so they reach the sinks IHS_LOG_SINK selects (console, file, DLT) next to
// the shell's own. Installed when the plugin is loaded.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <ihs/ihs.h>
#include <ihs/logging.h>

#include "log.h"

namespace ihs_godot {
namespace {

const IhsLoggingApi* Logging() {
  static const IhsLoggingApi* const logging = []() -> const IhsLoggingApi* {
    const IhsApi* api = ihs_get_api(IHS_SHARED_ABI_VERSION);
    if (api == nullptr ||
        api->struct_size <
            offsetof(IhsApi, logging) + sizeof(const IhsLoggingApi*) ||
        api->logging == nullptr) {
      return nullptr;
    }
    const IhsLoggingApi* l = api->logging;
    if (l->struct_size < offsetof(IhsLoggingApi, log) + sizeof(l->log) ||
        l->context_open == nullptr || l->log == nullptr) {
      return nullptr;
    }
    return l;
  }();
  return logging;
}

struct Channel {
  const char* tag;
  const char* description;
  std::atomic<int32_t> context{-1};
};

Channel g_channels[] = {
    {"GDVW", "ihs_godot_view plugin"},
    {"GODT", "Godot engine (libgodot)"},
};

// A context opens only once the shell has started logging; until then the
// open fails and is retried on the next line. Two threads racing here open
// it twice, which the service allows.
int32_t Context(const IhsLoggingApi& logging, Channel& channel) {
  int32_t ctx = channel.context.load(std::memory_order_acquire);
  if (ctx >= 0) {
    return ctx;
  }
  IhsLogContextOptions options{};
  options.struct_size = sizeof(options);
  options.description = channel.description;
  ctx = logging.context_open(channel.tag, &options);
  if (ctx >= 0) {
    channel.context.store(ctx, std::memory_order_release);
  }
  return ctx;
}

uint8_t ToIhsLevel(LogLevel level) {
  switch (level) {
    case LogLevel::kError:
      return IHS_LEVEL_ERROR;
    case LogLevel::kWarning:
      return IHS_LEVEL_WARN;
    case LogLevel::kInfo:
      return IHS_LEVEL_INFO;
    case LogLevel::kDebug:
      break;
  }
  return IHS_LEVEL_DEBUG;
}

bool IhsSink(LogChannel channel, LogLevel level, const char* text, size_t len) {
  const IhsLoggingApi* logging = Logging();
  if (logging == nullptr) {
    return false;
  }
  const int32_t ctx = Context(*logging, g_channels[static_cast<int>(channel)]);
  if (ctx < 0) {
    return false;
  }
  // A line the service drops (below IHS_LOG_LEVEL, or a full ring) is still
  // taken: falling back to stderr would undo the filter.
  logging->log(ctx, ToIhsLevel(level), text, len);
  // An error line is written out before a crash can lose it, as the shell's
  // own are.
  if (level == LogLevel::kError && logging->flush != nullptr) {
    logging->flush();
  }
  return true;
}

// Removed again when the plugin's statics are destroyed at exit, so a late
// line from the engine thread goes to stderr, not to a sink being torn down.
struct InstallSink {
  InstallSink() noexcept { SetLogSink(&IhsSink); }
  ~InstallSink() { SetLogSink(nullptr); }
};
const InstallSink g_install_sink;

}  // namespace
}  // namespace ihs_godot
