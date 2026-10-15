# Canonical index dump shared by the tests and the proj_index_dump tool.
add_library(proj_index_dump_lib STATIC index_dump.cpp)
target_link_libraries(proj_index_dump_lib PUBLIC proj_core)

# Catch2's amalgamated source provides main().
add_executable(proj_engine_tests
  test_core.cpp
  test_extractors.cpp
  test_index.cpp
  test_refresh.cpp
  test_resolution.cpp
  test_memory.cpp
  test_legacy.cpp
  test_dynamic.cpp)
target_link_libraries(proj_engine_tests PRIVATE proj_core proj_index_dump_lib proj_catch2)
target_compile_definitions(proj_engine_tests PRIVATE
  PROJ_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures")
target_compile_options(proj_engine_tests PRIVATE -Wall -Wextra)
add_test(NAME proj_engine_tests COMMAND proj_engine_tests "~[leak]")
# Leak checks need exact allocator accounting (see test_memory.cpp). They skip
# without glibc; --allow-running-no-tests keeps an all-skipped run a success.
add_test(NAME proj_engine_leaks COMMAND proj_engine_tests "[leak]" --allow-running-no-tests)
set_tests_properties(proj_engine_leaks PROPERTIES ENVIRONMENT "GLIBC_TUNABLES=glibc.malloc.tcache_count=0")

# Development tool: compare indexes, e.g. a refresh against a full analysis.
add_executable(proj_index_dump index_dump_main.cpp)
target_link_libraries(proj_index_dump PRIVATE proj_index_dump_lib)
