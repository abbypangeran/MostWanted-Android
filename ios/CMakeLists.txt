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
    ../sdk/runtime/core/log.cpp
    ../sdk/runtime/kernel/memory.cpp
)
target_compile_features(mostwanted_memory_tests PRIVATE cxx_std_20)
target_include_directories(mostwanted_memory_tests PRIVATE ../sdk/runtime)
target_link_libraries(mostwanted_memory_tests PRIVATE SDL3::SDL3-static)
set_target_properties(mostwanted_memory_tests PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Most Wanted Memory Tests"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.racas.mostwanted.memorytests"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
)
