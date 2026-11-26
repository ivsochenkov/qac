cmake_minimum_required(VERSION 3.18)
project(qac VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(QAC_BUILD_TESTS "Build unit and integration tests" ON)
option(QAC_BUILD_BENCH "Build the benchmark" ON)
option(QAC_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

set(QAC_DEFAULT_WARNINGS -Wall -Wextra -Wpedantic -Wno-unused-parameter)

if(QAC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Boost 1.75 REQUIRED COMPONENTS program_options json locale)

# All third-party code comes from one place: the JamSpell fork (branch master), a git
# submodule, and the submodules nested in it:
#
#   third_party/jamspell                                     spell correction (required)
#     third_party/hat-trie                                   the index (header-only)
#       third_party/array-hash                               hat-trie's buckets (header-only)
#         third_party/cereal                                 snapshot serialization (header-only)
#     third_party/robin-map                                  used by JamSpell (header-only)
#
# so that qac and JamSpell are built from the same copy of each of them. Only googletest, which
# the fork does not carry, is not part of it: the tests use an installed one (find_package(GTest)).
set(QAC_JAMSPELL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jamspell)
set(QAC_HAT_TRIE_ROOT ${QAC_JAMSPELL_ROOT}/third_party/hat-trie)
set(QAC_ARRAY_HASH_ROOT ${QAC_HAT_TRIE_ROOT}/third_party/array-hash)
set(QAC_CEREAL_ROOT ${QAC_ARRAY_HASH_ROOT}/third_party/cereal)
set(QAC_ROBIN_MAP_ROOT ${QAC_JAMSPELL_ROOT}/third_party/robin-map)

set(QAC_THIRD_PARTY_PROBES
  ${QAC_JAMSPELL_ROOT}/jamspell/spell_corrector.hpp
  ${QAC_HAT_TRIE_ROOT}/include/tsl/htrie_map.h
  ${QAC_ARRAY_HASH_ROOT}/include/tsl/array_map.h
  ${QAC_CEREAL_ROOT}/include/cereal/cereal.hpp
  ${QAC_ROBIN_MAP_ROOT}/include/tsl/robin_map.h)
foreach(probe IN LISTS QAC_THIRD_PARTY_PROBES)
  if(NOT EXISTS "${probe}")
    message(FATAL_ERROR
      "${probe} is missing.\n"
      "The third-party code lives in the JamSpell submodule and the submodules nested in it. Run:\n"
      "  git submodule update --init --recursive")
  endif()
endforeach()

# hat-trie (with the array-hash it includes) and cereal are header-only: interface targets.
add_library(qac_hat_trie INTERFACE)
target_include_directories(qac_hat_trie SYSTEM INTERFACE
  ${QAC_HAT_TRIE_ROOT}/include ${QAC_ARRAY_HASH_ROOT}/include)

add_library(qac_cereal INTERFACE)
target_include_directories(qac_cereal SYSTEM INTERFACE ${QAC_CEREAL_ROOT}/include)

# ---------------------------------------------------------------------------
# Core library: alphabet, tokenizer, index, WAL, snapshots, engine
# ---------------------------------------------------------------------------
add_library(qac_core STATIC
  src/log.cpp
  src/crc32c.cpp
  src/alphabet.cpp
  src/text.cpp
  src/index.cpp
  src/io.cpp
  src/snapshot.cpp
  src/wal.cpp
  src/dir_lock.cpp
  src/train.cpp
  src/engine.cpp
)
add_library(qac::core ALIAS qac_core)
target_include_directories(qac_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_options(qac_core PRIVATE ${QAC_DEFAULT_WARNINGS})
target_link_libraries(qac_core
  PUBLIC qac_hat_trie qac_cereal Boost::boost Threads::Threads)

# ---------------------------------------------------------------------------
# REST API (transport independent)
# ---------------------------------------------------------------------------
add_library(qac_api STATIC src/api.cpp src/http_server.cpp)
add_library(qac::api ALIAS qac_api)
target_compile_options(qac_api PRIVATE ${QAC_DEFAULT_WARNINGS})
target_link_libraries(qac_api PUBLIC qac_core Boost::json Boost::headers Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 13 reports a false positive from inside Boost.JSON (storage_ptr::release)
  # when it is built with sanitizer instrumentation.
  target_compile_options(qac_api PRIVATE -Wno-stringop-overflow)
endif()

# ---------------------------------------------------------------------------
# Spell correction through the JamSpell fork
# ---------------------------------------------------------------------------
add_subdirectory(cmake/jamspell jamspell_dep)
add_library(qac_jamspell_fixer STATIC src/jamspell_fixer.cpp)
add_library(qac::jamspell_fixer ALIAS qac_jamspell_fixer)
# Includes only qac/spell_fixer.hpp and qac/utf8.hpp (both dependency-free) plus the fork's
# headers, which keeps those large headers out of everything else in this project.
target_include_directories(qac_jamspell_fixer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(qac_jamspell_fixer PRIVATE qac::jamspell)
target_compile_options(qac_jamspell_fixer PRIVATE ${QAC_DEFAULT_WARNINGS})

# ---------------------------------------------------------------------------
# The command line tool / server
# ---------------------------------------------------------------------------
# The demo page is compiled in, so the binary is self-contained.
set(QAC_DEMO_PAGE ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${QAC_DEMO_PAGE})
file(READ ${QAC_DEMO_PAGE} QAC_DEMO_HTML)
if(QAC_DEMO_HTML MATCHES "\\)QACPAGE\"")
  message(FATAL_ERROR "web/index.html must not contain the raw-string terminator )QACPAGE\"")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/demo_page.cpp.tmp
  "// Generated from web/index.html. Do not edit.\nextern const char* const kQacDemoPage;\nconst char* const kQacDemoPage = R\"QACPAGE(${QAC_DEMO_HTML})QACPAGE\";\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
  ${CMAKE_CURRENT_BINARY_DIR}/generated/demo_page.cpp.tmp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/demo_page.cpp)

add_executable(qac src/main.cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/demo_page.cpp)
target_compile_options(qac PRIVATE ${QAC_DEFAULT_WARNINGS})
target_compile_definitions(qac PRIVATE
  QAC_BUNDLED_ALPHABET="${CMAKE_CURRENT_SOURCE_DIR}/data/alphabet.txt")
target_link_libraries(qac PRIVATE qac_api qac_jamspell_fixer Boost::program_options)

if(QAC_BUILD_BENCH)
  add_executable(qac_bench bench/bench.cpp)
  target_compile_options(qac_bench PRIVATE ${QAC_DEFAULT_WARNINGS})
  target_compile_definitions(qac_bench PRIVATE
    QAC_BENCH_ALPHABET="${CMAKE_CURRENT_SOURCE_DIR}/data/alphabet.txt")
  target_link_libraries(qac_bench PRIVATE qac_core Boost::program_options)
endif()

include(GNUInstallDirs)
install(TARGETS qac RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/alphabet.txt data/sample_queries.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/qac)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(QAC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
