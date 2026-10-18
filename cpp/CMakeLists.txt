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
    lib/param_store.cpp
    lib/aes128.cpp
    lib/creality_tag.cpp
    lib/sha256.cpp
    lib/runtime_config.cpp
    lib/app.cpp
    lib/tags/tag.cpp
    lib/tags/decoders.cpp
)
target_include_directories(cfs_core PUBLIC include)
if(MSVC)
    target_compile_options(cfs_core PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(cfs_core PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

# GD32F303 board port (K2 CFS). Built for the host only as a compile check: it
# touches MCU registers and must never run there. CI cross-compiles it for Cortex-M4.
option(CFS_BUILD_BOARD_CHECK "Compile the board port as a host syntax check" ON)
if(CFS_BUILD_BOARD_CHECK)
    add_library(cfs_board_check OBJECT
        board/k2cfs/board_io.cpp
        board/k2cfs/board_hal.cpp
        board/k2cfs/fm17622.cpp
        board/k2cfs/board_app.cpp
        board/k2cfs/tm1621.cpp
        board/k2cfs/bench.cpp
    )
    target_include_directories(cfs_board_check PRIVATE include board/k2cfs)
    if(MSVC)
        target_compile_options(cfs_board_check PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(cfs_board_check PRIVATE -Wall -Wextra -Werror -Wpedantic)
    endif()
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
    # Frame bridge for conformance tests against host code (docs/spec/14).
    add_executable(cfs_host_bridge tools/host_bridge.cpp)
    target_link_libraries(cfs_host_bridge PRIVATE cfs_sim)
endif()
