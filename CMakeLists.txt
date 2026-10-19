cmake_minimum_required(VERSION 3.25)

project(deterministic_lob
  VERSION 0.1.0
  DESCRIPTION "Deterministic limit order book and matching engine"
  LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "Only GCC and Clang are supported (got ${CMAKE_CXX_COMPILER_ID})")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_EXAMPLES "Build example programs" ON)
option(LOB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

add_subdirectory(src)

if(LOB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(LOB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
