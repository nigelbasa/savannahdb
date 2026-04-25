#pragma once

#include "savannah/cursor_registry.h"
#include "savannah/storage/backend.h"

#include <memory>

namespace savannah {

class Engine {
 public:
  Engine();
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
