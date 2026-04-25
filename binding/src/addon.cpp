#include "bindings.h"

namespace savannah::binding {

savannah::Engine& global_engine() {
  static savannah::Engine engine;
  return engine;
}

}  // namespace savannah::binding

namespace {

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  savannah::binding::RegisterCollection(env, exports);
  savannah::binding::RegisterCursor(env, exports);
  return exports;
}

}  // namespace

NODE_API_MODULE(savannah_engine, Init)
