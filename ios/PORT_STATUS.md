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

## Phase 2B results

Phase 2A's four local commits were reconciled against `origin/feature/ios-port` and pushed with a normal fast-forward before this work. `main` was not changed. Phase 2B adds an optional Vulkan bootstrap target; it is off by default so the existing SDL host build remains unchanged.

Phase 2B implementation commits: Darwin memory `03304f1730fc9e7801949a45e58cb6666ada683d`; Vulkan bootstrap `ac25c7a81567c69960f7cbf486f8962e0d9c7282`.

### Darwin memory

**MEMORY_DARWIN_BACKEND: BLOCKED.** Darwin backing and aliasing work; complete 4 KB guest-page semantics do not. The iOS backend uses an unlinked `mkstemp` file under `TMPDIR`, `ftruncate`, shared file mappings, and `MAP_NORESERVE` for the 4 GB guest address space. Both 64 MB physical windows map the same backing object. Initialization failure cleans up mappings and file descriptors; repeated `Init` and `Shutdown` are covered.

On a 16 KB host, `Commit` and `Decommit` fail closed unless the rounded guest range covers whole host pages. This preserves neighboring page contents and state, but means ordinary 4 KB system allocations and some image-section commits fail. For example, `AllocSystem(0x400, 64)` cannot commit its 4 KB page; early thread setup then cannot initialize its KPCR. Do not attempt Xbox runtime startup or game boot with this backend.

The remaining blocker is the guest access model: translated code uses direct `X86_MEM*` pointers and `memmove`/`memcpy`, while kernel code exposes raw `G<T>()` references and `HostPtr()` pointers. There is no shared seam through which every access can be checked. A safe 4 KB implementation needs either comprehensive access mediation for scalar, vector, atomic, bulk-copy, GPU, and audio paths, or a redesigned guest mapping that preserves independent 4 KB state on 16 KB hosts. Hardware protection alone cannot do this. Current tests cover host-page refusal and state preservation, but do not establish complete guest access permissions.

The macOS CTest target and the iOS Simulator memory-test app both passed. The simulator reported a 16 KB host page. Checks cover 4 GB reservation/footprint, alias coherence, commit/decommit, fail-closed neighboring-page behavior, zero-fill/recommit, address bounds, 64 KB reservation alignment, allocator rollback, failed initialization, and repeated cleanup. Simulator evidence is not physical-iPhone evidence.

macOS memory-test commands:

```sh
cmake -S ios/memory-tests -B .local/ios-port/memory-macos -G Xcode \
  -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build .local/ios-port/memory-macos --config Debug \
  --target mostwanted_memory_tests -- -quiet
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure
```

The iOS Simulator app was built, installed, and run on the iPhone 18 Pro simulator with these commands:

```sh
cmake -S ios -B .local/ios-port/ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
cmake --build .local/ios-port/ios-sim --config Debug \
  --target mostwanted_memory_tests -- -quiet
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC \
  .local/ios-port/ios-sim/Debug-iphonesimulator/mostwanted_memory_tests.app
xcrun simctl launch --console-pty --terminate-running-process \
  43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests
```

### Vulkan bootstrap

**IOS_VULKAN_BOOTSTRAP: PASS on iOS Simulator.** The optional `mostwanted_vulkan_ios` target pins MoltenVK v1.4.2 and its SHA-256 in `ios/vulkan/CMakeLists.txt`; the dependency archive and extracted XCFramework stay in the ignored build directory. The simulator and physical ARM64 targets both compiled and linked. The physical-device build was unsigned and was not installed or run.

On the iPhone 18 Pro simulator, logs showed 18 instance extensions and 114 device extensions. SDL enabled `VK_KHR_surface` and `VK_EXT_metal_surface`; `SDL_Vulkan_CreateSurface` created the UIKit Metal surface. The selected device was `Apple iOS simulator GPU`, Vulkan 1.1.357, with a graphics/presentation queue and three swapchain images at `2622x1206`. The app submitted and presented a blue frame; a simulator screenshot confirmed it.

The simulator exposes and the bootstrap enables `VK_KHR_portability_subset`. It does not expose `VK_KHR_portability_enumeration` for this direct static-linked MoltenVK path, so no instance portability flag was enabled. Device features logged: sampler anisotropy, non-solid fill, depth clamp, dynamic rendering, and timeline semaphore support; BC texture compression is unavailable. The existing Android Vulkan backend can fall back to CPU BC decode and fences on Vulkan 1.1; its basic API, swapchain, sampled-depth, and presentation gates passed on the simulator. This does not verify D3D8 translation or Xbox game rendering.

MoltenVK v1.4.2 is the latest tagged release used here, pinned by archive hash `562a15a29bc358446a56a4091c5f7e08f604184187c1d34f712148b61ef17276`. Upstream now marks MoltenVK deprecated and community-maintained on a best-effort basis, which is a long-term maintenance risk ([release](https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.4.2), [upstream notice](https://github.com/KhronosGroup/MoltenVK)). The runtime guide documents iOS 15 support and public Metal APIs ([MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)).

Configure and build the simulator targets with the bootstrap enabled; CMake downloads the pinned package when it is absent from the build directory:

```sh
cmake -S ios -B .local/ios-port/ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_BUILD_VULKAN_BOOTSTRAP=ON
cmake --build .local/ios-port/ios-sim --config Debug \
  --target mostwanted_ios mostwanted_memory_tests mostwanted_vulkan_ios -- -quiet
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC \
  .local/ios-port/ios-sim/Debug-iphonesimulator/mostwanted_vulkan_ios.app
xcrun simctl launch --console-pty --terminate-running-process \
  43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.vulkan
```

For a physical ARM64 compile/link only, configure with `-DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DMOSTWANTED_BUILD_VULKAN_BOOTSTRAP=ON`, then build with `-- -quiet CODE_SIGNING_ALLOWED=NO`. Do not install this output on a physical iPhone as part of Phase 2B.

The verified unsigned device configure and build commands were:

```sh
cmake -S ios -B .local/ios-port/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_BUILD_VULKAN_BOOTSTRAP=ON \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/SDL3-src"
cmake --build .local/ios-port/ios-device --config Debug \
  --target mostwanted_ios mostwanted_memory_tests mostwanted_vulkan_ios \
  -- -quiet CODE_SIGNING_ALLOWED=NO
```

### Android regression

**ANDROID_REGRESSION: PARTIAL.** Android CMake and Gradle files are unchanged. The modified shared runtime sources passed NDK 27.2 ARM64 `-fsyntax-only` compilation. `bash ./gradlew :app:assembleDebug` stopped before project configuration with `Unsupported class file major version 69` under the installed Java 25 runtime. The project requests NDK 28.2.13676358; only NDK 27.2.12479018 is installed. The required `third_party/SDL`, `assets/default.xbe`, and `generated/game/x86_func_mapping.cpp` are also absent, so a full Android game build was not available. No game assets or generated code were added.

The commands and result were:

```sh
/Users/abbypangeran/Library/Android/sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang++ \
  -std=c++20 -fsyntax-only -DANDROID -fno-strict-aliasing -fsigned-char \
  -include native/compat/crt.h -Inative/compat -Isdk/harness/compat -Isdk/runtime \
  sdk/runtime/kernel/memory.cpp sdk/runtime/kernel/boot.cpp sdk/runtime/kernel/xboxkrnl.cpp
bash ./gradlew :app:assembleDebug
```

The NDK syntax-only command passed. Gradle failed before project configuration with `BUG! exception in phase 'semantic analysis' in source unit '_BuildScript_' Unsupported class file major version 69` under Temurin 25.0.3.

## Vulkan and MoltenVK findings

SDL 3.4.16's UIKit Vulkan backend requires `VK_KHR_surface` and supports `VK_EXT_metal_surface`; it creates its own UIKit Metal view and `CAMetalLayer` during `SDL_Vulkan_CreateSurface`. The bootstrap uses that path and does not create a second view. The Android Vulkan backend remains platform-specific: `native/vulkan.cpp` waits for `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`, so its surface path is not reused on iOS.

References: [SDL iOS guide](https://wiki.libsdl.org/SDL3/README-ios), [SDL UIKit Vulkan implementation at 3.4.16](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/video/uikit/SDL_uikitvulkan.m), [MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md).

## Darwin memory findings before game boot

`sdk/runtime/kernel/memory.cpp` retains the 4 GB guest VA layout with sparse reservation and physical aliases. Darwin uses an unlinked temporary file instead of Linux `memfd_create`. `Commit`/`Decommit` still reject sub-host-page ranges. This is a fail-closed partial backend, not game-ready memory; see the Phase 2B blocker above. No game boot was attempted.

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

Design and implement comprehensive guest-access mediation or an equivalent safe 4 KB-on-16 KB memory model, then add tests for actual guest permissions and boot-thread allocations. Only after that gate, introduce Xbox runtime startup and use the existing `x86_trap` log to detect reachable trap sites. Keep simulator execution, physical-device execution, and game boot as separate acceptance checks.
