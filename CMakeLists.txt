cmake_minimum_required(VERSION 3.20)
project(lob LANGUAGES CXX VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOB_NATIVE   "Tune for this machine's CPU (-march/-mcpu=native)" OFF)
option(LOB_TESTS    "Build tests" ON)
option(LOB_BENCH    "Build benchmarks" ON)

# The library is header-only: the books are templates on their listener.
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)

if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()
if(LOB_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
  if(HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  else()
    add_compile_options(-mcpu=native)
  endif()
endif()

if(LOB_TESTS)
  enable_testing()
  # A sanitized test binary must not link a prebuilt, uninstrumented GoogleTest:
  # libc++'s container annotations then disagree between the two and can report
  # false container-overflows. Under LOB_SANITIZE, GoogleTest is built here with
  # the same flags.
  if(NOT LOB_SANITIZE)
    find_package(GTest CONFIG QUIET)
  endif()
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)
  set(LOB_TEST_SOURCES spec property components)
  # test_alloc replaces the global operator new/delete to count heap calls.
  # The sanitizers replace them too, so the two cannot coexist in one binary;
  # the test runs in the ordinary builds, where its count is meaningful.
  if(NOT LOB_SANITIZE)
    list(APPEND LOB_TEST_SOURCES alloc)
  endif()
  foreach(t IN LISTS LOB_TEST_SOURCES)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE lob lob_warnings GTest::gtest_main)
    gtest_discover_tests(test_${t} DISCOVERY_TIMEOUT 60)
  endforeach()
endif()

if(LOB_BENCH)
  add_executable(gen_day bench/gen_day.cpp)
  target_link_libraries(gen_day PRIVATE lob lob_warnings)
  add_executable(replay bench/replay.cpp)
  target_link_libraries(replay PRIVATE lob lob_warnings)
endif()
