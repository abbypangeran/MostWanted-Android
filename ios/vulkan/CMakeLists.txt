set(MOLTENVK_URL "https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-all.tar")
set(MOLTENVK_SHA256 "562a15a29bc358446a56a4091c5f7e08f604184187c1d34f712148b61ef17276")
set(MOLTENVK_DEP_DIR "${CMAKE_BINARY_DIR}/_deps/MoltenVK-1.4.2")
set(MOLTENVK_ARCHIVE "${CMAKE_BINARY_DIR}/_deps/MoltenVK-all.tar")
set(MOLTENVK_XCFRAMEWORK "${MOLTENVK_DEP_DIR}/MoltenVK/MoltenVK/static/MoltenVK.xcframework")
if(NOT EXISTS "${MOLTENVK_ARCHIVE}")
    file(DOWNLOAD "${MOLTENVK_URL}" "${MOLTENVK_ARCHIVE}"
        EXPECTED_HASH "SHA256=${MOLTENVK_SHA256}" TLS_VERIFY ON SHOW_PROGRESS)
endif()
file(SHA256 "${MOLTENVK_ARCHIVE}" _moltenvk_sha256)
if(NOT _moltenvk_sha256 STREQUAL MOLTENVK_SHA256)
    message(FATAL_ERROR "MoltenVK v1.4.2 archive SHA-256 mismatch: ${_moltenvk_sha256}")
endif()
if(NOT EXISTS "${MOLTENVK_XCFRAMEWORK}/Info.plist")
    file(ARCHIVE_EXTRACT INPUT "${MOLTENVK_ARCHIVE}" DESTINATION "${MOLTENVK_DEP_DIR}")
endif()
string(TOLOWER "${CMAKE_OSX_SYSROOT}" MOLTENVK_SYSROOT_LOWER)
if(MOLTENVK_SYSROOT_LOWER MATCHES "iphonesimulator")
    set(MOLTENVK_LIBRARY "${MOLTENVK_XCFRAMEWORK}/ios-arm64_x86_64-simulator/libMoltenVK.a")
elseif(MOLTENVK_SYSROOT_LOWER MATCHES "iphoneos")
    set(MOLTENVK_LIBRARY "${MOLTENVK_XCFRAMEWORK}/ios-arm64/libMoltenVK.a")
else()
    message(FATAL_ERROR "MoltenVK iOS target requires an iPhoneOS or iOS Simulator SDK")
endif()
if(NOT EXISTS "${MOLTENVK_LIBRARY}")
    message(FATAL_ERROR "MoltenVK v1.4.2 has no library for ${CMAKE_OSX_SYSROOT}")
endif()
add_library(MoltenVK::MoltenVK STATIC IMPORTED GLOBAL)
set_target_properties(MoltenVK::MoltenVK PROPERTIES
    IMPORTED_LOCATION "${MOLTENVK_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MOLTENVK_DEP_DIR}/MoltenVK/MoltenVK/include"
)

add_executable(mostwanted_vulkan_ios MACOSX_BUNDLE ../vulkan_main.c)
target_include_directories(mostwanted_vulkan_ios PRIVATE "${MOLTENVK_DEP_DIR}/MoltenVK/MoltenVK/include")
target_link_libraries(mostwanted_vulkan_ios PRIVATE SDL3::SDL3-static MoltenVK::MoltenVK c++
    "-framework Metal" "-framework Foundation" "-framework QuartzCore"
    "-framework CoreGraphics" "-framework IOSurface" "-framework UIKit")
set_target_properties(mostwanted_vulkan_ios PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Most Wanted Vulkan Bootstrap"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/../Info.plist"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.racas.mostwanted.vulkan"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"
)
