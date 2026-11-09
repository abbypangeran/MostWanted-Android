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

# Phase 2C-15: a second synthetic image whose code occupies the audited startup
# addresses and whose config instruments every reviewed instruction.
add_executable(execution_probe_fixture_builder execution_probe_fixture_builder.cpp)
set_property(TARGET execution_probe_fixture_builder PROPERTY CXX_STANDARD 20)
target_include_directories(execution_probe_fixture_builder PRIVATE ${XBE_SDK}/runtime ${CMAKE_CURRENT_SOURCE_DIR})
set(PROBE_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/probe_fixture)
set(PROBE_FIXTURE_XBE ${PROBE_FIXTURE_DIR}/execution_probe_fixture.xbe)
set(PROBE_FIXTURE_CONFIG ${PROBE_FIXTURE_DIR}/execution_probe_fixture.toml)
add_custom_command(
    OUTPUT ${PROBE_FIXTURE_XBE} ${PROBE_FIXTURE_CONFIG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROBE_FIXTURE_DIR}
    COMMAND execution_probe_fixture_builder ${PROBE_FIXTURE_XBE} ${PROBE_FIXTURE_CONFIG}
    DEPENDS execution_probe_fixture_builder
    VERBATIM
)
set(PROBE_GENERATED_DIR ${PROBE_FIXTURE_DIR}/generated)
set(PROBE_GENERATED_SOURCE ${PROBE_GENERATED_DIR}/x86_recomp.0.cpp)
set(PROBE_GENERATED_MAPPING ${PROBE_GENERATED_DIR}/x86_func_mapping.cpp)
add_custom_command(
    OUTPUT ${PROBE_GENERATED_SOURCE} ${PROBE_GENERATED_DIR}/x86_recomp_shared.h ${PROBE_GENERATED_MAPPING}
    COMMAND $<TARGET_FILE:xbe_recomp> ${PROBE_FIXTURE_CONFIG} ${PROBE_FIXTURE_XBE} ${PROBE_GENERATED_DIR}
    DEPENDS xbe_recomp ${PROBE_FIXTURE_XBE} ${PROBE_FIXTURE_CONFIG}
    VERBATIM
)
add_custom_target(generate_execution_probe_fixture DEPENDS ${PROBE_GENERATED_SOURCE})

add_executable(execution_probe_tests
    execution_probe.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/crypto.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/memory_protection.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/xbe/image_loader.cpp
    ${PROBE_GENERATED_SOURCE}
    ${PROBE_GENERATED_MAPPING}
)
add_dependencies(execution_probe_tests generate_execution_probe_fixture)
target_compile_features(execution_probe_tests PRIVATE cxx_std_20)
target_include_directories(execution_probe_tests PRIVATE ${XBE_SDK}/runtime ${PROBE_GENERATED_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(execution_probe_tests PRIVATE
    MOSTWANTED_EXECUTION_PROBE_TESTS=1
    MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1
    MOSTWANTED_THREAD_PREPARATION_TESTS=1
    EXECUTION_PROBE_MODE_SYNTHETIC=1
    EXECUTION_PROBE_FIXTURE_XBE="${PROBE_FIXTURE_XBE}"
    EXECUTION_PROBE_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
target_link_libraries(execution_probe_tests PRIVATE Threads::Threads Zydis)

# Phase 2C-16: a dedicated real-image mapping with the same 50 hooks.  The
# target is excluded from the default build and fails if the real XBE is absent.
set(EXECUTION_PROBE_REAL_XBE "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/default.xbe" CACHE FILEPATH
    "Validated NFS Most Wanted Black Edition default.xbe")
add_executable(execution_probe_real_config execution_probe_real_config.cpp)
set_property(TARGET execution_probe_real_config PROPERTY CXX_STANDARD 20)
target_include_directories(execution_probe_real_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set(REAL_PROBE_DIR ${CMAKE_CURRENT_BINARY_DIR}/probe_real)
set(REAL_PROBE_CONFIG ${REAL_PROBE_DIR}/execution_probe_real.toml)
set(REAL_PROBE_GENERATED_DIR ${REAL_PROBE_DIR}/generated)
set(REAL_PROBE_GENERATED_SOURCE ${REAL_PROBE_GENERATED_DIR}/x86_recomp.0.cpp)
set(REAL_PROBE_GENERATED_MAPPING ${REAL_PROBE_GENERATED_DIR}/x86_func_mapping.cpp)
add_custom_command(
    OUTPUT ${REAL_PROBE_CONFIG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${REAL_PROBE_DIR}
    COMMAND execution_probe_real_config ${REAL_PROBE_CONFIG} ${EXECUTION_PROBE_REAL_XBE}
    DEPENDS execution_probe_real_config
    VERBATIM
)
add_custom_command(
    OUTPUT ${REAL_PROBE_GENERATED_SOURCE} ${REAL_PROBE_GENERATED_DIR}/x86_recomp_shared.h ${REAL_PROBE_GENERATED_MAPPING}
    COMMAND $<TARGET_FILE:xbe_recomp> ${REAL_PROBE_CONFIG} ${EXECUTION_PROBE_REAL_XBE} ${REAL_PROBE_GENERATED_DIR}
    DEPENDS xbe_recomp ${REAL_PROBE_CONFIG}
    VERBATIM
)
add_custom_target(generate_execution_probe_real DEPENDS ${REAL_PROBE_GENERATED_SOURCE})
set_target_properties(generate_execution_probe_real PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_executable(execution_probe_real_preflight
    execution_probe.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/crypto.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/memory_protection.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/xbe/image_loader.cpp
    ${REAL_PROBE_GENERATED_SOURCE}
    ${REAL_PROBE_GENERATED_MAPPING}
)
add_dependencies(execution_probe_real_preflight generate_execution_probe_real)
target_compile_features(execution_probe_real_preflight PRIVATE cxx_std_20)
target_include_directories(execution_probe_real_preflight PRIVATE ${XBE_SDK}/runtime ${REAL_PROBE_GENERATED_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(execution_probe_real_preflight PRIVATE
    MOSTWANTED_EXECUTION_PROBE_TESTS=1
    MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1
    MOSTWANTED_THREAD_PREPARATION_TESTS=1
    EXECUTION_PROBE_MODE_REAL=1
    EXECUTION_PROBE_REAL_XBE="${EXECUTION_PROBE_REAL_XBE}"
    EXECUTION_PROBE_REAL_GENERATED_DIR="${REAL_PROBE_GENERATED_DIR}"
    EXECUTION_PROBE_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
target_link_libraries(execution_probe_real_preflight PRIVATE Threads::Threads Zydis)
set_target_properties(execution_probe_real_preflight PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Phase 2C-33: a separate real-image mapping for the secondary-thread SEH/TLS
# route.  It is deliberately independent of the root probe's generated mapping.
add_executable(execution_probe_secondary_real_config execution_probe_secondary_real_config.cpp)
set_property(TARGET execution_probe_secondary_real_config PROPERTY CXX_STANDARD 20)
target_include_directories(execution_probe_secondary_real_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set(SECONDARY_REAL_PROBE_DIR ${CMAKE_CURRENT_BINARY_DIR}/probe_secondary_real)
set(SECONDARY_REAL_PROBE_CONFIG ${SECONDARY_REAL_PROBE_DIR}/execution_probe_secondary_real.toml)
set(SECONDARY_REAL_PROBE_GENERATED_DIR ${SECONDARY_REAL_PROBE_DIR}/generated)
set(SECONDARY_REAL_PROBE_GENERATED_SOURCE ${SECONDARY_REAL_PROBE_GENERATED_DIR}/x86_recomp.0.cpp)
set(SECONDARY_REAL_PROBE_GENERATED_MAPPING ${SECONDARY_REAL_PROBE_GENERATED_DIR}/x86_func_mapping.cpp)
add_custom_command(
    OUTPUT ${SECONDARY_REAL_PROBE_CONFIG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SECONDARY_REAL_PROBE_DIR}
    COMMAND execution_probe_secondary_real_config ${SECONDARY_REAL_PROBE_CONFIG} ${EXECUTION_PROBE_REAL_XBE}
    DEPENDS execution_probe_secondary_real_config
    VERBATIM
)
add_custom_command(
    OUTPUT ${SECONDARY_REAL_PROBE_GENERATED_SOURCE} ${SECONDARY_REAL_PROBE_GENERATED_DIR}/x86_recomp_shared.h ${SECONDARY_REAL_PROBE_GENERATED_MAPPING}
    COMMAND $<TARGET_FILE:xbe_recomp> ${SECONDARY_REAL_PROBE_CONFIG} ${EXECUTION_PROBE_REAL_XBE} ${SECONDARY_REAL_PROBE_GENERATED_DIR}
    DEPENDS xbe_recomp ${SECONDARY_REAL_PROBE_CONFIG}
    VERBATIM
)
add_custom_target(generate_execution_probe_secondary_real DEPENDS ${SECONDARY_REAL_PROBE_GENERATED_SOURCE})
set_target_properties(generate_execution_probe_secondary_real PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_executable(execution_probe_secondary_real_preflight
    execution_probe_secondary_real.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/crypto.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/memory_protection.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/xbe/image_loader.cpp
    ${SECONDARY_REAL_PROBE_GENERATED_SOURCE}
    ${SECONDARY_REAL_PROBE_GENERATED_MAPPING}
)
add_dependencies(execution_probe_secondary_real_preflight generate_execution_probe_secondary_real)
target_compile_features(execution_probe_secondary_real_preflight PRIVATE cxx_std_20)
target_include_directories(execution_probe_secondary_real_preflight PRIVATE ${XBE_SDK}/runtime ${SECONDARY_REAL_PROBE_GENERATED_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(execution_probe_secondary_real_preflight PRIVATE
    MOSTWANTED_EXECUTION_PROBE_TESTS=1
    MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1
    MOSTWANTED_THREAD_PREPARATION_TESTS=1
    MOSTWANTED_GUEST_CALLBACK_TESTS=1
    EXECUTION_PROBE_SECONDARY_REAL_XBE="${EXECUTION_PROBE_REAL_XBE}"
    EXECUTION_PROBE_SECONDARY_REAL_GENERATED_DIR="${SECONDARY_REAL_PROBE_GENERATED_DIR}"
    EXECUTION_PROBE_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
target_link_libraries(execution_probe_secondary_real_preflight PRIVATE Threads::Threads Zydis)
set_target_properties(execution_probe_secondary_real_preflight PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Phase 2C-35: one combined mapping carries both frozen routes so the retained
# ThreadRef survives without a dispatch reset or a second generated mapping.
add_executable(execution_probe_integrated_config execution_probe_integrated_config.cpp)
set_property(TARGET execution_probe_integrated_config PROPERTY CXX_STANDARD 20)
target_include_directories(execution_probe_integrated_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set(INTEGRATED_PROBE_DIR ${CMAKE_CURRENT_BINARY_DIR}/probe_integrated)
set(INTEGRATED_PROBE_CONFIG ${INTEGRATED_PROBE_DIR}/execution_probe_integrated.toml)
set(INTEGRATED_PROBE_GENERATED_DIR ${INTEGRATED_PROBE_DIR}/generated)
set(INTEGRATED_PROBE_GENERATED_SOURCE ${INTEGRATED_PROBE_GENERATED_DIR}/x86_recomp.0.cpp)
set(INTEGRATED_PROBE_GENERATED_MAPPING ${INTEGRATED_PROBE_GENERATED_DIR}/x86_func_mapping.cpp)
add_custom_command(
    OUTPUT ${INTEGRATED_PROBE_CONFIG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${INTEGRATED_PROBE_DIR}
    COMMAND execution_probe_integrated_config ${INTEGRATED_PROBE_CONFIG} ${EXECUTION_PROBE_REAL_XBE}
    DEPENDS execution_probe_integrated_config
    VERBATIM
)
add_custom_command(
    OUTPUT ${INTEGRATED_PROBE_GENERATED_SOURCE} ${INTEGRATED_PROBE_GENERATED_DIR}/x86_recomp_shared.h ${INTEGRATED_PROBE_GENERATED_MAPPING}
    COMMAND $<TARGET_FILE:xbe_recomp> ${INTEGRATED_PROBE_CONFIG} ${EXECUTION_PROBE_REAL_XBE} ${INTEGRATED_PROBE_GENERATED_DIR}
    DEPENDS xbe_recomp ${INTEGRATED_PROBE_CONFIG}
    VERBATIM
)
add_custom_target(generate_execution_probe_integrated DEPENDS ${INTEGRATED_PROBE_GENERATED_SOURCE})
set_target_properties(generate_execution_probe_integrated PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_executable(execution_probe_integrated_real_preflight
    execution_probe_integrated_real.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/crypto.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/memory_protection.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/xbe/image_loader.cpp
    ${INTEGRATED_PROBE_GENERATED_SOURCE}
    ${INTEGRATED_PROBE_GENERATED_MAPPING}
)
add_dependencies(execution_probe_integrated_real_preflight generate_execution_probe_integrated)
target_compile_features(execution_probe_integrated_real_preflight PRIVATE cxx_std_20)
target_include_directories(execution_probe_integrated_real_preflight PRIVATE ${XBE_SDK}/runtime ${INTEGRATED_PROBE_GENERATED_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(execution_probe_integrated_real_preflight PRIVATE
    MOSTWANTED_EXECUTION_PROBE_TESTS=1
    MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1
    MOSTWANTED_THREAD_PREPARATION_TESTS=1
    MOSTWANTED_GUEST_CALLBACK_TESTS=1
    EXECUTION_PROBE_INTEGRATED=1
    EXECUTION_PROBE_MODE_REAL=1
    EXECUTION_PROBE_REAL_XBE="${EXECUTION_PROBE_REAL_XBE}"
    EXECUTION_PROBE_REAL_GENERATED_DIR="${INTEGRATED_PROBE_GENERATED_DIR}"
    EXECUTION_PROBE_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
target_link_libraries(execution_probe_integrated_real_preflight PRIVATE Threads::Threads Zydis)
set_target_properties(execution_probe_integrated_real_preflight PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Phase 2C-31: a separate synthetic fixture for the authentic secondary-thread
# entry contract.  It deliberately does not share the root route's generated
# mapping, so the Phase 2C-30 route stays byte-for-byte unchanged.
add_executable(execution_probe_secondary_fixture_builder execution_probe_secondary_fixture_builder.cpp)
set_property(TARGET execution_probe_secondary_fixture_builder PROPERTY CXX_STANDARD 20)
target_include_directories(execution_probe_secondary_fixture_builder PRIVATE ${XBE_SDK}/runtime ${CMAKE_CURRENT_SOURCE_DIR})
set(SECONDARY_PROBE_DIR ${CMAKE_CURRENT_BINARY_DIR}/probe_secondary)
set(SECONDARY_PROBE_XBE ${SECONDARY_PROBE_DIR}/execution_probe_secondary_fixture.xbe)
set(SECONDARY_PROBE_CONFIG ${SECONDARY_PROBE_DIR}/execution_probe_secondary_fixture.toml)
add_custom_command(
    OUTPUT ${SECONDARY_PROBE_XBE} ${SECONDARY_PROBE_CONFIG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SECONDARY_PROBE_DIR}
    COMMAND execution_probe_secondary_fixture_builder ${SECONDARY_PROBE_XBE} ${SECONDARY_PROBE_CONFIG}
    DEPENDS execution_probe_secondary_fixture_builder
    VERBATIM
)
set(SECONDARY_PROBE_GENERATED_DIR ${SECONDARY_PROBE_DIR}/generated)
set(SECONDARY_PROBE_GENERATED_SOURCE ${SECONDARY_PROBE_GENERATED_DIR}/x86_recomp.0.cpp)
set(SECONDARY_PROBE_GENERATED_MAPPING ${SECONDARY_PROBE_GENERATED_DIR}/x86_func_mapping.cpp)
add_custom_command(
    OUTPUT ${SECONDARY_PROBE_GENERATED_SOURCE} ${SECONDARY_PROBE_GENERATED_DIR}/x86_recomp_shared.h ${SECONDARY_PROBE_GENERATED_MAPPING}
    COMMAND $<TARGET_FILE:xbe_recomp> ${SECONDARY_PROBE_CONFIG} ${SECONDARY_PROBE_XBE} ${SECONDARY_PROBE_GENERATED_DIR}
    DEPENDS xbe_recomp ${SECONDARY_PROBE_XBE} ${SECONDARY_PROBE_CONFIG}
    VERBATIM
)
add_custom_target(generate_execution_probe_secondary DEPENDS ${SECONDARY_PROBE_GENERATED_SOURCE})
add_executable(execution_probe_secondary_tests
    execution_probe_secondary.cpp
    ${XBE_SDK}/runtime/cpu/dispatch.cpp
    ${XBE_SDK}/runtime/cpu/dispatch_table.cpp
    ${XBE_SDK}/runtime/core/log.cpp
    ${XBE_SDK}/runtime/kernel/clock_worker.cpp
    ${XBE_SDK}/runtime/kernel/crypto.cpp
    ${XBE_SDK}/runtime/kernel/memory.cpp
    ${XBE_SDK}/runtime/kernel/memory_protection.cpp
    ${XBE_SDK}/runtime/kernel/objects.cpp
    ${XBE_SDK}/runtime/kernel/ordinals.cpp
    ${XBE_SDK}/runtime/kernel/threads.cpp
    ${XBE_SDK}/runtime/kernel/worker_lifecycle.cpp
    ${XBE_SDK}/runtime/xbe/image_loader.cpp
    ${SECONDARY_PROBE_GENERATED_SOURCE}
    ${SECONDARY_PROBE_GENERATED_MAPPING}
)
add_dependencies(execution_probe_secondary_tests generate_execution_probe_secondary)
target_compile_features(execution_probe_secondary_tests PRIVATE cxx_std_20)
target_include_directories(execution_probe_secondary_tests PRIVATE ${XBE_SDK}/runtime ${SECONDARY_PROBE_GENERATED_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(execution_probe_secondary_tests PRIVATE
    MOSTWANTED_EXECUTION_PROBE_TESTS=1
    MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1
    MOSTWANTED_THREAD_PREPARATION_TESTS=1
    MOSTWANTED_GUEST_CALLBACK_TESTS=1
    EXECUTION_PROBE_SECONDARY_XBE="${SECONDARY_PROBE_XBE}"
    EXECUTION_PROBE_SECONDARY_GENERATED_DIR="${SECONDARY_PROBE_GENERATED_DIR}"
    EXECUTION_PROBE_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
target_link_libraries(execution_probe_secondary_tests PRIVATE Threads::Threads Zydis)

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
target_compile_definitions(checked_recompiler_tests PRIVATE MOSTWANTED_KERNEL_WORKER_TESTS=1)
target_compile_definitions(checked_recompiler_tests PRIVATE MOSTWANTED_GUEST_CALLBACK_TESTS=1 MOSTWANTED_GUEST_PAGE_LEASE_TESTS=1)
target_compile_definitions(checked_recompiler_tests PRIVATE MOSTWANTED_SYNTHETIC_WORKER_TESTS=1)
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
    add_test(NAME execution_probe_tests COMMAND execution_probe_tests)
    add_test(NAME execution_probe_secondary_tests COMMAND execution_probe_secondary_tests)
endif()
