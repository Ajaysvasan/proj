# Third-party native dependencies for proj-engine.
#
# Every dependency is pinned to an exact release archive and verified with a
# SHA-256 hash. Sources are downloaded into the build tree only; nothing is
# installed system-wide. For offline builds, point
# FETCHCONTENT_SOURCE_DIR_<NAME> at an already-extracted copy, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_PROJ_SQLITE=/path/to/sqlite-amalgamation-3500400
#
# Upstream CMake files are intentionally NOT used (SOURCE_SUBDIR points at a
# directory that does not exist), so only the exact files listed below are
# compiled.

include(FetchContent)

macro(proj_fetch name url sha256)
  FetchContent_Declare(${name}
    URL ${url}
    URL_HASH SHA256=${sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR __proj_sources_only__)
  FetchContent_MakeAvailable(${name})
endmacro()

proj_fetch(proj_sqlite
  https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
  1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032)
proj_fetch(proj_json
  https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
proj_fetch(proj_tree_sitter
  https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.25.10.tar.gz
  ad5040537537012b16ef6e1210a572b927c7cdc2b99d1ee88d44a7dcdc3ff44c)
proj_fetch(proj_ts_javascript
  https://github.com/tree-sitter/tree-sitter-javascript/archive/refs/tags/v0.25.0.tar.gz
  9712fc283d3dc01d996d20b6392143445d05867a7aad76fdd723824468428b86)
proj_fetch(proj_ts_typescript
  https://github.com/tree-sitter/tree-sitter-typescript/archive/refs/tags/v0.23.2.tar.gz
  2c4ce711ae8d1218a3b2f899189298159d672870b5b34dff5d937bed2f3e8983)
proj_fetch(proj_ts_python
  https://github.com/tree-sitter/tree-sitter-python/archive/refs/tags/v0.25.0.tar.gz
  4609a3665a620e117acf795ff01b9e965880f81745f287a16336f4ca86cf270c)
proj_fetch(proj_ts_java
  https://github.com/tree-sitter/tree-sitter-java/archive/refs/tags/v0.23.5.tar.gz
  cb199e0faae4b2c08425f88cbb51c1a9319612e7b96315a174a624db9bf3d9f0)
proj_fetch(proj_ts_cpp
  https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz
  7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051)
proj_fetch(proj_ts_c
  https://github.com/tree-sitter/tree-sitter-c/archive/refs/tags/v0.24.1.tar.gz
  25dd4bb3dec770769a407e0fc803f424ce02c494a56ce95fedc525316dcf9b48)

# --- SQLite (amalgamation) ---------------------------------------------------
add_library(proj_sqlite3 STATIC ${proj_sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(proj_sqlite3 SYSTEM PUBLIC ${proj_sqlite_SOURCE_DIR})
target_compile_definitions(proj_sqlite3 PRIVATE
  SQLITE_DQS=0
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_ENABLE_FTS5)
target_compile_options(proj_sqlite3 PRIVATE -w)
find_package(Threads REQUIRED)
target_link_libraries(proj_sqlite3 PUBLIC Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(proj_sqlite3 PUBLIC m)
endif()

# --- nlohmann/json (header only) ---------------------------------------------
add_library(proj_json INTERFACE)
target_include_directories(proj_json SYSTEM INTERFACE ${proj_json_SOURCE_DIR}/include)

# --- Tree-sitter runtime -----------------------------------------------------
add_library(proj_tree_sitter STATIC ${proj_tree_sitter_SOURCE_DIR}/lib/src/lib.c)
target_include_directories(proj_tree_sitter
  SYSTEM PUBLIC ${proj_tree_sitter_SOURCE_DIR}/lib/include
  PRIVATE ${proj_tree_sitter_SOURCE_DIR}/lib/src ${proj_tree_sitter_SOURCE_DIR}/lib/src/wasm)
target_compile_definitions(proj_tree_sitter PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_compile_options(proj_tree_sitter PRIVATE -w)

# --- Tree-sitter grammars ----------------------------------------------------
# Generated parser.c files include "tree_sitter/parser.h" relative to their own
# directory, so no extra include paths are required.
add_library(proj_grammars STATIC
  ${proj_ts_javascript_SOURCE_DIR}/src/parser.c
  ${proj_ts_javascript_SOURCE_DIR}/src/scanner.c
  ${proj_ts_typescript_SOURCE_DIR}/typescript/src/parser.c
  ${proj_ts_typescript_SOURCE_DIR}/typescript/src/scanner.c
  ${proj_ts_typescript_SOURCE_DIR}/tsx/src/parser.c
  ${proj_ts_typescript_SOURCE_DIR}/tsx/src/scanner.c
  ${proj_ts_python_SOURCE_DIR}/src/parser.c
  ${proj_ts_python_SOURCE_DIR}/src/scanner.c
  ${proj_ts_java_SOURCE_DIR}/src/parser.c
  ${proj_ts_cpp_SOURCE_DIR}/src/parser.c
  ${proj_ts_cpp_SOURCE_DIR}/src/scanner.c
  ${proj_ts_c_SOURCE_DIR}/src/parser.c)
target_compile_options(proj_grammars PRIVATE -w)
# The TypeScript scanners include ../../common/scanner.h, which in turn
# includes "tree_sitter/parser.h" from the grammar's own src directory.
set_source_files_properties(${proj_ts_typescript_SOURCE_DIR}/typescript/src/scanner.c
  PROPERTIES INCLUDE_DIRECTORIES ${proj_ts_typescript_SOURCE_DIR}/typescript/src)
set_source_files_properties(${proj_ts_typescript_SOURCE_DIR}/tsx/src/scanner.c
  PROPERTIES INCLUDE_DIRECTORIES ${proj_ts_typescript_SOURCE_DIR}/tsx/src)

# --- Catch2 (tests only) -----------------------------------------------------
if(PROJ_BUILD_TESTS)
  proj_fetch(proj_catch2
    https://github.com/catchorg/Catch2/archive/refs/tags/v3.8.1.tar.gz
    18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79)
  add_library(proj_catch2 STATIC ${proj_catch2_SOURCE_DIR}/extras/catch_amalgamated.cpp)
  target_include_directories(proj_catch2 SYSTEM PUBLIC ${proj_catch2_SOURCE_DIR}/extras)
  target_compile_options(proj_catch2 PRIVATE -w)
endif()
