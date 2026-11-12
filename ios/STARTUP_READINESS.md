# Authentic startup readiness gate (Phase 2D-21)

This is the human-readable form of [`startup_readiness.json`](startup_readiness.json). The verified
144-instruction authentic route is a historical PASS. The new 186-instruction start-routine extension is
**statically closed and synthetically validated only**; no authentic instruction was executed in this phase.

`PHASE_2D21_START_ROUTINE_READINESS = PASS`.

## Historical reference (2D-20)

The accepted authentic run retired 144 instructions: parent 71 before `0x002B3987`, child 73 before
`0x002B38A2`, with kernels 255/187/277/294, live SEH/TLS and clean accounting. Its evidence is preserved in
`.local/ios-port/phase2d19-authentic-run/` and `.local/ios-port/phase2d20-second-authentic-run/`.

| Historical identity | Value |
|---|---|
| Mapping | `EC3486BCAFA498BF` (152 entries) |
| Parent manifest | `7F8AD72F529DC906` |
| Child manifest | `DBCFD15B661E8CE6` |

These identities are **historical only**. Do not reuse them after the route change.

## New extension boundary

The child now continues through `0x002B38A2`'s first call and the hardware/version selector:

| Item | Value |
|---|---|
| Parent | entry `002B3916`, 71 instructions, pre-instruction stop `002B3987` |
| Child | entry `002B5C0F`, 115 instructions (73 + 42), pre-instruction stop `002B38A7` |
| Total | 186 executed / 188 boundary steps |
| Child start routine | `002B38A2`; first call `002B38A2 -> 002B58B8`; return `002B58B7 -> 002B38A7` |
| New mapping | `07AEBFC052075330` (200 entries) |
| New parent manifest | `48120EDC4D3D4466` |
| New child manifest | `322795BF56BE60B6` |

The parent route and its 71 instructions are unchanged. Its identity changed because `RouteManifest.start_routine`
is now part of the reviewed identity; the child's start routine is `002B38A2` while its stop is `002B38A7`.

The reviewed build-5838 path reads `XboxHardwareInfo`/`XboxKrnlVersion`, takes the `JNLE` at `002B58F6` to
`002B5967`, reads `KPRCB+0x250`, takes the null branch to the `002B5854` probe, reads the physical window,
finds no `INIT` signature, executes `leave`/`ret`, and reaches the pre-instruction stop `002B38A7`. The
second start-routine call and all later dependencies are excluded.

## KPRCB and physical-window findings

| Dependency | Finding | State |
|---|---|---|
| `FS:[0x20]` | `KPCR.Prcb` (`KPCR+0x28`) | verified layout |
| `[eax+0x250]` | `KPRCB+0x250` = `KPCR+0x278` | zero; null branch taken |
| `[0x8001003C]` | first physical MB, committed, not initialised by `InitializeKernelData` | zero |
| derived INIT address | `0x8000FFF0` with `[0x8001003C]=0` | zero; no `INIT` |
| physical aliases | `0x80000000` and `0xF0000000` share the checked backing store | verified synthetically |
| `XboxHardwareInfo` | production flags `0x20` | registered through `PrepareAuthenticKernelData` |
| `XboxKrnlVersion` | production build 5838 | registered through `PrepareAuthenticKernelData` |

`PrepareAuthenticKernelData` uses the production `InitializeKernelData` values and registers the data at
`0x80001000+`. The future run driver must patch the image's data import slots `003C9E34`/`003C9E3C` to the
returned hardware/version addresses. The static preflight checks those slots still contain `0x80000142`/`0x80000144`.

A non-null `KPRCB+0x250`, an `INIT` signature, a non-zero `[0x8001003C]` that resolves to a matching signature,
every other kernel build, and all unreviewed branch targets fail closed. No kernel object, physical value or
version is fabricated.

## CLI/STI and privileged instructions

The build-5838 path skips the protection block. `CLI` at `002B5957` and `STI` at `002B595E` remain checked
traps and were not converted to NOPs; synthetic tests assert `kUnsupportedInstruction`. `INT3` at `002B5CA6`
and every unsupported opcode remain trapped. The same fail-closed rule applies to exception-generating forms.
The proposed boundary stops before any of them.

## Validation

| Check | Debug | Release |
|---|---:|---:|
| `authentic_run_tests` | 95 PASS / 0 FAIL | 95 PASS / 0 FAIL |
| Checked recompiler fixtures | 812 PASS / 0 FAIL | 812 PASS / 0 FAIL |
| Safe CTests (`-E execution_probe`) | 5/5 | 5/5 |
| `authentic_run_preflight --generated` | PASS, 200/200 | PASS, 200/200 |
| `authentic_run_accounting` | PASS, 1280 -> 0 | PASS, 1280 -> 0 |
| `gate_b_preflight` (historical 145) | PASS, execution prohibited | PASS |
| Root / secondary / integrated static preflights | PASS; integrated 143/143 | PASS |
| Android ARM64 harness syntax (`authentic_run.cpp`) | PASS | — |

The new preflight reports the mapping/parent/child identities above, child 115, total 186, boundary 188,
child stop `002B38A7`, data import slots 322/324 and `AUTH_RUN_GENERATED_CHECK=PASS`. The synthetic suite
covers the extended child stop with live SEH/TLS, the 5838 version branch, the physical values and
`0xF0000000` alias, the KPRCB null/non-null cases, the `CLI`/`STI` traps, and the production kernel-data
flags/build.

## Gates

| Gate | Result | Scope |
|---|---|---|
| A — authentic preparation | **GO** | image, kernel data, mapping, manifests and assessment |
| B — the new 186-instruction boundary | **static/synthetic ready; execution NO-GO** | separate authorisation, kernel-data/import preparation and observed KPRCB/physical preconditions required |
| C — authentic startup | **NO-GO** | all later start-routine calls, 867 initializers, retail SEH 302/312, ordinal 258, dynamic list callback, `001C8C20`, CLI/STI and protection |

Gate B is not granted by this document. The old 144 boundary remains the verified regression reference; the
new 186 boundary is a static/synthetic extension.

## Proposed third run (not authorised)

Exactly the 186-instruction boundary above, using mapping `07AEBFC052075330` and manifests
`48120EDC4D3D4466`/`322795BF56BE60B6`. The driver must:

1. initialise the reviewed kernel data and patch data import slots `003C9E34`/`003C9E3C`;
2. retain the four reviewed kernel import patches and the parent/child claim model;
3. observe `KPRCB+0x250 == 0`, `[0x8001003C] == 0` and no `INIT` signature before the probe;
4. reach both stops `002B3987` and `002B38A7` with 71/115 instructions;
5. observe kernels 255/187/277/294 and the live 14-predicate SEH/TLS state;
6. stop before `002B38A7`'s second call and reject every unreviewed branch;
7. return accounting to `virtual=0` after shutdown and finalise all workers/claims/leases.

No later start-routine call, initializer, callback, retail SEH delivery, ordinal 258 or CLI/STI execution is
included. A separate explicit authorisation is required.

## Remaining blockers

- The third-run activation/driver and its exact scoped preparation are not committed.
- The full start routine (`002B5664`, `002B582B`, `002B57D3`, `001C8C20`, `002B55EE`) and its callbacks remain unreviewed.
- The 867 live initializer targets and the dynamic list callback remain Gate C.
- Retail SEH `RtlRaiseException`/`RtlUnwind` and handler delivery remain prohibited.
- Authentic ordinal 258 termination and the system-routine post-start path remain unreviewed.
- Protection 179/182/204, `CLI`/`STI`, `INT3` and the `001C8C20` fan-out remain fail-closed outside the boundary.

Do not authorise authentic execution from this document. The 144 boundary is historical; the 186 boundary is
static/synthetic only until a separately approved run.


## Phase 2D-24 — checked DPC/timer kernel objects

`PHASE_2D24_CHECKED_KERNEL_OBJECTS = PARTIAL`.

`KeInitializeDpc` (107) and `KeInitializeTimerEx` (113) are now implemented through checked guest memory in
production (`sdk/runtime/kernel/kernel_objects.{h,cpp}`): full `0x1C`/`0x28` span validation and pinning,
reviewed field publication, invalid/uncommitted/read-only rejection, and an `x86_trap` failure path. No DPC
callback or timer scheduling is invoked. The authenticated 186-instruction third-run PASS remains historical.

`HalRegisterShutdownNotification` (47) is **not implemented**. The nxdk ABI/layout is authoritative
(`PHAL_SHUTDOWN_REGISTRATION`, `NotificationRoutine +0`, `Priority +4`, `ListEntry +8`, void return), but the
kernel list head, ordering/lifetime, duplicate/removal and shutdown consumption remain unresolved. The no-op
is retained rather than replaced by a speculative list.

The 210-instruction boundary is **not generated**: ordinal 47 must be established first. The current mapping
`07AEBFC052075330`, parent `48120EDC4D3D4466` and child `322795BF56BE60B6` remain the historical 186-route
identities and must not be reused after a route change.

Validation: `mostwanted_memory_tests` 439 PASS / 0 FAIL including the new checked DPC/timer tests; authentic
suite 95/0; checked fixtures 812/0; authentic preflight PASS 200/200; Android ARM64 `kernel_objects.cpp`
syntax PASS. Gate A remains GO; the historical Gate B 186 is PASS; the proposed 210 boundary is NO-GO;
Gate C remains NO-GO. A fourth authentic run is not technically justified yet.


## Phase 2D-25 — shutdown registration

`PHASE_2D25_SHUTDOWN_REGISTRATION = PARTIAL`.

Ordinal-47 evidence: nxdk void ABI with two stdcall arguments; `HAL_SHUTDOWN_REGISTRATION` layout
`NotificationRoutine +0`, `Priority +4`, `ListEntry +8`; local authentic object `0x0046CD30` contains
`002B8D6D`, `80000000`, `{0,0}`; the call at `0x002B567B` registers with action `1`. No authoritative
evidence was found for the kernel-private list head, ordering, duplicate/removal or shutdown consumption,
and Cxbx has no implementation. The no-op is retained; no speculative list or callback was written. The
210-instruction boundary remains ungenerated and NO-GO. A fourth authentic run is not justified.


## Phase 2D-26 — scoped shutdown registration

`PHASE_2D26_SCOPED_REGISTRATION = PARTIAL`.

The reviewed extension from `002B38A7` to the proposed `002B5E37` stop does not depend on the unknown
shutdown-list semantics: `0046CD30` is only passed to ordinal 47; the object is not read again, its
`ListEntry` is not consulted, and no callback, removal or ordering effect is used. A checked host-owned
registration record is implemented and tested (`RegisterShutdownNotificationChecked`): full `0x10` span
validation, callback range, raw priority/object/owner/generation/image identity, duplicate rejection,
unsupported-removal rejection, no callback execution and explicit cleanup. The xboxkrnl HLE traps on
failure. SCOPED_STARTUP_COMPATIBILITY is established; FULL_KERNEL_SHUTDOWN_FIDELITY remains unresolved.
The 210-instruction route was not generated; the 186-route identities remain historical. Gate A remains
GO, the historical Gate B 186 PASS remains, the 210 boundary is NO-GO and Gate C remains NO-GO.


## Phase 2D-27 — 211-instruction boundary

`PHASE_2D27_211_BOUNDARY_READINESS = PASS` (offline/static/synthetic).

The 25-instruction extension is parent 71 / child 140 / total 211 / 213 boundary steps; the `002B5681`
call is retained. The new mapping is `E9DFEF8BB5AC0970` (225 entries), parent `48120EDC4D3D4466`, child
`2FA1FDFDA0F3F86B`; the 186-route identities are historical only. Ordinals 47/107/113 use the production
checked services; the scoped registration record clears on `mem::Shutdown`. The preflight, Debug/Release
authentic tests, checked fixtures, CTests, accounting and memory/registration suite pass. Gate A GO; the
211 boundary is static/synthetic ready for a separately authorised run; Gate C remains NO-GO. A fourth
authentic run is technically justified only for exactly this 211/213 boundary and with the explicit
`SCOPED_STARTUP_COMPATIBILITY` / `FULL_KERNEL_SHUTDOWN_FIDELITY=unresolved` caveat.

## Phase 2D-27B — transfer-budget remediation

`PHASE_2D27B_TRANSFER_BUDGET_REMEDIATION = PASS`.

The fourth run stopped early because the child's per-worker transfer budget was 128. The reviewed route
needs parent 73 / child 143 transfers including the terminal stop; the parent stops before `002B3987` and
the child before `002B5E37`. `ComputeRequiredTransfers` and `RouteManifest.required_transfers` bind the
requirement into the fingerprint; authorisation and the static preflight reject the child budget 128 before
execution, and the gate counts the terminal stop. The new identities are parent `85527B8FA581229F`, child
`585C5E1A742B6924`; mapping `E9DFEF8BB5AC0970` is unchanged.

A generated synthetic mirror retires 211 instructions in 213 boundary steps (parent 71/73, child 140/143)
and reaches the production checked implementations of 47/107/113. Exact KDPC/KTIMER/shutdown-registration
state, the live 14-predicate SEH frame, TLS, the parent handle close, worker finalisation and the
allocation baseline all pass. Negative budget, loop, unknown-transfer and post-stop cases fail closed.

Fresh Debug/Release: authentic tests 144/0, checked fixtures 812/0, safe CTests 5/5, preflight PASS 225/225,
accounting 1280 -> 0, memory tests 454/0, Android ARM64 syntax PASS.

Gate A GO; Gate B is static/synthetic ready and execution NO-GO; Gate C NO-GO. A single fifth authentic
execution of the unchanged 211/213 route is technically justified only with a separate explicit
authorisation and the 47/107/113 kernel-slot preparation; the temporary activation/driver is not committed
by this phase and must use the 73/143 floors. It is not Gate C. Do not authorise it from this document.

## Phase 2D-27C — TLS evidence lifecycle remediation

`FIFTH_RUN_TLS_REMEDIATION = PASS` (offline only).

The historical `FIFTH_AUTHENTIC_RUN = FAIL` is preserved. The fifth run completed 211/211 instructions,
reached both stops (`002B3987`, `002B5E37`), completed seven reviewed kernel calls, and passed the
stop-time TLS verification, but the driver re-read released TLS memory after child-worker finalisation,
reported `pointer=00000000`/`read_only=0`, and hashed the unread zero buffer (`9D899E55`). The driver exited
4. There was no retry.

The corrected harness captures TLS evidence at the stop while the allocation is live: address,
expected/self pointer, pointer and full-block checked-read status, block size and FNV-1a hash. The hash is
recorded only after a successful full read; `TlsEvidenceStatus` distinguishes not-evaluated, valid,
unavailable, read-failed and mismatch. `RunEvidence.tls_evidence` is host-owned and survives finalisation.
The local corrected driver consumes that snapshot and never re-reads TLS guest memory after release; it was
compiled in both builds and its prepare-only admission passed, but was not executed authentically.

Fresh Debug/Release: `authentic_run_tests` 152/0 (8 new TLS lifecycle checks), checked fixtures 812/0,
safe CTests 5/5, preflight PASS 225/225, accounting 1280 -> 0, memory tests 454/0, Android ARM64 syntax PASS.
The full synthetic 71/140 route still retires 211 instructions in 213 boundary steps with unchanged
instruction/transfer counts and kernel/SEH/stop conditions.

Identities are unchanged: mapping `E9DFEF8BB5AC0970`, parent `85527B8FA581229F`, child
`585C5E1A742B6924`. The fifth run remains closed as FAIL. No sixth run, authentic retry or Gate C
activation is authorised by this phase.

## Phase 2D-28 — startup continuation to `0x002B5E5F`

`PHASE_2D28_STARTUP_CONTINUATION = PASS` (offline/static/synthetic).

The reviewed route now continues from the former child stop `0x002B5E37` through 13 instructions
(`002B5E37..002B5E5D`) and stops before `call 0x002B39B9` at **`0x002B5E5F`**. The helper's remaining
device query enters `ExQueryNonVolatileSetting` (24) via the `002B95AC` tail jump and `RtlNtStatusToDosError`
(301); the following `002B5E7F` call schedules the KTIMER with `KeSetTimer` (149) and the `002B5E00` DPC,
which is a prohibited authentic callback. The boundary therefore stops before those dependencies.

| Item | Value |
|---|---|
| Parent | 71 instructions / 73 transfers (unchanged) |
| Child | 153 instructions / 156 transfers |
| Total | 224 instructions / 226 boundary steps |
| Child stop | `0x002B5E5F` (pre-instruction) |
| Kernels | parent 255,187; child 277,294,47,107,113 |
| Parent manifest | `85527B8FA581229F` |
| Child manifest | `3440CAC0FC3A57FB` |
| Mapping | `94166820FBB48A28` (242 entries) |

Required runtime support: checked `test r/m8, imm8` for `002B5E4A` in `sdk/tools/recomp/codegen.cpp`
(checked guest-memory read, exact CF/OF/ZF/SF/PF). No new kernel export was added.

The synthetic mirror retires 224 instructions in 226 boundary steps, checks the new flag clears, the
existing kernel/SEH/TLS/stop conditions, the `AcceptFullRoute` acceptance contract, and all negative
budget/identity/terminal/loop/unknown-transfer/unsupported/memory/TLS-lifecycle cases.

Fresh Debug/Release: authentic tests 160/0, checked fixtures 812/0, safe CTests 5/5, preflight PASS 242/242,
accounting 1280 -> 0, memory tests 454/0, Android ARM64 syntax PASS.

The historical `FIFTH_AUTHENTIC_RUN = FAIL` (211/211 completed, both stops, post-finalisation TLS evidence
failure) is preserved and not converted. Gate A GO; Gate B for the new 224 boundary is static/synthetic
READY with execution NO-GO pending a new driver/admission and separate authorisation; Gate C remains
NO-GO. No authentic execution or sixth run was performed.

## Phase 2D-28B — sixth-run preparation

`PHASE_2D28B_SIXTH_RUN_PREPARATION = PASS` (offline preparation only).

A reproducible local driver (never committed) was built off and on. The reversible activation patch adds
`AUTHENTIC_RUN_SIXTH_AUTHORISED` (default 0) and `authentic_run_sixth_exec`; it was restored and
`git apply --check` passes. The sixth admission agrees with the 2D-28 preflight (`242/242`) and the compiled
route: mapping `94166820FBB48A28` (242), parent `85527B8FA581229F` (71/73), child `3440CAC0FC3A57FB`
(153/156), stop `002B5E5F`, kernels 2/5. The off build refuses; the on build's prepare-only admission
passes with cleanup and accounting, and every negative (missing/wrong token, missing explicit
authorisation, off build, stale fifth admission, tampered instructions/transfers/stop) fails closed without
starting a worker.

The synthetic mirror checks the exact five-argument continuation stack, `eax = ebp-0x0C`, stop
`esp`/`ebp`/`eax`, zero timer flags, the not-taken `002B5E4D` conditional (never `002B5E7F`), the absence of
ordinals 24/301/149, and a `0x28` taken-branch negative that fails with `kForbiddenBranch` without entering
the alternate target. Route completion remains separate from driver acceptance. No authentic instruction
ran, the fifth run was not retried, no sixth run was started, and the historical evidence is unchanged.

Fresh Debug/Release: authentic tests 160/0, checked fixtures 812/0, safe CTests 5/5, preflight PASS 242/242,
accounting 1280 -> 0, memory tests 454/0, Android ARM64 syntax PASS. Gate A GO; Gate B is preparation
READY with sixth execution NOT AUTHORISED; Gate C remains NO-GO.
