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

}  // namespace

Engine::Engine() {
  if (env_or_empty("SAVANNAH_STORAGE_BACKEND") == "canopy") {
    const std::string root_env = env_or_empty("SAVANNAH_STORAGE_ROOT");
    std::filesystem::path root = root_env.empty()
        ? std::filesystem::current_path() / ".savannahdb" / "canopy"
        : std::filesystem::path(root_env);
    backend_ = std::make_unique<storage::CanopyBackend>(std::move(root));
    return;
  }
  backend_ = std::make_unique<storage::MemoryBackend>();
}
Engine::~Engine() = default;

}  // namespace savannah
