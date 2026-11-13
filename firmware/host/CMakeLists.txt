# AST data link - host (PC) build: unit tests, ThreadSanitizer test and the
# ast_host_device executable. Uses the SAME sources as the ESP32 sketch
# (firmware/core). No downloads.
#
#   cmake -S firmware/host -B firmware/host/build && cmake --build firmware/host/build -j2
#   ctest --test-dir firmware/host/build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(ast_datalink_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core)
file(GLOB CORE_SOURCES ${CORE_DIR}/*.cpp)
set(WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
set(ASAN -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
set(TSAN -fsanitize=thread -fno-omit-frame-pointer)
find_package(Threads REQUIRED)

# Core built three ways: AddressSanitizer+UBSan (unit tests), ThreadSanitizer
# (concurrency test) and plain (the device executable used by the E2E tests).
function(add_core name)
  add_library(${name} STATIC ${CORE_SOURCES})
  target_include_directories(${name} PUBLIC ${CORE_DIR})
  target_compile_options(${name} PRIVATE ${WARNINGS} ${ARGN})
  target_compile_options(${name} PUBLIC ${ARGN})
  target_link_options(${name} PUBLIC ${ARGN})
  target_link_libraries(${name} PUBLIC Threads::Threads)
endfunction()
add_core(ast_core_asan ${ASAN})
add_core(ast_core_tsan ${TSAN})
add_core(ast_core)

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
add_executable(ast_tests ${TEST_SOURCES} host_http.cpp)
target_include_directories(ast_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_options(ast_tests PRIVATE ${WARNINGS})
target_link_libraries(ast_tests PRIVATE ast_core_asan)

add_executable(ast_tsan_test tests/tsan_concurrency.cpp)
target_include_directories(ast_tsan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_options(ast_tsan_test PRIVATE ${WARNINGS})
target_link_libraries(ast_tsan_test PRIVATE ast_core_tsan)

add_executable(ast_host_device host_device.cpp host_http.cpp)
target_include_directories(ast_host_device PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ast_host_device PRIVATE ${WARNINGS})
target_link_libraries(ast_host_device PRIVATE ast_core)

enable_testing()
add_test(NAME unit_tests_asan_ubsan COMMAND ast_tests)
add_test(NAME concurrency_tsan COMMAND ast_tsan_test)
add_test(NAME host_device_usage COMMAND ast_host_device --selftest)
