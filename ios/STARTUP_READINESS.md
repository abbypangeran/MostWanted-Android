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
