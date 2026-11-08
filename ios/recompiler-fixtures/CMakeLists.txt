cmake_minimum_required(VERSION 3.22)
project(MostWantedCheckedRecompilerFixtures LANGUAGES CXX)

include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(XBE_SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk)

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus)

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS ${XBE_SDK}/tools/recomp/*.cpp)
add_executable(xbe_recomp ${RECOMP_SOURCES})
target_include_directories(xbe_recomp PRIVATE ${XBE_SDK}/runtime ${XBE_SDK}/tools/recomp)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

add_executable(checked_fixture_builder fixture_builder.cpp)
target_include_directories(checked_fixture_builder PRIVATE ${XBE_SDK}/runtime)
set(FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixture)
set(FIXTURE_XBE ${FIXTURE_DIR}/checked_fixture.xbe)
set(FIXTURE_CONFIG ${FIXTURE_DIR}/checked_fixture.toml)
add_custom_command(
    OUTPUT ${FIXTURE_XBE} ${FIXTURE_CONFIG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FIXTURE_DIR}
    COMMAND checked_fixture_builder ${FIXTURE_XBE} ${FIXTURE_CONFIG}
    DEPENDS checked_fixture_builder
    VERBATIM
)

set(GENERATED_DIR ${FIXTURE_DIR}/generated)
set(GENERATED_SOURCE ${GENERATED_DIR}/x86_recomp.0.cpp)
set(GENERATED_MAPPING ${GENERATED_DIR}/x86_func_mapping.cpp)
add_custom_command(
    OUTPUT ${GENERATED_SOURCE} ${GENERATED_DIR}/x86_recomp_shared.h ${GENERATED_DIR}/x86_func_mapping.cpp
    COMMAND $<TARGET_FILE:xbe_recomp> ${FIXTURE_CONFIG} ${FIXTURE_XBE} ${GENERATED_DIR}
    DEPENDS xbe_recomp ${FIXTURE_XBE} ${FIXTURE_CONFIG}
    VERBATIM
)
add_custom_target(generate_checked_fixture DEPENDS ${GENERATED_SOURCE})

add_executable(checked_recompiler_tests
    fixture_runner.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/memory_protection.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${GENERATED_SOURCE}
    ${GENERATED_MAPPING}
)
add_executable(checked_fault_handler_tests
    fault_handler_test.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/guest_strings.cpp
    ${XBE_SDK}/runtime/kernel/rtl.cpp
    ${XBE_SDK}/runtime/cpu/memory_fault.cpp
    ${GENERATED_SOURCE}
    ${GENERATED_MAPPING}
)
add_executable(synthetic_startup_integration_tests
    startup_integration_tests.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/cpu/memory_fault.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/crypto.cpp
    ${XBE_SDK}/runtime/kernel/kernel_data_init.cpp
    ${XBE_SDK}/runtime/kernel/kernel_thunks.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${XBE_SDK}/runtime/xbe/image_loader.cpp
    ${GENERATED_SOURCE}
    ${GENERATED_MAPPING}
)
add_executable(vblank_runtime_tests
    vblank_runtime_tests.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/cpu/memory_fault.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${XBE_SDK}/runtime/kernel/vblank_worker.cpp
    ${GENERATED_SOURCE}
    ${GENERATED_MAPPING}
)
add_dependencies(checked_fault_handler_tests generate_checked_fixture)
add_dependencies(synthetic_startup_integration_tests generate_checked_fixture)
add_dependencies(vblank_runtime_tests generate_checked_fixture)
target_compile_features(checked_fault_handler_tests PRIVATE cxx_std_20)
target_compile_features(synthetic_startup_integration_tests PRIVATE cxx_std_20)
target_compile_features(vblank_runtime_tests PRIVATE cxx_std_20)
target_include_directories(checked_fault_handler_tests PRIVATE ${XBE_SDK}/runtime ${GENERATED_DIR})
target_include_directories(synthetic_startup_integration_tests PRIVATE ${XBE_SDK}/runtime ${GENERATED_DIR})
target_include_directories(vblank_runtime_tests PRIVATE ${XBE_SDK}/runtime ${GENERATED_DIR})
add_dependencies(checked_recompiler_tests generate_checked_fixture)
target_compile_features(checked_recompiler_tests PRIVATE cxx_std_20)
target_include_directories(checked_recompiler_tests PRIVATE ${XBE_SDK}/runtime ${GENERATED_DIR})
target_compile_definitions(checked_recompiler_tests PRIVATE MOSTWANTED_CHECKED_RECOMPILER_FIXTURE_CLI=1)
target_compile_definitions(checked_recompiler_tests PRIVATE MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1)
target_compile_definitions(checked_recompiler_tests PRIVATE MOSTWANTED_THREAD_PREPARATION_TESTS=1)
target_compile_definitions(checked_recompiler_tests PRIVATE MOSTWANTED_GUEST_CALLBACK_TESTS=1 MOSTWANTED_GUEST_PAGE_LEASE_TESTS=1)
target_link_libraries(checked_recompiler_tests PRIVATE Threads::Threads)
target_compile_definitions(synthetic_startup_integration_tests PRIVATE
    MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1
    MOSTWANTED_THREAD_PREPARATION_TESTS=1
    MOSTWANTED_GUEST_CALLBACK_TESTS=1
    MOSTWANTED_GUEST_PAGE_LEASE_TESTS=1
)
target_link_libraries(synthetic_startup_integration_tests PRIVATE Threads::Threads)
target_compile_definitions(vblank_runtime_tests PRIVATE
    MOSTWANTED_GUEST_CALLBACK_TESTS=1
    MOSTWANTED_GUEST_PAGE_LEASE_TESTS=1
    MOSTWANTED_KERNEL_WORKER_TESTS=1
    MOSTWANTED_VBLANK_RUNTIME_TESTS=1
)
target_link_libraries(vblank_runtime_tests PRIVATE Threads::Threads)
if(BUILD_TESTING)
    add_test(NAME checked_recompiler_tests COMMAND checked_recompiler_tests)
    add_test(NAME checked_fault_handler_tests COMMAND checked_fault_handler_tests)
    add_test(NAME synthetic_startup_integration_tests COMMAND synthetic_startup_integration_tests ${FIXTURE_XBE})
    add_test(NAME vblank_runtime_tests COMMAND vblank_runtime_tests)
endif()
