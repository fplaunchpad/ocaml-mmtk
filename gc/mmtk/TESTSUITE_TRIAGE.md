# OCaml testsuite triage under always-on MMTk (item #19 / M7)

Goal: **zero untriaged testsuite failures** — every non-passing test is either fixed
or disabled with a greppable `(* MMTk DISABLED: <reason> [category] *)` marker (its
first line, replacing the `(* TEST ... *)` block so `ocamltest -list-tests` excludes
it). Enumerate the disabled set with:

```
grep -rn 'MMTk DISABLED' testsuite/tests
```

## Run configuration

- Host: turing (28-core Linux Xeon), fresh clone of `origin/5.5+mmtk` @ `6db255e1ee`.
- Build: `make -j28 world.opt` + `make ocamltest ocamltest.opt` (the latter needed
  `./configure --enable-ocamltest`; without it ocamltest is disabled and `make
  ocamltest.opt` errors). `testing.cma`/`testing.cmxa`/`testsuite/tools/codegen` all built.
- Suite run: `setarch x86_64 -R env MMTK_PLAN=GenImmix MMTK_HEAP_SIZE_MB=512 make -C
  testsuite parallel TIMEOUT=120`.
- Cross-checked the weak/finaliser cluster on `Immix`, `GenCopy`, `MarkSweep` (same).

## Result (GenImmix, default plan)

| metric | before triage | after triage |
|---|---|---|
| passed | 1700 | 1700 |
| failed (unique test files) | **29** | **0** (all marked or flaky) |
| skipped/disabled (`MMTk DISABLED`) | 30 (baseline) | **57** (30 + 27 new) |

All 29 unique failing tests were classified with evidence (re-run in isolation,
diffed actual-vs-reference, minimal repros for the weak/finaliser root cause).
**27** were disabled with markers; **2** (`lib-unix/.../sigwait`,
`weak-ephe-final/finaliser_handover`) are timing-flaky-under-load and PASS in
isolation, so they were left enabled and documented as flaky.

## The one REAL behavioural finding worth tracking

A single **deterministic** behavioural difference (not timing, not unsupported):

- `tests/callback/signals_alloc.ml` (bytecode only; native passes): the program
  prints `01243` under MMTk vs stock's `01234`, every run. A SIGUSR1 handler's
  state transitions interleave with an allocation at a different poll point in the
  **bytecode interpreter** under MMTk. The signal IS delivered and the value IS
  seen — only the interleaving order differs. Minor, but a genuine semantic diff in
  bytecode signal-vs-allocation-poll placement. **Candidate for a follow-up issue**
  (low priority): align the bytecode allocation poll/safepoint placement so a signal
  raised mid-allocation is observed in the stock order.

Everything else is a **known MMTk semantic difference** (finaliser/weak deferral,
stock `Gc.stat` counters, runtime_events, #12c) or a test-infra/timing artifact —
**no MMTk correctness gap** (no lost reachable object, no UAF, no heap corruption;
`sanity` is unaffected).

## Root cause of the largest cluster (verified)

`Gc.full_major ()` under MMTk does **not synchronously** run finalisers / clear weak
references the way stock OCaml does at that exact call. Minimal repros (turing):

- `Gc.finalise` / `Gc.finalise_last` on a value that dies and is collected by
  `Gc.full_major()`: the callback does **not** fire on the `full_major` call — but
  **does** fire (1000/1000) once there is allocation pressure + further GCs.
- `Weak.create` + set 20 keys, drop 10, `Gc.full_major()` twice: reachable keys
  survive **20/20** (no false clearing — correctness OK); dead keys cleared **0/10**
  synchronously, but **10/10** after heavy allocation + repeated full GC.

So finalisation and weak-clearing are **deferred** under MMTk (driven by collection
work, not by the `full_major` call site). Reachability is correct; only the timing
of the observable side-effects differs. Many testsuite tests assert stock's
synchronous-on-`full_major` semantics, hence the cluster below.

## Triage table (all 29 unique GenImmix failures)

Categories: **unsupported** (feature absent under MMTk) · **semantic-timing**
(deferred finaliser/weak/ephemeron clearing) · **stock-counter** (depends on stock
`Gc.stat`/minor_collections values) · **unsupported-12c** (alignment-aware alloc) ·
**behavioral-diff** (real but minor) · **infra-artifact** · **timeout-slow** ·
**flaky** (passes in isolation; NOT disabled).

| test | variants | category | one-line reason | action |
|---|---|---|---|---|
| lib-runtime-events/test | byte+nat | unsupported | runtime_events GC-event stream not emitted under MMTk | DISABLED |
| lib-runtime-events/test_caml | byte+nat | unsupported | GC-event counters (minors/major cycles) report 0 | DISABLED |
| lib-runtime-events/test_caml_counters | byte+nat | unsupported | GC counters not emitted | DISABLED |
| lib-runtime-events/test_caml_exception | byte+nat | unsupported | GC-event stream not emitted | DISABLED |
| lib-runtime-events/test_caml_parallel | byte+nat | unsupported | GC-event stream not emitted | DISABLED |
| lib-runtime-events/test_caml_reentry | byte+nat | unsupported | GC-event stream not emitted | DISABLED |
| lib-runtime-events/test_caml_slot_reuse | byte+nat | unsupported | GC-event stream not emitted | DISABLED |
| lib-runtime-events/test_caml_stubs_gc | byte+nat | unsupported | GC-event stream not emitted | DISABLED |
| lib-runtime-events/test_compact | byte+nat | unsupported | compaction events not emitted (no stock compactor) | DISABLED |
| lib-runtime-events/test_external | byte+nat | unsupported | external GC-event stream not emitted | DISABLED |
| lib-runtime-events/test_instrumented | nat | unsupported | instrumented GC-event stream not emitted | DISABLED |
| weak-ephe-final/ephetest | byte+nat | semantic-timing | finalise_last "unset" flag not flipped on full_major | DISABLED |
| weak-ephe-final/ephetest2 | byte+nat | semantic-timing | finalise_last flag not flipped on full_major | DISABLED |
| weak-ephe-final/ephetest3 | byte+nat | semantic-timing | finalise_last flag not flipped on full_major | DISABLED |
| weak-ephe-final/pr12001 | byte+nat | semantic-timing | "finalised" line missing (finaliser deferred) | DISABLED |
| lib-sys/opaque | byte+nat | semantic-timing | "lifetime" finaliser flag not flipped on full_major | DISABLED |
| backtrace/callstack | byte+nat | semantic-timing | finalizer-invoked backtrace differs (different call site) | DISABLED |
| regression/pr3612 | byte+nat | semantic-timing | custom-block finalize not run on full_major (1000001 vs -1) | DISABLED |
| c-api/alloc_async | byte+nat | semantic-timing | finaliser fires at different poll point vs C alloc (#12c) | DISABLED |
| tool-ocaml/t340-weak | byte (ocaml) | semantic-timing | dead weak keys not cleared on full_major → Not_found | DISABLED |
| asmcomp/polling_insertion | nat | stock-counter | asserts minor_gcs() (stock minor_collections) increments | DISABLED |
| regression/pr5233 | byte+nat | stock-counter | (Gc.stat()).heap_words/3 alloc → Out of memory | DISABLED |
| tool-ocaml/t350-heapcheck | byte (ocaml) | stock-counter | stock Gc.stat + synchronous weak clearing | DISABLED |
| c-api/aligned_alloc | byte+nat | unsupported-12c | asserts NOT all Atomic.t aligned; MMTk aligns all | DISABLED |
| callback/signals_alloc | byte | behavioral-diff | bytecode signal/alloc poll order 01243 vs 01234 (deterministic) | DISABLED |
| native-debugger/linux-gdb-amd64 | nat | infra-artifact | gdb sees 28 MMTk worker threads; multi-thread bp format | DISABLED |
| lib-marshal/fuzzy | byte+nat | timeout-slow | -n 10000 marshalling too slow under MMTk (n<=100 pass) | DISABLED |
| lib-unix/common/sigwait | nat | flaky | passes in isolation; sigwait flake under parallel load | left enabled |
| weak-ephe-final/finaliser_handover | byte | flaky | passes 3/3 in isolation; flakes under concurrent load | left enabled |

## Counts per category (the 27 disabled)

| category | count | tests |
|---|---|---|
| unsupported (runtime_events) | 11 | lib-runtime-events/* |
| semantic-timing (finaliser/weak/ephemeron deferral) | 9 | ephetest, ephetest2, ephetest3, pr12001, opaque, callstack, pr3612, alloc_async, t340-weak |
| stock-counter (Gc.stat / minor_collections) | 3 | polling_insertion, pr5233, t350-heapcheck |
| unsupported-12c (alignment) | 1 | aligned_alloc |
| behavioral-diff (REAL, minor) | 1 | signals_alloc |
| infra-artifact (gdb worker threads) | 1 | linux-gdb-amd64 |
| timeout-slow | 1 | fuzzy |

Plus the **30 baseline** `MMTk DISABLED` markers already present (22 statmemprof +
8 others), for **57 total** disabled.

## Notes

- The competing `~/ocaml-mmtk-rmstock` GenImmix testsuite run (another tree) and a
  `curche` run were active on turing during the suite run — this inflates
  timing-flaky failures (sigwait, finaliser_handover). Both pass cleanly in isolation.
- Disabling convention verified: a file whose first line is `(* MMTk DISABLED: … *)`
  (no `(* TEST *)` block) is **excluded** by `ocamltest -list-tests`, so `make -C
  testsuite parallel` never runs it and it is not counted as a failure. (Invoking
  `ocamltest` directly on such a file prints "could not read test script" — that is
  expected and is how the pre-existing baseline markers already behave.)
