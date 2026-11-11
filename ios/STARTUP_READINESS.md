# Authentic startup readiness gate (Phase 2D-17)

This is the human-readable form of [`startup_readiness.json`](startup_readiness.json). It records what
the runtime can prove about authentic startup and separates three milestones that must not be collapsed
into one status. Passing synthetic tests does not make an authentic dependency ready.

`PHASE_2D17_CONTROL_FLOW_AND_ADMISSION = PASS`. Authentic execution remains prohibited: Apple
`kExecutionDisabled`, the production `Boot()` guard, the retail SEH/`RtlUnwind` paths, the protection
imports, and the authentic callback manifest (`auth=0`) are unchanged. The frozen Phase 2C-41 route is
still 143 instructions stopping at `0x002B5C7B`.

## Control-flow authorisation (new in 2D-17)

A jump that leaves its function is a **tail transfer**: it enters another generated function without
pushing a return address, and the target's `RET` completes the caller's frame. Checked code now admits
it exactly like a checked CALL instead of calling the host function directly:

| Form | Checked emission | Admission |
|---|---|---|
| `JMP rel8/rel32` inside the function | `goto loc_…` | none needed (same body) |
| `JMP rel8/rel32` leaving the function | `x86_checked_jmp` | session slot, target, depth, generations, run lease |
| `JMP r32` | `x86_checked_register_jmp` | resolved destination is validated, not a slot value |
| `JMP [disp32]` / `JMP [base+index]` (flat) | `x86_checked_indirect_jmp` | slot re-read at admission |
| `Jcc` / `LOOP` leaving the function | `x86_checked_jmp` | same as a direct tail transfer |
| jump-table case | `x86_checked_jmp` per case | same as a direct tail transfer |
| jump-table default | `x86_checked_jmp` with the read value | same as a register tail transfer |
| fallthrough into another function | `x86_checked_jmp` | same as a direct tail transfer |
| `JMP` with FS/segment/16-bit memory operand | checked trap | documented limitation |

Authorised transfers preserve GP registers, flags, ESP and the return-address chain: no return address
is pushed, no `X86CheckedCallFrame` is created, and the checked `RET` in the target validates the
outermost frame. Fail-closed tests cover an unauthorised target, an out-of-code target, a
non-executable destination page, a slot changed during admission, a corrupted return address, the
session depth limit, and a revoked claim. An **interior target that is statically reachable** is
compiled into the admitted function's own body; it is not a new entry, consumes no policy slot, and
remains under the same admission, lease and checked-memory rules (tested).

## Authentic readiness binding (new in 2D-17)

`ValidateAuthenticSystemRoutineAdmission` is production-facing and non-executing. It requires the
reviewed constants for this image and returns **`VALIDATED_BUT_EXECUTION_PROHIBITED`** for a matching
request; anything else gets a specific rejection (`kInvalidImage`, `kInvalidCallSite`,
`kInvalidPolicyIdentity`, or `kRejected` with the precise `SystemRoutineAdmission` reason). The result
type is not accepted by `StartThread`, `CallGuestCallback` or any guest-entry function.

| Constant | Value | Evidence |
|---|---|---|
| Image SHA-1 | `f636a16af04171089f879a4bed646e185c734233` | local Black Edition XBE |
| System routine | `0x002B5C0F` | ordinal-255 argument 10 at the call site |
| Start routine | `0x002B38A2` | same argument block |
| Ordinal-255 call site | `0x002B5CE0` | static disassembly (`call [0x003C9E68]`; the wrapper starts at `0x002B5CA7`) |
| Argument count | 10 (stdcall cleanup 40) | pushes `0x2B5CB9..0x2B5CDF` |
| TLS block | `0x94` (`align16(0x90)+4`, index `-37`) | `0x002B3916`, `0x004DF78C` |
| Callback-policy identity | `7eec0a102a0485f4aa6e2036aece1aa00381fd99` | SHA-1 of `ios/recompiler-fixtures/callback_policy_manifest.txt`, re-checked by the integrated preflight |

The result also carries the unresolved flags for this boundary: `KPRCB+0x250`, retail SEH, the
initializer chain, and the physical-window global.

## `KPRCB+0x250` conclusion: **CONDITIONALLY_SAFE**

Boundary: the start routine `0x002B38A2` prologue through the reviewed pre-start list walk, including
the system routine `0x002B5C0F` prologue and TLS fill, with the field zero.

Statically verified read sites on that boundary:

| PC | Instruction | Null path |
|---|---|---|
| `0x002B38B2` | `mov eax, [eax+0x250]` | `test eax,eax` / `jz 0x002B38C1` → `xor ecx,ecx` → `test ecx,ecx` / `jz 0x002B38EE`; the only dependent dereferences (`[eax+0x24]`, `[ecx]`, `[ecx+4]`) are skipped |
| `0x002B596D` | `cmp dword [eax+0x250], 0` | `jnz` returns; null falls to `0x002B5854` |
| `0x002B5BD9` | `mov eax, [eax+0x250]` | `jz 0x002B5BEC` → `xor eax,eax` → `jz 0x002B5C0A` → `xor eax,eax; ret 4`; `[eax+0x14]` is skipped |

Preconditions, and why this is not `VERIFIED_NULL_PATH_SAFE`:

1. The field must stay zero. No writer exists in the image and the runtime leaves it zero.
2. The `0x002B596D` null branch is reached from the start routine's first call (`0x002B38A2` →
   `0x002B58B8` → `0x002B5967`) and enters the hardware-info probe at `0x002B5854`, which reads
   `[0x8001003C]`. This runtime keeps that global zero, so the probe derives `0x80010000`, scans for the
   `"INIT"` signature at `0x8000FFF0` and returns without a match. It is memory-safe only while
   `[0x8001003C]` stays zero (or points into the mapped physical window with no matching signature).
   This corrects the earlier "not on the startup path" note for this boundary.
3. The reads at `0x002B83F6`, `0x0037927B` and `0x0039CD5F` are outside the boundary and remain
   unresolved.
4. The static-initializer walkers reached at `0x002B38EE`/`0x002B38F3` are a separate dependency (the
   initializer chain), not part of this conclusion.

No fake object and no non-null value is set.

## Gate A — authentic preparation: **GO**

Prepare and inspect the authentic image and the required guest-thread structures without executing a
single guest instruction.

- Evidence: the real image parses and prepares through `xbe::PrepareImage` in all three preflights; the
  image identity is bound at boot and comparable; a prepared system-routine request with the authentic
  addresses, TLS size, KPCR/ETHREAD layout, generations, stack and executable pages returns
  `VALIDATED_BUT_EXECUTION_PROHIBITED`; no worker starts and the thread stays prepared.
- Blocking conditions: none for preparation.
- Not included: any guest instruction, any initializer, any authentic callback.

## Gate B — bounded authentic execution beyond the frozen route: **NO-GO**

Proposed smallest boundary: the ordinal-255 child activation → system routine `0x002B5C0F` (SEH
prologue, TLS fill) → the start-routine call `0x002B5C7E` → the first instruction of `0x002B38A2`, then
**stop before the first call inside the start routine**. This boundary needs neither the 867
initializers nor the dynamic list walk.

Blocking conditions:

1. **Retail SEH** — `0x002B5C16` calls the authentic prologue `0x00319804`; the handler and epilogue
   contracts are not validated and 302/312 remain fail-closed.
2. **Claim-bound authentic admission** — the production ordinal-255 path still cannot bind
   `0x002B5C0F` to a validated claim; only the test-only synthetic bridge does.
3. **`KPRCB+0x250` and the physical-window probe** — accepted only under the preconditions above; the
   start routine's first call enters the probe, so Gate B must either stop before it or accept the
   physical-window dependency explicitly.
4. **Instruction coverage** — the frozen 143 instructions do not cover the start routine or the system
   routine's return/termination paths.

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

## Dependency matrix

| Dependency | Evidence | Implementation | Validation | State |
|---|---|---|---|---|
| Tail-transfer authorisation | static x86 forms + recompiler emission | `x86_checked_jmp`/`_register_jmp`/`_indirect_jmp`, session admission, run lease | generated success/fail-closed tests, Debug+Release | implemented, synthetically validated |
| Executable code pages | `AcquireExecutableRun`, per-thread coverage | every admission path | memory tests + cross-page/nested/TOCTOU fixtures | implemented, synthetically validated |
| Authentic admission binding | static call site `0x002B5CE0`, TLS `0x94`, manifest SHA-1 | `ValidateAuthenticSystemRoutineAdmission` | prepared authentic-address request, 12 negative cases, preflight identity check | implemented, non-executing |
| `KPRCB+0x250` | six reads; three on the boundary; no writer | stays zero; null branches taken | static disassembly + guarded generated access | conditionally safe |
| `[0x8001003C]` physical window | read on the null branch at `0x002B5854` | stays zero | static only | unresolved, on the Gate B path |
| Retail SEH 302/312 | prologue `0x00319804`, handler `0x00319858` | fail-closed | none | prohibited |
| 867 initializer targets | 877 slots, 838 verified, 5 inferred, 34 prohibited, `auth=0` | static policy only | policy validator | static-only |
| Ordinal 258 | `ExitThread` marks and throws | production bridge unchanged | synthetic child exit | partial |
| Remaining instructions/traps | frozen route 143/143 | checked recompiler | integrated preflight | partial |
| Protection 179/182/204 | 2D-11 ABI ledger | fail-closed | none | fail-closed |

Do not authorise authentic execution from this document. Gate A is a preparation statement only; Gates
B and C require a separately authorised package that resolves their blockers first.
