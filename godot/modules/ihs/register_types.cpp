/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "register_types.h"

#include "ihs_host.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

// The display server is not registered here: Main::setup() validates
// --display-driver before any module is initialized, so registration happens
// in the bridge's configure(), which the producer calls before it creates the
// instance (see ihs_bridge.cpp).

static IhsHost *ihs_host = nullptr;

void initialize_ihs_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(IhsHost);
	ihs_host = memnew(IhsHost);
	Engine::get_singleton()->add_singleton(Engine::Singleton("IhsHost", IhsHost::get_singleton()));
}

void uninitialize_ihs_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	if (ihs_host) {
		memdelete(ihs_host);
		ihs_host = nullptr;
	}
}
