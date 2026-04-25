#include "bindings.h"

namespace savannah::binding {

// Phase 0.2 will add streaming cursors that survive across getMore calls.
// Phase 0.1 returns all results inline, so this file registers nothing yet.
void RegisterCursor(Napi::Env /*env*/, Napi::Object /*exports*/) {}

}  // namespace savannah::binding
