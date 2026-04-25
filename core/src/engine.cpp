#include "savannah/engine.h"
#include "savannah/storage/memory.h"

namespace savannah {

Engine::Engine() : backend_(std::make_unique<storage::MemoryBackend>()) {}
Engine::~Engine() = default;

}  // namespace savannah
