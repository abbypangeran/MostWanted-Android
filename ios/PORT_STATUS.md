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

## Phase 2C-1 guest memory results

Verified on 2026-11-04. The Phase 2C-1 architecture record is in [`GUEST_MEMORY_ARCHITECTURE.md`](GUEST_MEMORY_ARCHITECTURE.md).

| Gate | Result | Evidence |
| --- | --- | --- |
| `GUEST_ACCESS_INVENTORY` | **PASS** | Runtime, recompiler, generated code, kernel/HLE, boot, I/O, graphics, audio, threads, atomics, and direct-pointer access categories have documented handling or explicit gaps. The inventory found `X86_MEM*` in 128 generated units, raw `X86_PTR` in 65, and atomics in 18; counts overlap. |
| `MEMORY_ARCHITECTURE_PROTOTYPE` | **PASS — isolated prototype only** | Checked 4 KB logical-page operations and access rejection pass on 16 KB macOS and iOS Simulator hosts. This is not runtime integration. |
| `BOOT_ALLOCATION_PROBE` | **BLOCKED** | Prototype allocation shapes pass, but real runtime `AllocSystem(0x400, 64)`, ETHREAD-sized allocation, and unaligned image-section commit remain blocked. Real runtime guarded stack allocation passes. No Xbox runtime or game boot was attempted. |
| `ANDROID_REGRESSION` | **PARTIAL** | Java 21 Gradle task configuration passes. `assembleDebug` reaches Android CMake configuration, then fails because generated game code is unavailable. No Android C++ compilation or device run occurred. |

### Prototype checks

The prototype owns a 256 KB temporary backing file and two shared aliases, with 64 logical 4 KB page entries mapped onto a 16 KB host. It validates whole spans through its access API, serializes operations against decommit/protection changes, and never returns a raw guest pointer. The standalone prototype and macOS runtime allocator CTest targets each passed (`1/1`). The iOS Simulator memory-test app passed runtime and prototype checks; logs confirmed `Darwin host page size=16384`, all prototype checks passed, and the app printed `memory tests: finished`. The SDL indirect-input Info.plist warning was nonfatal.

AppleClang/LLVM coverage for `guest_memory.cpp`: 95.00% lines, 83.33% regions, 100.00% functions, and 64.52% branches. Line and region coverage exceed the project 80% threshold; branch coverage remains below it.

Prototype limitations: logical permissions only; host aliases remain read/write. Raw pointer accesses can bypass checks. `CheckExecute` is not wired into CPU fetch. All aliases share permission state, so alias-specific protection is absent. `FetchAdd32` is serialized only for API users. Decommit clears contents/state but retains backing storage. The allocation probes model shapes only and do not invoke Xbox runtime allocators. See the architecture record for all unimplemented access paths.

Run standalone macOS prototype tests:

```sh
cmake -S ios/guest-memory-prototype -B .local/ios-port/guest-memory-prototype-macos -G Xcode
cmake --build .local/ios-port/guest-memory-prototype-macos --config Debug \
  --target guest_memory_prototype_tests -- -quiet
ctest --test-dir .local/ios-port/guest-memory-prototype-macos -C Debug --output-on-failure
```

Run macOS runtime allocator checks:

```sh
cmake -S ios/memory-tests -B .local/ios-port/memory-macos -G Xcode \
  -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build .local/ios-port/memory-macos --config Debug \
  --target mostwanted_memory_tests -- -quiet
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure
```

Build and run the combined memory checks on the iPhone 18 Pro simulator:

```sh
cmake -S ios -B .local/ios-port/ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_BUILD_VULKAN_BOOTSTRAP=ON
cmake --build .local/ios-port/ios-sim --config Debug \
  --target mostwanted_memory_tests -- -quiet
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC \
  .local/ios-port/ios-sim/Debug-iphonesimulator/mostwanted_memory_tests.app
xcrun simctl launch --console-pty --terminate-running-process \
  43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests
```

Runtime checks intentionally report the current backend limitations: `AllocSystem(0x400, 64)` and the ETHREAD-sized allocation are refused under 16 KB host-page rules; 64 KB-aligned guarded stack allocation succeeds; unaligned `Commit(0x30000, 0x5000)` is refused. Prototype equivalents for the KPCR/ETHREAD, guarded stack/TLS, and image-section shapes pass through the checked API. This does not establish actual boot readiness.

### Android Java 21 regression check

Used JDK 21.0.13 for this command only; no global Java setting changed. Project Gradle 8.7 configured successfully and requested NDK 28.2.13676358. Android SDK Platform 34, that NDK, and CMake 3.31.6 were added under the existing local Android SDK. The Java 25 `Unsupported class file major version 69` failure is resolved.

CMake 3.31.6 was installed with:

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
/Users/abbypangeran/Library/Android/sdk/cmdline-tools/latest/bin/sdkmanager \
  --sdk_root=/Users/abbypangeran/Library/Android/sdk 'cmake;3.31.6'
```

Gradle task configuration passed:

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
bash ./gradlew --no-daemon :app:tasks
```

Output: `BUILD SUCCESSFUL`.

Full app build attempt:

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
PATH=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home/bin:$PATH \
bash ./gradlew --no-daemon :app:assembleDebug
```

Gradle reached `:app:configureCMakeRelease[arm64-v8a]`, using NDK Clang 19.0.1, then stopped at the repository's missing-input guard:

```text
CMake Error at CMakeLists.txt:33 (message):
  No recompiled code: put the game in assets/ or iso/ and run build.bat, it
  writes generated/game
```

Gradle result: `BUILD FAILED` during CMake configuration; the warning about SDK XML version 4 was nonfatal.

No ISO, generated code, or copyrighted assets were added to tracked source. Android CMake and Gradle files were not changed. Therefore this verifies Gradle/JDK setup and the missing-input blocker, but not Android native compilation, packaging, or runtime behavior.

### Phase 2C-2 recommendation

**GO** for Phase 2C-2 work on comprehensive access mediation: convert recompiler emissions and every kernel/HLE, boot, I/O, graphics, audio, and diagnostic pointer path to checked spans or lifetime-pinned access. **NO-GO** for Xbox runtime startup or game boot until raw guest-pointer paths are eliminated or validated, real runtime allocation probes pass on 16 KB hosts, and protection/decommit races are covered. Keep runtime, simulator, physical-device, and game-boot evidence separate.

Phase 2C-1 commits:

- Architecture inventory and choice: `7f46bc69a5fa82fc4ab88d7067b4cc8a656b9316`
- Isolated translated-page prototype: `6ff32dedc7556345bf08bb88c91faaa2281a3e79`
- Prototype and runtime-shape tests: `351ef1e82c25a6d6c084b1bc423a17d201ce29f1`

## Phase 2C-2 production memory checkpoint

Verified on 2026-11-05. The earlier Phase 2B and 2C-1 statements above describe those checkpoints; the current Darwin memory behavior is described here and in `GUEST_MEMORY_ARCHITECTURE.md`.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `GUEST_MEMORY_RUNTIME` | **PARTIAL** | Production checked read/write, bulk, atomic, software permission, 4 KB commit/decommit, and real allocator probes pass on 16 KB Darwin hosts. Full page-table/frame translation and all runtime callers are not integrated. |
| `RECOMPILER_ACCESS_MEDIATION` | **BLOCKED** | `X86_MEM*`, `X86_PTR`, x87/SIMD, atomics, and REP helpers still use raw guest pointers. No generated-code fixture was attempted because Stages A/B are partial. |
| `HOST_ACCESS_COVERAGE` | **BLOCKED** | `Base()` is null on Apple and `Boot()` stops before raw access. Kernel `G<T>()`/`HostPtr()`, boot image loading, I/O, graphics, audio, and asynchronous retained pointers remain unconverted. |
| `IOS_MEMORY_REGRESSION` | **PARTIAL** | macOS CTest and iOS Simulator execution pass; unsigned physical ARM64 app compiles and links. Full-component branch coverage is 55.64%, below the 80% target. Physical device execution was not attempted. |
| `ANDROID_REGRESSION` | **PARTIAL** | NDK 28.2 ARM64 syntax compilation and full `:app:assembleDebug` native build/package pass. Android execution was not attempted. |

The test uses the actual `mem::Init`, `AllocSystem(0x400, 64)`, ETHREAD-sized allocation, `AllocStack`, and `ReserveImage` implementations. It verifies KPCR/ETHREAD-sized field access through checked operations, TLS and guard behavior, independent adjacent 4 KB pages, recommit zero-fill, alias coherence, 10-byte/16-byte spans, invalid bounds, concurrent atomic increments, and reads racing with protection/decommit. These are allocator and access probes; a guest thread was not started. The existing isolated prototype also runs in the simulator app.

Build and run commands from the repository root:

```sh
cmake --build .local/ios-port/memory-macos --config Debug --target mostwanted_memory_tests -- -quiet
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure
cmake --build .local/ios-port/ios-sim --config Debug --target mostwanted_memory_tests -- -quiet
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC \
  .local/ios-port/ios-sim/Debug-iphonesimulator/mostwanted_memory_tests.app
xcrun simctl launch --console-pty --terminate-running-process \
  43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests
cmake --build .local/ios-port/ios-device --config Debug \
  --target mostwanted_memory_tests -- -quiet CODE_SIGNING_ALLOWED=NO
/Users/abbypangeran/Library/Android/sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang++ \
  -std=c++20 -fsyntax-only -DANDROID -fno-strict-aliasing -fsigned-char \
  -include native/compat/crt.h -Inative/compat -Isdk/harness/compat -Isdk/runtime \
  sdk/runtime/kernel/memory.cpp sdk/runtime/kernel/boot.cpp
```

Results: macOS CTest `1/1 Passed`; simulator app exit 0 with `memory tests: finished` and the new concurrency and alias checks marked `PASS`; unsigned device target linked; NDK syntax check exit 0. The device build directory was reconfigured with the exact iPhoneOS 27.2 SDK and `-DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/SDL3-src"` after its stale Xcode project initially failed with `Undefined symbols for architecture arm64: "_RunGuestMemoryPrototypeTests"`. Reconfiguration resolved that build-file-list error. No physical-device install or game boot occurred.

LLVM coverage command and result:

```sh
LLVM_PROFILE_FILE=.local/ios-port/memory-coverage/final2-%p.profraw \
  .local/ios-port/memory-coverage/Debug/mostwanted_memory_tests
xcrun llvm-profdata merge -sparse .local/ios-port/memory-coverage/final2-*.profraw \
  -o .local/ios-port/memory-coverage/final2.profdata
xcrun llvm-cov report .local/ios-port/memory-coverage/Debug/mostwanted_memory_tests \
  -instr-profile=.local/ios-port/memory-coverage/final2.profdata sdk/runtime/kernel/memory.cpp
```

Full `memory.cpp`: 75.55% lines, 70.83% regions, 85.42% functions, 55.64% branches. This includes legacy allocator, platform, and failure paths. The 80% branch target was not met; rollback failure injection and generated instruction paths are not covered.

Debug microbenchmark prints nanoseconds per round. On the iOS Simulator: local scalar 2, checked scalar read+write 107; local 4 KB write+read 265, checked 4 KB write+read 366. On macOS: 1, 107, 217, and 302 respectively. The local operations use observable copy-in/copy-out into a local 4 KB buffer as a lower-bound proxy because Darwin no longer exposes the previous raw guest pointer. These figures show measurable checked-access cost, but are not a reliable direct-pointer slowdown ratio or a release-performance claim.

Android full build used the validated local XBE and generated output through ignored `assets/default.xbe` and `generated/game` symlinks pointing into `.local/ios-port/`. Nothing under those paths was staged or committed. With local Java 21 and NDK 28.2.13676358:

```sh
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
bash ./gradlew --no-daemon :app:assembleDebug
```

Result: initial `BUILD SUCCESSFUL in 11m 14s`, 37 tasks (10 executed, 27 up-to-date), including `:app:buildCMakeRelease[arm64-v8a]` and `:app:assembleDebug`. The final code rebuild also passed in 29s (5 executed, 32 up-to-date). Nonfatal diagnostics were SDK XML version 4 versus reader version 3 and SDL's deprecated `ASensorManager_getInstance`. No Android device installation or runtime test was performed.

**Next milestone, Phase 2C-3:** convert a narrow recompiler fixture and startup-critical host helpers to checked access, including 10/16-byte, atomic and REP semantics; then audit every pointer escape and fault path before enabling `Boot()`. Runtime startup is **not safe to attempt** at this checkpoint. Android/Linux 16 KB host-page behavior remains unproven, and the production implementation still uses software state over linear backing rather than the selected page-table/frame architecture.

## Vulkan and MoltenVK findings

SDL 3.4.16's UIKit Vulkan backend requires `VK_KHR_surface` and supports `VK_EXT_metal_surface`; it creates its own UIKit Metal view and `CAMetalLayer` during `SDL_Vulkan_CreateSurface`. The bootstrap uses that path and does not create a second view. The Android Vulkan backend remains platform-specific: `native/vulkan.cpp` waits for `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`, so its surface path is not reused on iOS.

References: [SDL iOS guide](https://wiki.libsdl.org/SDL3/README-ios), [SDL UIKit Vulkan implementation at 3.4.16](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/video/uikit/SDL_uikitvulkan.m), [MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md).

## Phase 2B Darwin memory findings (historical)

At the Phase 2B checkpoint, `sdk/runtime/kernel/memory.cpp` retained the 4 GB guest VA layout with sparse reservation and physical aliases. Darwin used an unlinked temporary file instead of Linux `memfd_create`. `Commit`/`Decommit` rejected sub-host-page ranges. Phase 2C-2 supersedes that limitation through checked software page state, but does not establish game readiness. No game boot was attempted.

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

## Phase 2A next milestone (historical)

Design and implement comprehensive guest-access mediation or an equivalent safe 4 KB-on-16 KB memory model, then add tests for actual guest permissions and boot-thread allocations. Only after that gate, introduce Xbox runtime startup and use the existing `x86_trap` log to detect reachable trap sites. Keep simulator execution, physical-device execution, and game boot as separate acceptance checks.

## Phase 2C-3 checked recompiler checkpoint

Verified on 2026-11-05 from branch `feature/ios-port`, starting HEAD `8841aa36558d807da6a1e0d816987e5af334ef46`. This checkpoint uses a synthetic XBE generated locally by the fixture builder; no game executable, assets, or generated game C++ were added to tracked source.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `RECOMPILER_CHECKED_FIXTURE` | **PASS (scoped whitelist)** | Real `xbe_recomp` output compiled and ran against production `mem::` APIs: MOV/MOVZX/MOVSX, 64-bit MMX MOVQ, memory ADD/SUB/INC, success/fault paths, and correct flags. |
| `CPU_FAULT_PROPAGATION` | **PASS (diagnostic stop)** | Generated uncommitted read, denied write, and blocked opcode reached production logging and `SIGABRT` in isolated child processes. Not Xbox exception delivery. |
| x87/SIMD | **BLOCKED** | 10-byte FLD and 16-byte MOVUPS are rejected by checked mode; no x87/SIMD guest instruction executes. |
| Atomics | **BLOCKED** | Implicit memory XCHG, locked XADD, and other atomic shapes are rejected; alignment and guest concurrency semantics are not established. |
| REP/string | **BLOCKED** | REP MOVSD is rejected; DF, overlap, multi-page partial progress, and fault boundaries are not established. |
| `HOST_STARTUP_ACCESS` | **PARTIAL** | Apple scalar kernel helpers, KPCR/ETHREAD setup, bounded/counting strings, and selected Rtl bulk helpers are checked and tested. XBE copies and raw pointer callers remain. |
| macOS ARM64 | **PASS** | `mostwanted_memory_tests`: CTest `1/1`; coverage report below. |
| iOS Simulator ARM64 | **PASS** | App launched on iPhone 18 Pro simulator; production memory tests, generated fixture, and fault outcomes printed PASS through `memory tests: finished`. |
| iOS device ARM64 compile/link | **PASS** | Unsigned iPhoneOS app linked as Mach-O ARM64. No physical-device install or execution. |
| Android regression | **PASS (build)** | Java 21 / NDK 28.2.13676358 `:app:assembleDebug` passed; generated Ninja source list includes the new runtime files. No Android runtime execution. |

### Checked fixture and fault semantics

Checked mode is opt-in with `[xbe] checked_memory = true`; it defaults off. It supports scalar MOV, MOVZX/MOVSX, MMX MOVQ, memory ADD/SUB/INC, conditional branches, and RET. Unsupported operand forms and locked instructions use `x86_checked_trap`, which avoids the existing guest-stack scan. Faults carry guest PC, address, read/write direction, and `AccessStatus`; `mem::Read`/`Write` validate the full span before touching guest data. The production handler logs and aborts to stop guest execution deterministically. It does not synthesize Xbox exception records or recover the guest thread.

The fixture verifies 4 KB guest-page boundaries on a 16 KB Darwin host, unaligned scalar spans, signed and unsigned extension, 64-bit MMX transfers, overflow, uncommitted/no-access/read-only pages, faulting PCs, no partial cross-page stores, and RMW results/flags. The generated fault handler test separately exercises uncommitted reads, permission-denied writes, and blocked instructions through the production handler. Unlocked RMW read/write is not one atomic transaction.

### Host helpers and remaining raw paths

Apple-only checked helpers now cover R8/R16/R32/R64 and W8/W16/W32/W64, complete KPCR/ETHREAD initialization with rollback, counted ANSI and bounded C strings with full descriptor-span validation, and RtlZeroMemory/RtlFillMemory/RtlMoveMemory. RtlFillMemoryUlong validates the whole dword span before writing; RtlCompareMemory rejects cursor overflow. The ULONG fill does not pin across per-word writes, so concurrent decommit can still cause partial progress. Darwin thread naming uses the Apple pthread API. Android/Windows/Linux retain direct scalar, thread initialization, and string paths. Android's existing recompiler output remains the default because checked emission defaults off.

Still unconverted: `kernel::G<T>()`, `HostPtr()`, XBE header/section copies in `boot.cpp`, `XeLoadSection` host copies, other RTL string copy/conversion and interlocked exports, `X86_MEM*`/`X86_PTR` paths outside the checked subset, file/network buffers, GPU/texture/vertex/render buffers, audio buffers, and pointers retained by asynchronous operations. Relevant code remains in `sdk/runtime/kernel/{boot,files,objects,rtl,xboxkrnl}.cpp`, `sdk/runtime/gpu`, `sdk/runtime/xdk`, `src/d3d`, and `src/sound`. Apple `Boot()` still rejects a null base before these paths. The complete access inventory and architecture decision remain above.

### Platform and coverage evidence

The Darwin test binary now covers the production memory implementation plus `guest_strings.cpp`: `memory.cpp` 75.19% lines, 70.38% regions, 85.71% functions, and 55.92% branches; `guest_strings.cpp` 100% lines and functions, 74.07% regions, and 61.11% branches. Combined branch coverage is 56.14%. Active checked read/write, access status, 4 KB page, string, allocation, and fault branches are exercised. The generated child-process tests also exercise production memory-fault logging, denied writes, checked helper overflow, and unsupported-instruction stops, but are not included in this coverage binary. Remaining gaps include rollback failure injection, rare host mapping errors, and non-Apple platform branches. The 80% branch target remains unmet.

Release fixture benchmark on this Mac ARM64 host: empty generated call 1.1 ns; generated checked scalar load+store 32.0 ns; measured delta 30.9 ns per call, about 62.57 million checked scalar operations/s. Checked `mem::Copy` measured 60.43 GiB/s; a local-buffer `memcpy` proxy measured 79.27 GiB/s and is not direct guest memory. Measurements are microbenchmarks, not a device performance claim.

Nonfatal diagnostics observed: Zydis CMake minimum-version deprecation and missing optional Doxygen; SDL configure could not find optional PkgConfig/LibUSB; simulator logged `You need UIApplicationSupportsIndirectInputEvents in your Info.plist for mouse support`; Android tooling warned that SDK XML version 4 exceeds its version-3 reader. These did not fail the builds or tests.

Intermediate failures were fixed before this checkpoint. The first generated fixture CTest reported failures for memory SUB/INC flags because checked RET did not save local flags; checked RET now emits `X86_SAVE_FLAGS()`. The first standalone macOS Xcode build reported `Undefined symbols for architecture arm64: "kernel::ReadCString(unsigned int, unsigned long)"` and `ReadAnsiString`; explicitly regenerating the Xcode project included the newly added `guest_strings.cpp` and resolved the link failure. The first macOS syntax check reported `no matching function for call to 'pthread_setname_np'`; Darwin now uses its one-argument form, and macOS/iPhoneOS syntax checks pass. An earlier unchecked raw-output probe exited 139 when it dereferenced the intentionally null Apple base, confirming that unchecked generated memory must remain blocked.

### Exact commands

From repository root:

```sh
cmake -S ios/recompiler-fixtures -B .local/ios-port/recompiler-fixtures-xcode -G Xcode
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Debug --target checked_recompiler_tests checked_fault_handler_tests -- -quiet
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Debug --output-on-failure
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Release --target checked_recompiler_tests checked_fault_handler_tests -- -quiet
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Release -V

cmake -S ios/memory-tests -B .local/ios-port/memory-macos -G Xcode
cmake --build .local/ios-port/memory-macos --config Debug --target mostwanted_memory_tests -- -quiet
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure

cmake -S ios -B .local/ios-port/ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_CHECKED_FIXTURE_DIR="$PWD/.local/ios-port/recompiler-fixtures-xcode/fixture/generated" \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/sdl3-src"
cmake --build .local/ios-port/ios-sim --config Debug --target mostwanted_memory_tests -- -quiet
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC .local/ios-port/ios-sim/Debug-iphonesimulator/mostwanted_memory_tests.app
xcrun simctl launch --console-pty --terminate-running-process 43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests

cmake -S ios -B .local/ios-port/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_CHECKED_FIXTURE_DIR="$PWD/.local/ios-port/recompiler-fixtures-xcode/fixture/generated" \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/sdl3-src"
cmake --build .local/ios-port/ios-device --config Debug --target mostwanted_memory_tests -- -quiet CODE_SIGNING_ALLOWED=NO

JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
bash ./gradlew --no-daemon :app:assembleDebug
```

Coverage commands:

```sh
cmake -S ios/memory-tests -B .local/ios-port/memory-coverage -G Xcode \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping"
cmake --build .local/ios-port/memory-coverage --config Debug --target mostwanted_memory_tests -- -quiet
LLVM_PROFILE_FILE=.local/ios-port/memory-coverage/phase2c3-%p.profraw \
  .local/ios-port/memory-coverage/Debug/mostwanted_memory_tests
xcrun llvm-profdata merge -sparse .local/ios-port/memory-coverage/phase2c3-*.profraw \
  -o .local/ios-port/memory-coverage/phase2c3.profdata
xcrun llvm-cov report .local/ios-port/memory-coverage/Debug/mostwanted_memory_tests \
  -instr-profile=.local/ios-port/memory-coverage/phase2c3.profdata \
  sdk/runtime/kernel/memory.cpp sdk/runtime/kernel/guest_strings.cpp
```

Apple thread-source syntax checks:

```sh
xcrun --sdk macosx clang++ -std=c++20 -fsyntax-only -Isdk/runtime -Isdk/harness/compat -Inative/compat sdk/runtime/kernel/threads.cpp
xcrun --sdk iphoneos clang++ -target arm64-apple-ios15.0 -std=c++20 -fsyntax-only -Isdk/runtime -Isdk/harness/compat -Inative/compat sdk/runtime/kernel/threads.cpp
xcrun --sdk iphoneos clang++ -target arm64-apple-ios15.0 -std=gnu++20 -fsyntax-only -fno-strict-aliasing -fsigned-char -I native/compat -I sdk/harness/compat -I native -I src -I sdk/runtime -I .local/ios-port/checked-recompiler-census -I third_party/SDL/include -include native/compat/crt.h sdk/runtime/kernel/boot.cpp sdk/runtime/kernel/xboxkrnl.cpp
```

### Generated trap reachability

The existing “Generated x86 trap sites” table above records 16 static unimplemented-instruction sites. It does not count divide-error checks or `int3` traps, which also call `x86_trap`. During a future gated startup run, capture complete logs from `x86_trap` and `x86_checked_trap`, record each guest PC/reason, and match PCs against the 16-site table. A matching PC proves reachability. No match is meaningful only after a defined milestone is reached with complete logs. Both handlers stop execution, so each process can identify only its first fatal trap; do not skip blocked instructions to continue startup.

**Phase 2C-3 recommendation (historical): NO-GO for restricted Xbox runtime startup.** The Phase 2C-4 results below supersede that checkpoint. Simulator execution, device compilation, physical-device execution, and game boot remain separate claims.

## Phase 2C-4: checked XBE preparation and instruction census

Verified 2026-11-05 on branch `feature/ios-port`, starting from `606881c15cc6c8d12ff9fea6add68783deeb6eda`. Generated XBE data, generated recompiler C++, logs, and the local game image stayed under ignored `.local/ios-port/`; no game asset or generated game source was added to tracked files.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `CHECKED_XBE_LOADER` | **PASS** | Synthetic XBE and local Black Edition XBE prepared by checked writes; entry, header, preloaded bytes, zero tails, and reference counts verified; no guest code executed. |
| `XBE_FAILURE_HANDLING` | **PASS** | Malformed XBE cases rejected; injected preparation and section-load write failures rolled back reservations/newly committed pages without damaging a shared-host-page neighbor. |
| `STARTUP_POINTER_AUDIT` | **PASS (inventory)** | Sequence and earliest remaining raw pointer paths documented below. Inventory completion does not mean those remaining paths are safe. |
| `CHECKED_GAME_INSTRUCTION_CENSUS` | **PASS (static)** | Actual checked recompiler output analyzed; 129 generated translation units passed C++ syntax-only compilation. No game code ran. |
| macOS ARM64 | **PASS** | Production memory and XBE loader CTests passed in Debug and Release; actual XBE CLI preparation passed. |
| iOS Simulator ARM64 | **PASS** | iPhone 18 Pro simulator ran production memory, generated fixture, fault, and synthetic loader tests through `memory tests: finished`. |
| iPhoneOS ARM64 | **PASS (compile/link)** | Unsigned test app linked as Mach-O ARM64 with code signing disabled. Not installed or run on a physical device. |
| Android | **PASS (build)** | Java 21.0.13 and NDK 28.2.13676358 `:app:assembleDebug` passed. No Android runtime execution. |

### Checked XBE loader results

The ignored local image `.local/ios-port/default.xbe` was 4,739,072 bytes, title ID `4541007B`, entry `0x002B3916`, and SHA-1 `f636a16af04171089f879a4bed646e185c734233`. The checked loader accepted all 15 sections, including 12 preloaded sections, prepared the actual guest image, verified the committed entry address, then released the reservation. The header matched the file apart from loader-updated section reference counts. Each preloaded section's raw data, virtual zero tail, and reference count matched expected state.

The parser rejects a truncated header, out-of-bounds section table, truncated section, overflowing raw offset, image or section address overflow, overlapping virtual sections, unterminated kernel thunk table, and overflowing TLS range. Identity and entry mismatches fail before reservation. An injected third checked-write failure occurs after header and section bytes/zero-fill have been written; rollback leaves no image reservation or committed pages. A second synthetic section shares a 16 KB host page with a preloaded neighbor. After decommitting its guest page, injected data-write and reference-count-write failures verify that section bytes are cleared, only the new page is decommitted, the reference count stays zero, and the neighboring preloaded page remains intact. The valid load/reload/unload reference-count sequence also passes. These checks ran in the macOS harness and simulator; the real Black Edition XBE was parsed and prepared only in the macOS harness.

`xbe::PrepareImage` is separate from `Boot()`: it checks expected SHA-1 and entry, reserves guest space, writes the header and preloaded sections through `mem::Write`, zero-fills virtual tails with `mem::Fill`, writes reference counts through checked scalar access, and releases the reservation after a failed write. `Parse` now bounds section/header/file/image/TLS/certificate metadata, checks the preloaded executable entry, and requires a terminated kernel thunk table in file-backed bytes. The existing Apple `Boot()` null-base guard remains unchanged.

### Startup pointer-escape ledger

This sequence is the path that would be reached if the Apple `Boot()` guard were removed. It was inspected statically; the real `Boot()` sequence did not run.

| Earliest stage | Code and access | Current status |
| --- | --- | --- |
| Apple host entry | `kernel::Boot()` calls `mem::Init()`, then sees `mem::Base() == nullptr`, logs, shuts memory down, and returns before parsing the XBE. | **Guarded.** No later row is currently reachable on Apple through `Boot()`. |
| XBE preparation | `xbe::Image::Parse` and `xbe::PrepareImage`; checked writes/fill only. | **Checked and tested** in isolation against synthetic and actual XBE. No guest instruction or kernel initialization. |
| Loaded callback | `app::kGame.loaded()` runs after image preparation and before kernel initialization. The target game descriptor sets `.loaded = nullptr`. | **No callback for this title.** |
| Kernel data initialization | `InitKernelData` initializes kernel data with checked `W8/W16/W32`; `PutAnsi` writes descriptor and NUL-terminated string together with one `mem::Write` and returns failure. The detached clock updater uses checked `W32` stores. | **Checked access, not runtime-tested.** Other scalar helper faults abort through the current fault policy rather than returning a status. |
| Dispatch and thunk patching | `dispatch::Init` builds host-side mapping tables. The import loop reads and patches the parser-validated, terminated kernel thunk table using checked `R32/W32`. | **No direct pointer escape.** Invalid guest access still stops via the checked fault handler. |
| File-system setup | `MountDefaults` maps host directories and creates the configured directories. It does not access guest memory. | **Host filesystem side effects only.** No game file was opened by this audit. |
| Previous launch data | `startup_data::ImportLaunchData` reads exactly 4 KiB into a bounded host buffer, validates allocation ownership, writes the data and pointer with `mem::Write`, and removes the source only after both writes succeed. | **Checked and failure-injected.** Failed reads/writes preserve `launch.bin`, keep the pointer unchanged, and release the new allocation. |
| Persisted display | After a successful launch-data import, `startup_data::ImportPersistedDisplay` validates header, marker, pixel/picture limits, and exact file length; writes pixels and the full surface metadata block through checked memory; publishes state only after both writes succeed. | **Checked and failure-injected.** Failed import preserves the file and old state, releases the allocation, and retains neighboring bytes. On a cold start with no imported launch page, display data is left untouched, preserving the prior quick-reboot pairing. |
| Guest thread allocation | `CreateThread` allocates stack/TLS/KPCR/ETHREAD and calls `InitializeThreadGuestMemory`; the isolated synthetic preparation checkpoint verifies KPCR/ETHREAD contents and cleanup without starting a host thread. | **Structure helper tested.** Full thread creation remains outside the isolated boot path. |
| Guest execution boundary | `StartThread` registers/spawns the host thread, initializes context, then enters generated code through `CallGuest`. | **Prohibited in this phase.** Checked-mode execution still stops at unsupported instructions. |
| First game entry sequence | Checked generated code supports MOV/MOV/SUB/ADD and CMP at `0x002B3928`; the following JNB uses the CMP result. The next linear blocker is PUSH at `0x002B393F`, followed by LEA, AND, and CALL sites. | **Static only.** No runtime reachability claim; each unsupported instruction still traps. |
| Guest kernel exports | `XeLoadSection`/`XeUnloadSection` validate section-header addresses against the parsed table, serialize multi-call changes against guest page changes, and use checked fill/write/reference-count operations with rollback of newly committed pages on load failure. Other object, critical-section, string, and context exports still use `G<T>()` or `HostPtr()`. | **Section helpers compiled and tested through the loader API, not executed as kernel exports.** Remaining exports are unsafe on Darwin. |
| Guest I/O and device work | Contiguous synchronous `NtReadFile`/`NtWriteFile` data uses checked staging; its IOSB uses one checked 8-byte publication after access preflight. Scatter/gather IOSB/data, socket buffers, event/APC metadata, guest objects, critical sections, waits, and GPU/audio paths retain raw or retained-pointer behavior. | **Partial and blocked.** The IOSB is not pinned across host I/O, so protection/decommit can still race after file side effects. Callback and object paths below remain outside the checked boundary. |

`sdk/runtime/kernel/rtl.cpp` also retains raw string copy/conversion and interlocked paths. `sdk/runtime/kernel/xboxkrnl.cpp` has raw object, critical-section, context, and crypto paths. The Phase 2C-1 inventory above lists the graphics, audio, network, and recompiler families. The Phase 2C-5 section at the end records the current file-level raw-pointer inventory. Do not remove the Apple guard until all paths reachable on the selected startup route have checked semantics and rollback/lifetime tests.

### Checked-game instruction census

The actual local XBE was regenerated with `[xbe] checked_memory = true`; it was not executed. Recompiler log reports 28,848 functions, 1,237,217 unique guest instruction PCs, 1,369,241 generated instruction emissions across functions, 132,024 duplicate emissions, 90 overlapping decodes, and 3,705,446 generated code bytes out of 3,902,736. There were 451 jump tables and 13 callback entries. The checked output has 702,875 `x86_checked_trap` emissions at 641,426 distinct PCs and 666,366 supported emissions at 595,791 distinct PCs. The rejected-emission histogram contains 234 mnemonics. Counts are static code-generation counts, not runtime frequency.

The rejected instruction operand-shape histogram is based on top-level Intel-syntax operands (memory base/index registers are part of the memory operand):

| Operand shape | Emissions |
| --- | ---: |
| Register | 329,223 |
| Immediate | 160,709 |
| Memory | 75,688 |
| Memory + register | 68,372 |
| Immediate + register | 46,469 |
| Immediate + memory | 11,767 |
| Other | 4 |
| No explicit operand | 10,623 |
| Immediate + memory + register | 20 |

Primary access category is a syntax/operation heuristic; it assigns each rejected emission once and is not a full x86 semantic decoder. In particular, stack operations include their implicit stack access, and indirect call targets are counted with stack writes.

| Primary access category | Emissions |
| --- | ---: |
| Implicit stack write | 283,474 |
| Implicit stack read | 70,954 |
| No explicit memory operand | 204,354 |
| Address-only LEA | 48,798 |
| Explicit memory read | 67,957 |
| Indirect-control memory read | 688 |
| Explicit memory write | 16,966 |
| Explicit memory read-modify-write | 4,605 |
| Implicit REP/string | 4,406 |
| Atomic/locked memory | 282 |
| Prefetch hint | 339 |
| Memory-order/cache control | 49 |
| Implicit XLAT read | 3 |

The family counts include 77,895 x87 emissions; 16,441 SIMD/MMX emissions (explicit XMM/MMX operands plus EMMS/SFENCE); and 4,406 implicit string operations, including 2,896 REP-prefixed forms (2,778 REP, 115 REPE, 3 REPNE). The 282 atomic/locked memory cases are LOCK-prefixed memory instructions plus memory XCHG, which is implicitly locked. They do not include ordinary unlocked read-modify-write instructions.

All rejected emissions grouped by mnemonic:

```text
aas=1, adc=263, addps=1115, addss=631, and=10279, bound=2, bsf=4, bswap=28
bt=1, bts=1, call=88893, cdq=1164, cld=22, cli=4, cmovb=3, cmovbe=1
cmovl=11, cmovle=4, cmovnb=1, cmovnbe=14, cmovnl=6, cmovnle=26, cmovz=7, cmp=49168
cmpsb=30, cmpsd=66, cmpsw=19, cmpxchg=21, comiss=70, cpuid=5, cvtpi2ps=4, cvtsi2ss=104
cvtss2si=29, cvttss2si=316, das=1, dec=5944, div=319, divss=29, emms=114, f2xm1=9
fabs=216, fadd=4575, faddp=1267, fchs=1066, fcmovnbe=2, fcom=1044, fcomi=2, fcomip=23
fcomp=4575, fcompp=119, fcos=36, fdiv=679, fdivp=166, fdivr=215, fdivrp=22, fiadd=20
fidiv=37, fidivr=2, fild=2465, fimul=28, fist=16, fistp=48, fisub=12, fisubr=7
fld=20395, fld1=67, fldcw=15, fldl2e=6, fldlg2=5, fldln2=6, fldpi=2, fldz=51
fmul=10055, fmulp=324, fnclex=2, fnsave=2, fnstcw=15, fnstsw=6343, fpatan=10, fprem=1
fptan=1, frndint=14, frstor=2, fscale=18, fsin=48, fsqrt=185, fst=1550, fstp=16068
fsub=3162, fsubp=484, fsubr=500, fsubrp=24, ftst=6, fucomip=1, fucompp=598, fwait=63
fxam=3, fxch=1198, fxtract=1, fyl2x=19, hlt=1, idiv=552, imul=2981, in=3
insd=1, int=2, int3=193, into=1, jmp=29306, lahf=29, lea=48798, leave=1343
lodsb=1, maxps=26, maxss=7, minps=26, minss=5, mov=1, movaps=2931, movd=254
movdqa=4, movdqu=2, movhlps=12, movhps=383, movlhps=42, movlps=494, movntps=16, movntq=208
movq=329, movsb=441, movsd=1731, movss=2277, movsw=76, movups=628, mul=84, mulps=1313
mulss=632, neg=1311, nop=686, not=262, or=6447, out=1, outsd=2, packssdw=12
packsswb=3, packuswb=22, paddb=4, paddd=21, paddsw=350, paddusb=5, paddw=231, pand=109
pcmpeqd=14, pcmpeqw=8, pinsrw=8, pmaddwd=25, pmaxsw=33, pminsw=33, pmovmskb=12, pmulhuw=2
pmulhw=197, pmullw=104, pop=69603, popad=4, popfd=4, por=116, prefetchnta=279, prefetcht0=60
pshufw=28, pslld=3, psllq=45, psllw=5, psrad=11, psraw=102, psrld=29, psrlq=70
psrlw=4, psubd=4, psubsw=164, psubw=24, punpckhbw=12, punpckhdq=32, punpckhwd=28, punpcklbw=54
punpckldq=34, punpcklwd=43, push=194573, pushad=4, pushfd=4, pxor=69, rcl=2, rcr=11
rdtsc=3, rol=20, ror=5, rsqrtss=6, sahf=10, sar=2951, sbb=985, scasb=3
setb=25, setl=53, setle=42, setnbe=2, setnl=44, setnle=43, setnz=704, sets=6
setz=698, sfence=43, sgdt=1, shl=5691, shld=39, shr=4187, shrd=19, shufps=466
sqrtss=9, std=17, sti=3, stosb=294, stosd=1609, stosw=133, subps=962, subss=476
test=53765, ucomiss=28, unpckhps=118, unpcklps=162, wbinvd=6, xadd=279, xchg=19, xlat=3
xor=21708, xorps=167
```

The original 16 static `x86_trap` sites remain in the separate table above. Do not add them to the 702,875 checked-mode rejections. To detect reachability in a future approved startup run, capture complete `x86_trap` and `x86_checked_trap` logs, match each reported guest PC against the original table, and record the startup milestone reached. Since the handlers stop the process, a single run identifies only its first fatal trap. No game boot or instruction reachability run occurred in Phase 2C-4.

The checked-memory emitter remains viable for staged startup: the real entry function's first linear instructions use supported checked MOV/SUB/ADD, then first reject memory CMP at `0x002B3928`, followed by stack/control and address-generation work. Add and test that startup sequence first. Full game coverage requires broader semantics across 234 rejected mnemonics; the census alone does not justify a wholesale memory-addressing rewrite because many rejects are register/immediate/control instructions. Keep checked memory operand lowering shared, then add explicit classes for stack/control, flags, REP/string direction and partial-fault semantics, locked atomics, and 10/16-byte x87/SIMD access.

**Controlled kernel initialization: NO-GO.** Earliest remaining raw startup pointers are `launch.bin` and persisted-display reads, followed by broad kernel/HLE/device paths; the actual entry also blocks on memory CMP. Do not lift the Apple `Boot()` guard or start Xbox guest threads in this phase.

### Phase 2C-4 exact commands

From repository root. Generated game files and real XBE paths below are local ignored development outputs.

```sh
# macOS ARM64 production memory/XBE tests, both configurations
cmake --build .local/ios-port/memory-macos --config Debug --target mostwanted_memory_tests --parallel 8
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure
cmake --build .local/ios-port/memory-macos --config Release --target mostwanted_memory_tests --parallel 8
ctest --test-dir .local/ios-port/memory-macos -C Release --output-on-failure
.local/ios-port/memory-macos/Debug/mostwanted_memory_tests "$PWD/.local/ios-port/default.xbe"

# checked-mode actual game generation; no output is executed
.local/ios-port/recompiler-fixtures-xcode/Debug/xbe_recomp \
  .local/ios-port/checked-game.toml \
  "$PWD/.local/ios-port/default.xbe" \
  "$PWD/.local/ios-port/checked-recompiler-census" \
  --xdk "$PWD/sdk/xdk" > .local/ios-port/checked-recompiler-census.log 2>&1

# generated C++ syntax check, all 129 translation units
find .local/ios-port/checked-recompiler-census -name 'x86_*.cpp' -print0 | \
  xargs -0 -n 16 -P 8 xcrun --sdk macosx clang++ -std=c++20 -fsyntax-only -fno-strict-aliasing \
    -fsigned-char -Wno-unused-label -Wno-unused-variable -Wno-parentheses-equality \
    -Isdk/runtime -I.local/ios-port/checked-recompiler-census

# generated fixture and fault handler, Debug and Release
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Debug \
  --target checked_recompiler_tests checked_fault_handler_tests --parallel 8
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Debug --output-on-failure
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Release \
  --target checked_recompiler_tests checked_fault_handler_tests --parallel 8
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Release --output-on-failure

# iOS Simulator ARM64; simulator UDID is local to this Mac
cmake --build .local/ios-port/ios-sim --config Debug --target mostwanted_memory_tests --parallel 8
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC \
  .local/ios-port/ios-sim/Debug-iphonesimulator/mostwanted_memory_tests.app
xcrun simctl launch --console-pty --terminate-running-process \
  43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests

# regenerate iPhoneOS project with the available SDL checkout, then compile/link unsigned ARM64
cmake -S ios -B .local/ios-port/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_CHECKED_FIXTURE_DIR="$PWD/.local/ios-port/recompiler-fixtures-xcode/fixture/generated" \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/sdl3-src"
cmake --build .local/ios-port/ios-device --config Debug --target mostwanted_memory_tests --parallel 8 -- CODE_SIGNING_ALLOWED=NO

# Android Java 21 / NDK 28.2.13676358
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
PATH=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home/bin:$PATH \
bash ./gradlew --no-daemon :app:assembleDebug
```

### Resolved errors and nonfatal diagnostics

- Before the alias-copy fix, optimized macOS CTest printed `FAIL: multi-chunk overlapping copy across physical aliases preserves source order and prefix`; an `-O3` probe reproduced repeated source bytes. Volatile overlap-ordered alias access fixed it; Debug and Release now pass.
- The first iPhoneOS build without `CODE_SIGNING_ALLOWED=NO` failed with `Signing for "mostwanted_memory_tests" requires a development team.` The retry disabled signing as requested.
- The first unsigned retry linked a stale generated Xcode project and failed with `Undefined symbols for architecture arm64: "_RunXbeImageLoader"`. Explicit CMake regeneration fixed the source list. Its first configure used a nonexistent `ios-device/_deps/sdl3-src`; the final configure points to the existing simulator SDL source checkout.
- An initial Android invocation selected Java 25 and failed with `SDK location not found. Define a valid SDK location...`. The final command sets Java 21 and `ANDROID_HOME`; build passed. Android also emitted nonfatal SDK XML v4 versus reader v3 warnings.
- The first Phase 2C-4 CTest run after expanding the synthetic XBE failed only the `truncated raw section rejected` assertion because the larger valid fixture now contained the old test's chosen eight-byte range. The malformed case now starts one byte before EOF with a two-byte section; final Debug and Release CTests pass.
- An early fixture build failed parser validation with `entry point is not in a preloaded executable section`; the synthetic section was marked preloaded and the thunk table fixture was moved to bounded header data. Debug/Release fixture CTests now pass.
- The simulator logged `You need UIApplicationSupportsIndirectInputEvents in your Info.plist for mouse support`; memory, loader, fixture, and fault tests still completed. No device install or game execution was attempted.

The old Phase 2C-3 coverage report is retained as historical evidence. No new whole-file coverage target was pursued in this phase; semantic loader rollback, malformed-file, checked-access, and platform tests were prioritized.

## Phase 2C-5 startup data, checked CMP, and preparation boundary

Verified 2026-11-05 on `feature/ios-port`, starting at `7d8f5820f8551bf8fc43b7e752ab708be9804f18`. Work stayed in the requested checkout. Generated XBE data, generated C++, logs, and build products remained under ignored `.local/ios-port/`; the pre-existing `.DS_Store` was not staged.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `LAUNCH_DATA_CHECKED_IMPORT` | **PASS** | Exact 4 KiB host-buffer read, allocation ownership, checked page and pointer writes, source preservation on injected read/allocation/write failures, cleanup, repeat import, missing and malformed file cases. |
| `PERSISTED_DISPLAY_CHECKED_IMPORT` | **PASS** | Header, arithmetic, pixel and picture bounds, exact file length, checked pixel and metadata writes, state publication, replacement cleanup, neighbor preservation, and injected failures covered. |
| `CHECKED_CMP_FIXTURE` | **PASS** | Actual generated CMP variants execute against independent reference flags; destination preservation, JNB/JP/signed-JL branches, and checked-memory fault state are verified. Debug and Release CTests pass. |
| `NON_EXECUTING_KERNEL_PREPARATION` | **BLOCKED** | Isolated synthetic XBE preparation and KPCR/ETHREAD checked initialization/cleanup pass. The complete production preparation chain is not isolated: `InitKernelData` starts an unjoined worker, dispatch consumes production generated globals, filesystem mappings have no reset seam, and thunk patching has no synthetic fixture. No guest code ran. |
| `STARTUP_REACHABILITY_LEDGER` | **PASS (static)** | Current generated entry blocks and guest-memory dependencies recorded below. This is not runtime reachability evidence. |
| macOS ARM64 | **PASS** | Memory/import/XBE tests pass in Debug and Release; CMP and fault-handler fixtures pass in Debug and Release. |
| iOS Simulator ARM64 | **PASS** | Memory/import/XBE test app built without a generated guest fixture, launched on iPhone 18 Pro simulator, and logged `memory tests: finished`. CMP and fault-handler fixtures pass macOS Debug and Release CTests. |
| iPhoneOS ARM64 | **PASS (compile/link)** | Unsigned `mostwanted_memory_tests` and `mostwanted_vulkan_ios` targets linked with code signing disabled. No physical-device install or run. |
| Android | **PASS (build)** | Java 21 / NDK 28.2.13676358 `:app:assembleDebug` passed on the final shared-source revision in 1m 5s. No Android runtime execution. |

### Startup import and CMP behavior

`startup_data::ImportLaunchData` and `ImportPersistedDisplay` replace the boot-time `HostPtr()` reads with bounded file buffers and checked writes. Launch data is exactly 4096 bytes; the guest pointer is published only after both checked writes succeed. Persisted display is imported only after launch data imports successfully, preserving the existing quick-reboot pairing and avoiding stale display imports on cold start. The display importer bounds pixels to 8 MiB and the path string to 1040 bytes, checks the 28-byte header and exact total file length using 64-bit arithmetic, writes pixels before the 64-byte surface structure, then publishes state and releases any previous allocation. Failed imports preserve their files and previous display state. A failure to remove a source after a successful import is reported as a warning while the valid imported state remains published.

The checked recompiler now admits only the existing non-locked binary RMW operand class for CMP. It computes CF, PF, AF, ZF, SF, and OF at the operand width, sign-extends narrow immediates as x86 requires, performs a checked memory read in source order, does not write the destination, and retains the instruction PC/address/status on faults. AF is now preserved in `X86Context` alongside the other saved flags; ADD/SUB/INC update it too. Fixtures cover byte/word/dword register, memory and immediate forms; carry, signed overflow, zero, negative, parity and nibble borrow; unaligned and SIB addressing; read-only/uncommitted/cross-page/overflow faults; JNB/JP decisions; and signed JL decisions including SF/OF overflow. The checked whitelist did not expand to PUSH, LEA, AND, CALL, or other unsupported operations.

### Non-executing preparation checkpoint

`CheckSafePreparationCheckpoint` in `ios/memory-tests/xbe_image_tests.cpp` parses and prepares a synthetic XBE, verifies its entry is mapped, allocates a temporary stack/KPCR/ETHREAD, calls the production checked `InitializeThreadGuestMemory`, verifies structure fields, and frees those test resources. It calls neither `Boot()` nor `StartThread`, `CallGuest`, or a callback that can enter guest code. Stage A import tests run separately against dedicated temporary directories.

This is not the full preparation sequence. `InitKernelData` launches a detached clock worker with no stop/join API, so a test cannot deterministically release guest memory while that worker may still write. `dispatch::Init` reads `X86CodeBase`, `X86CodeEnd`, `X86FuncMappings`, and `X86Continuations` production globals rather than receiving a synthetic mapping set. `MountDefaults` also mutates process-global mappings and creates host directories without a reset path. The production `Boot()` guard remains, and these steps were not invoked. Do not treat the safe checkpoint as permission to begin guest execution.

### Current startup pointer and host-buffer inventory

The early `Boot()` load paths now use checked helpers, but guest execution would still enter shared runtime code containing unchecked guest references or pointers. The file-level scan found these remaining source locations:

- `sdk/runtime/kernel/kernel.h` defines raw `G<T>()` and `HostPtr()`; `rtl.cpp` retains raw string/copy/conversion and interlocked operations; `xboxkrnl.cpp` retains raw object, critical-section, context and crypto operations; `files.cpp` hands guest buffers to host file APIs. `threads.cpp` keeps a non-Apple direct-memory fallback; Apple thread setup uses the checked whole-structure helper. `guest_strings.cpp` also retains a non-Apple `HostPtr()` fallback; its Apple path uses checked reads.
- `sdk/runtime/xdk/d3d8.cpp`, `dsound.cpp`, `xapi.cpp`, and `xnet.cpp`, plus `sdk/runtime/xdk/hle.h`, read or write guest objects through `HostPtr()`/`G<T>()`. D3D's save-side persisted-display caller passes a raw guest pixel pointer into `SavePersistedDisplay`.
- `sdk/runtime/gpu/render_thread.cpp`, `texture.cpp`, and `vertex.cpp`, plus `src/d3d/d3d8.cpp`/`render_thread.cpp`, retain graphics buffers or pointers. `sdk/runtime/gpu/d3d12.cpp` also contains raw accesses but is a Windows-only backend and is not compiled into the iOS target. Deferred render work can outlive a call unless its source is copied or pinned.
- `src/sound/buffers.cpp`, `device.cpp`, `mixer.cpp`, and `streams.cpp` retain raw audio buffers across device/mixer work.
- `src/game/fe_mouse.cpp`, `game.cpp`, `game_font.cpp`, `hud_layout.cpp`, `options_menu.cpp`, and `prompts.cpp` use `kernel::G<T>()`/`HostPtr()` in game hooks, menus, fonts, HUD data, and prompt text.
- `sdk/runtime/xdk/crt_math.cpp` reads x87 math arguments from guest stack memory through raw `X86_MEMF64`.
- Default-mode generated code still has direct `X86_MEM*`/`X86_PTR` emissions. The actual checked-mode output uses checked scalar helpers for its supported whitelist and emits traps for unsupported instruction shapes; it does not make the runtime/HLE paths above safe.

Scalar `R8/R16/R32/R64` and `W8/W16/W32/W64` are checked on Apple and abort on access failure; they are not the remaining pointer escapes. The kernel thunk patch loop uses those checked scalar helpers, but has not run in an isolated startup-preparation test. Every raw path that could be called by the selected game route still needs an owner-specific checked/copy/pin conversion and rollback or lifetime test before the Apple guard can change.

### Checked generated entry audit

Fresh checked-mode output was generated under `.local/ios-port/phase2c5-checked-game/`; all 129 generated translation units passed syntax-only compilation. Static inspection of `sub_002B3916` shows:

| Guest PCs | Generated operations | Dependency / status |
| --- | --- | --- |
| `0x002B3916–0x002B3923` | Load ECX from `[0x00010118]`, load EAX from `[0x00010108]`, subtract ECX, add `0x10000`. | Checked 32-bit reads; register arithmetic updates flags. |
| `0x002B3928–0x002B392C` | `cmp eax,[ecx]`; `jnb 0x002B392E`; if the branch is not taken, store EAX to `[ecx]`. | CMP is newly supported, JNB consumes its carry result, and the conditional store is checked. Read/write address depends on guest ECX. |
| `0x002B392E–0x002B3939` | Read `[0x003ED3FC]`, subtract `[0x003ED3EC]`, then read `[0x003ED3F0]`. | Checked 32-bit guest-memory reads. |
| `0x002B393F–0x002B3948` | `push esi`, `lea eax,[eax+ecx+0x0F]`, `push edi`, `and eax,0xFFFFFFF0`, `push 0xFFFFFFFC`. | First blocker is PUSH at `0x002B393F`; LEA and AND are also rejected. PUSH needs checked stack writes and ESP semantics. |
| `0x002B396A` | `call 0x002B5CA7`. | Direct guest call remains blocked; target and any host-export calls beneath it were not executed or audited as reachable. |

The `CMP` memory read reports guest PC `0x002B3928`, the ECX-derived address, and the checked access status. Its generated fault fixture confirms the CMP does not mutate architectural registers or flags when that read fails. Static block ordering does not prove any of these instructions were reached at runtime. Before the first rejected PUSH, no host API call appears in this generated sequence; later guest/HLE dependencies remain unknown until separately audited.

### Phase 2C-5 exact verification commands

Build and run macOS ARM64 import, loader, and synthetic preparation tests:

```sh
cmake -S ios/memory-tests -B .local/ios-port/memory-macos -G Xcode \
  -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build .local/ios-port/memory-macos --config Debug --target mostwanted_memory_tests --parallel 8 -- -quiet
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure
cmake --build .local/ios-port/memory-macos --config Release --target mostwanted_memory_tests --parallel 8 -- -quiet
ctest --test-dir .local/ios-port/memory-macos -C Release --output-on-failure
```

Build and run actual generated CMP and fault-handler fixtures:

```sh
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Debug \
  --target checked_recompiler_tests checked_fault_handler_tests --parallel 8 -- -quiet
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Debug --output-on-failure
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Release \
  --target checked_recompiler_tests checked_fault_handler_tests --parallel 8 -- -quiet
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Release --output-on-failure

mkdir -p .local/ios-port/phase2c5-checked-game
.local/ios-port/recompiler-fixtures-xcode/Debug/xbe_recomp \
  .local/ios-port/checked-game.toml "$PWD/.local/ios-port/default.xbe" \
  "$PWD/.local/ios-port/phase2c5-checked-game" --xdk "$PWD/sdk/xdk"
find .local/ios-port/phase2c5-checked-game -name 'x86_*.cpp' -print0 | \
  xargs -0 -n 16 -P 8 xcrun --sdk macosx clang++ -std=c++20 -fsyntax-only \
    -fno-strict-aliasing -fsigned-char -Wno-unused-label -Wno-unused-variable \
    -Wno-parentheses-equality -Isdk/runtime -I.local/ios-port/phase2c5-checked-game
```

The iPhone 18 Pro simulator memory-test app was rebuilt with `MOSTWANTED_CHECKED_FIXTURE_DIR` empty and launched from `.local/ios-port/ios-sim`; it contains no generated guest fixture and logged `memory tests: finished`. Generated CMP and fault fixtures were verified by macOS Debug and Release CTests. The device project uses the iPhoneOS 27.2 SDK and local SDL checkout; the unsigned memory-test and Vulkan bootstrap targets are rebuilt with `CODE_SIGNING_ALLOWED=NO`. Android uses the verified local Java/SDK paths:

```sh
cmake -S ios -B .local/ios-port/ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_BUILD_VULKAN_BOOTSTRAP=ON \
  -DMOSTWANTED_CHECKED_FIXTURE_DIR= \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/sdl3-src"
cmake --build .local/ios-port/ios-sim --config Debug --target mostwanted_memory_tests --parallel 8 -- -quiet
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC \
  .local/ios-port/ios-sim/Debug-iphonesimulator/mostwanted_memory_tests.app
xcrun simctl launch --terminate-running-process \
  43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests

cmake -S ios -B .local/ios-port/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_BUILD_VULKAN_BOOTSTRAP=ON \
  -DMOSTWANTED_CHECKED_FIXTURE_DIR="$PWD/.local/ios-port/recompiler-fixtures-xcode/fixture/generated" \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/sdl3-src"
cmake --build .local/ios-port/ios-device --config Debug \
  --target mostwanted_memory_tests mostwanted_vulkan_ios --parallel 8 -- -quiet CODE_SIGNING_ALLOWED=NO

JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
PATH=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home/bin:$PATH \
bash ./gradlew --no-daemon :app:assembleDebug
```

### Resolved failures and limits

- The first Android compile found `kPersistMagic` had become private to the importer while `SavePersistedDisplay` still referenced it. The constant is now shared from `startup_data.h`; the initial full Java 21/NDK 28.2.13676358 build passed in 8m 5s, and the final-source rebuild passed in 1m 5s.
- The first simulator rebuild used a stale generated Xcode project and failed to link `_RunStartupDataTests`. Explicit CMake reconfiguration refreshed the source list; the current simulator app built, installed, and launched.
- An initial Android invocation omitted `ANDROID_HOME` and stopped during Gradle configuration. The final command above exports the existing SDK path and Java 21.
- The simulator build reported unavailable optional `PkgConfig`/`LibUSB`; SDL configured and linked its enabled iOS backends. This did not block the build.
- No physical iPhone installation, real Xbox guest instruction, `Boot()` path, `StartThread`, `CallGuest`, merge, or release was performed.

**Next-stage recommendation: NO-GO for kernel initialization or game execution.** Keep the Apple `Boot()` null-base guard. CMP is covered, but the entry path next reaches unsupported stack/control operations, the complete kernel-preparation sequence is not isolated, and runtime/HLE raw guest pointers remain.

## Phase 2C-6 deterministic preparation and checked stack instructions

Verified on `feature/ios-port`, starting from `206e41ed239760cf404fc4ab270b0d2ae1a43b93`. The Apple `Boot()` null-base guard remains. No real game instruction ran; neither `StartThread` nor `CallGuest` was invoked. Generated game output and build products stayed under ignored `.local/ios-port/`; no game asset or generated game source was added to the change.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `KERNEL_CLOCK_LIFECYCLE` | **PASS** | Synchronous kernel-data initialization is separate from a joinable clock worker. Tests cover repeated starts/stops, update failure, concurrent stop, shutdown races, stale-write prevention, and stop-before-memory teardown. |
| `DISPATCH_PREPARATION` | **PASS** | Synthetic function/continuation metadata is bounded and validated; checked thunk patching covers invalid/unterminated data and rollback after a write mutates then fails. No mapped guest function ran. |
| `FILESYSTEM_PREPARATION` | **PASS (scoped; see limit)** | Each filesystem context owns its maps; temporary-root tests cover five drive mappings, rollback, remount, cleanup, traversal and symlink escape checks. A symlink can still be replaced between path validation and open. |
| `NON_EXECUTING_KERNEL_PREPARATION` | **PASS** | Two complete synthetic setup/teardown passes cover checked memory, XBE parse/prepare, synchronous kernel data, dispatch, import resolution/patching, temporary filesystem setup, and KPCR/ETHREAD/stack/TLS preparation. Injected XBE, kernel-data, and thunk-write failures roll back. No worker, guest thread, or guest callback remains after teardown. Optional launch/display data was not included in this combined harness; its checked import tests remain separately covered by Phase 2C-5. |
| `CHECKED_ENTRY_INSTRUCTIONS` | **PUSH: PASS; LEA: PASS; AND: PASS; CALL: BLOCKED** | Generated synthetic fixtures pass semantic and fault checks in Debug and Release. The actual XBE output was regenerated and statically inspected: PUSH/LEA/AND now emit checked forms at the target entry addresses. Direct CALL remains a checked trap; see the required B4 fixture plan below. |
| macOS ARM64 memory tests | **PASS** | Debug and Release CTests passed 1/1 each, including the preparation harness and injected failures. |
| Recompiler fixture tests | **PASS** | `checked_recompiler_tests` and `checked_fault_handler_tests` passed 2/2 in both Debug and Release. These are synthetic guest snippets, not game code. |
| iOS Simulator ARM64 | **PASS** | The memory-test app ran on the iPhone 18 Pro simulator and logged `memory tests: finished`; generated fixture execution was excluded from this app. |
| iPhoneOS ARM64 | **PASS (compile/link)** | The memory-test target compiled and linked unsigned for ARM64 with signing disabled. It was not installed on a physical iPhone. |
| Android `:app:assembleDebug` | **PASS (build)** | Java 21 and NDK 28.2.13676358 built successfully in 2m 11s using the fresh ignored Phase 2C-6 generated directory; the final-source rebuild after the lifecycle/rollback fixes passed in 45s. The first attempt against stale ignored `generated/game` failed at link because it lacked the newly emitted count constants; the local retry passed. Android runtime was not exercised. SDK XML v4 and an SDL sensor deprecation warning were nonfatal. |

### Preparation and instruction findings

`InitKernelData()` now performs only synchronous, checked guest-memory initialization. `StartKernelClock()` separately starts one owned worker after preparation; stop joins it before memory shutdown. A memory lifecycle gate prevents a new worker from racing with initialization or shutdown. The update callback stops on its first failed checked write, and tests verify repeated initialization/teardown does not leave a writer attached to a later mapping.

Dispatch consumes an explicit bounded metadata table rather than walking a zero-terminated generated array. Initialization rejects missing, duplicate, out-of-range, and oversized metadata and supports reset/reinitialization. Kernel thunk patching validates the file-backed thunk span, ordinal range and termination before accepting the table; a failed write restores the original slot, including a slot whose injected writer changed bytes before returning failure. `Boot()` checks every preparation stage and retains rollback on failure.

Filesystem tests use a value-owned context rooted in temporary directories. Failed setup removes only directories created by that attempt and leaves the previous mappings intact. Resolution rejects path components and symlinks that escape the mapped root. There is a residual time-of-check/time-of-use window if another process replaces a symlink after resolution but before the host file API opens the path; descriptor-relative opens would be needed if concurrent untrusted writers are in scope.

The actual checked-mode entry output at `0x002B393F–0x002B3948` now emits checked `PUSH ESI`, address-only `LEA EAX,[EAX+ECX+0x0F]`, `PUSH EDI`, `AND EAX,0xFFFFFFF0`, and sign-extended `PUSH -4`. Checked PUSH commits ESP only after a successful write; memory-source PUSH remains rejected. LEA does not dereference guest memory or change flags. AND updates defined flags and leaves undefined AF unchanged; fault fixtures verify guest-state preservation. CDQ, IDIV and XOR remain checked traps later in the same function. The direct CALL at `0x002B396A` remains blocked. The audit was static output inspection only and does not establish runtime reachability.

### Direct CALL design plan (B4; implementation remains blocked)

Before enabling direct CALL, add actual generated synthetic fixtures and require all of the following:

1. A caller/callee fixture proves the 32-bit return address is written little-endian at `ESP - 4`, ESP changes only after a successful checked write, and the callee returns to the exact generated continuation.
2. Nested caller/callee fixtures prove LIFO return-address order and correct continuation dispatch across multiple generated functions, including any emitted continuation/resume path.
3. `RET` and `RET imm16` fixtures prove stack advancement and callee-cleanup semantics, and reject a return address that is not a valid mapped function/continuation for the active call.
4. Injected write/read faults at uncommitted, read-only, guard, and boundary-crossing stack addresses prove guest registers/ESP remain consistent with the chosen x86 fault point; production fault-handler tests verify the reported guest PC/address/direction.
5. Invalid target and continuation cases trap without null dispatch, stale continuation use, or host-stack escape. Audit the direct target and its reachable helper dependencies in generated output before the fixture can authorize support.

The current generator still emits a checked trap for CALL. Existing `RET` behavior and `CallGuest` host-return handling are not evidence for checked guest CALL correctness. No CALL fixture was enabled and no game code ran.

### Current startup pointer-safety ledger

This table distinguishes preparation exercised in the synthetic harness from paths that require game execution or later services. The Apple guard keeps all later Boot rows unreachable on Apple.

| Path | Phase 2C-6 status |
| --- | --- |
| Apple `Boot()` entry | **Guarded.** `mem::Base()` is null on Apple; Boot exits before XBE or generated-code access. |
| XBE image, kernel data, dispatch and thunk preparation | **Checked and exercised** by the synthetic non-executing harness with rollback cases. |
| Clock worker | **Owned and joined.** Its checked writes are covered by lifecycle and failure tests; startup remains guarded. |
| Temporary filesystem mappings | **Scoped and tested.** Path traversal and symlink escape checks pass, with the replacement race above. No user save directory was used by tests. |
| KPCR, ETHREAD, stack and TLS setup | **Prepared and checked** in the harness; no host guest thread was started. |
| `SavePersistedDisplay` save side | **Raw pointer remains.** `src/d3d/d3d8.cpp` can pass guest pixels through `HostPtr()` to `SavePersistedDisplay`; this is not exercised during preparation. |
| File and socket buffers | **Raw pointers remain** in `files.cpp` and `xnet.cpp` when runtime exports are called. Copy direction, access bounds and lifetime are not covered by this preparation checkpoint. |
| Guest objects and critical sections | **Raw access remains** in `xboxkrnl.cpp` and related HLE paths through `G<T>()`/`HostPtr()`. |
| Deferred GPU/audio pointers | **Raw or retained pointers remain** in graphics and sound services; asynchronous lifetime is not validated here. |
| Guest thread start and kernel callbacks | **Not entered.** `StartThread`, `CallGuest`, direct CALL, and real guest instructions remain outside scope. |

### Phase 2C-6 verification commands

The following were run from the repository root. Simulator and device commands use the existing ignored build directories; no physical-device installation was performed.

```sh
# macOS ARM64 preparation harness, Debug and Release
cmake --build .local/ios-port/memory-macos --config Debug --target mostwanted_memory_tests --parallel 8
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure
cmake --build .local/ios-port/memory-macos --config Release --target mostwanted_memory_tests --parallel 8
ctest --test-dir .local/ios-port/memory-macos -C Release --output-on-failure

# checked instruction and fault fixtures, Debug and Release
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Debug --target checked_recompiler_tests checked_fault_handler_tests --parallel 8
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Debug --output-on-failure
cmake --build .local/ios-port/recompiler-fixtures-xcode --config Release --target checked_recompiler_tests checked_fault_handler_tests --parallel 8
ctest --test-dir .local/ios-port/recompiler-fixtures-xcode -C Release --output-on-failure

# simulator run and unsigned iPhoneOS ARM64 compile/link
xcrun simctl launch --console-pty --terminate-running-process 43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests
cmake --build .local/ios-port/ios-device --config Debug --target mostwanted_memory_tests --parallel 8 -- CODE_SIGNING_ALLOWED=NO

# Android, Java 21 and NDK 28.2.13676358; fresh local generated code is not tracked
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
PATH=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home/bin:$PATH \
bash ./gradlew -PxbeGenerated="$PWD/.local/ios-port/phase2c6-entry-audit" :app:assembleDebug
```

**Next-stage decision: NO-GO for guest execution.** The synthetic preparation gate passes, but the Apple `Boot()` guard stays in place, direct CALL remains blocked, later instructions remain unsupported, and file/socket/object/critical-section/display/GPU/audio paths still expose raw or retained guest pointers. This checkpoint does not authorize guest execution, physical-device installation, merge, or release.

## Phase 2C-7 checked CALL/RET and runtime access boundaries

Verified on `feature/ios-port`, starting at `9bcb8e81e9c94eec3fd758fd836217f88edd85e3`. The Apple `Boot()` null-base guard is unchanged. The real Black Edition XBE was regenerated under ignored `.local/ios-port/phase2c7-entry-audit/` and statically inspected only; no real XBE instruction ran.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `CHECKED_DIRECT_CALL` | **PASS (narrow support)** | Actual `xbe_recomp` output from the synthetic XBE tests direct relative 32-bit CALLs to mapped function entries. The checked helper writes the little-endian return address through `mem::Write` before changing ESP, records nested expected returns, and rejects unmapped or continuation targets before stack mutation. Indirect CALL, `CALL $+5`, and unsupported operand/target classes remain traps. Unchecked emission is unchanged. |
| `CHECKED_RET_CONTINUATIONS` | **PASS (covered paths)** | Generated fixtures cover nested LIFO calls, the caller's generated post-call block, `RET`, `RET imm16`, corrupted return words, a valid synthetic `X86Resume` continuation, stale continuation rejection, and uncommitted CALL-write/RET-read faults. Fault fixtures check exact guest PC, address, direction, and status; production handlers log and abort. Xbox SEH recovery and real-game continuation behavior were not executed. |
| `RUNTIME_POINTER_MIGRATION` | **PARTIAL** | Persisted-display CPU snapshot and contiguous `NtReadFile`/`NtWriteFile` buffers now use checked memory APIs. See raw paths and limits below. |
| `ENTRY_SEQUENCE_AUDIT` | **PASS (static only)** | Regenerated checked output confirms blockers before the entry CALL and inside its target. This is not runtime reachability evidence. |
| macOS ARM64 memory tests | **PASS** | Debug and Release CTests passed 1/1 each, including display allocation failure and checked file-transfer tests. |
| Recompiler fixtures | **PASS** | Debug and Release CTests passed 2/2 each using generated synthetic-XBE code and production dispatch/memory helpers. |
| iOS Simulator ARM64 | **PASS** | The memory/preparation app installed and ran on the iPhone 18 Pro simulator; it logged `memory tests: finished` with no `FAIL` assertions. Generated fixture execution was excluded from this app. |
| iPhoneOS ARM64 | **PASS (unsigned compile/link)** | `mostwanted_memory_tests` built with signing disabled. It was not installed on a physical iPhone. |
| Android | **PASS (build)** | `:app:assembleDebug` passed with Java 21 and NDK 28.2.13676358, using the fresh ignored Phase 2C-7 generated directory. Android runtime was not exercised. Android's SDK XML v4 compatibility message was nonfatal. |

### Checked CALL/RET scope

Checked mode accepts only direct relative 32-bit CALL instructions whose destination is a mapped generated function entry and is not an `X86Resume` continuation. Runtime dispatch repeats the check before writing the stack. After a successful checked stack write, the callee runs with a tracked expected return address; checked RET reads and validates that word before advancing ESP, then applies the optional 16-bit callee cleanup. Nested frames unwind in LIFO order. `CallGuest` uses a tracked host sentinel so checked guest RETs return to the host without dispatching the sentinel as guest code. `x86_resume` validates that its destination is a currently listed continuation before dispatch.

The synthetic return path resumes at the generated caller's instruction following CALL; that is an in-function fallthrough, not an `X86Resume` table entry. A separate generated function validates the runtime's listed-continuation path. The fixture rejects a direct CALL to a continuation before touching ESP. Full guest exception/SEH behavior remains unverified. Indirect CALL, `CALL $+5`, unmapped direct targets, and targets classified as continuations remain blocked. The default unchecked emitter retains its prior CALL/RET behavior.

### Regenerated real-game entry audit

In checked output, `0x002B3916` now emits checked code through `PUSH` at `0x002B393F`, `LEA` at `0x002B3940`, `PUSH` at `0x002B3944`, `AND` at `0x002B3945`, and `PUSH -4` at `0x002B3948`. The next instruction is unsupported `POP ECX` at `0x002B394A`; later blockers before the direct CALL are `CDQ` at `0x002B3953`, `IDIV ECX` at `0x002B3954`, and `XOR ESI,ESI` at `0x002B395C`. Therefore the CALL at `0x002B396A` is not statically reachable along this checked entry path.

The CALL target `0x002B5CA7` is a mapped generated function. If entered in isolation, its first blocker is `TEST EAX,EAX` at `0x002B5CAD`. Later emitted traps include `SHR ECX,2` at `0x002B5CC0`, memory-source PUSH instructions beginning at `0x002B5CCA`, indirect CALL `[0x003C9E68]` at `0x002B5CE0`, `TEST ECX,ECX` at `0x002B5CE8`, and `POP EBP` at `0x002B5D11`. `RET 0x18` at `0x002B5D12` is emitted as checked RET, but is not reachable through the current checked target path. The CALL's fallthrough is `0x002B396F` inside the same caller; the generated exception-continuation table contains only `0x000279CA`, an unrelated EH continuation. After a hypothetical return, more blocked direct calls occur at `0x002B397A` and `0x002B3980`, followed by blocked POPs at `0x002B3985–0x002B3986`.

### Converted runtime APIs and remaining raw paths

- `SavePersistedDisplay` now receives a guest virtual address. Its save helper validates the 32-bit range, 1–8 MiB pixel bound, and 1040-byte picture bound; it copies pixels with one checked `mem::Read`, then publishes a complete temporary file using flush, sync, and atomic replacement. Allocation, guest-read, write, or publication failure preserves the previous save. An injected `std::bad_alloc` test confirms allocation failure returns an error instead of escaping the caller.
- This converts only the CPU snapshot. Both D3D persist callers first call `g_backend->Sync(data, bytes)`. On iOS, `native/vulkan.cpp::VulkanBackend::Sync` still copies readback rows through `kernel::HostPtr(data)` and raw `memcpy`; the Windows D3D12 backend has the same raw readback pattern. GPU readback is unconverted and untested, and the complete display-save route is therefore still unsafe for guest execution.
- Contiguous synchronous `NtReadFile` and `NtWriteFile` now use bounded staging (64 MiB, inferred from the modeled Xbox RAM size). The complete guest span is preflighted, writes snapshot guest bytes before the host callback, and reads publish with one checked guest write after the host callback. Race tests cover concurrent decommit, permissions, overflow, partial host reads, host short-write counts, and no partial guest publication. This does not claim a maximum native Xbox request size was established.
- `NtReadFileScatter`/`NtWriteFileGather` retain their raw scatter/gather path. IOSB/event/APC structures and other file exports still use raw guest access. Network buffers, guest objects/critical sections, RTL/interlocked paths, deferred audio/GPU buffers, and default-mode generated memory instructions also remain raw or retained. No guest object/critical-section change was made because atomic ownership could not be tested in this bounded phase.

### Filesystem race and execution decision

Tests use process-owned temporary directories and do not create or follow attacker-controlled links. The symlink replacement race remains. App-container paths on iOS are private to the app, but Android's configured `/storage/emulated/0/MostWanted` and removable-storage game directories are shared locations when `MANAGE_EXTERNAL_STORAGE` is granted. A concurrent writer can replace a path component between validation and open/rename. A portable repair would need descriptor-relative operations for the full file lifecycle plus separate Windows reparse-point handling; that exceeds this phase, so shared-path use remains unsafe under a concurrent untrusted writer.

**Future restricted execution decision: NO-GO.** The entry stops at `POP ECX` before reaching the checked CALL, and the callee stops at `TEST EAX,EAX`. The synthetic preparation harness does not invoke production `Boot()` or combine launch/display imports with later startup stages; GPU/file/object/audio/network paths also remain unconverted. Preserve the Apple `Boot()` guard. No physical-device installation, real guest execution, merge to `main`, or customer release occurred.

## Phase 2C-8 checked entry-chain instructions and GPU readback

Verified on 2026-11-06 on `feature/ios-port`. Phase commits: `35b6d17` (entry-chain integer instructions), `8b42dc9` (callee instruction forms), and `4f74c0f` (checked Vulkan readback). The Apple `Boot()` null-base guard remains. Current Black Edition output was regenerated under ignored `.local/ios-port/phase2c8-final-entry-audit/` with the current Debug `xbe_recomp` and inspected statically. Generated real-game functions were not executed, and no XBE, ISO, or generated game C++ was committed.

| Gate | Result | Evidence and limits |
| --- | --- | --- |
| `CHECKED_ENTRY_CHAIN` | **PASS** | Actual recompiler-generated synthetic fixtures execute `POP ECX`, `CDQ`, signed `IDIV ECX`, and `XOR ESI,ESI` through production checked-memory helpers. Tests cover stack guard/cross-page faults, register and flag preservation, signed dividends, `INT32_MIN`, zero divisor, quotient overflow, and undefined-flag preservation. IDIV failures stop at the guest PC with diagnostics; this does not implement Xbox `#DE` delivery. |
| `CHECKED_CALLEE_INSTRUCTIONS` | **PASS for tested forms only** | The three supported callee instruction classes below each pass their synthetic generated-code fixtures; no broader instruction or operand coverage is claimed. |
| `TEST EAX,EAX` | **PASS** | Synthetic code tests representative values, defined arithmetic flags, and preservation of undefined AF. Memory-source TEST remains rejected. |
| `SHR ECX,2` | **PASS** | Synthetic code tests the entry's immediate-count form plus counts 1, 0, 31, and 32 masking, result and defined flags, and preservation of undefined flags. `SHR r32,CL` and memory-destination SHR remain rejected. |
| Memory-source dword `PUSH` | **PASS for tested flat-memory forms** | Fixtures cover `[EBP+disp]`, absolute memory, `[ESP]`, and `[ESI]`, including source-read and stack-write faults. 16-bit and segment-overridden forms remain rejected. |
| `GPU_GUEST_READBACK` | **PASS for helper; Vulkan runtime unverified** | The isolated helper tests completed/padded rows, destination-padding preservation, partial output, invalid/oversized/overflowing layouts, injected `std::bad_alloc`, read-only/uncommitted pages, and concurrent decommit. Vulkan `Sync` and `DeliverLate` now use this boundary; Android `assembleDebug` compiled it. No real Vulkan GPU readback was run. |
| `STARTUP_RUNTIME_SAFETY` | **NO-GO** | Static output now emits checked code through the direct call at `0x002B396A`. The called function's next unsupported instruction is indirect `CALL [0x003C9E68]` at `0x002B5CE0`; callback target and reachable kernel/HLE access graph remain unresolved. The `Boot()` guard stays. |
| macOS ARM64 memory tests | **PASS** | Debug and Release CTests passed `1/1` each after the deterministic decommit-test fix. |
| Generated recompiler/fault fixtures | **PASS** | Debug and Release CTests passed `2/2` each, executing synthetic generated code only. |
| iOS Simulator ARM64 | **PASS** | Fresh memory-test app build and iPhone 18 Pro simulator run passed 372 assertions with no `FAIL`; generated instruction fixtures were excluded from the app. |
| iPhoneOS ARM64 | **PASS (unsigned compile/link)** | The `mostwanted_memory_tests` target built with signing disabled. It was not installed on a physical iPhone. |
| Android | **PASS (build)** | `:app:assembleDebug` passed with Java 21 and NDK 28.2.13676358, compiling `native/vulkan.cpp` and `checked_gpu_transfer.cpp`. The APK was not installed or run. SDK XML v4 and SDL sensor deprecation warnings were nonfatal. |

### Checked entry and callee scope

The regenerated entry block at `0x002B3916` now has checked `POP ECX` at `0x002B394A`, `CDQ` at `0x002B3953`, `IDIV ECX` at `0x002B3954`, and `XOR ESI,ESI` at `0x002B395C`, followed by the checked direct CALL at `0x002B396A` to mapped function `0x002B5CA7`. POP reads the full stack dword before changing ECX or ESP. CDQ sign-extends EAX into EDX. IDIV forms the signed EDX:EAX dividend, uses truncation toward zero, and checks divide-by-zero and signed quotient range before updating EAX/EDX. Its fatal diagnostic is not Xbox exception delivery. XOR updates CF/OF/SF/ZF/PF and preserves undefined AF.

The isolated callee fixture validates `TEST EAX,EAX` defined flags while preserving undefined AF; immediate 32-bit SHR count masking, result, CF/SF/ZF/PF, OF handling, and count-zero preservation; and read-before-write ordering for memory-source dword PUSH. The supported PUSH forms use flat guest memory and default/DS/SS addressing. Read or stack-write faults preserve guest state and report guest PC, address, direction, and access status. Unsupported instruction shapes remain traps.

Static inspection of the regenerated callee shows checked `TEST EAX,EAX` at `0x002B5CAD`, `SHR ECX,2` at `0x002B5CC0`, and memory pushes at `0x002B5CCA`, `0x002B5CCD`, `0x002B5CD0`, and `0x002B5CD3`. The next blocker is indirect `CALL [0x003C9E68]` at `0x002B5CE0`. Later blocked instructions include `TEST ECX,ECX` at `0x002B5CE8` and `POP EBP` at `0x002B5D11`; checked `RET 0x18` at `0x002B5D12` is not statically reachable through this path. After a hypothetical return, caller direct calls at `0x002B397A` and `0x002B3980`, then POPs at `0x002B3985–0x002B3986`, are additional blockers. These are code-generation observations, not runtime reachability evidence.

### GPU transfer boundary and remaining raw access

`kernel::checked_gpu_transfer::MakeLayout` validates nonzero dimensions, source pitch, destination capacity, guest address range, checked row/total arithmetic, and a 64 MiB per-span staging bound. `Transfer` first validates the entire guest destination, invokes the synchronous readback callback into host-owned storage, rejects short output, then reacquires the memory lock and revalidates the complete write span before copying rows with `mem::Write`. Guest row padding remains untouched. No guest pointer crosses GPU work; retained packed pixels publish only after all guest rows succeed. The decommit fixture completes decommit before the callback returns, then verifies the second full-span check prevents any row publication.

`VulkanBackend::Sync` waits for GPU submission completion before using the helper. `DeliverLate` copies only after its fence is complete. On submission failure, the readback buffer is retired so it is not destroyed while an already-submitted command may still be in flight. Checked signatures are used by `Sync` and `RecordLate`, but broader Vulkan input paths such as `MemSig`, `Changed`, `LatchCpuChanges`, texture/vertex uploads, and other rendering buffers still use raw or retained pointers. Windows D3D12 readback in `sdk/runtime/gpu/d3d12.cpp` remains unchanged and raw. The Vulkan integration is compile-verified only; simulator helper tests are not GPU-driver readback tests.

| Runtime path | Phase 2C-8 status |
| --- | --- |
| Apple `Boot()` and real-game entry | Guarded; no game instruction was run. Checked fixture support is limited to the tested instruction forms. |
| Contiguous synchronous `NtReadFile`/`NtWriteFile` data | Checked staging and memory transfer remain covered by Phase 2C-7 tests. IOSB/status, event, APC metadata and related exports still contain raw accesses. |
| `NtReadFileScatter`/`NtWriteFileGather` | Raw scatter/gather path remains blocked. |
| Guest objects, critical sections, and waits | Raw guest structure access remains in object and kernel exports; not converted or invoked by the preparation harness. |
| Thread, APC, and DPC callbacks | `StartThread`, `RunApcs`, DPC dispatch, and callback exports can reach `CallGuest`; callback targets and lifetimes are not bounded by this phase. |
| Vulkan output readback | Checked transfer helper is tested and Vulkan call sites compile; actual GPU runtime remains unverified. |
| Vulkan input, D3D12, audio, network, and other deferred buffers | Raw or retained guest pointers remain; not converted here. |
| Filesystem paths | Tests remain in owned temporary roots. The documented symlink-replacement time-of-check/time-of-use race remains for shared paths with concurrent writers. |

### Restricted-execution prerequisites

**NO-GO for restricted or real guest execution.** A future restricted experiment requires, at minimum: (1) a complete static instruction and call graph for the exact entry route, with the indirect call target resolved and checked or rejected before side effects; (2) checked or lifetime-pinned access for every reachable file status/event/APC, object, critical-section, wait, scatter/gather, graphics, and audio buffer, or a validated pre-access guard proving a path cannot run; (3) scoped guest callback dispatch and no guest thread start before all gates pass; (4) failure-injected rollback and decommit/lifetime tests for the combined startup preparation; and (5) target-platform compile and simulator verification for that exact bounded route. Separate authorization would still be required for any later execution experiment. Passing synthetic instruction fixtures alone is insufficient. Keep the Apple `Boot()` guard.

### Phase 2C-8 verification commands

From the repository root. All generated XBE outputs and build directories below are ignored local artifacts. The simulator memory-test build used an empty `MOSTWANTED_CHECKED_FIXTURE_DIR`; it did not include generated game functions.

```sh
# Production memory tests, Debug and Release
cmake --build .local/ios-port/memory-macos --config Debug --target mostwanted_memory_tests --parallel 8
ctest --test-dir .local/ios-port/memory-macos -C Debug --output-on-failure
cmake --build .local/ios-port/memory-macos --config Release --target mostwanted_memory_tests --parallel 8
ctest --test-dir .local/ios-port/memory-macos -C Release --output-on-failure

# Synthetic generated instruction and fault-handler fixtures
cmake --build .local/ios-port/recompiler-fixtures-tracka --config Debug --target checked_recompiler_tests checked_fault_handler_tests --parallel 8
ctest --test-dir .local/ios-port/recompiler-fixtures-tracka -C Debug --output-on-failure
cmake --build .local/ios-port/recompiler-fixtures-tracka-release --config Release --target checked_recompiler_tests checked_fault_handler_tests --parallel 8
ctest --test-dir .local/ios-port/recompiler-fixtures-tracka-release -C Release --output-on-failure

# Regenerate actual Black Edition output for inspection only
.local/ios-port/recompiler-fixtures-xcode/Debug/xbe_recomp \
  .local/ios-port/checked-game.toml "$PWD/.local/ios-port/default.xbe" \
  "$PWD/.local/ios-port/phase2c8-final-entry-audit" --xdk "$PWD/sdk/xdk" \
  > .local/ios-port/phase2c8-final-entry-audit.log 2>&1

# Simulator memory/preparation app; no generated fixture or game execution
cmake -S ios -B .local/ios-port/phase2c8-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator27.2.sdk \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_CHECKED_FIXTURE_DIR= \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/sdl3-src"
cmake --build .local/ios-port/phase2c8-sim --config Debug --target mostwanted_memory_tests --parallel 8
xcrun simctl install 43A6197F-AD68-48D4-84DB-6A147D89B8DC \
  .local/ios-port/phase2c8-sim/Debug-iphonesimulator/mostwanted_memory_tests.app
xcrun simctl launch --console-pty --terminate-running-process \
  43A6197F-AD68-48D4-84DB-6A147D89B8DC com.racas.mostwanted.memorytests

# Unsigned iPhoneOS ARM64 compile/link; no physical install
cmake -S ios -B .local/ios-port/ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DMOSTWANTED_CHECKED_FIXTURE_DIR= \
  -DFETCHCONTENT_SOURCE_DIR_SDL3="$PWD/.local/ios-port/ios-sim/_deps/sdl3-src"
cmake --build .local/ios-port/ios-device --config Debug --target mostwanted_memory_tests --parallel 8 -- CODE_SIGNING_ALLOWED=NO

# Android ARM64 build only
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home \
ANDROID_HOME=/Users/abbypangeran/Library/Android/sdk \
PATH=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home/bin:$PATH \
bash ./gradlew --no-daemon -PxbeGenerated="$PWD/.local/ios-port/phase2c8-final-entry-audit" :app:assembleDebug
```

Phase integration is limited to the pushed `feature/ios-port` branch; no merge to `main`, physical-device install, real guest execution, IPA, sideload, or customer release occurred.

## Phase 2C-9 checked indirect dispatch and runtime boundaries

This checkpoint advances the checked instruction and readback rows above. The baseline was `3649a1f2dd065c1eda90850c6fc4836b23b8b539` on `feature/ios-port`. The actual Black Edition XBE was regenerated into ignored local output and inspected statically only. No real XBE instruction ran, and the Apple `Boot()` guard remains.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `CHECKED_INDIRECT_CALL` | **PASS (narrow support)** | `xbe_recomp`-generated fixtures cover an absolute-memory `CALL dword ptr [disp32]`, generated-entry target, nested direct/indirect calls, RET/RET imm16, target and stack faults, protection/decommit races, and architectural state preservation. Target read uses checked memory before checked return-address write; ESP changes only after that write. Only mapped generated function entries are admitted. |
| `INDIRECT_TARGET_PROVENANCE` | **PASS (static, dynamic value unknown)** | The XBE bytes, loader thunk patch, guest address mapping, and current source write sites were inspected below. Initial and expected post-patch values classify as a kernel thunk; dynamically written values remain possible and are reclassified on every call. |
| `RUNTIME_CALLBACK_BOUNDARIES` | **PARTIAL; remaining paths blocked** | Contiguous file IOSB publication now uses a checked atomic helper with focused success, invalid-span, and concurrent-protection tests. Event handles, queued APCs, guest object/critical-section fields, wait objects, scatter/gather, and other callbacks are not converted. The helper tests do not inject a protection race through the complete `files.cpp` export. |
| `VULKAN_READBACK_RUNTIME` | **PASS (iOS Simulator GPU)** | The simulator Vulkan app rendered an offscreen 4×4 RGBA8 pattern, waited for a fence, copied through the real staging/readback path and `checked_gpu_transfer`, and matched pixels plus row-padding sentinels in guest memory. Source pitch was 32 bytes; guest pitch was 64 bytes. |
| `STARTUP_EXECUTION_READINESS` | **NO-GO** | The patched target at the audited CALL is a kernel thunk, which the checked policy rejects before stack mutation. Earlier unsupported/runtime paths, callback dispatch, raw HLE pointers, and file side-effect/IOSB race still prevent guest execution. Preserve the Apple `Boot()` guard. |

### Checked CALL and real-game target analysis

Checked code generation admits only the 32-bit absolute-memory form with no base or index register and the supported default/DS addressing. The helper reads the operand through `x86_mem_read` with the original guest PC, classifies the result, and permits only a mapped generated-function entry. A continuation, kernel thunk, registered host thunk, null, uninitialized table, unmapped code address, or unknown target traps before writing the return address. The default unchecked Android emitter is unchanged.

An outer `dispatch::CallLease` keeps the dispatch table shared-locked through checked calls, legacy `x86_icall`, and `x86_resume` so `Reset`/`Init` cannot invalidate a resolved function while it runs. Nested calls reuse the outer lease. The legacy dispatcher still admits its existing kernel/host callback classes; the stricter generated-entry-only policy applies to checked CALL. A generated concurrency fixture holds a lease while another thread calls `Reset`, then verifies reset completes only after release. This relies on the existing lifecycle boundary that dispatch initialization/reset occurs outside guest execution; resetting the table from inside a leased guest call would deadlock and is not a supported lifecycle operation.

The checked helper reads the target operand before checking/writing the guest stack, matching the required memory-operand-before-push ordering. The emulator policy deliberately rejects disallowed target classes before the guest return-address push; this is stricter than executing an arbitrary x86 CALL target and prevents an unsupported HLE/callback target from mutating guest stack state. Operand-read faults report the original instruction PC and read address; stack-write faults preserve ESP/register/flag state. Fatal checked traps still do not provide Xbox exception delivery. See the [Intel SDM, Volume 2, CALL](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf) for the architectural instruction semantics.

For the real call `0x002B5CE0: CALL dword ptr [0x003C9E68]`, the ignored XBE `.local/ios-port/default.xbe` has SHA-1 `f636a16af04171089f879a4bed646e185c734233`. Guest VA `0x003C9E68` maps to file offset `0x003BA148`; its initial dword is `0x800000FF`, the import-ordinal encoding for ordinal 255. The loader's thunk patch pass writes the resolved kernel thunk at this slot after image preparation; ordinal 255 resolves to `0xFE0003FC` (`PsCreateSystemThreadEx`). The XBE loader prepares the image as writable guest memory and this implementation has no relocation pass that makes the slot immutable. No other static source write to this literal slot was found. Guest instructions and raw host/HLE memory paths may still change it, so the runtime value is not inferred solely from the initial XBE bytes.

At the call site, the resolver distinguishes a generated function entry, generated continuation, kernel thunk, registered host thunk, null target, uninitialized table, in-code unmapped address, and unknown target. Only the first is allowed. The expected patched value is therefore rejected before stack mutation; any dynamic replacement is read and classified at runtime. `PsCreateSystemThreadEx` reaches thread start and `CallGuest` callback code if invoked through an authorized path, but checked indirect CALL does not authorize that kernel path.

The regenerated checked output emits the indirect helper at `0x002B5CE0`. It also emits `TEST ECX,ECX` at `0x002B5CE8`, a short in-function JMP at `0x002B5D0C` to `0x002B5D11`, `POP EBP` at `0x002B5D11`, and checked `RET 0x18` at `0x002B5D12`. After a hypothetical return, caller `POP EDI` and `POP ESI` at `0x002B3985–0x002B3986` are now supported. These instruction emissions do not make the call reachable: the expected kernel-thunk target is rejected, and no game code was run.

The additional TEST, POP, and same-function short-JMP forms have generated semantic and fault fixtures. JMP targets are required to resolve inside the current generated function; unsupported forms remain checked traps. Debug and Release fixture suites each pass 2/2 CTests.

### Runtime ownership and access ledger

| Path | Owner and access timing | Retention and invalidation behavior |
| --- | --- | --- |
| Contiguous `NtReadFile`/`NtWriteFile` data | Caller owns the guest buffer. The synchronous export snapshots write data or stages host-read data, then uses checked guest memory APIs. | No guest data pointer is retained across host I/O. A complete-span access check runs before and after host work. |
| Contiguous IOSB | Caller owns the 8-byte guest status block. `files.cpp` preflights write access, then `WriteStatusBlock` publishes status and byte count with one `mem::Write`. | The IOSB page is not pinned across file I/O. Concurrent protection/decommit can make final publication fail after the file cursor, host stream, or file contents changed; the helper test covers atomic memory publication but not that export-level interleaving. Event/APC completion is skipped on publication failure. |
| File event | The event value is a kernel handle resolved to a host-owned shared object after IOSB publication. | The handle value is not retained as a guest memory pointer; file completion signals the host object. Other guest dispatcher-object paths still access raw guest fields. |
| File APC and guest callback | `Complete` copies the routine, context, and IOSB guest addresses into a thread-owned APC queue; `RunApcs` later calls `CallGuest`. | Guest callback target and arguments are retained after the issuing call. No page pin, checked target-class policy, or decommit/protection handling is established for this path. Thread start, DPC, timer, and other callbacks are also blocked. |
| Guest objects, critical sections, and waits | Guest owns object fields; `G<T>()`/`HostPtr()` and scalar helpers access some fields directly while the kernel export runs or waits. | No complete checked/pinned lifetime boundary or rollback contract. Concurrent decommit/protection and invalid guest structure addresses remain unsafe. |
| `NtReadFileScatter` / `NtWriteFileGather` | Caller owns the segment list and data pages. Current code traverses them through raw `HostPtr()` and uses raw IOSB stores. | No checked span validation, pinning, or safe partial-completion contract; remain blocked. Socket, deferred graphics/audio, texture/vertex, and Windows D3D12 paths remain separately raw or retained. |

### Verification evidence

All generated files and build directories below are ignored local artifacts. The Phase 2C-9 Black Edition output was inspected only.

| Check | Result |
| --- | --- |
| macOS ARM64 memory tests | Debug and Release CTests passed 1/1 each (`phase2c9-memory-red`, `phase2c9-memory-release`). |
| Synthetic generated instruction and fault fixtures | Debug and Release CTests passed 2/2 each (`phase2c9-fixtures-red`, `phase2c9-fixtures-release`). |
| iOS Simulator memory/preparation app | `mostwanted_memory_tests` ran to `memory tests: finished`; checked IOSB full-span publication and concurrent protection assertions passed. No real generated game code was included. |
| iOS Simulator Vulkan runtime | `VULKAN_READBACK_PASS: 4x4 RGBA8, source_pitch=32, guest_pitch=64, fence-complete, padding-preserved` on the iPhone 18 Pro simulator GPU using MoltenVK. Probe memory was allocated and released through the guest allocator; existing memory state is not shut down. |
| iPhoneOS ARM64 compile/link | `mostwanted_ios` and `mostwanted_vulkan_ios` built with signing disabled. No physical-device install or IPA. |
| Android | `:app:assembleDebug` passed with Java 21 and NDK 28.2.13676358, using fresh ignored `.local/ios-port/phase2c9-entry-audit` generated output. Android runtime was not exercised. SDK XML v4 compatibility warnings were nonfatal. |

The first Android attempt used a stale ignored generated map and failed because the mapping-count symbols were missing. The rerun used the fresh Phase 2C-9 generator output and passed. No XBE, ISO, or generated game source was staged.

**Integration and release status:** Phase work remains on `feature/ios-port`; this checkpoint does not merge to `main`, install to a physical iPhone, execute real guest code, create an IPA, or release to customers. **Next action:** keep the Apple `Boot()` guard and treat guest execution readiness as NO-GO until callback lifetime, IOSB transaction behavior, remaining HLE pointer paths, and the exact reachable instruction/call route are bounded and tested.

## Phase 2C-10 checked kernel ABI and inactive thread preparation

Baseline: `3c074bc5dbffe5d197fb604c893b753d54f86488` on `feature/ios-port`. Implementation and tests: `2edb0b03d14801a6210a4a46154036068e81017f`. The real Black Edition output was inspected statically; the tests use an actual recompiler-generated synthetic XBE. The Apple `Boot()` guard remains. No real XBE instruction, guest callback, thread activation, or physical-device install ran.

| Gate | Result | Evidence and limit |
| --- | --- | --- |
| `CHECKED_KERNEL_THUNK_ABI` | **PASS (fixture-only ordinal 255)** | Generated synthetic-XBE indirect CALL reaches only `0xFE0003FC` when the fixture bridge is enabled and the registered name is exactly `PsCreateSystemThreadEx`. The bridge validates a generated-code return address and 40 readable argument bytes, preserves checked frame and return-word behavior, and verifies 10-argument stdcall cleanup (`ESP = S + 40`), EAX, flags, and fault behavior. The test handler is not the production export. Other kernel/host thunk classes remain rejected. |
| `THREAD_PREPARE_NO_START` | **PASS** | The production `PrepareSystemThreadEx` path builds stack/TLS/KPCR/ETHREAD state and publishes optional outputs while leaving the thread prepared, absent from the active-thread list, and without a host worker. Cancellation closes its handle, clears still-owned output values, unbinds ETHREAD, and releases guest allocations. Allocation-stage and publication failures, invalid sizes/pages, duplicate output-reference use, distinct repeated preparations, and repeated cancellation are covered. |
| `KERNEL_255_PREPARATION` | **PASS (test-only service bridge)** | Synthetic ordinal-255 calls reach the non-starting test handler, which calls the production preparation routine with all ten arguments. Handle/ID publication and cancellation pass; worker-launch count stays unchanged. The production `PsCreateSystemThreadEx` export remains connected to its normal explicit activation call and was not invoked. |
| `IOSB_TRANSACTION_LIFETIME` | **PARTIAL** | Export-level `NtReadFile`/`NtWriteFile` tests cover valid data/status/event publication, invalid IOSB rejection before file or event side effects, and a barrier-controlled IOSB host-page decommit after read I/O. The last case returns access violation after guest read data and the host file cursor have advanced; the next read observes EOF. There is no host-side rollback or IOSB pin. Scatter/gather exports still use raw IOSB publication. |
| `CALLBACK_READINESS_AUDIT` | **PASS (static inventory only)** | The thread-start, file APC, timer APC, and DPC routes, retained values, dispatch, cancellation, and page-lifetime limits are recorded below. This audit does not authorize dispatch. Detached worker shutdown is unresolved and blocks execution. |
| `PLATFORM_REGRESSION` | **PASS** | macOS ARM64 memory/preparation CTests: Debug 1/1, Release 1/1. Generated instruction/CALL/RET/ABI/fault fixtures: Debug 2/2, Release 2/2. iOS Simulator memory/preparation app ran to completion with no failed assertions. Unsigned iPhoneOS ARM64 `mostwanted_ios` and `mostwanted_memory_tests` compiled and linked. Android `:app:assembleDebug` passed with Java 21 and NDK 28.2.13676358. |
| `GUEST_EXECUTION_READINESS` | **NO-GO** | The fixture handler does not activate threads or execute callbacks. Real callback target/page lifetime, detached-worker quiescence, raw runtime/HLE pointers, IOSB side effects, scatter/gather, and later graphics/audio/filesystem boundaries remain unresolved. Keep the Apple `Boot()` guard. |

### Checked ordinal-255 ABI and preparation behavior

The checked bridge exists only in builds defining `MOSTWANTED_CHECKED_KERNEL_ABI_TESTS`; it is not compiled into the Android or production iOS host. It admits the exact registered kernel thunk `kKernelThunkBase + 255 * 4` only while the fixture enables the bridge and ordinal 255 is registered under the exact name `PsCreateSystemThreadEx`. The current indirect target is still read through checked memory. The expected test call must have a return address in the generated-code span and all ten argument dwords readable before the handler runs. The bridge performs one checked return-address write, calls the fixture handler under the dispatch-table lease, then verifies the return word and exact 10-argument stdcall cleanup. Fixture cases reject the initial ordinal encoding, a wrong ordinal/name, invalid target, bad return span, unmapped/denied arguments, modified return word, and bad stack cleanup. It does not enable arbitrary kernel or host thunks and does not route through unrestricted legacy `x86_icall`.

The fixture handler's success branch calls `PrepareSystemThreadEx`, not the production `PsCreateSystemThreadEx` export. That production-relevant helper validates stack/TLS arithmetic, allocates and initializes guest stack/TLS/KPCR/ETHREAD state, binds the ETHREAD, and publishes optional handle/ID outputs while holding the guest-memory lock. Output spans must be readable and writable so rollback can compare and clear only values this operation published; write-only output spans fail before publication, with preparation resources rolled back. A test-only failpoint after each allocation stage and before output publication verifies rollback. Cancellation is allowed only from the prepared state; it closes the operation's handle, clears output locations only when they still contain values published by that operation, removes the matching pointer binding, and frees stack/system-pool allocations. The checked fixture confirms no active thread, worker launch, or guest callback is produced. `StartThread` remains a separate operation; the production Xbox exports call it explicitly after successful preparation to preserve Android behavior.

Host worker launch now reports allocation and OS thread-creation failure, and a failed launch removes the active registration so the caller can cancel the still-prepared thread. The workers remain detached, however. There is no stop/join or runtime-wide quiescence operation for guest threads, DPC, timer, audio, or vblank workers. Closure ownership and launch-failure rollback do not solve shutdown. This is an explicit blocker for eventual iOS execution; this phase did not start workers to test shutdown.

### Static startup and callback dependency map

The audited static route is `0x002B3916 → 0x002B396A → 0x002B5CA7 → 0x002B5CE0 → PsCreateSystemThreadEx`. The bytes at guest `0x003C9E68` start as `0x800000FF`; XBE import patching resolves ordinal 255 to `0xFE0003FC`. The checked fixture tests this mapping synthetically. The real XBE's entry code was not executed.

At `0x002B3916`, six pushed values feed the call to `0x002B396A`: `0, 0, 0x002B38A2, 0, 0, 0` (rightmost push becomes argument 0). In the downstream `0x002B5CA7` sequence, ten arguments are pushed to the import slot at `0x002B5CE0`:

| `PsCreateSystemThreadEx` argument | Static source |
| --- | --- |
| 0, handle output | `&[ebp+0x0C]` |
| 1 | `0` |
| 2, stack size | incoming argument 1 or fallback `[0x00010130]` (runtime-derived value) |
| 3, TLS size | `[0x004EDC74]` (runtime-derived value; caller writes it earlier at `0x002B394E`) |
| 4, thread-ID output | `0` |
| 5, start routine | `0x002B38A2` |
| 6, start context | `0` |
| 7, suspended | incoming argument 4, statically zero on this route |
| 8 | `0` |
| 9, system routine | `0x002B5C0F` |

The static routine VAs and zero context are established by the generated instructions; the effective stack size and TLS contents are runtime-derived. The thread object now retains the supplied `system_routine`, `start_routine`, and `start_context` directly. Activation waits for suspension to clear, then calls the system routine with `(start_routine, start_context)`, or the start routine with `(start_context)` when no system routine is supplied.

The candidate start routine `0x002B38A2` begins with a direct call to `0x002B58B8`, an unsupported checked instruction boundary. The inspected path reads `0x003C9E34` and `0x003C9E3C`, then reaches dynamic calls through `[0x003C9D30]` and EDI loaded from `[0x003C9D28]`. These are static dependencies, not evidence that the routine is reached.

The candidate system routine `0x002B5C0F` begins with `PUSH 0x18`, also outside the checked instruction set. It then pushes `0x003ED5B8`, calls `0x00319804`, accesses FS plus globals `0x003ED3F0`, `0x003ED3EC`, and `0x003ED3FC`, and calls the dynamic start routine from `[ebp+8]`. Later it calls import `[0x003C9E64]` and reaches `INT3`. None of these instructions ran.

Each callback path ultimately uses legacy `CallGuest`/`x86_icall`. Its dispatch lease protects the selected dispatch entry only for the synchronous call; it does not keep queued callback targets or guest pages valid while they wait.

| Callback path | Retained target and arguments | Dispatch, invalidation, and shutdown boundary |
| --- | --- | --- |
| Thread start | `Thread` retains start/system routine VAs and context from the export arguments. | `StartThread`'s detached host worker later calls legacy `CallGuest`; it does not pin guest code/data pages or classify the target under checked policy. No worker join or guest-code cancellation exists. |
| File completion APC | `Complete` queues `(routine, context, IOSB, 0)` on the thread after IOSB publication; `RunApcs` later invokes it on that thread context. | Callback target/arguments remain VAs; pages are not pinned or revalidated as a complete callback span. The APC queue has no cancellation/removal API. `CallGuest` holds a dispatch lease only for the synchronous invocation. |
| Timer APC | Timer entry retains APC routine/context and a weak thread reference; expiry queues `(context, timeLo, timeHi)`. | Cancelled timer sequences are ignored, but the timer worker is detached and has no stop/join path. Queued APC values and guest pages are not pinned. |
| DPC | Queue retains DPC VA and copied system arguments; dispatch later reads `DeferredRoutine` and `DeferredContext` from guest DPC fields and calls `(dpc, context, arg1, arg2)`. | Guest field reads and callback target are not checked/pinned; removing a DPC clears the guest `Inserted` field but does not remove an already queued host record. The DPC worker is detached. |

Before any callback dispatch can be authorized, targets and arguments need checked access or lifetime pins covering the full call, callback code/data mappings must survive protection/decommit, cancellation must invalidate already queued work, dispatch-table reset must remain excluded until all callbacks quiesce, and every host worker must stop/join before guest memory or runtime state is torn down. The current `Shutdown` path stops the kernel clock and calls `_Exit`; it does not establish this callback quiescence. Later graphics, audio, filesystem, socket, object, wait, scatter/gather, and GPU-input paths retain additional raw or long-lived guest pointers.

### Phase 2C-10 verification evidence

All generated XBE/C++ and build outputs remain in ignored `.local/ios-port/` directories. The fixture suite uses `xbe_recomp` to build the synthetic import call and does not run the real XBE.

| Check | Result |
| --- | --- |
| macOS ARM64 memory/preparation | Debug and Release CTests passed 1/1 each, including contiguous file-export lifecycle checks. |
| Generated checked fixtures | Debug and Release each passed 2/2 CTests for checked instructions, CALL/RET, ordinal-255 ABI, thread preparation, and faults. |
| iOS Simulator | iPhone 18 Pro simulator ran `mostwanted_memory_tests` through completion; memory, preparation, IOSB export, and generated fixture assertions had no failures. |
| iPhoneOS ARM64 | `mostwanted_ios` and `mostwanted_memory_tests` built unsigned as arm64 Mach-O executables. Neither app was installed on hardware. |
| Android | `:app:assembleDebug` passed with Java 21 and NDK 28.2.13676358; the existing SDK XML v4 compatibility notices were nonfatal. |

**Integration and release status:** changes remain on `feature/ios-port`; no merge to `main`, release, real XBE execution, guest callback, thread activation, or physical-device installation occurred. The task authorizes a normal fast-forward push. **Next action:** preserve the Apple guard and do not start a restricted guest-execution experiment until callback page lifetime, worker shutdown/join, non-transactional file-status semantics, scatter/gather, and later HLE access boundaries receive separate review and tests.
