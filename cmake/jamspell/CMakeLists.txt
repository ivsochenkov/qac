# Builds the JamSpell fork (git submodule third_party/jamspell) as an external
# dependency, without modifying it.
#
# The fork's root CMakeLists cannot be add_subdirectory()'d (it puts its own root
# on the include path, which would leak into this project), so only its library
# and contrib directories are added here, with the include path scoped to this
# directory. The result is the interface target qac::jamspell.

set(QAC_JAMSPELL_ROOT ${PROJECT_SOURCE_DIR}/third_party/jamspell)
if(NOT EXISTS "${QAC_JAMSPELL_ROOT}/jamspell/spell_corrector.hpp")
  message(FATAL_ERROR
    "third_party/jamspell is missing. Run: git submodule update --init --recursive\n"
    "(or configure with -DQAC_WITH_JAMSPELL=OFF)")
endif()

set(Boost_USE_MULTITHREADED ON)
find_package(Boost CONFIG REQUIRED COMPONENTS locale)

# The fork includes its own headers as <contrib/...> and <jamspell/...>.
include_directories(${QAC_JAMSPELL_ROOT})

add_subdirectory(${QAC_JAMSPELL_ROOT}/contrib contrib EXCLUDE_FROM_ALL)
add_subdirectory(${QAC_JAMSPELL_ROOT}/jamspell jamspell EXCLUDE_FROM_ALL)

# The fork bundles its own, older copy of the tsl hash containers (namespace `tsl`,
# the same include guards and template names as the hat-trie dependency of this
# project). Linking both into one binary would let the linker merge same-named
# template instantiations that have different definitions (a silent ODR
# violation), and including both in one translation unit does not compile.
# Every translation unit that sees the fork's headers is therefore built with the
# fork's `tsl` renamed; nothing in the fork's sources has to change.
set(QAC_JAMSPELL_ISOLATION tsl=qac_jamspell_tsl)
target_compile_definitions(sivspell_lib PRIVATE ${QAC_JAMSPELL_ISOLATION})

add_library(qac_jamspell INTERFACE)
add_library(qac::jamspell ALIAS qac_jamspell)
target_include_directories(qac_jamspell SYSTEM INTERFACE ${QAC_JAMSPELL_ROOT})
# Only meant for translation units that include the fork's headers and none of this
# project's headers (the wrapper and the test helper below). Do not link
# qac::jamspell into code that includes qac/index.hpp.
target_compile_definitions(qac_jamspell INTERFACE ${QAC_JAMSPELL_ISOLATION})
target_link_libraries(qac_jamspell INTERFACE sivspell_lib Boost::locale Threads::Threads)
