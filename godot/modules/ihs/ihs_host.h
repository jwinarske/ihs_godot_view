/**************************************************************************/
/*  ihs_host.h                                                            */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#include "core/object/object.h"

// Script-facing side of the bridge: a message channel between the game and
// the Flutter app that embeds it.
//
//   IhsHost.message_received.connect(func(text): ...)   # from the app
//   IhsHost.send_message("score=42")                     # to the app
//
// Messages are plain strings; JSON is the obvious encoding but nothing here
// assumes one.
class IhsHost : public Object {
	GDCLASS(IhsHost, Object);

	static IhsHost *singleton;

protected:
	static void _bind_methods();

public:
	static IhsHost *get_singleton() { return singleton; }

	// True when running under the ihs display driver (inside a Flutter view).
	bool is_embedded() const;
	void send_message(const String &p_message);

	// Called by the bridge on the main thread.
	void deliver(const String &p_message);

	IhsHost();
	~IhsHost();
};
