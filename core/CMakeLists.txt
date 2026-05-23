find_package(bson CONFIG REQUIRED)

add_library(savannah_core STATIC
  src/engine.cpp
  src/storage/canopy.cpp
  src/storage/canopy_backend.cpp
  src/storage/memory.cpp
  src/query/value.cpp
  src/query/filter.cpp
  src/query/update.cpp
  src/query/sort.cpp
  src/query/key_order.cpp
  src/query/project.cpp
  src/query/planner.cpp
  src/query/expression.cpp
  src/query/pipeline/dispatch.cpp
  src/query/pipeline/match.cpp
  src/query/pipeline/shaping.cpp
  src/query/pipeline/project.cpp
  src/query/pipeline/group.cpp
  src/query/pipeline/lookup.cpp
  src/query/pipeline/unwind.cpp
  src/index/manager.cpp
  src/cursor_registry.cpp
)

target_include_directories(savannah_core PUBLIC include)
target_compile_features(savannah_core PUBLIC cxx_std_20)

# Propagate libbson to consumers of savannah_core (the binding).
target_link_libraries(savannah_core PUBLIC
  $<IF:$<TARGET_EXISTS:bson::static>,bson::static,bson::shared>
)
