/**************************************************************************/
/*  ihs_logger.cpp                                                        */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_logger.h"

#include "ihs_state.h"

#include "core/object/script_backtrace.h"
#include "core/os/os.h"

#include <cstdio>
#include <cstring>

namespace {

// Text printed without a newline yet (print_raw, or a line built from several
// prints), held per thread so lines from different threads do not interleave.
// Plain storage, no destructor: Godot may print while a thread or the process
// is being torn down. A line longer than this is handed over in pieces.
constexpr uint32_t LINE_MAX_BYTES = 4096;
thread_local char pending[LINE_MAX_BYTES];
thread_local uint32_t pending_len = 0;

void emit_pending(int p_level) {
	const IhsGodotHostCallbacks &cb = IhsState::get().callbacks;
	uint32_t len = pending_len;
	pending_len = 0;
	if (len > 0 && pending[len - 1] == '\r') {
		--len;
	}
	pending[len] = '\0';
	// Godot's blank separator lines carry nothing for a log reader.
	if (len > 0 && cb.engine_log) {
		cb.engine_log(cb.user_data, p_level, pending);
	}
}

// Hands every complete line of p_text to the host; keeps the remainder.
void emit_lines(int p_level, const char *p_text) {
	for (const char *c = p_text; *c; ++c) {
		if (*c == '\n') {
			emit_pending(p_level);
			continue;
		}
		pending[pending_len++] = *c;
		if (pending_len == LINE_MAX_BYTES - 1) {
			emit_pending(p_level);
		}
	}
}

// OS::_set_logger is protected, and there is no public way to drop the
// terminal logger. A pointer to the inherited member, taken in a derived
// class, may be called on any OS.
struct OSLoggerAccess : OS {
	static constexpr void (OS::*set_logger)(CompositeLogger *) = &OSLoggerAccess::_set_logger;
};

} // namespace

void IhsLogger::logv(const char *p_format, va_list p_list, bool p_err) {
	if (!should_log(p_err)) {
		return;
	}
	char stack[1024];
	va_list copy;
	va_copy(copy, p_list);
	const int n = vsnprintf(stack, sizeof(stack), p_format, copy);
	va_end(copy);
	if (n < 0) {
		return;
	}
	const int level = p_err ? IHS_GODOT_LOG_ERROR : IHS_GODOT_LOG_INFO;
	if (size_t(n) < sizeof(stack)) {
		emit_lines(level, stack);
		return;
	}
	char *big = static_cast<char *>(memalloc(size_t(n) + 1));
	vsnprintf(big, size_t(n) + 1, p_format, p_list);
	emit_lines(level, big);
	memfree(big);
}

void IhsLogger::log_error(const char *p_function, const char *p_file, int p_line, const char *p_code, const char *p_rationale, bool p_editor_notify, ErrorType p_type, const Vector<Ref<ScriptBacktrace>> &p_script_backtraces) {
	if (!should_log(true)) {
		return;
	}
	const int level = p_type == ERR_WARNING ? IHS_GODOT_LOG_WARNING : IHS_GODOT_LOG_ERROR;
	const char *details = (p_rationale && *p_rationale) ? p_rationale : p_code;
	// One record per error: the location goes on the same line.
	char line[1024];
	snprintf(line, sizeof(line), "%s: %s (at %s, %s:%i)\n", error_type_string(p_type), details ? details : "", p_function ? p_function : "?", p_file ? p_file : "?", p_line);
	emit_lines(level, line);
	for (const Ref<ScriptBacktrace> &backtrace : p_script_backtraces) {
		if (backtrace.is_valid() && !backtrace->is_empty()) {
			emit_lines(level, (backtrace->format(3) + "\n").utf8().get_data());
		}
	}
}

void IhsLogger::stdio_print(const char *p_text, StdioPrintType p_type) {
	if (!p_text) {
		return;
	}
	int level = IHS_GODOT_LOG_INFO;
	if (p_type == STDIO_PRINT_ERROR) {
		level = IHS_GODOT_LOG_ERROR;
	} else if (p_type == STDIO_PRINT_WARNING) {
		level = IHS_GODOT_LOG_WARNING;
	}
	emit_lines(level, p_text);
}

void IhsLogger::install_if_requested() {
	OS *os = OS::get_singleton();
	if (!os || !IhsState::get().callbacks.engine_log) {
		return;
	}
	Vector<Logger *> loggers;
	loggers.push_back(memnew(IhsLogger));
	(os->*OSLoggerAccess::set_logger)(memnew(CompositeLogger(loggers)));
}
