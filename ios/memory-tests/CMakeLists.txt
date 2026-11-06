cmake_minimum_required(VERSION 3.22)
project(MostWantedDarwinMemoryTests LANGUAGES CXX)

enable_testing()
add_executable(mostwanted_memory_tests
    ../../sdk/runtime/core/log.cpp
    ../../sdk/runtime/kernel/crypto.cpp
    ../../sdk/runtime/kernel/memory.cpp
    ../../sdk/runtime/kernel/clock_worker.cpp
    ../../sdk/runtime/kernel/kernel_data_init.cpp
    ../../sdk/runtime/cpu/dispatch_table.cpp
    ../../sdk/runtime/kernel/kernel_thunks.cpp
    ../../sdk/runtime/kernel/filesystem_context.cpp
    ../../sdk/runtime/kernel/startup_data.cpp
    ../../sdk/runtime/kernel/checked_file_transfer.cpp
    ../../sdk/runtime/kernel/checked_gpu_transfer.cpp
    ../../sdk/runtime/kernel/guest_strings.cpp
    ../../sdk/runtime/xbe/image_loader.cpp
    memory_tests.cpp
    xbe_image_tests.cpp
    startup_data_tests.cpp
    checked_file_transfer_tests.cpp
    checked_gpu_transfer_tests.cpp
    clock_worker_tests.cpp
    dispatch_table_tests.cpp
    kernel_thunk_tests.cpp
    filesystem_setup_tests.cpp
    memory_test_cli.cpp
)
target_compile_features(mostwanted_memory_tests PRIVATE cxx_std_20)
target_include_directories(mostwanted_memory_tests PRIVATE ../../sdk/runtime)
add_test(NAME mostwanted_memory_tests COMMAND mostwanted_memory_tests)
