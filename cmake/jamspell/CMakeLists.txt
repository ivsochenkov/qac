# Builds the JamSpell fork (git submodule third_party/jamspell, branch master) as an
# external dependency, without modifying it. The result is the interface target
# qac::jamspell.
#
# The fork's root CMakeLists cannot be add_subdirectory()'d (it puts its own root on the
# include path of everything below it), so only its contrib and library directories are
# added here, with the include path scoped to this directory.
#
# Third-party code is shared: the fork builds against the hat-trie, array-hash and robin-map
# in its own third_party directory, and the top-level CMakeLists.txt gives qac the very same
# hat-trie, array-hash and cereal. There is one copy of each, so JamSpell's and qac's code may
# be mixed in one binary (and, in principle, one translation unit).

set(QAC_JAMSPELL_ROOT ${PROJECT_SOURCE_DIR}/third_party/jamspell)
if(NOT EXISTS "${QAC_JAMSPELL_ROOT}/jamspell/spell_corrector.hpp")
  message(FATAL_ERROR
    "third_party/jamspell is missing. Run: git submodule update --init --recursive")
endif()

set(Boost_USE_MULTITHREADED ON)

# The fork includes its own headers as <contrib/...> and <jamspell/...>.
include_directories(${QAC_JAMSPELL_ROOT})

add_subdirectory(${QAC_JAMSPELL_ROOT}/contrib contrib EXCLUDE_FROM_ALL)
add_subdirectory(${QAC_JAMSPELL_ROOT}/jamspell jamspell EXCLUDE_FROM_ALL)

# Workaround for the fork's jamspell/CMakeLists.txt (master 90b9483 and probably older): for
# hat-trie's array-hash it adds third_party/hat-trie/third_party/include, which does not
# exist; array-hash is the submodule third_party/hat-trie/third_party/array-hash. Without
# this, spell_corrector.cpp fails with "tsl/array_map.h: No such file or directory".
# Harmless once the fork is fixed (the same directory is simply listed twice). The fix for the
# fork is in docs/jamspell-fork-array-hash-include.patch.
target_include_directories(sivspell_lib PUBLIC "${QAC_ARRAY_HASH_ROOT}/include")

add_library(qac_jamspell INTERFACE)
add_library(qac::jamspell ALIAS qac_jamspell)
target_include_directories(qac_jamspell SYSTEM INTERFACE ${QAC_JAMSPELL_ROOT})
target_link_libraries(qac_jamspell INTERFACE sivspell_lib Boost::locale Threads::Threads)
