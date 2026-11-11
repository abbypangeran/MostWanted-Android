# Authentic startup readiness gate (Phase 2D-20)

This is the human-readable form of [`startup_readiness.json`](startup_readiness.json). It separates three
milestones that must not be collapsed into one status. Passing synthetic tests does not make an authentic
dependency ready.

`FIRST_AUTHENTIC_RUN_REMEDIATION = PASS`. The first authentic run remains frozen as `EARLY_STOP`; its
evidence was not modified. The return-classification and assessment-handle defects are corrected, the SEH
failure is determined from the original trace and code as an incorrect verifier expectation, and the
stack-lease/resource accounting is resolved. **No authentic Xbox instruction ran in this phase.** Gate A is
**GO**; Gates B and C remain **NO-GO**. Authentic execution authorisation is **NOT GRANTED**;
`kAuthenticExecutionAuthorised` is false and the ordinary app, `Boot()` and `main` are unchanged.

## Accepted first-run evidence

The run used a local, uncommitted activation and is preserved unchanged in
`.local/ios-port/phase2d19-authentic-run/` (`run.log`, preflight and synthetic logs, the dry-run refusal and
the exact local patch). It executed 128 authentic instructions: parent 55 retired (56 entries, first
divergence `0x002B5D12`), child 73 retired, child stopped at `0x002B38A2`. Kernel 255/277/294 returned
success; 187 was not reached. Cleanup: `workers=0 claims=0 threads_empty=1 system=0 contiguous=0
virtual=1280`. The authorisation was restored to disabled; no rerun occurred.

## The exact experiment boundary

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

The Phase 2D-18 "145" counted the parent stop entry inside its 72-step root figure. The exact executed
total is 144.

## Current identities

| Identity | Value |
|---|---|
| Image SHA-1 | `f636a16af04171089f879a4bed646e185c734233` |
| Generated function mapping | 152/152 entries, `00011000..003714D4` |
| Mapping identity | `EC3486BCAFA498BF` (unchanged by this remediation) |
| Parent manifest (full policy) | `7F8AD72F529DC906` |
| Child manifest (full policy) | `DBCFD15B661E8CE6` |
| Parent route-table prefix | `0CAB6ED490B54606` |
| Child route-table prefix | `14FD36A4C6519854` |

## Remediation A — RET classification: corrected

The Phase 2C root route table has no return step kind and marks `0x002B5D12 ret 0x18` and
`0x002B2774 ret 0x04` as sequential. The Phase 2D-19 manifest inherited that kind; the checked return gate
requires `kRet`, so the first authentic run refused `0x002B5D12` before executing the `RET`.

The manifest now marks both as `kRet` and records their reviewed immediates (`0x18`, `0x04`; the child walk
`0x04`; the SEH return `0`). `AdmitGuestReturn` validates the generated cleanup bytes, the call depth, the
return address and the caller call node's continuation. `authentic_run_preflight` now rejects a decoded
`RET`/`RET imm16` classified as anything else, a `kRet` that does not decode to `RET`, a wrong immediate, or
a continuation outside the reviewed route/stop. The synthetic `RET imm16` route and a deliberately
misclassified RET reproduce the defect and the corrected behaviour.

## Remediation B — SEH failure: incorrect verifier expectation

The original `VerifyStopSeh` hard-coded the scope flag as `0xFFFFFFFF`. The authentic caller executes
`0x002B5C1B and dword ptr [ebp-4], 0x00` after the prologue returns, so at the stop the trylevel is `0`
while the registration, scope table and chain remain live. The run's trace contains the `0x002B5C1B`
instruction and the child stop; the frame is `E=D005FF3C`, the saved ESP is `D005FF0C` (`E-0x30`), EBP is
`D005FF40` (`E+4`), and the stop ESP is `D005FF04`.

| Predicate | Expected | Observed | Source | Result |
|---|---|---|---|---|
| Capture | captured | captured before the call | gate capture at `0x002B5C16` | pass |
| Entry address | valid E | `D005FF3C` | trace call record | pass |
| Stack bounds | frame in stack | E inside derived stack | prepared layout | pass |
| Post-prologue ESP | `D005FF0C` | stop ESP `D005FF04` | prologue/stop traces | pass |
| Caller EBP | `D005FF40` | `D005FF40` | stop trace | pass |
| Previous record | captured `fs:[0]` (`FFFFFFFF`) | stack word not in trace | capture/prologue | pass (reached scope) |
| Handler | `00319858` | stack word not in trace | manifest/prologue | pass (reached scope) |
| **Scope flag** | **`FFFFFFFF` (hard-coded)** | **`0`** | `0x002B5C1B and [ebp-4],0` | **fail** |
| Scope table | `003ED5B8` | not in trace | manifest/prologue | not evaluated |
| Saved ESP | `D005FF0C` | derived `0x00319824 esp` | prologue trace | not evaluated |
| Caller EBP held | `0` (skipped) | `0` | call capture | skipped |
| Chain | registration in `fs:[0]` | `fs:[0]` not in trace | KPCR | not evaluated |
| Handler resolve | generated entry | mapping has `00319858` | dispatch | not evaluated |
| Scope terminator | `FFFFFFFF` | static image datum | image `.rdata` | not evaluated |

The trace does not record the stack words or `fs:[0]`; the post-scope predicates are therefore marked not
evaluated. The new predicate diagnostics record all fourteen checks with expected/observed/address/status
(`kPass`/`kFail`/`kUnknown`/`kSkipped`) for a future run. The synthetic suite corrupts the scope flag,
handler, previous record, scope table, saved ESP, exception chain and caller EBP and asserts the specific
recorded predicate. Retail exception delivery and the handler remain prohibited; the handler is never
called.

## Remediation C — assessment handle corruption: corrected

The original assessment prepared its candidate with the live `handle_out`/`thread_id_out` and its cancel
cleared those guest outputs. The assessment now prepares an isolated candidate with **zero output
addresses** and only validates that the live outputs are readable/writable. Five repeated assessments
preserve a live synthetic child's handle/id; wrong call site, TLS, policy and image are rejected without
touching the live child; candidate allocations/registrations return to baseline; an assessment followed by
real synthetic creation preserves the output, publishes the real handle/id and permits an ordinary checked
`NtClose`. The checked-fixture authentic assessment cases pass 812/812 and the success result records
`reason=kAdmitted`.

## Stack lease and allocation accounting

The whole-run stack lease is retained: the code run and worker stack are leased for the route, the stop
verification is read-only and takes no conflicting lease, and both leases release before the worker's
finalisation frees the stack. Synthetic tests cover concurrent stack decommit (waits for the lease,
completes only after finalisation, neither stop disturbed), cancellation while the lease is held, failed
child preparation, shutdown and generation changes. The harness admits no nested callback, so the stack
lease does not cross one; the callback path remains covered by the checked-fixture callback suite.

`authentic_run_accounting` (static/preparation only) reports:

```
BASELINE            virtual=0
AFTER_IMAGE         virtual=1280  available=15104
AFTER_THREADS       virtual=1280  system=0 contiguous=0
AFTER_CANCEL        virtual=1280  system=0 contiguous=0
AFTER_SHUTDOWN      virtual=0     available=16384
```

The `virtual=1280` is the image reservation (`0x500000` for the `0x4F8040` image rounded up), not a
retained worker or stack. The first run's `virtual=1280` is the same reservation; `AFTER_SHUTDOWN` returns to
`0`.

## Validation

| Check | Debug | Release |
|---|---:|---:|
| `authentic_run_tests` | 79 PASS / 0 FAIL | 79 PASS / 0 FAIL |
| Checked recompiler fixtures | 812 PASS / 0 FAIL | 812 PASS / 0 FAIL |
| Applicable safe CTests (`-E execution_probe`) | 5/5 | 5/5 |
| `authentic_run_preflight --generated` | PASS, exit 0 | PASS, exit 0 |
| `gate_b_preflight` / root / secondary / integrated static | PASS / exit 0 | PASS / exit 0 |
| `authentic_run_accounting` | PASS, exit 0 | PASS, exit 0 |
| Android ARM64 syntax | PASS | — |

The integrated frozen preflight still reports 143/143, 72 root + 71 secondary, stop `002B5C7B`, and
`production assessment: status=validated-but-execution-prohibited unresolved=F threads=0`. The
`execution_probe*` targets were not used as acceptance evidence.

## Gate A — authentic preparation: **GO**

Image identity, reviewed manifests/mapping, prepared thread layout and the non-executing assessment all
pass. No worker starts and no guest instruction executes.

## Gate B — bounded authentic execution: **NO-GO**

The harness and safety contract are ready, but Gate B is not granted:

- the first run's authorisation is consumed; a second run requires separate user approval;
- retail SEH exception delivery remains prohibited; the predicate diagnostics have not observed an
  authentic frame at a stop yet;
- ordinal 187 was not reached in the first run; it is on the corrected next path and must be observed;
- the authentic parent start driver/activation patch is not committed and the execution switch is false.

## Gate C — authentic startup towards the game loop: **NO-GO**

Everything in Gate B plus the 867 initializer targets, `CLI`/`STI`, the hardware/version selector, the
dynamic list callback, retail SEH 302/312, ordinal 258 on the authentic path, the frame-sensitive
`0x00316DA8`, protection imports and multiple authentic workers/scheduling.

## Proposed second experiment (review only, not authorised)

The same original 144-instruction boundary: parent 71 + stop `0x002B3987`, child 73 + stop `0x002B38A2`,
mapping `EC3486BCAFA498BF`, manifests `7F8AD72F529DC906` / `DBCFD15B661E8CE6`, SEH scope flag `0`, isolated
assessment, whole-run code/stack leases and the full predicate diagnostics. It requires one new explicit
authorisation and must reach **both** stops, observe 255/187/277/294, verify the live SEH predicate set and
TLS, and reproduce the allocation baseline. No boundary extension is included.

## Dependency matrix

| Dependency | Evidence | Implementation | Validation | State |
|---|---|---|---|---|
| RET classification | decoded rets, immediates, continuations | `kRet` manifest entries, `ret_pop_bytes`, gate checks, preflight classifier | synthetic RET imm16, misclassification, return-target tests | implemented_synthetic |
| Claim-bound authorisation | policy/manifest/mapping identities and generations | `AuthorizeAuthenticRun` validates and withholds; isolated assessment | synthetic claims/refusals; checked fixture | withheld |
| Assessment isolation | zero-output candidate + live-output access checks | generic `AssessSystemThreadRequest`; authentic wrapper | repeated assessments, creation/close, negatives | implemented_synthetic |
| SEH normal path | captured frame; scope 0 at the stop | `VerifyStopSeh` + predicate diagnostics | normal and seven malformed-frame cases | implemented_synthetic |
| Kernel 277/294 | checked object/owner/recursion | checked implementations | synthetic enter/leave and final layout | implemented_synthetic |
| Kernel 255/187 | reviewed call site/arguments | harness bridge | child created, handle closed, rejected admission | implemented_synthetic |
| Parent/child shutdown | per-worker claims/cancellation | `CancelAllRuns`/`RevokeRun` | both workers stop; no residual resources | implemented_synthetic |
| Executable/stack leases | code and stack leases for the run | `RunBoundWorker` | concurrent decommit, cancellation, finalisation | implemented_synthetic |
| Diagnostic trace | fixed records + SEH checks | `TraceRecord`/`RunEvidence`/`SehCheck` | synthetic origin/PC/stop/predicates | implemented_synthetic |
| 867 initializer targets | 877 slots, `auth=0` | static policy only | policy validator | static_only |
| Retail SEH 302/312 | handler `0x00319858` | fail-closed | none | prohibited |
| `KPRCB+0x250` / `[0x8001003C]` | six reads; none in this boundary | stays zero | static | not_on_path |
| Protection 179/182/204 | 2D-11 ABI ledger | fail-closed | none | fail_closed |

Do not authorise authentic execution from this document. The next step is a separately approved scoped
activation of the corrected harness for exactly the 144-instruction boundary above, followed by review of
the trace before any boundary extension.
