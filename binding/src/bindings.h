#pragma once

#include <napi.h>
#include "savannah/engine.h"

namespace savannah::binding {

savannah::Engine& global_engine();

void RegisterCollection(Napi::Env env, Napi::Object exports);
void RegisterCursor(Napi::Env env, Napi::Object exports);

}  // namespace savannah::binding
