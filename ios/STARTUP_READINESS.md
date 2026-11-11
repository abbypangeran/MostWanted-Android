# Authentic startup readiness gate (Phase 2D-16)

This is the human-readable form of [`startup_readiness.json`](startup_readiness.json). It records what
the runtime can now prove about a *synthetic* startup and what still separates it from a separately
authorised, controlled authentic single-worker experiment. Passing synthetic tests does not make an
authentic dependency ready; the evidence column names the source each line rests on.

`PHASE_2D16_SYSTEM_ROUTINE_ADMISSION = PASS`. Authentic execution remains prohibited:
Apple `kExecutionDisabled`, the production `Boot()` guard and the frozen Phase 2C-41 route are
unchanged.

| Dependency | Evidence | Implementation | Validation | Blocker |
|---|---|---|---|---|
| Authentic ordinal-255 ABI | Static call site `002B5C87`–`002B5CA7`: 10 dword arguments, stdcall cleanup 40, TLS size from `[0x004EDC74]`, system routine `002B5C0F` as argument 10 (Phase 2D-14) | Production `PsCreateSystemThreadEx` unchanged; test-only bridge builds the same layout | Generated synthetic parent through the checked bridge | Production path must stay fail-closed until a claim-bound admission route exists |
| System-routine entry contract | `002B5C0F` pushes `0x18`/`0x3ED5B8`, calls the SEH prologue `00319804`, fills TLS (`002B5C28`), calls the start routine, terminates through 258 | `ValidateSystemRoutineAdmission` checks the prepared context; a synthetic routine models the shape at `13F10` | Generated parent/child run, TLS fill, nested callbacks, ordinal 258 | Authentic entry target is not admitted; SEH prologue not implemented |
| Parent/child lifecycle | Ordinal-255 argument order, handle publication, `NtClose` retention | Per-worker claims and two concurrent managed workers (Phase 2D-15), system-routine claim target (2D-16) | Child-first and parent-first synthetic runs | None for the synthetic model |
| Guest stack and TLS | CRT formula `align16(raw + zero_fill) + 4`; real image block `0x94`, index `-37`; block ends at `StackBase` | `SetupGuestBlocks` + `ValidateThreadGuestMemory`; admission re-checks `TlsData == StackBase - tls_size` | Generated `FS:[4 + index*4]` and TLS read-back | None for the supported subset |
| KPCR / KPRCB / ETHREAD | Named offsets in `thread_memory.h`; `ValidateThreadGuestMemory` read-back | 0x400 KPCR, 0x200 ETHREAD, `FS:[0x20] = kpcr+0x28` | Generated reads through `FS:[0x28]`, `FS:[0x20]` | `KPRCB+0x250` and `KTHREAD+0x24` remain unresolved and zero |
| `KPRCB+0x250` | Read as a non-null object by `002B38B2`, `002B596D`, `002B5BD9`, `002B83F6`, `0037927B`, `0039CD5F`; no writer in the image; not a KTHREAD | Slot stays zero; authentic code takes the null branch | Generated guarded access proves the null path | Owner, initialisation and lifetime unknown; do not fake a pointer |
| Executable code-page lifetime | `AcquireExecutableRun` leases the contiguous executable run; nested acquisitions never wait while a lease is held | `ValidateCallbackTicket`, `PrepareGuestCallback`, nested calls, `x86_icall`, continuations | Memory tests + cross-page callback + nested distinct-page callbacks + admission/execute TOCTOU | Tail jumps still bypass session *authorisation* (pages are pinned); needs a codegen change |
| Persistent callback admission | Phase 2D-13 session policy, per-call slot re-read, depth limit 8, dispatch/memory generations | `SyntheticSessionPolicy` / `AdmitSyntheticCallback` | Multi-call, nested, revocation, stale-generation tests | Only synthetic slots; no authentic target is authorised |
| 867 initializer targets | Phase 2D-12 census: 877 slots, 838 `VERIFIED_STATIC`, 5 `INFERRED`, 34 `PROHIBITED`, all `auth=0` | Static policy validator only | Positive + 14 negative policy cases | No runtime authorisation or multi-call persistent route for authentic tables |
| 34 `PROHIBITED` callbacks | Phase 2D-12 manifest | None | Policy validator rejects them | Prohibited under the current execution policy |
| Frame-sensitive `00316DA8` | Reads its caller's stack frame although the call site pushes no argument (2D-11/12) | Not implementable as a zero-argument callback | Static classification only | Caller-frame layout and ABI unresolved |
| Dynamic list callback `0032062A` | Registered through `0031BBA5`/`002B5B8A`, invoked after `002B5C86`, `RET 4` | Static reconstruction only | Static | Runtime target provenance and node lifetime unresolved |
| Authentic ordinal 258 | `PsTerminateSystemThread` 1 argument; `ExitThread` marks the thread and throws | Production bridge unchanged; synthetic child exits through it | Child exits `0xC0DE1502`; cleanup and lease release verified | Authentic worker cannot start, so the authentic path is untested by design |
| Retail SEH 302/312 | `RtlRaiseException`/`RtlUnwind` still raw `R32`/`W32` traversal; handler `00319858`, epilogue `002B5BD3` not validated | Fail-closed | — | Prohibited until the contracts are established |
| Remaining x86 instructions | Frozen route 143/143 supported (72 root + 71 secondary, stop `002B5C7B`) | Checked recompiler | Integrated preflight | Startup beyond `002B5C7B`, `CLI`/`STI`, `INT3` traps unproven |
| `[0x8001003C]` physical window | Read-only; build-5838 path does not reach it (2D-11) | Not initialised | Static only | Not on the current path; unresolved if reached |
| Protection 179/182/204 | Strongly corroborated ABI, incomplete retail semantics (2D-11) | Fail-closed | None | Not on the build-5838 startup path |
| `CLI` / `STI` / `INT3` | Authentic startup dependencies | Traps | — | Fail-closed; no authentic path executes them yet |

## Smallest blocker set for a first authentic experiment

1. **`KPRCB+0x250`** — establish the object, or prove the authentic path is safe with the null branch.
2. **A claim-bound authentic admission route** for `PsCreateSystemThreadEx` → `002B5C0F`, still without
   executing guest code.
3. **A persistent authentic callback authorisation** for the 867 live initializer targets.
4. **Retail SEH 302/312** contracts (handler, epilogue, unwind targets).
5. **Remaining traps/imports** on the startup path: `CLI`/`STI`, `INT3`, any unimplemented instruction,
   and the fail-closed protection imports (not on this path).

Do not authorise authentic execution from this document. A PASS on Phase 2D-16 means the synthetic
admission and page-lifetime machinery is implemented and validated; it does not mean the authentic
start routine can run.
