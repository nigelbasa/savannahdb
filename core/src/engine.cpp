#include "savannah/engine.h"
#include "savannah/storage/canopy_backend.h"
#include "savannah/storage/memory.h"

#include <cstdlib>
#include <stdexcept>
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
    bool canopy, const std::filesystem::path& root, bool full_sync) {
  if (canopy) {
    const auto resolved = root.empty() ? default_canopy_root() : root;
    const auto policy = full_sync ? storage::SyncPolicy::Full
                                  : storage::SyncPolicy::Batched;
    try {
      return std::make_unique<storage::CanopyBackend>(resolved, policy);
    } catch (const std::filesystem::filesystem_error& e) {
      // Persistence is the default, so a process with an unwritable working
      // directory (read-only container rootfs, a serverless runtime outside
      // its temp dir) now hits this on startup where it previously ran in
      // memory without noticing. Name the opt-out in the error: falling back
      // to memory silently is the exact failure 0.1.1 fixed, and doing it
      // here would make it the path every user takes.
      throw std::runtime_error(
          std::string("SavannahDB could not open its storage directory at '") +
          resolved.string() + "' (" + e.code().message() +
          "). Pass an explicit writable path with storage: { backend: "
          "'canopy', root: '/path' }, or opt out of persistence with "
          "storage: { backend: 'memory' }.");
    }
  }
  return std::make_unique<storage::MemoryBackend>();
}

}  // namespace

Engine::Engine() {
  // Persistent by default. An unset variable means canopy; only an explicit
  // "memory" opts out. Matching both values exactly (rather than treating
  // anything-not-"canopy" as memory) means a typo like "Memory" or "mem"
  // fails loudly instead of silently discarding the caller's data on exit --
  // silently misreading storage config is the bug 0.1.1 was released to fix.
  const auto choice = env_or_empty("SAVANNAH_STORAGE_BACKEND");
  bool canopy = true;
  if (!choice.empty()) {
    if (choice == "memory") {
      canopy = false;
    } else if (choice != "canopy") {
      throw std::runtime_error(
          "unrecognized SAVANNAH_STORAGE_BACKEND '" + choice +
          "' -- expected \"canopy\" (default, persists to disk) or "
          "\"memory\" (data discarded on exit)");
    }
  }
  backend_ = make_backend(
      canopy, std::filesystem::path(env_or_empty("SAVANNAH_STORAGE_ROOT")),
      env_or_empty("SAVANNAH_STORAGE_SYNC") == "full");
}

Engine::Engine(const StorageOptions& options) {
  backend_ = make_backend(options.canopy, options.root, options.full_sync);
}

Engine::~Engine() = default;

}  // namespace savannah
