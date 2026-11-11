# Authentic startup readiness gate (Phase 2D-18)

This is the human-readable form of [`startup_readiness.json`](startup_readiness.json). It records what
the runtime can prove about authentic startup and separates three milestones that must not be collapsed
into one status. Passing synthetic tests does not make an authentic dependency ready.

`PHASE_2D18_GATE_B_PREPARATION = PASS` (gate result unchanged: Gate A **GO**, Gates B and C
**NO-GO**). Authentic execution remains prohibited: Apple `kExecutionDisabled`, the production `Boot()`
guard, the retail SEH/`RtlUnwind` paths, the protection imports, and the authentic callback manifest
(`auth=0`) are unchanged. The frozen Phase 2C-41 route is still 143 instructions stopping at
`0x002B5C7B`.

## The exact Gate B boundary

`0x002B38A2`'s **first instruction** is `call 0x002B58B8`. A boundary that stops "before the start
routine's first internal CALL" therefore executes **zero** instructions of the start routine. The
proposed boundary is an *entry* boundary, not a first portion:

| Segment | Executed | Detail |
|---|---:|---|
| Frozen root route | 72 | CRT entry `0x002B3916` through the ordinal-255 call `0x002B5CE0` |
| System routine `0x002B5C0F` | 35 | prologue, TLS fill, both argument pushes, the pre-start call |
| SEH prologue `0x00319804` | 19 | registration and return to `0x002B5C1B` |
| Pre-start walk `0x002B597D` | 17 | empty-list path; imports 277/294 |
| `0x002B5C7B` `push [ebp+0x0C]` | 1 | first argument to the start routine |
| `0x002B5C7E` `call [ebp+0x08]` | 1 | dynamic start-routine call, target `0x002B38A2` |
| `0x002B38A2` | 0 | **stop pre-instruction** |
| **Total with the frozen root** | **145** | 73 beyond the frozen secondary segment |

The boundary excludes everything that begins at or after `0x002B38A2`:

- `0x002B58B8` (hardware/version selector) and the `0x002B5854` `[0x8001003C]` probe;
- every `KPRCB+0x250` read (`0x002B38B2`, `0x002B596D`, `0x002B5BD9`);
- the static initializer walkers (`0x002B57D3`, `0x002B582B`) and all 867 live initializer targets;
- the dynamic list callback `0x0032062A` and ordinal-258 termination.

`KPRCB+0x250` is therefore **not on the Gate B path**. The Phase 2D-17 CONDITIONALLY_SAFE conclusion
still applies to the wider boundary that includes the start routine from `0x002B38B2` onwards; it is not
needed for the entry boundary.

## Gate B static preflight (`gate_b_preflight`)

A new static-only executable decodes the authentic image and checks, without executing any guest
instruction or starting any worker:

| Item | Value |
|---|---|
| Image identity | SHA-1 `f636a16af04171089f879a4bed646e185c734233` |
| Title / entry / base | `4541007B`, `0x002B3916`, `0x00010000` |
| Route text | 79/79 reviewed instructions match bytes and lengths |
| Supported instructions | 73 (executed route); 1 forbidden dynamic list callback, 3 empty-list-unreachable instructions |
| Kernel imports | ordinal 277 at `0x003C9E44`, ordinal 294 at `0x003C9E40` (`0x80000115`/`0x80000126`) |
| Out-of-boundary imports | ordinal 255 at `0x003C9E68`, ordinal 258 at `0x003C9E64` |
| TLS data | copy 0, zero-fill `0x90`, directory-derived block `0x94` |
| List and scope data | list head `0x0046C8CC` is its own sentinel; scope table `0x003ED5B8` starts `0xFFFFFFFF` |
| SEH | prologue `0x00319804` registers `{previous, 0x00319858}`; handler is stored only |
| Stop | `0x002B38A2`, pre-instruction |
| Execution | **PROHIBITED** |

The tool prints `GATE_B_*` key/value lines or `--json`; it exits 0 only when every static check passes.
It does not link the generated authentic code, so it cannot execute it.

## Authentic ordinal-255 assessment (production, non-executing)

`AssessAuthenticSystemThreadRequest` is compiled in production (`sdk/runtime/kernel/threads.cpp`) and
takes the raw ten-argument request. It validates the reviewed call site, ABI, routines, TLS block and
policy identity, prepares a candidate exactly as the export would, calls the **same** core validator
(`ValidateAuthenticSystemRoutineAdmission`), cancels the candidate and returns evidence. A matching
request yields `VALIDATED_BUT_EXECUTION_PROHIBITED`; anything else yields a specific status. It never
creates a callback ticket or a synthetic claim, and `StartThread`/`CallGuestCallback` cannot accept its
result (checked by `static_assert` in the fixtures). The production export is intentionally **not**
wired to start from this result; doing so is a Gate B authorisation decision, not a validation one.

## SEH prologue contract `0x00319804`

The full decode and frame layout are in `ios/PORT_STATUS.md` (Phase 2C-32) and
`sdk/runtime/kernel/seh_prologue.h`. With `E` = ESP at the `call` into the prologue:

| Address | Content |
|---|---|
| `E+0x04` | caller EBP (the frame-size argument slot) |
| `E+0x00` | scope flag, `-1` while live (the scope-table argument slot) |
| `E-0x04` | scope table after return (the helper's return address before) |
| `E-0x08` | handler `0x00319858` |
| `E-0x0C` | previous `fs:[0]`, and the registration the helper installs |
| `E-0x14` | saved post-prologue ESP (`E-0x30` for the `0x18` frame) |
| after `ret` | `EBP = E+4`, `ESP = E-0x30`; the caller's own return address is untouched |

`00319804` consumes only its own return address, so `002B5C0F` *could* return, but it terminates the
thread at `0x002B5CA0` instead. `002B5BD3` is a different routine on the exception path, not an SEH
epilogue, and it reads `KPRCB+0x250`.

The checked representation (`VerifyRegistration`, `VerifyNormalReturn`) validates the live record, the
scope flag/table, saved ESP, caller EBP, the chain position (head or nested) and a clean unlink. It is
exercised by generated synthetic guest code that builds this exact frame, nests a second record, probes
a foreign FS base and corrupted record, unlinks and returns. The handler is resolved but never called.

## `KPRCB+0x250` conclusion: **not_on_path for Gate B; CONDITIONALLY_SAFE beyond it**

The field is zero in the prepared layout and no writer exists in the image. The reviewed read sites and
their null paths remain as recorded in Phase 2D-17 (`0x002B38B2`, `0x002B596D`, `0x002B5BD9`); all are
after the Gate B stop. If a later boundary enters `0x002B38A2`, the `0x002B596D` null branch reaches the
`0x002B5854` hardware probe and re-introduces the `[0x8001003C]` precondition. No fake object and no
non-null value is set.

## Gate A — authentic preparation: **GO**

Prepare and inspect the authentic image and the required guest-thread structures without executing a
single guest instruction.

- Evidence: all three preflights parse and prepare the real image; identity is bound and compared; a
  prepared request with the authentic addresses, TLS size, KPCR/ETHREAD layout, generations, stack and
  executable pages returns `VALIDATED_BUT_EXECUTION_PROHIBITED`; the new production assessment reaches
  the same result by preparing and cancelling its own candidate (the integrated preflight runs it on the
  real argument block and prints `validated-but-execution-prohibited`); no worker starts and the thread
  stays prepared or is released.
- Blocking conditions: none for preparation.
- Not included: any guest instruction, any initializer, any authentic callback.

## Gate B — bounded authentic execution beyond the frozen route: **NO-GO**

Scope: the 145-instruction entry boundary above.

| Blocker | State after 2D-18 |
|---|---|
| Retail SEH exception delivery | Prologue registration and normal return are statically represented and synthetically tested. Exception dispatch (`0x00319858`) and `RtlRaiseException`/`RtlUnwind` remain prohibited and unvalidated. If nothing raises before the stop, delivery is not needed — but that is a runtime property, not a static one. |
| Claim-bound authentic execution authorisation | Still absent by design. The assessment returns evidence only; there is no ticket/claim that a worker could use to enter `0x002B5C0F`. |
| Route coverage | The frozen preflight stops at `0x002B5C7B`; the 2 instructions `0x002B5C7B`/`0x002B5C7E` and the arrival at `0x002B38A2` are statically checked but not part of any execution route. |
| Dynamic start-routine call | `0x002B5C7E call [ebp+0x08]` needs a checked indirect admission whose target is the prepared thread's start routine. |
| Kernel imports on the boundary | 277/294 are implemented; they must be bound and observed on the authentic path if it ever runs. |
| `KPRCB+0x250` / physical window | Not on this boundary. They become blockers at the next instruction (`0x002B38A2`'s first call). |

## Gate C — authentic startup towards the game loop: **NO-GO**

Everything in Gate B plus:

- the persistent authentic authorisation of the 867 live initializer targets (table A/C/D/B walkers at
  `0x002B57D3`/`0x002B582B` called from `0x002B38F3`/`0x002B38EE`);
- `CLI`/`STI` at `0x002B5957`/`0x002B595E` and the version-selected hardware path in `0x002B58B8`;
- the dynamic list callback `0x0032062A` and its registration;
- retail SEH 302/312, ordinal 258 on the authentic path, and the frame-sensitive `0x00316DA8`;
- kernel-import compatibility (including the fail-closed protection imports) and the remaining
  unsupported instructions;
- multiple authentic guest workers and scheduling.

`0x002B5C0F` never returns normally: after the start routine it terminates the thread through ordinal
258. The CRT's thread entry `0x002B3916` therefore relies on thread termination, not on a system-routine
return. Any later boundary must preserve that.

## Dependency matrix

| Dependency | Evidence | Implementation | Validation | State |
|---|---|---|---|---|
| Tail-transfer authorisation | static x86 forms + recompiler emission | `x86_checked_jmp`/`_register_jmp`/`_indirect_jmp`, session admission, run lease | generated success/fail-closed tests, Debug+Release | implemented, synthetically validated |
| Executable code pages | `AcquireExecutableRun`, per-thread coverage | every admission path | memory tests + cross-page/nested/TOCTOU fixtures | implemented, synthetically validated |
| Authentic admission binding | static call site `0x002B5CE0`, TLS `0x94`, manifest SHA-1 | `ValidateAuthenticSystemRoutineAdmission` + `AssessAuthenticSystemThreadRequest` | prepared request, production assessment, 10 assessment negatives plus the existing binding negatives, preflight identity check | implemented, non-executing |
| Gate B boundary preflight | 79 decoded instructions, 73 executed | `gate_b_preflight` (static only) | image, route text, data, imports, stop | machine-checkable, PASS |
| SEH prologue `0x00319804` | complete decode; frame at `E-0x0C` | `VerifyRegistration` / `VerifyNormalReturn` | generated synthetic registration, nesting, unlink, negatives | implemented, synthetically validated (normal path only) |
| `KPRCB+0x250` | six reads; none in Gate B; no writer | stays zero; null branches taken | static disassembly + guarded generated access | not on the Gate B path |
| `[0x8001003C]` physical window | read only after `0x002B38A2` | stays zero | static only | unresolved, outside Gate B |
| Retail SEH 302/312 | prologue, handler `0x00319858` | fail-closed | none | prohibited |
| 867 initializer targets | 877 slots, 838 verified, 5 inferred, 34 prohibited, `auth=0` | static policy only | policy validator | static-only |
| Ordinal 258 | `ExitThread` marks and throws | production bridge unchanged | synthetic child exit | partial |
| Remaining instructions/traps | frozen route 143/143 + Gate B 73/73 | checked recompiler | integrated preflight + `gate_b_preflight` | partial |
| Protection 179/182/204 | 2D-11 ABI ledger | fail-closed | none | fail-closed |

Do not authorise authentic execution from this document. Gate A is a preparation statement only; Gates
B and C require a separately authorised package that resolves their blockers first.
