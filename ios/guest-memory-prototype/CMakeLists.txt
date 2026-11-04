cmake_minimum_required(VERSION 3.22)
project(GuestMemoryPrototypeTests LANGUAGES CXX)

enable_testing()
find_package(Threads REQUIRED)
add_executable(guest_memory_prototype_tests
    guest_memory.cpp
    guest_memory_tests.cpp
    guest_memory_prototype_cli.cpp
)
target_compile_features(guest_memory_prototype_tests PRIVATE cxx_std_20)
target_link_libraries(guest_memory_prototype_tests PRIVATE Threads::Threads)
add_test(NAME guest_memory_prototype_tests COMMAND guest_memory_prototype_tests)
