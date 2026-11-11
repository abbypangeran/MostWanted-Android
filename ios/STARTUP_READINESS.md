# Authentic startup readiness gate (Phase 2D-19)

This is the human-readable form of [`startup_readiness.json`](startup_readiness.json). It separates three
milestones that must not be collapsed into one status. Passing synthetic tests does not make an authentic
dependency ready.

`PHASE_2D19_AUTHENTIC_RUN_HARNESS = PASS` (gate results unchanged: Gate A **GO**, Gates B and C
**NO-GO**). This PASS means the experimental harness and its safety contract are ready for review. The
authentic image was **not** executed and authentic execution authorisation is **NOT GRANTED**.

`kAuthenticExecutionAuthorised` is false. Apple `kExecutionDisabled`, production `Boot()`, retail
SEH/`RtlUnwind`, the protection imports and the authentic callback manifest (`auth=0`) are unchanged. The
frozen Phase 2C-41 route remains 143 instructions stopping at `0x002B5C7B`; it was not rerun or extended.

## The exact experiment boundary

The proposed run has two workers and two pre-instruction stops:

| Segment | Executed | Detail |
|---|---:|---|
| Parent root | 71 | CRT entry `0x002B3916` through `0x002B3986` |
| Parent stop | 0 | `0x002B3987 ret`; entry refused before execution |
| Child system routine | 35 | `0x002B5C0F` prologue/TLS fill through `0x002B5C76` |
| Child SEH prologue | 19 | `0x00319804` registration and return |
| Child pre-start walk | 17 | `0x002B597D` empty-list path, imports 277/294 |
| Child start-call setup | 2 | `0x002B5C7B push [ebp+0x0C]`, `0x002B5C7E call [ebp+0x08]` |
| Child stop | 0 | target `0x002B38A2`; entry refused before execution |
| **Total executed** | **144** | 71 parent + 73 child; 146 boundary steps including both stops |

The Phase 2D-18 "145" counted the parent stop entry inside its 72-step root figure. The independently
confirmed count is 71 executed parent instructions plus the refused `0x002B3987` entry, and 73 executed
child instructions plus the refused `0x002B38A2` entry. The `0x002B5C7E` call retires with return address
`0x002B5C81`; zero instructions of `0x002B38A2` execute.

The boundary still excludes `0x002B58B8`, the `0x002B5854` probe, every `KPRCB+0x250` read, the initializer
walkers, the 867 live initializer targets, the dynamic list callback `0x0032062A` and ordinal-258
termination.

## Why the pre-instruction stop is exact

The experiment mapping declares one generated function per reviewed instruction
(`ios/recompiler-fixtures/authentic_run_config.cpp`; `checked_memory = true`; no entry, gap, pointer or
callback seeding). Every transfer is therefore a checked dispatch boundary. The harness gate refuses the
terminal target before the generated function is entered; the call's return-address push is performed and
the callee executes zero instructions. No post-execution PC check and no return from an arbitrary generated
function is used.

`authentic_run_preflight` decodes the real image and checks every instruction, branch target, kernel slot
and startup datum. With `--generated` it checks the generated mapping:

| Item | Value |
|---|---|
| Image SHA-1 / title / entry / base | `f636a16af04171089f879a4bed646e185c734233`, `4541007B`, `002B3916`, `00010000` |
| Executed / boundary steps | 144 / 146 |
| Generated mapping | 152 entries (144 reviewed + 8 trap-only), `00011000..003714D4` |
| Mapping identity | `EC3486BCAFA498BF` |
| Unchecked dispatch | none (`x86_icall` 0, direct host calls 0) |
| Execution | **PROHIBITED** |

## Claim-bound authorisation

`AuthorizationPolicy` binds the origin, role, reviewed manifest and identity, XBE SHA-1, callback-policy
identity, generated mapping identity, dispatch/memory generations and separate parent/child budgets.
`RunAuthorization` is move-only and names a slot bound to the exact `Thread*`, thread id, image and
generations. It is not transferable, is revocable (`RevokeRun`/`CancelAllRuns`), and fails closed after
shutdown, a generation change, a mapping change or an image change. The gate checks these on every
boundary and admits only exact reviewed edges and kernel calls.

`AuthorizeAuthenticRun` validates every binding and then returns `kAuthenticExecutionNotAuthorised`
because `kAuthenticExecutionAuthorised` is false; it creates no claim. Synthetic rehearsal admission
(`AuthorizeRunForThread`) exists only under `MOSTWANTED_AUTHENTIC_RUN_TESTS` and cannot be compiled into an
ordinary build. The production `VALIDATED_BUT_EXECUTION_PROHIBITED` assessment remains evidence only; there
is no conversion from it into execution permission.

## SEH and kernel imports

The child frame is captured at the `0x002B5C16` call and verified read-only at the stop: live record,
`fs:[0]` chain, scope flag `-1`, scope table, saved ESP, caller EBP, handler resolution and the
scope-table terminator. Retail exception delivery is not enabled; a checked memory fault while the frame
is live stops the run and the synthetic handler marker remains zero.

Ordinals 255, 187, 277 and 294 have exact-name checked harness exports. The ordinal-255 bridge calls
`PrepareSystemThreadEx`, the reviewed admission core, the child binding and `StartThread`; a rejected
admission creates no child. Ordinals 277/294 use checked memory: the reviewed object `0x0046C8B0`, empty
wait list, owner/recursion updates, and fail-closed contention or malformed state. No fabricated success is
returned. An authentic-origin child additionally reuses
`AssessAuthenticSystemThreadRequest`.

## Validation

| Check | Debug | Release |
|---|---:|---:|
| `authentic_run_tests` | 44 PASS / 0 FAIL | 44 PASS / 0 FAIL |
| Repeat determinism | 20 runs, 0 failures | 5 runs, 0 failures |
| Checked recompiler fixtures | 812 PASS / 0 FAIL | 812 PASS / 0 FAIL |
| Applicable safe CTests (`-E execution_probe`) | 5/5 | 5/5 |
| `authentic_run_preflight --generated` / `gate_b_preflight` | PASS | PASS |
| Root / secondary / integrated `--preflight` | PASS, exit 0 | PASS, exit 0 |
| Android ARM64 syntax | PASS | — |

The suite covers the exact stops and counts, SEH/TLS verification, claim identities, failed child
admission, budget overrun, forbidden branch, unreviewed kernel ordinal, unsupported instruction,
uncommitted memory read, a memory fault while SEH is live, parent/child cancellation, revocation before
start, image/mapping/generation/shutdown refusals and deterministic finalisation. The exploratory
`execution_probe*` CTests execute synthetic fixtures containing copies of authentic route bytes and are
excluded from the acceptance evidence.

## Gate A — authentic preparation: **GO**

Preparation, image identity binding, reviewed-manifest and mapping checks, prepared-thread layout and the
non-executing assessment all pass. No worker starts and no guest instruction executes.

## Gate B — bounded authentic execution: **NO-GO**

The harness and safety contract are ready, but Gate B is not granted:

- authentic execution authorisation is deliberately withheld (`kAuthenticExecutionAuthorised = false`);
- the authentic parent start path is not enabled (the synthetic rehearsal start is the tested path);
- the authentic path has not observed ordinals 277/294 or the SEH registration at runtime;
- retail SEH exception delivery remains prohibited and unvalidated (not needed on the normal path, but a
  runtime property).

## Gate C — authentic startup towards the game loop: **NO-GO**

Everything in Gate B plus the 867 initializer targets, `CLI`/`STI`, the hardware/version selector, the
dynamic list callback, retail SEH 302/312, ordinal 258 on the authentic path, the frame-sensitive
`0x00316DA8`, protection imports and multiple authentic workers/scheduling.

`0x002B5C0F` never returns normally: after the start routine it terminates through ordinal 258. Any later
boundary must preserve that.

## Dependency matrix

| Dependency | Evidence | Implementation | Validation | State |
|---|---|---|---|---|
| Pre-instruction stop | per-instruction generated functions, checked transfers | `authentic_run` gate | exact stops and counts, 20+5 repeat runs | implemented, synthetically validated |
| Authentic admission binding | reviewed manifests, SHA-1, policy and mapping identities | `AuthorizeAuthenticRun`, `BindChildThreadForRequest` | synthetic binding, refusals, preflight | implemented, authentic authorisation withheld |
| Executable code pages | `AcquireExecutableRun` at worker start, released at the stop | harness worker | code-page/lease path, static preflight | implemented |
| SEH normal registration | captured `E`/chain and read-only stop verification | `VerifyStopSeh` | live-frame, fault-while-live, handler-never-called | implemented, normal path only |
| Kernel 277/294 | reviewed object and call sites | checked critical-section implementations | enter/leave state, ABI, final layout | implemented, synthetic |
| Kernel 255/187 | reviewed call site and arguments | harness ordinal-255/187 bridge | child created/closed, rejected admission | implemented, synthetic |
| Parent/child shutdown | per-worker claims, cancellation | `CancelAllRuns`/`RevokeRun` | both stopped, no residual workers | implemented, synthetic |
| Diagnostic trace | fixed trace records | `TraceRecord`/`RunEvidence` | origin/PC/ESP/SEH/TLS/stop checks | implemented, synthetic |
| 867 initializer targets | 877 slots, `auth=0` | static policy only | policy validator | static-only, Gate C |
| Retail SEH 302/312 | handler `0x00319858` | fail-closed | none | prohibited |
| `KPRCB+0x250` / `[0x8001003C]` | six reads; none in this boundary | stays zero | static | not on this path |
| Protection 179/182/204 | 2D-11 ABI ledger | fail-closed | none | fail-closed |

Do not authorise authentic execution from this document. The next step is a separately approved scoped
activation of the harness for exactly the 144-instruction boundary above, followed by review of the trace
before any boundary extension.
