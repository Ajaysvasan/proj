cmake_minimum_required(VERSION 3.24)
project(proj_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PROJ_BUILD_TESTS "Build the engine unit tests" ON)
option(PROJ_WARNINGS_AS_ERRORS "Treat compiler warnings in engine sources as errors" OFF)

include(cmake/Dependencies.cmake)

add_library(proj_core STATIC
  src/core/hash.cpp
  src/core/log.cpp
  src/core/paths.cpp
  src/core/sysinfo.cpp
  src/core/time.cpp
  src/db/schema.cpp
  src/db/sqlite.cpp
  src/scan/ignore.cpp
  src/scan/language.cpp
  src/scan/scanner.cpp
  src/parse/parser.cpp
  src/parse/ts_util.cpp
  src/parse/extract_js.cpp
  src/parse/extract_python.cpp
  src/parse/extract_java.cpp
  src/parse/extract_cpp.cpp
  src/parse/extract_config.cpp
  src/index/registry.cpp
  src/index/store.cpp
  src/index/refresh.cpp
  src/index/resolver.cpp
  src/index/resolver_config.cpp
  src/index/resolver_overloads.cpp
  src/index/indexer.cpp
  src/query/query.cpp
  src/protocol/protocol.cpp)
target_include_directories(proj_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(proj_core PUBLIC PROJ_VERSION="${PROJECT_VERSION}")
target_link_libraries(proj_core PUBLIC proj_sqlite3 proj_tree_sitter proj_grammars proj_json)
target_compile_options(proj_core PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
if(PROJ_WARNINGS_AS_ERRORS)
  target_compile_options(proj_core PRIVATE -Werror)
endif()

add_executable(proj-engine src/main.cpp)
target_link_libraries(proj-engine PRIVATE proj_core)
target_compile_options(proj-engine PRIVATE -Wall -Wextra -Wpedantic)

if(PROJ_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
