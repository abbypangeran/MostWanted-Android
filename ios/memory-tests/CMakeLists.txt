cmake_minimum_required(VERSION 3.22)
project(MostWantedDarwinMemoryTests LANGUAGES CXX)

enable_testing()
add_executable(mostwanted_memory_tests
    ../../sdk/runtime/core/log.cpp
    ../../sdk/runtime/kernel/memory.cpp
    ../../sdk/runtime/kernel/guest_strings.cpp
    memory_tests.cpp
    memory_test_cli.cpp
)
target_compile_features(mostwanted_memory_tests PRIVATE cxx_std_20)
target_include_directories(mostwanted_memory_tests PRIVATE ../../sdk/runtime)
add_test(NAME mostwanted_memory_tests COMMAND mostwanted_memory_tests)
