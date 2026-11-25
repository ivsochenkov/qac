if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
  message(FATAL_ERROR "third_party/googletest is missing. Run: git submodule update --init --recursive")
endif()

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/googletest googletest EXCLUDE_FROM_ALL)

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

if(QAC_WITH_JAMSPELL)
  # The fork's headers cannot be mixed with qac's, so everything that touches its
  # training API sits in a separate library. The model is trained once, at build
  # time (training takes several GB of RAM), and the tests only load it.
  set(JS ${PROJECT_SOURCE_DIR}/third_party/jamspell)
  add_library(qac_jamspell_testkit STATIC jamspell_train.cpp)
  target_link_libraries(qac_jamspell_testkit PRIVATE qac::jamspell)
  add_executable(qac_jamspell_train_model jamspell_train_main.cpp)
  target_link_libraries(qac_jamspell_train_model PRIVATE qac_jamspell_testkit)

  set(QAC_TEST_MODEL ${CMAKE_CURRENT_BINARY_DIR}/jamspell_test_model.bin)
  add_custom_command(
    OUTPUT ${QAC_TEST_MODEL}.stamp
    COMMAND qac_jamspell_train_model ${JS}/test_data/sherlockholmes.txt ${JS}/data/dict.txt
            ${JS}/data/ru_en_alphabet.txt ${QAC_TEST_MODEL}
    COMMAND ${CMAKE_COMMAND} -E touch ${QAC_TEST_MODEL}.stamp
    DEPENDS qac_jamspell_train_model ${JS}/test_data/sherlockholmes.txt ${JS}/data/dict.txt
            ${JS}/data/ru_en_alphabet.txt
    COMMENT "Training the JamSpell test model"
    VERBATIM)
  add_custom_target(qac_jamspell_test_model DEPENDS ${QAC_TEST_MODEL}.stamp)

  target_sources(qac_tests PRIVATE test_jamspell.cpp)
  add_dependencies(qac_tests qac_jamspell_test_model)
  target_link_libraries(qac_tests PRIVATE qac_jamspell_fixer)
  target_compile_definitions(qac_tests PRIVATE QAC_JAMSPELL_MODEL="${QAC_TEST_MODEL}")
endif()
