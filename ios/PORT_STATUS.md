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
| Previous launch data | If `launch.bin` exists, `boot.cpp` allocates a page then calls `ifstream::read(HostPtr(page), 0x1000)` before removing the file. Allocation and read completion are not checked before the file is removed. | **Raw guest pointer. Earliest remaining boot-time escape after `MountDefaults`.** Must use checked copy-in and preserve the file on failed import before any guard change. |
| Persisted display | When the launch-data path is taken and `display.bin` metadata is valid, `LoadPersistedDisplay` reads up to 8 MiB directly into `HostPtr(block + 0x40)`. A failed pixel read returns without releasing `block`. | **Raw guest pointer and failure-path allocation leak.** Needs bounded host-buffer read, checked guest write, and cleanup before startup. |
| Guest thread allocation | `CreateThread` allocates stack/TLS/KPCR/ETHREAD and calls `InitializeThreadGuestMemory`; Apple failure paths release partial allocations. | **Checked setup and rollback.** Not invoked by Phase 2C-4. |
| Guest execution boundary | `StartThread` registers/spawns the host thread, initializes context, then enters generated code through `CallGuest`. | **Prohibited in this phase.** Checked-mode execution still stops at unsupported instructions. |
| First game entry sequence | Static generated function for `0x002B3916` has supported MOV/MOV/SUB/ADD instructions, then `cmp eax, [ecx]` is rejected at `0x002B3928`, before the first conditional branch. Subsequent linear instructions include PUSH, LEA, AND, and CALL rejections. | **Static blocker only.** No runtime reachability claim. |
| Guest kernel exports | `XeLoadSection`/`XeUnloadSection` validate section-header addresses against the parsed table, serialize multi-call changes against guest page changes, and use checked fill/write/reference-count operations with rollback of newly committed pages on load failure. Other object, critical-section, string, and context exports still use `G<T>()` or `HostPtr()`. | **Section helpers compiled and tested through the loader API, not executed as kernel exports.** Remaining exports are unsafe on Darwin. |
| Guest I/O and device work | `files.cpp` and `xnet.cpp` pass guest buffers to host file/socket APIs; `gpu`, D3D, texture/vertex, and sound paths use raw pointers; asynchronous paths can retain them. | **Unconverted.** Earliest use is after guest code invokes these exports; lifetime and copy direction need per-call review. |

`sdk/runtime/kernel/rtl.cpp` also retains raw string copy/conversion and interlocked paths. `sdk/runtime/kernel/xboxkrnl.cpp` has raw object, critical-section, context, and crypto paths. The Phase 2C-1 inventory above lists the graphics, audio, network, and recompiler families. Do not remove the Apple guard until all paths reachable on the selected startup route have checked semantics and rollback/lifetime tests.

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
