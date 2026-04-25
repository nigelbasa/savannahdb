cmake_minimum_required(VERSION 3.20)
project(savannah_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
  # Match the vcpkg `x64-windows-static-md` triplet: static libs but dynamic
  # MSVC runtime. Node.js itself links /MD, so the addon must too — without
  # this cmake-js defaults to static CRT and libbson's CRT calls fail to link.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# cmake-js supplies CMAKE_JS_INC, CMAKE_JS_SRC, CMAKE_JS_LIB.
# Locate node-addon-api headers via the npm package.
execute_process(
  COMMAND node -p "require('path').resolve(require('node-addon-api').include_dir)"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NODE_ADDON_API_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT NODE_ADDON_API_DIR OR NOT EXISTS "${NODE_ADDON_API_DIR}/napi.h")
  message(FATAL_ERROR "node-addon-api headers not found (resolved: '${NODE_ADDON_API_DIR}')")
endif()

add_compile_definitions(
  NAPI_VERSION=8
  NAPI_CPP_EXCEPTIONS
)

add_subdirectory(core)
add_subdirectory(binding)
