# googletest is not part of the JamSpell submodule tree, so the tests use an installed one
# (Debian/Ubuntu: libgtest-dev; Nix: the gtest package), as the fork's own tests do.
find_package(GTest REQUIRED)

set(QAC_TEST_SOURCES
  test_index.cpp
  test_hot.cpp
  test_tail.cpp
  test_text.cpp
  test_wal.cpp
  test_engine.cpp
  test_train.cpp
  test_api.cpp
  test_http.cpp
  test_jamspell.cpp
)

add_executable(qac_tests ${QAC_TEST_SOURCES})
target_link_libraries(qac_tests PRIVATE qac_api qac_core GTest::gtest_main)
target_compile_definitions(qac_tests PRIVATE QAC_DATA_DIR="${PROJECT_SOURCE_DIR}/data")
target_compile_options(qac_tests PRIVATE ${QAC_DEFAULT_WARNINGS})
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(qac_tests PRIVATE -Wno-stringop-overflow)  # false positive inside Boost.JSON
endif()
include(GoogleTest)
gtest_discover_tests(qac_tests DISCOVERY_TIMEOUT 60)

# Everything that touches the fork's training API sits in a separate library, which keeps
# its large headers out of the rest of the tests. The model is trained once, at build
# time, and the tests only load it. By default the fork reserves memory for 100 million
# n-grams and a 5 million word dictionary (about 4 GB) however small the text is; the
# test text has ~155 thousand n-grams, so the fork's own SPLL_* variables lower the
# reservation. Nothing else changes (they are capacities and caps far above the data), and
# training needs about 250 MB.
set(JS ${PROJECT_SOURCE_DIR}/third_party/jamspell)
add_library(qac_jamspell_testkit STATIC jamspell_train.cpp)
target_link_libraries(qac_jamspell_testkit PRIVATE qac::jamspell)
add_executable(qac_jamspell_train_model jamspell_train_main.cpp)
target_link_libraries(qac_jamspell_train_model PRIVATE qac_jamspell_testkit)

set(QAC_TEST_MODEL ${CMAKE_CURRENT_BINARY_DIR}/jamspell_test_model.bin)
add_custom_command(
  OUTPUT ${QAC_TEST_MODEL}.stamp
  COMMAND ${CMAKE_COMMAND} -E env SPLL_MAX_GRAMS_SIZE=4000000 SPLL_MAX_DICT_SIZE=200000
          $<TARGET_FILE:qac_jamspell_train_model>
          ${JS}/test_data/sherlockholmes.txt ${JS}/data/dict.txt
          ${JS}/data/ru_en_alphabet.txt ${QAC_TEST_MODEL}
  COMMAND ${CMAKE_COMMAND} -E touch ${QAC_TEST_MODEL}.stamp
  DEPENDS qac_jamspell_train_model ${JS}/test_data/sherlockholmes.txt ${JS}/data/dict.txt
          ${JS}/data/ru_en_alphabet.txt
  COMMENT "Training the JamSpell test model"
  VERBATIM)
add_custom_target(qac_jamspell_test_model DEPENDS ${QAC_TEST_MODEL}.stamp)

add_dependencies(qac_tests qac_jamspell_test_model)
target_link_libraries(qac_tests PRIVATE qac_jamspell_fixer)
target_compile_definitions(qac_tests PRIVATE QAC_JAMSPELL_MODEL="${QAC_TEST_MODEL}")
