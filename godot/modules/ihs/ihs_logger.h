/**************************************************************************/
/*  ihs_logger.h                                                          */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#include "core/io/logger.h"

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
};
