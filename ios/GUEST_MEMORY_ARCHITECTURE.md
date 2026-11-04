# Guest memory architecture for 16 KB hosts

## Phase 2C-1 access inventory

The inventory covers the runtime and static recompiler sources plus generated files under the ignored `.local/ios-port/generated/game/` directory. Generated game code was inspected in place and was not copied into tracked source.

The generated-code scan found `X86_MEM*` in 128 translation units, raw `X86_PTR` in 65 units, and atomic operations in 18 units. These counts overlap.

| Guest access category | Current mechanism | Required handling |
| --- | --- | --- |
| Recompiled scalar instructions | `X86_MEM8/16/32/64` dereference `base + guest_va`; emitted by `sdk/tools/recomp/codegen.cpp`; used throughout 128 generated translation units | Recompiler must emit checked reads and writes with width, direction, and full-span validation. |
| Recompiled raw pointers | `X86_PTR` in generated code; used in 65 generated units | Replace pointer escape with checked operations or pinned spans whose lifetime prevents decommit. |
| x87 and SIMD | x87 10-byte operands and 16-byte XMM loads/stores use raw `X86_PTR`; SIMD lane `memcpy` can also be local-only | Distinguish guest operands from local lane copies; validate complete 10/16-byte spans. |
| Atomic guest instructions | Recompiler emits atomic load, exchange, XADD, CMPXCHG, CMPXCHG8B, and bit operations through raw pointers; 18 generated units contain atomic operations | Add checked atomic operations with alignment, span, permission, and decommit synchronization. |
| REP/string instructions | `x86_rep_movs` and `x86_rep_stos` use direct `base + ESI/EDI`, `memmove`, `memcpy`, and `memset` | Validate full source and destination spans, overlap, direction flag, and fault/partial-progress behavior. |
| Kernel address helpers | `G<T>()`, `R16/R32/R64`, `W16/W32/W64`, and `HostPtr()` expose unchecked references, copies, or raw pointers from `g_base + va` | Replace raw references and pointers with checked scalar/span operations; direction and length must be explicit. |
| Boot and image loading | `boot.cpp` copies XBE headers and image sections directly through `g_base`; `XeLoadSection` uses `HostPtr()` | Use checked image writes and section permissions; validate zero-fill and raw/virtual lengths. |
| Kernel/HLE and OS I/O | `rtl.cpp` bulk operations; `files.cpp` passes guest buffers to file calls; `xnet.cpp` passes buffers to sockets; `xboxkrnl.cpp` contains atomic guest structure operations | Check the entire guest span before handing data to host APIs; copy or pin for the duration of asynchronous operations. |
| Threads and concurrent access | KPCR, ETHREAD, stack, and TLS initialization use `G/W`; guest work runs on host threads; audio and render workers can retain guest pointers | Use pinned views or copy-in/copy-out operations; serialize protection changes against active access. |
| Graphics | Vertex, texture, command, upload, and readback code uses `HostPtr()` and bulk copies; Vulkan and D3D12 paths access guest spans directly | Validate full row, texture, vertex, index, and command spans; pin or copy data for deferred GPU work. |
| Audio | Mixer and XDK sound paths use `HostPtr()`, typed helpers, and bulk copies, including asynchronous reads | Validate complete sample/block spans and lifetime across mixer callbacks. |
| Game/diagnostic hooks | Game/D3D hooks use `HostPtr()`, `G/R/W`, and C string or formatted-output routines; diagnostics export raw guest ranges | Replace with bounded guest APIs and explicit lengths. |

Changing `X86_MEM*` alone is insufficient. It misses `X86_PTR`, string helpers, kernel references and raw pointers, graphics/audio buffers, host I/O, and boot copies. The static recompiler must change and generated code must be regenerated outside tracked source. Host-side callers must also stop retaining unchecked guest pointers. `memcpy` in generated code is not automatically a guest access: several matches only pack local SIMD lanes.

The current memory manager stores commit bits at 4 KB granularity, but `IsCommitted()` is not a checked span operation. `NtProtectVirtualMemory` reports success without applying protection, `MmSetAddressProtect` is a no-op, and `MmQueryAddressProtect` always reports read/write. The Darwin backend therefore does not currently enforce guest permissions. A 16 KB host mapping cannot independently hardware-protect four 4 KB guest pages.

## Architecture choices

| Approach | Correctness | Complexity and speed | Memory and pointer compatibility | iOS fit |
| --- | --- | --- | --- | --- |
| A. Keep the linear guest mapping and add 4 KB software state | Correct only if every read, write, execute, and atomic access checks the complete range and decommit is synchronized with active users. Raw pointers bypass state. | Lower initial change; broad API conversion remains. Per-access page checks add cost; page-span pinning can amortize it. | Preserves sparse 4 GB VA and current backing layout. Existing direct pointers remain unsafe until every caller is converted. | Uses public file mapping and synchronization APIs; no JIT/private API needed. |
| B. Keep guest addresses logical and translate through a 4 KB page table to backing frames | Software page state is authoritative; one checked translation path can validate permissions, multi-page spans, and aliases. Correct once all access paths use that path. | Highest conversion effort: recompiler, kernel/HLE, I/O, GPU, and audio changes. Per-page translation costs more than direct pointer arithmetic; checked spans/pins can reduce repeated lookup. | Can retain sparse physical backing and map both physical aliases to the same frame. Raw host pointers must be removed or pinned through an explicit API. | Best fit for preserving guest semantics without relying on host sub-page protection. Uses public `mmap`/file backing; no executable-memory requirement. |
| C. Give each 4 KB guest page a separate 16 KB host page | Host protection can then represent each guest page independently. | Simpler protection semantics after translation, but every access still needs 4:1 address translation. | About 4x committed-page memory and address-space use; a 4 GB guest space expands to 16 GB before aliases. Existing direct pointers are incompatible. | 64-bit address reservation is plausible, but 4x physical cost is a poor mobile fit. |

Rounding host mappings to 16 KB is not an alternative: it changes neighboring guest-page state and permissions. Copying or pinning data does not remove the need for checked translation; it is an access API implementation technique.

## Selected direction and prototype boundary

Use **B** as the target architecture: guest VAs remain 32-bit logical values, each 4 KB PTE identifies a backing frame and logical permissions, and APIs validate the complete requested span before access. Physical and write-combined aliases refer to the same backing frames. Decommit/protection changes must synchronize with operations already using a page; APIs must not return unpinned pointers.

The Phase 2C-1 prototype is intentionally isolated from `sdk/runtime`. It demonstrates translation, page-level state, checked scalar/bulk/vector/atomic operations, alias coherence, and small boot-allocation shapes over a 16 KB host page. It does not convert the runtime, prove generated-code coverage, or establish game boot readiness. The existing runtime backend remains the fallback until full access mediation is implemented and audited.

## Prototype evidence and limits

The prototype has 64 logical 4 KB pages backed by a 256 KB temporary file and two `MAP_SHARED` aliases. Each four-page group maps to frames in reverse order, so logical addresses must pass through the page table. Initialization rejects hosts whose page size is not 16 KB. Checked reads, writes, bulk copy/fill, atomic increment, and execute checks validate the complete logical span before access; one mutex serializes operations against decommit and protection changes.

The test suite passed on macOS ARM64 and an ARM64 iOS Simulator with 16 KB host pages. It covers independent commit/decommit of all four guest pages within one host page, zero-filled recommit, per-page logical permissions, invalid and cross-boundary accesses, scalar and bulk operations, overlap-safe copy, a checked 16-byte span around an ARM NEON operation, alias coherence, concurrent increments, failed initialization, cleanup, and allocation-shaped probes.

These are wrapper-level logical permissions. Both host mappings remain read/write, so a raw pointer can bypass checks. `CheckExecute` is not connected to CPU instruction fetch. Permission metadata is shared by guest, physical, and write-combined aliases; alias-specific protection is not modeled. `FetchAdd32` is mutex-serialized only for callers using this API, and does not interoperate atomically with raw host accesses. Decommit clears page contents and state but retains the full 256 KB backing file and mappings, so it does not return individual 4 KB frames to the operating system. REP fault/partial-progress behavior, asynchronous I/O pinning, and all runtime/recompiler conversions remain unimplemented.

The allocation probes reproduce address alignment and access shapes; they do not call the Xbox runtime allocators or instantiate real KPCR/ETHREAD structures. The existing runtime memory-test target still rejects `AllocSystem(0x400, 64)`, an ETHREAD-sized allocation, and an unaligned `0x5000` image commit on the 16 KB host. Its 64 KB-aligned guarded stack probe succeeds. Therefore prototype semantics pass, but runtime boot-allocation readiness remains blocked.
