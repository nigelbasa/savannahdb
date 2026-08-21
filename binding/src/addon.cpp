#include "bindings.h"

#include <filesystem>
#include <memory>
#include <mutex>

namespace savannah::binding {

namespace {

// The engine is process-global. All N-API entry points run on the single JS
// thread, so calls are already serialized; the mutex only guards lazy
// construction and the configure()-driven rebuild against each other.
std::mutex g_engine_mutex;
std::unique_ptr<savannah::Engine> g_engine;
bool g_configured = false;
bool g_canopy = false;
std::filesystem::path g_root;
bool g_full_sync = false;

}  // namespace

savannah::Engine& global_engine() {
  std::lock_guard<std::mutex> lock(g_engine_mutex);
  if (!g_engine) {
    // No explicit configure() yet — fall back to the env-driven default so
    // shell-launched usage (the REST server) keeps working unchanged.
    g_engine = std::make_unique<savannah::Engine>();
  }
  return *g_engine;
}

bool configure_engine(const savannah::StorageOptions& options) {
  std::lock_guard<std::mutex> lock(g_engine_mutex);
  if (g_engine && g_configured && g_canopy == options.canopy &&
      g_root == options.root && g_full_sync == options.full_sync) {
    return false;
  }
  g_engine = std::make_unique<savannah::Engine>(options);
  g_configured = true;
  g_canopy = options.canopy;
  g_root = options.root;
  g_full_sync = options.full_sync;
  return true;
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
