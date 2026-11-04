# NFSMW iOS port status

## Phase 2A host

The separate `ios/` CMake project builds a minimal SDL3 application using SDL `release-3.4.16`, an iOS 15 deployment target, UIKit, and SDL's Metal view API. It does not alter the Android root CMake target or Gradle configuration. Generated game code and disc assets remain outside tracked source.

Verified on 2026-11-04:

- Xcode 27.2 (27B52), iPhoneOS and iPhoneSimulator SDK 27.2, Apple Clang 21.0.4.
- The SDL host compiled and linked for the iPhone 18 Pro simulator, launched, and emitted UIKit lifecycle events for background and foreground transitions.
- Simulator logs: `Darwin host page size=16384`; `video driver=uikit`; a non-null Metal layer; first drawable size `2622x1206` pixels.
- The same host compiled and linked for `arm64-apple-ios15.0` with signing disabled. The executable is a Mach-O arm64 binary. It was not installed on a physical iPhone.
- Xcode reported no valid code-signing identities. Physical-device launch is therefore unverified and was not attempted.
- Metal view/layer creation is verified. No Metal frame was rendered, and no Vulkan surface or swapchain was created.

### Rebuild commands

From the repository root, configure and build the simulator target:

```sh
cmake -S ios -B .local/ios-port/ios-simulator -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0

cmake --build .local/ios-port/ios-simulator --config Debug \
  --target mostwanted_ios -- -sdk iphonesimulator
```

Configure a separate physical-device build directory, reusing SDL's already-fetched source, then build without signing:

```sh
cmake -S ios -B .local/ios-port/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-simulator/_deps/sdl3-src"

cmake --build .local/ios-port/ios-device --config Debug \
  --target mostwanted_ios -- -sdk iphoneos CODE_SIGNING_ALLOWED=NO
```

The simulator app bundle is `.local/ios-port/ios-simulator/Debug-iphonesimulator/mostwanted_ios.app`; the device bundle is `.local/ios-port/ios-device/Debug-iphoneos/mostwanted_ios.app`. These outputs are ignored and must stay out of commits.

The first simulator install attempt reported `Missing bundle ID`; setting `com.racas.mostwanted.ios` in the Xcode target fixed it. Final simulator install and launch succeeded. The final simulator and arm64 device builds completed without errors. SDL's optional `PkgConfig` and `LibUSB` probes were unavailable and did not prevent the build.

## Vulkan and MoltenVK findings

SDL 3.4.16's UIKit Vulkan backend requires `VK_KHR_surface` and either `VK_EXT_metal_surface` or `VK_MVK_ios_surface`; with `VK_EXT_metal_surface`, SDL creates its own UIKit Metal view and `CAMetalLayer` when `SDL_Vulkan_CreateSurface` is called. The standalone host currently creates a Metal view directly, so Vulkan integration should replace that view-creation path rather than create a second view for the same window.

MoltenVK provides Vulkan over Metal and supports iOS 15+, iOS Simulator, and Metal-capable iPhones. The iOS app will need MoltenVK linked or embedded; SDL's UIKit backend can use a statically linked Vulkan entry point or a bundled `libvulkan.dylib`. If using the Khronos Vulkan loader, account for MoltenVK's `VK_KHR_portability_subset` and portability-enumeration requirements.

The Android Vulkan backend cannot be reused unchanged: `native/vulkan.cpp` waits for `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER` before asking SDL for a surface. Instance/device extension negotiation also needs an iOS pass. MoltenVK has not been added; no Vulkan instance, surface, device, or frame has been tested.

References: [SDL iOS guide](https://wiki.libsdl.org/SDL3/README-ios), [SDL UIKit Vulkan implementation at 3.4.16](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/video/uikit/SDL_uikitvulkan.m), [MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md).

## Darwin memory findings before game boot

The simulator reports a 16 KB host page. The Xbox guest allocator uses 4 KB pages (`sdk/runtime/kernel/memory.h`, `kPage = 0x1000`) and currently calls `mprotect` at those boundaries. `sdk/runtime/kernel/memory.cpp` also reserves 4 GB, creates two aliases of 64 MB backing memory, and uses `memfd_create`; the iPhoneOS SDL configure probe did not find `memfd_create`.

A one-off local Darwin probe, kept under ignored `.local/ios-port/`, confirmed:

- Host page size: `16384` bytes; `MAP_NORESERVE` is defined.
- Two file-backed `MAP_SHARED | MAP_FIXED` aliases observe the same byte.
- `mprotect` on a host-page-aligned base with a 4 KB length succeeds, while a request beginning at base + 4 KB fails with `Invalid argument`.

This is not a game-runtime test. The backing-file probe ran on macOS, and the page-size log ran in the iOS simulator. Physical-device mapping behavior remains unverified. Before game boot, implement and validate Darwin backing-memory creation and a protection strategy that preserves the guest's 4 KB allocation semantics on a 16 KB host page. Do not treat the single aligned 4 KB `mprotect` success as proof of sub-page protection.

## Generated x86 trap sites

These 16 unimplemented guest instructions were found in ignored generated files under `.local/ios-port/generated/game/`; generated code and copyrighted assets were not copied into tracked source.

| Guest PC | Instruction | Generated file |
| --- | --- | --- |
| `0x002CDC13` | `pxor xmm7, xmm7` | `x86_recomp.91.cpp` |
| `0x002CDC20` | `psraw xmm1, 0x0F` | `x86_recomp.91.cpp` |
| `0x002CDC25` | `pxor xmm0, xmm1` | `x86_recomp.91.cpp` |
| `0x002CDC29` | `psubw xmm0, xmm1` | `x86_recomp.91.cpp` |
| `0x002CDC2D` | `paddw xmm0, xmm2` | `x86_recomp.91.cpp` |
| `0x002CDC31` | `pmulhuw xmm0, xmm3` | `x86_recomp.91.cpp` |
| `0x002CDC35` | `pxor xmm0, xmm1` | `x86_recomp.91.cpp` |
| `0x002CDC39` | `psubw xmm0, xmm1` | `x86_recomp.91.cpp` |
| `0x0037F956` | `bound esp, qword ptr [edx+0x52]` | `x86_recomp.118.cpp` |
| `0x003801ED` | `outsd` | `x86_recomp.118.cpp` |
| `0x00384966` | `bound esp, qword ptr [edx+0x52]` | `x86_recomp.118.cpp` |
| `0x00397BDA` | `outsd` | `x86_recomp.120.cpp` |
| `0x00397BEB` | `insd` | `x86_recomp.120.cpp` |
| `0x003C03EA` | `das` | `x86_recomp.125.cpp` |
| `0x003C03EC` | `into` | `x86_recomp.125.cpp` |
| `0x003C044B` | `aas` | `x86_recomp.125.cpp` |

`x86_trap` in `sdk/runtime/cpu/dispatch.cpp` logs `guest trap at <PC>: <reason>`, flushes the guest stack, then aborts. Once runtime startup is integrated, capture the simulator/device log and match the reported PC against this table. A match proves that site is reachable on that run. An empty match is meaningful only if the run reaches a defined startup milestone (for example, the main menu) with complete logs. Since the handler aborts, each run can report only the first trap it reaches; no reachability run has been performed yet.

## Next milestone

Add a separate iOS Xbox-runtime compile target only after this host gate (now passed). First resolve the Darwin memory backing and 4 KB/16 KB protection design, then link MoltenVK and prove Vulkan surface creation/presentation. Keep game boot, physical-device execution, and generated trap reachability as separate later checks.
