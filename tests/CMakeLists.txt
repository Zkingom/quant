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
