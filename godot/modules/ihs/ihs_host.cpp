/**************************************************************************/
/*  ihs_host.cpp                                                          */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "ihs_host.h"

#include "ihs_state.h"

#include "core/object/class_db.h"
#include "servers/display/display_server.h"

IhsHost *IhsHost::singleton = nullptr;

void IhsHost::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_embedded"), &IhsHost::is_embedded);
	ClassDB::bind_method(D_METHOD("send_message", "message"), &IhsHost::send_message);
	ADD_SIGNAL(MethodInfo("message_received", PropertyInfo(Variant::STRING, "message")));
}

bool IhsHost::is_embedded() const {
	DisplayServer *ds = DisplayServer::get_singleton();
	return ds && ds->get_name() == IHS_GODOT_DISPLAY_DRIVER;
}

void IhsHost::send_message(const String &p_message) {
	const IhsGodotHostCallbacks &cb = IhsState::get().callbacks;
	if (cb.message) {
		cb.message(cb.user_data, p_message.utf8().get_data());
	}
}

void IhsHost::deliver(const String &p_message) {
	emit_signal(SNAME("message_received"), p_message);
}

IhsHost::IhsHost() {
	singleton = this;
}

IhsHost::~IhsHost() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
