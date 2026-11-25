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
option(QAC_WITH_JAMSPELL "Spell correction through the JamSpell fork (third_party/jamspell)" ON)
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
find_package(Boost 1.75 REQUIRED CONFIG COMPONENTS program_options json)

foreach(dep hat-trie cereal)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep}")
    message(FATAL_ERROR
      "third_party/${dep} is missing. Run: git submodule update --init --recursive")
  endif()
endforeach()

# hat-trie and cereal are header-only: expose them as interface targets.
add_library(qac_hat_trie INTERFACE)
target_include_directories(qac_hat_trie SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hat-trie/include)

add_library(qac_cereal INTERFACE)
target_include_directories(qac_cereal SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cereal/include)

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
# Optional spell correction through the JamSpell fork
# ---------------------------------------------------------------------------
if(QAC_WITH_JAMSPELL)
  add_subdirectory(cmake/jamspell jamspell_dep)
  add_library(qac_jamspell_fixer STATIC src/jamspell_fixer.cpp)
  add_library(qac::jamspell_fixer ALIAS qac_jamspell_fixer)
  # Includes only qac/spell_fixer.hpp and qac/utf8.hpp (both dependency-free) plus the
  # fork's headers; linking qac_core here would put hat-trie on the include path of a
  # translation unit that sees the fork's differing copy of it.
  target_include_directories(qac_jamspell_fixer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_link_libraries(qac_jamspell_fixer PRIVATE qac::jamspell)
  target_compile_options(qac_jamspell_fixer PRIVATE ${QAC_DEFAULT_WARNINGS})
endif()

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
target_link_libraries(qac PRIVATE qac_api Boost::program_options)
if(QAC_WITH_JAMSPELL)
  target_compile_definitions(qac PRIVATE QAC_HAVE_JAMSPELL=1)
  target_link_libraries(qac PRIVATE qac_jamspell_fixer)
endif()

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
