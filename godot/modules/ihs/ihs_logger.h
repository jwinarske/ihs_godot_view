/**************************************************************************/
/*  ihs_logger.h                                                          */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#include "core/io/logger.h"
#include "core/string/print_string.h"

// Godot's own output (print, printerr, errors, warnings) handed to the host's
// engine_log callback one line at a time, in place of the terminal logger.
class IhsLogger : public Logger {
public:
	virtual void logv(const char *p_format, va_list p_list, bool p_err) override _PRINTF_FORMAT_ATTRIBUTE_2_0;
	virtual void log_error(const char *p_function, const char *p_file, int p_line, const char *p_code, const char *p_rationale, bool p_editor_notify = false, ErrorType p_type = ERR_ERROR, const Vector<Ref<ScriptBacktrace>> &p_script_backtraces = {}) override;

	// Replaces the OS's loggers with an IhsLogger when the host passed an
	// engine_log callback. Call once the OS exists and before Main adds the
	// project's file logger (MODULE_INITIALIZATION_LEVEL_CORE).
	static void install_if_requested();

	// Godot's stdio print function (set_stdio_print_func): what it would
	// otherwise write straight to stdout or stderr, through the terminal logger
	// until install_if_requested() replaces it, and before the OS exists and
	// during final cleanup. Set from configure() when the host passed an
	// engine_log callback.
	static void stdio_print(const char *p_text, StdioPrintType p_type);
};
