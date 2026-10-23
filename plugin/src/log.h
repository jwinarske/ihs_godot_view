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

#include <cstdarg>
#include <cstddef>

namespace ihs_godot {

enum class LogLevel { kError = 0, kWarning = 1, kInfo = 2, kDebug = 3 };

// Where a line comes from: the plugin itself, or libgodot (the ihs module's
// diagnostics and Godot's own output).
enum class LogChannel { kPlugin = 0, kGodot = 1 };

// Takes a finished line; returns false when it could not take it (no logging
// service yet), and the line then goes to stderr.
using LogSink = bool (*)(LogChannel channel,
                         LogLevel level,
                         const char* text,
                         size_t len);

// Logging with a fixed prefix, to the installed sink or else stderr. The
// level floor comes from IHS_GODOT_VIEW_LOG (error|warning|info|debug,
// default info).
void Log(LogLevel level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

// A line from libgodot, verbatim.
void LogGodot(LogLevel level, const char* text);

// Installs the sink (nullptr removes it). Any thread.
void SetLogSink(LogSink sink) noexcept;

// From now on, stderr only: the host process is exiting, and the sink's
// service may already be gone.
void LogToStderrOnly();

}  // namespace ihs_godot

#define LOG_E(...) ::ihs_godot::Log(::ihs_godot::LogLevel::kError, __VA_ARGS__)
#define LOG_W(...) \
  ::ihs_godot::Log(::ihs_godot::LogLevel::kWarning, __VA_ARGS__)
#define LOG_I(...) ::ihs_godot::Log(::ihs_godot::LogLevel::kInfo, __VA_ARGS__)
#define LOG_D(...) ::ihs_godot::Log(::ihs_godot::LogLevel::kDebug, __VA_ARGS__)
