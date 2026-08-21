#pragma once

#include "savannah/cursor_registry.h"
#include "savannah/storage/backend.h"

#include <filesystem>
#include <memory>

namespace savannah {

// Explicit storage selection. The default Engine() constructor reads the
// SAVANNAH_STORAGE_* environment variables; this struct lets callers (the
// N-API binding's configure() path) pass the same choice directly, which is
// the only reliable channel on Windows where runtime process.env writes are
// invisible to the C runtime's getenv/_dupenv_s.
struct StorageOptions {
  // Persistent by default, matching Engine()'s env-driven default. Callers
  // that want an ephemeral store must ask for it explicitly.
  bool canopy = true;
  // Only meaningful when `canopy` is true. Empty means "use the default root"
  // (<cwd>/.savannahdb/canopy), matching the env-driven path.
  std::filesystem::path root;
};

class Engine {
 public:
  Engine();
  explicit Engine(const StorageOptions& options);
  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  storage::IStorageBackend& backend() { return *backend_; }
  CursorRegistry& cursors() { return cursors_; }

 private:
  std::unique_ptr<storage::IStorageBackend> backend_;
  CursorRegistry cursors_;
};

}  // namespace savannah
