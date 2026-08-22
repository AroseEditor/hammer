cmake_minimum_required(VERSION 3.20)
project(hammer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(HAMMER_VERSION "dev" CACHE STRING "version string baked into the binary")
set(HAMMER_SANITIZE "off" CACHE STRING "off | address | thread")

include(CheckCXXSourceCompiles)

function(hammer_probe_sanitizer flags out_var)
  set(CMAKE_REQUIRED_FLAGS "${flags}")
  set(CMAKE_REQUIRED_LINK_OPTIONS ${flags})
  check_cxx_source_compiles("int main() { return 0; }" ${out_var})
  set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

add_library(hammer_build_opts INTERFACE)

if(MSVC)
  target_compile_options(hammer_build_opts INTERFACE /W4 /permissive- /Zc:preprocessor /utf-8)
  target_compile_definitions(hammer_build_opts INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(hammer_build_opts INTERFACE -Wall -Wextra -Wpedantic)
endif()

set(HAMMER_SANITIZER_ACTIVE "none")

if(HAMMER_SANITIZE STREQUAL "address")
  if(MSVC)
    set(_san_flags "/fsanitize=address")
  else()
    set(_san_flags "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all")
  endif()
  hammer_probe_sanitizer("${_san_flags}" HAMMER_HAS_ASAN)
  if(HAMMER_HAS_ASAN)
    separate_arguments(_san_list NATIVE_COMMAND "${_san_flags}")
    target_compile_options(hammer_build_opts INTERFACE ${_san_list})
    target_link_options(hammer_build_opts INTERFACE ${_san_list})
    set(HAMMER_SANITIZER_ACTIVE "address")
  endif()
elseif(HAMMER_SANITIZE STREQUAL "thread")
  if(MSVC)
    message(FATAL_ERROR "thread sanitizer has no MSVC implementation; use the tsan preset on linux")
  endif()
  set(_san_flags "-fsanitize=thread -fno-omit-frame-pointer")
  hammer_probe_sanitizer("${_san_flags}" HAMMER_HAS_TSAN)
  if(HAMMER_HAS_TSAN)
    separate_arguments(_san_list NATIVE_COMMAND "${_san_flags}")
    target_compile_options(hammer_build_opts INTERFACE ${_san_list})
    target_link_options(hammer_build_opts INTERFACE ${_san_list})
    set(HAMMER_SANITIZER_ACTIVE "thread")
  endif()
endif()

if(NOT HAMMER_SANITIZE STREQUAL "off" AND HAMMER_SANITIZER_ACTIVE STREQUAL "none")
  message(WARNING "HAMMER_SANITIZE=${HAMMER_SANITIZE} requested but this toolchain cannot link it; building unsanitized")
endif()

message(STATUS "hammer version ${HAMMER_VERSION}, sanitizer ${HAMMER_SANITIZER_ACTIVE}")

if(WIN32)
  set(HAMMER_POLLER_SOURCE src/poller_wsapoll.cpp)
else()
  set(HAMMER_POLLER_SOURCE src/poller_epoll.cpp)
endif()

add_library(hammer_core STATIC
  src/cli.cpp
  src/http_parser.cpp
  src/histogram.cpp
  src/conn.cpp
  src/stats.cpp
  src/loop.cpp
  src/worker.cpp
  ${HAMMER_POLLER_SOURCE}
)
target_include_directories(hammer_core PUBLIC src)
target_compile_definitions(hammer_core PUBLIC HAMMER_VERSION_STRING="${HAMMER_VERSION}")
target_link_libraries(hammer_core PUBLIC hammer_build_opts)

if(WIN32)
  target_link_libraries(hammer_core PUBLIC ws2_32 winmm)
endif()

add_executable(hammer src/main.cpp)
target_link_libraries(hammer PRIVATE hammer_core)

include(CTest)
enable_testing()

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(hammer_tests
  tests/test_cli.cpp
  tests/test_parser.cpp
  tests/test_histogram.cpp
  tests/test_integration.cpp
  tests/test_alloc.cpp
)
target_link_libraries(hammer_tests PRIVATE hammer_core Catch2::Catch2WithMain)
target_include_directories(hammer_tests PRIVATE tests)

find_package(Threads REQUIRED)
target_link_libraries(hammer_core PUBLIC Threads::Threads)

catch_discover_tests(hammer_tests)

find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  add_test(
    NAME hostile_server_modes
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/hostile_suite.py
            $<TARGET_FILE:hammer>)
  set_tests_properties(hostile_server_modes PROPERTIES TIMEOUT 400)
else()
  message(WARNING "python3 not found; the hostile server suite will not run")
endif()

option(HAMMER_FUZZ "build the libFuzzer parser target (clang only)" OFF)

if(HAMMER_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "HAMMER_FUZZ needs clang; ${CMAKE_CXX_COMPILER_ID} has no libFuzzer")
  endif()
  add_executable(fuzz_parser tests/fuzz_parser.cpp src/http_parser.cpp)
  target_include_directories(fuzz_parser PRIVATE src)
  target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined -g)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
