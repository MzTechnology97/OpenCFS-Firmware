cmake_minimum_required(VERSION 3.16)
project(OpenCFS VERSION 0.1.0 LANGUAGES C)

option(OPEN_CFS_BUILD_HOST_TESTS "Build host-only test harness" ON)

add_library(opencfs_core STATIC
    src/safety_gate.c
    src/rs485_rx.c
)
target_include_directories(opencfs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(opencfs_core PUBLIC c_std_11)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(opencfs_core PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

if(OPEN_CFS_BUILD_HOST_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(opencfs_safety_tests tests/test_safety_gate.c)
    target_link_libraries(opencfs_safety_tests PRIVATE opencfs_core Threads::Threads)
    target_compile_features(opencfs_safety_tests PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(opencfs_safety_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
    endif()
    add_test(NAME safety_gate COMMAND opencfs_safety_tests)
    add_executable(opencfs_rs485_rx_tests tests/test_rs485_rx.c)
    target_link_libraries(opencfs_rs485_rx_tests PRIVATE opencfs_core)
    target_compile_features(opencfs_rs485_rx_tests PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(opencfs_rs485_rx_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
    endif()
    add_test(NAME rs485_rx COMMAND opencfs_rs485_rx_tests)
endif()

# Deliberately no target ELF/BIN, startup, linker script, GPIO writes,
# bootloader updater, or flash target. Only a portable static library.
