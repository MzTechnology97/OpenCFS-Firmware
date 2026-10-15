cmake_minimum_required(VERSION 3.20)
project(cfs_reimpl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Portable core: no heap, no exceptions in the library code, RTOS/board via cfs/hal.hpp.
add_library(cfs_core
    lib/frame.cpp
    lib/feeder.cpp
    lib/spool.cpp
    lib/preload.cpp
    lib/print_tracker.cpp
    lib/dispatcher.cpp
    lib/auto_address.cpp
    lib/usage.cpp
    lib/supervisor.cpp
)
target_include_directories(cfs_core PUBLIC include)
if(MSVC)
    target_compile_options(cfs_core PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(cfs_core PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

option(CFS_BUILD_TESTS "Build the host simulator and tests" ON)
if(CFS_BUILD_TESTS)
    enable_testing()
    add_library(cfs_sim sim/sim.cpp)
    target_include_directories(cfs_sim PUBLIC sim)
    target_link_libraries(cfs_sim PUBLIC cfs_core)
    add_executable(cfs_tests tests/test_main.cpp)
    target_link_libraries(cfs_tests PRIVATE cfs_sim)
    add_test(NAME cfs_tests COMMAND cfs_tests)
endif()
