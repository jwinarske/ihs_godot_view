/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#include "register_types.h"

#include "ihs_host.h"
#include "ihs_logger.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

#include <ihs_godot/bridge.h>

// The display server is not registered here: Main::setup() validates
// --display-driver before any module is initialized, so registration happens
// in the bridge's configure(), which the producer calls before it creates the
// instance (see ihs_bridge.cpp).

// Modules are linked from a static archive, and nothing in the engine calls
// the bridge: without a reference from an object that is pulled in, the
// linker drops ihs_bridge.o and libgodot.so exports no bridge at all.
extern "C" const IhsGodotApiV1 *ihs_godot_bridge_get_api(uint32_t p_version);
extern "C" __attribute__((used, visibility("default"))) const void *ihs_godot_bridge_anchor =
		reinterpret_cast<const void *>(&ihs_godot_bridge_get_api);

static IhsHost *ihs_host = nullptr;

void initialize_ihs_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_CORE) {
		// Before Main adds the project's file logger, so that one is kept.
		IhsLogger::install_if_requested();
		return;
	}
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
