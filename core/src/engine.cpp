#include "savannah/engine.h"
#include "savannah/storage/canopy_backend.h"
#include "savannah/storage/memory.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace savannah {

namespace {

std::string env_or_empty(const char* key) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&value, &len, key) != 0 || !value) {
    return {};
  }
  std::string out(value, len > 0 ? len - 1 : 0);
  std::free(value);
  return out;
#else
  const char* val = std::getenv(key);
  return val ? std::string(val) : std::string{};
#endif
}

std::filesystem::path default_canopy_root() {
  return std::filesystem::current_path() / ".savannahdb" / "canopy";
}

std::unique_ptr<storage::IStorageBackend> make_backend(
    bool canopy, const std::filesystem::path& root) {
  if (canopy) {
    return std::make_unique<storage::CanopyBackend>(
        root.empty() ? default_canopy_root() : root);
  }
  return std::make_unique<storage::MemoryBackend>();
}

}  // namespace

Engine::Engine() {
  const bool canopy = env_or_empty("SAVANNAH_STORAGE_BACKEND") == "canopy";
  backend_ = make_backend(
      canopy, std::filesystem::path(env_or_empty("SAVANNAH_STORAGE_ROOT")));
}

Engine::Engine(const StorageOptions& options) {
  backend_ = make_backend(options.canopy, options.root);
}

Engine::~Engine() = default;

}  // namespace savannah
