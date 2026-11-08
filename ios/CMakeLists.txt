cmake_minimum_required(VERSION 3.22)
project(MostWantedIOS LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "Configure this target with -DCMAKE_SYSTEM_NAME=iOS")
endif()

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.16
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)
find_package(Threads REQUIRED)

add_executable(mostwanted_ios MACOSX_BUNDLE main.c)
target_link_libraries(mostwanted_ios PRIVATE SDL3::SDL3-static)
set_target_properties(mostwanted_ios PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Most Wanted iOS Host"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.racas.mostwanted.ios"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
)

add_executable(mostwanted_memory_tests MACOSX_BUNDLE
    memory-tests/memory_test_main.c
    memory-tests/memory_tests.cpp
    guest-memory-prototype/guest_memory.cpp
    guest-memory-prototype/guest_memory_tests.cpp
    ../sdk/runtime/core/log.cpp
    ../sdk/runtime/kernel/crypto.cpp
    ../sdk/runtime/kernel/memory.cpp
    ../sdk/runtime/kernel/clock_worker.cpp
    ../sdk/runtime/kernel/worker_lifecycle.cpp
    ../sdk/runtime/kernel/kernel_data_init.cpp
    ../sdk/runtime/cpu/dispatch_table.cpp
    ../sdk/runtime/kernel/kernel_thunks.cpp
    ../sdk/runtime/kernel/filesystem_context.cpp
    ../sdk/runtime/kernel/files.cpp
    ../sdk/runtime/kernel/disc.cpp
    ../sdk/runtime/kernel/startup_data.cpp
    ../sdk/runtime/kernel/checked_file_transfer.cpp
    ../sdk/runtime/kernel/checked_gpu_transfer.cpp
    ../sdk/runtime/kernel/guest_strings.cpp
    ../sdk/runtime/xbe/image_loader.cpp
    memory-tests/xbe_image_tests.cpp
    memory-tests/startup_data_tests.cpp
    memory-tests/checked_file_transfer_tests.cpp
    memory-tests/file_transfer_export_tests.cpp
    memory-tests/checked_gpu_transfer_tests.cpp
    memory-tests/clock_worker_tests.cpp
    memory-tests/worker_lifecycle_tests.cpp
    memory-tests/dispatch_table_tests.cpp
    memory-tests/kernel_thunk_tests.cpp
    memory-tests/filesystem_setup_tests.cpp
)
set(MOSTWANTED_CHECKED_FIXTURE_DIR "" CACHE PATH "Generated checked-recompiler fixture directory")
if(MOSTWANTED_CHECKED_FIXTURE_DIR)
    foreach(FIXTURE_FILE x86_recomp.0.cpp x86_func_mapping.cpp x86_recomp_shared.h)
        if(NOT EXISTS "${MOSTWANTED_CHECKED_FIXTURE_DIR}/${FIXTURE_FILE}")
            message(FATAL_ERROR "Missing checked-recompiler fixture file: ${MOSTWANTED_CHECKED_FIXTURE_DIR}/${FIXTURE_FILE}")
        endif()
    endforeach()
    target_sources(mostwanted_memory_tests PRIVATE
        recompiler-fixtures/fixture_runner.cpp
        ../sdk/runtime/cpu/dispatch.cpp
        ../sdk/runtime/kernel/memory_protection.cpp
        ../sdk/runtime/kernel/objects.cpp
        ../sdk/runtime/kernel/ordinals.cpp
        ../sdk/runtime/kernel/threads.cpp
        "${MOSTWANTED_CHECKED_FIXTURE_DIR}/x86_recomp.0.cpp"
        "${MOSTWANTED_CHECKED_FIXTURE_DIR}/x86_func_mapping.cpp"
    )
    target_include_directories(mostwanted_memory_tests PRIVATE "${MOSTWANTED_CHECKED_FIXTURE_DIR}")
    target_compile_definitions(mostwanted_memory_tests PRIVATE
        MOSTWANTED_CHECKED_RECOMPILER_FIXTURE=1
        MOSTWANTED_CHECKED_KERNEL_ABI_TESTS=1
        MOSTWANTED_THREAD_PREPARATION_TESTS=1
        MOSTWANTED_GUEST_CALLBACK_TESTS=1
        MOSTWANTED_FIXTURE_EXTERNAL_G_BASE=1
    )
endif()
target_compile_features(mostwanted_memory_tests PRIVATE cxx_std_20)
target_compile_definitions(mostwanted_memory_tests PRIVATE
    MOSTWANTED_FILE_TRANSFER_EXPORT_TESTS=1
    MOSTWANTED_KERNEL_WORKER_TESTS=1
    MOSTWANTED_GUEST_PAGE_LEASE_TESTS=1
)
if(NOT MOSTWANTED_CHECKED_FIXTURE_DIR)
    target_compile_definitions(mostwanted_memory_tests PRIVATE MOSTWANTED_FILE_TRANSFER_TEST_STUBS=1)
endif()
target_include_directories(mostwanted_memory_tests PRIVATE ../sdk/runtime guest-memory-prototype)
target_link_libraries(mostwanted_memory_tests PRIVATE SDL3::SDL3-static Threads::Threads)
set_target_properties(mostwanted_memory_tests PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Most Wanted Memory Tests"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.racas.mostwanted.memorytests"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
)

option(MOSTWANTED_BUILD_VULKAN_BOOTSTRAP "Build the optional MoltenVK iOS bootstrap app" OFF)
if(MOSTWANTED_BUILD_VULKAN_BOOTSTRAP)
    add_subdirectory(vulkan)
endif()
