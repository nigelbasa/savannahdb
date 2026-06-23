#pragma once

#include <napi.h>
#include "savannah/engine.h"

namespace savannah::binding {

savannah::Engine& global_engine();

// (Re)build the process-global engine with explicit storage options. Returns
// true if the engine was actually rebuilt, false if the requested options
// already match the live engine (idempotent no-op). Rebuilding drops any live
// cursors and in-memory state; a canopy engine reloads from its durable root.
bool configure_engine(const savannah::StorageOptions& options);

void RegisterCollection(Napi::Env env, Napi::Object exports);
void RegisterCursor(Napi::Env env, Napi::Object exports);

}  // namespace savannah::binding
