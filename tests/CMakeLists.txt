find_package(GTest 1.11 QUIET)
if(NOT GTest_FOUND)
  message(STATUS "System GoogleTest not found; fetching v1.14.0")
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# One test executable per module: lob_<module>_tests.
function(lob_add_test_suite name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main lob_project_options)
  gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

lob_add_test_suite(lob_core_tests
  SOURCES
    core/test_types.cpp
    core/test_checked.cpp
    core/test_config.cpp
    core/test_validation.cpp
  LIBS lob::core)

lob_add_test_suite(lob_book_tests
  SOURCES
    book/test_order_book.cpp
    book/test_order_book_tamper.cpp
    book/test_order_book_differential.cpp
  LIBS lob::book lob::testkit)

lob_add_test_suite(lob_engine_tests
  SOURCES
    engine/test_matching_engine.cpp
    engine/test_event_auditor.cpp
    engine/test_engine_differential.cpp
  LIBS lob::engine lob::testkit)

lob_add_test_suite(lob_io_tests
  SOURCES
    io/test_text_format.cpp
    io/test_journal.cpp
    io/test_snapshot.cpp
    io/test_recovery.cpp
    io/test_session.cpp
    io/test_golden.cpp
  LIBS lob::io lob::testkit)
target_compile_definitions(lob_io_tests PRIVATE LOB_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

# Separate executables: lob_testkit_alloc_fault replaces the global
# operator new/delete of the binary it is linked into.
lob_add_test_suite(lob_book_alloc_tests
  SOURCES
    book/test_order_book_alloc_failure.cpp
  LIBS lob::book lob::testkit lob_testkit_alloc_fault)

lob_add_test_suite(lob_engine_alloc_tests
  SOURCES
    engine/test_engine_alloc_failure.cpp
    engine/test_replay_contract.cpp
  LIBS lob::engine lob::testkit lob_testkit_alloc_fault)
