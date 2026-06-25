# ocaml-mmtk roadmap

The living plan for `ocaml-mmtk`, meant to be picked up cold in a fresh session.
**Where we are:** the engineering bring-up (M0–M7) and the M9 stock-GC excision are
done; **M8 (benchmark + optimise) is the open milestone.** The correctness/perf tail
below is deferrable engineering; the agenda in
[`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) drives priority.

Companion docs: [`README.md`](README.md) (overview + build/run),
[`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) (what this platform is *for*),
[`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) (dated design notes, deferred investigations,
known-failure repros — **newest first; the depth behind every item below lives
there**), [`fork-handoff.md`](fork-handoff.md) (original rationale).

**Project shape.** This repo *is* the OCaml fork (base `5.5.0` final, branch
`5.5+mmtk`), distributed as `ocaml-mmtk`. The MMTk binding is in-tree at
[`gc/mmtk/`](gc/mmtk) and depends on `mmtk-core` 0.32 from crates.io (not vendored).
MMTk is **always-on and the only collector** — no opt-out; the stock minor *and*
major GC have been excised (M9). `MMTK_PLAN` selects the plan (default `GenImmix`).
Native code uses TLAB nursery-aliasing onto an MMTk bump/Immix region, so it requires a
plan whose Default allocator is a bump/Immix region (the seven:
`Immix`/`StickyImmix`/`ConcurrentImmix`, `GenImmix`/`GenCopy`, `SemiSpace`/`NoGC`); bytecode runs under any plan. Run
knobs: `MMTK_PLAN`, `MMTK_HEAP_SIZE_MB` (pins a **fixed** heap; the default is now a
**space-overhead** heap — `heap = live × 2.2` after each full GC, à la stock's `Gc.space_overhead`,
clamped 16 MiB..RAM; replaced MemBalancer, whose sqrt rule under-provisioned big live sets — binarytrees
3.5× → 1.27× slower than stock), `MMTK_NURSERY` (default bounded 2–64 MiB), `MMTK_VERBOSE`;
mmtk-core's own `MMTK_*` options are honoured (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`,
`MMTK_IMMIX_ALWAYS_DEFRAG`, …).

---

## Status

| Milestone | Description | Status |
|-----------|-------------|--------|
| M0 | Build skeleton: in-tree binding links into the bytecode runtime | ✅ done |
| M1 | MMTk **NoGC** backs every bytecode allocation | ✅ done |
| M2 | **MarkSweep**: precise root scanning + stop-the-world (real collection) | ✅ done |
| M2+ | **Multi-domain** stop-the-world (`Domain.spawn` programs) | ✅ done |
| M3 | **Immix** (moving): copy/forward, infix-pointer fixup, updatable roots, clean `Out_of_memory` | ✅ done |
| M4 | **Generational plans** (GenImmix / StickyImmix) — mutator write barrier (region/slot-remembering) | ✅ done |
| M5 | **Native-code integration** — all-MMTk via TLAB/nursery-aliasing (`Immix`/`StickyImmix`), single- and multi-domain (`Domain.spawn` clean); staticlib auto-linked via configure global-link | ✅ done |
| M6 | **Weak arrays, ephemerons, finalisers** — `process_weak_refs` on by default (`MMTK_WEAK_REFS=0` opts out to the memory-safe never-clear interim). Weak-clear, ephemeron-release, `Gc.finalise`/`finalise_last`, custom-block finalisers (incl. unmarshalled blocks), cross-domain orphaned-finaliser adoption. `pr3612`+`pr5233` pass. Bar #11 (resurrection ordering + orphaned ephemerons). | 🟢 done (default-on) |
| M7 | **Pass the OCaml testsuite** — full bytecode suite ~1450/1547 pass under Immix/StickyImmix (`setarch -R`, per-test timeout). Non-pass are known-unsupported (statmemprof, runtime-events, `Gc.stat`-pacing) or the bug #3b intermittent multidomain hang — none are MMTk correctness diffs (output byte-identical to stock). | 🟢 done |
| M8 | **Benchmark + optimise** vs. the stock GC — **the open milestone.** First native sweep: parity-or-better on 5 of 6 CLBG benchmarks (~1.5× faster on parallel alloc-heavy), one structural outlier (spectralnorm ~1.74× — MMTk's eager zero-fill double-write). Obvious-removal levers ~neutral (only C1-sftbound ~+1%); GenImmix-default validated. Method: `PERFORMANCE.md`. | 🟡 **open milestone** |
| M9 | **MMTk-only: excise the stock GC** — always-on; stock minor + major GC deleted; `shared_heap.c`/`.h` deleted (−1665 lines, live colour-machinery relocated to `major_gc.{c,h}`); per-domain minor-heap arena removed; `Gc.stat` reimplemented on MMTk stats; `Is_young` reservation retired. `ocaml-mmtk` is a single-GC runtime. **Complete** bar #11 (weak-clear semantics) + the flagged `memprof.c` colour read. Stage/bug depth: `gc/mmtk/NOTES.md`. | 🟢 done |
| — | Parallel collection: verified correct; marking scales ~8.4× on 16 threads (parallel-friendly heaps) | ✅ |
| — | **GC plans:** 10 wired (bytecode), 1 deferred (Compressor) — see the GC plans table below | 🟢 |

---

## Open work (prioritized)

Correctness before performance; dependencies noted. **Depth for every item is in
`gc/mmtk/NOTES.md`** (dated, newest-first) — this list is the index, not the detail.

1. **bug #3c — cross-STW rendezvous deadlock — FIXED** (`runtime/minor_gc.c`, merged
   `7d66a6172f`). Re-diagnosed (not the burn-pattern / `Gc.minor` race first suspected):
   a terminating domain leads OCaml's all-domains minor STW while still in MMTk's RUNNING
   set, so MMTk's `stop_all_mutators` and OCaml's minor STW capture each other's domains.
   Fix: bracket `caml_empty_minor_heaps_once` with `caml_mmtk_enter_blocking` /
   `caml_mmtk_become_running` — the domain is STOPPED in MMTk's view while it leads/joins
   the minor STW (still a registered mutator, roots still scanned). church gate (Immix/512/
   threads=8): hang **52.5% → ~1%**, `sanity` clean. **Residual:** ~1% rarer interleaving
   (concurrent multi-domain terminate; needs `rr`) + the separate **bug #57**
   (`active_plan.rs:59` "cannot trace object") now dominate at threads=8. → NOTES (2026-06-24).

2. **#11 — weak-ref resurrection ordering + retire `MMTK_WEAK_REFS`.** Fix
   `process_weak_refs` resurrection ordering (`pr5233` — a value resurrected only for
   its finaliser must still read cleared through a weak pointer) and the
   orphaned-ephemeron handover gap; **then** make `process_weak_refs` unconditional and
   delete the transitional `MMTK_WEAK_REFS` flag (like `caml_mmtk_enabled` was). Until
   then `MMTK_WEAK_REFS=0` (conservative never-clear) stays as the safety fallback.
   (Latent: `weak-ephe-final/weaklifetime.ml` asserts under StickyImmix — weak-clear
   timing tied to stock generational pacing.) → NOTES M6 entries.

3. **#12 — evacuation-time OOM.** Convert the `copy_object` assert (fires if
   `alloc_copy` fails mid-defrag; Immix reserves headroom to avoid it) to a graceful
   `Out_of_memory`. → NOTES Workstream-A material.

4. **#12c — testsuite-triage minor items.**
   - `c-api/aligned_alloc` — `caml_atomic_make_contended` needs a `Cache_line_bsize`-aligned
     block; MMTk's bytecode bump allocator isn't alignment-aware (native lands aligned via TLAB).
     Route it through an alignment-aware MMTk alloc.
   - `lib-marshal/fuzzy` — unmarshalling pathologically slow (per-object `caml_mmtk_try_alloc_shr`
     with GC disabled across each unmarshal). Profile; confirm slowdown not loop.
   - `output-complete-obj/test` — `-output-complete-obj` + manual `${mkexe}` link omits
     `mmtk_c_libraries` → `undefined reference to mmtk_ocaml_*`. Real gap for hand-linked objects.
   - Re-enable candidates (disabled with now-stale "finaliser not supported" reasons —
     finalisers work under `MMTK_WEAK_REFS=1`): `callback/test_finaliser_gc.ml`,
     `callback/test_gc_alarm.ml`, `basic-more/simplif_under_lambda.ml` — restore `(* TEST *)` + verify.

5. **#14 / i386 — platform.** macOS native linking: **DONE** (2026-06-24, arm64) — native
   compile + link + run verified (`-lmmtk_ocaml` resolves via the stdlib symlink + auto
   `-L<stdlib>`, the Linux mechanism). The earlier "Linux-only" status was a stale configured
   tree, not a source gap: the relocatable `mmtk_c_libraries` wiring (configure.ac →
   `{bytecomp,native}_c_libraries`) already carries the Darwin native-lib set; a tree
   configured before that commit just needs a reconfigure. The **i386** Build job is
   deliberately skipped — the 32-bit MMTk staticlib
   won't build (`Makefile.mmtk` `mmtk-lib` Error 127). Hygiene CI (`check-typo`) is red
   on whole-tree non-ASCII/long-lines — scope it to changed files, keep new C/build
   comments ASCII ≤80 col. → NOTES various; ROADMAP archive entry.

6. **#16 — native bump-pointer plans.** **GenImmix + GenCopy native: DONE** (2026-06-23) —
   the copy-nursery `BumpPointer` TLAB aliasing; the anticipated "lots of issues" didn't
   materialise because the moving-root fixup is already general (no minor-vs-major root path —
   reused from the major/defrag path), so it was a 2-file change. **GenImmix is the
   stock-faithful generational default.** → Shipped / NOTES (2026-06-23).
   **`SemiSpace` + `NoGC` native: DONE (2026-06-23)** — they already worked via the `BumpPointer`
   generalization (their Default allocator is a bump pointer); `SemiSpace` is `sanity`-clean (0 Invalid,
   3M+ objects copied). The native set is now **7**: Immix/StickyImmix/GenImmix/GenCopy/SemiSpace/NoGC/ConcurrentImmix.
   **`MarkCompact` native: INFEASIBLE via TLAB aliasing (confirmed empirically by two agents).** Although
   it bump-allocates internally, it needs per-object bookkeeping the inlined *gapless* TLAB bump cannot
   produce — a **reserved header word before every object** (its Lisp-2 forwarding slot) and a
   **per-object VO bit** (set via `post_alloc`) that `compact`'s linear scan relies on; an aliased region
   is unscannable/uncompactable (first compaction panics *"does not have a forwarding pointer"*). **Also
   bytecode-only:** `MarkSweep` (free-list, no bump region) and `PageProtect` (page-per-object debug) —
   native would need a codegen change, not a binding tweak.

7. **#17 — benchmarking + perf tuning (M8).** The open milestone; ties to
   `RESEARCH_QUESTIONS.md` RQ2. **Method of record: [`PERFORMANCE.md`](PERFORMANCE.md)**
   (heap-size-multiple sweeps not single numbers; workload fingerprint first; median +
   dispersion; GC-vs-mutator split) — adopt it before publishing any number. Standing
   order: **obvious fast-path removals first, then measure, then deeper levers.**
   - **Dominant levers (static hypotheses, measure-gated):** #A1 give *bytecode* a TLAB /
     inline its alloc fast-path (it has none — every object is a C-call + root-publish vs
     stock/native's 3-insn bump, `memory.h:263`); #C1 per-slot SFT lookup + a verified
     double slot-load on every traced edge (`slot.rs:92`/`:179`); #B1 inline the native
     write barrier (a no-op *call* under the non-generational Immix plan, `cmm_helpers.ml:2290`).
     **Native small-alloc is byte-for-byte stock — do NOT touch it** (all MMTk cost is in
     slow paths).
   - **Obvious removals (low-risk quick pass):** cache the classified slot word (kill the
     double load), single header read in `scan_object`, hoist the per-root debug branch,
     drop the dead `match semantics` small-alloc arm, drop the empty-`src` slice in the
     scalar barrier.
   - **Measurement blocker:** `runtime_events` is **broken under MMTk** — the real STW
     window (`collection.rs:224-287`) emits *zero* events while surviving stock spans wrap
     neutered no-ops, so **olly currently reports fictional tiny pauses**; use `bpftrace`
     uprobes until #R1–#R4 land. Host `turing`: set governor=performance + `opam install
     runtime_events_tools` (perf/turbo already OK).
   - **Prereqs that don't exist yet:** a lifetime-dispersion (Gini) profiler (#P1) + a
     per-GC survival/mutation meter (#P2) — RQ2's workload fingerprint needs them.
   - **Heap/nursery defaults + the MMTk minor-GC cost (2026-06-24, LANDED).** The fixed-1 GB
     heap (→ ~15× stock RSS) became dynamic (`8777a22082`), then a **space-overhead heap**
     (`70a709e179`): after each full GC `heap = live × 2.2` (stock's `Gc.space_overhead`), with a
     bounded nursery (2–8 MiB; **raised to 2–64 MiB on 2026-06-25** — the 8 MiB max forced 100s–1000s
     of near-empty minor GCs on high-alloc workloads, making GenImmix 1.3–3× slower single-domain; see
     NOTES + SCALABILITY.md). This *replaced* MemBalancer, whose sqrt rule under-provisioned
     big-live-set programs — **binarytrees 3.5× → 1.27× slower than stock** (major-GC thrash
     gone; the residual 1.27× is the per-collection copy cost, not the heap). RSS ≈ 4× live is
     Immix *fragmentation* (a separate defrag lever). Deeper finding: **MMTk's per-minor-GC cost ≈ 4× stock's**
     (~1.2 ms vs 0.3–0.4 ms/collection at a 2 MiB nursery) — every nursery collection goes
     through the full STW + GC-worker + work-packet machinery vs stock's inline on-mutator
     Cheney copy. Being profiled on turing to attribute the floor. The nursery should be an
     **adaptive survival-rate (~10%) controller with a per-GC-cost amortization floor** (not
     heap-proportional, not a fixed cap) — future work, isolated as an opt-in *mode* (a
     trigger/policy feature, not a new plan); keep a frozen baseline config. → NOTES 2026-06-24.
   - **GC worker pool — FIXED 2026-06-24.** The `nproc` default made every worker park/wake on every
     collection (82% of GC-worker CPU in futex contention); defaulting to **1 worker** is 1.37× faster on
     single-domain minor GC. Intended policy "workers = running domains" is a gc/mmtk-core-fork follow-up
     (the pool is fixed at init). Also pending: **#G1** (the binding does full major-root scanning every
     minor — narrow it to young-only + recent-frames; machinery already in the C runtime).
   - Earlier first-round levers (StickyImmix closes much of the gap). → full ranked backlog in
     `PERFORMANCE.md` Appendix A; NOTES `Workstreams archive`.

8. **`ConcurrentImmix` + SATB write barrier — RQ1 flagship (LANDED, bytecode + native, 2026-06-23; `lazy`-clean; Q3 continuations fixed).**
   The low-latency line (`RESEARCH_QUESTIONS.md` RQ1: does OCaml's immutability make
   read-barrier-free concurrent GC unusually cheap?). **De-risked:** OCaml's *stock* major
   barrier is *already* SATB (Yuasa grey-old-referent) — bug-#3 rewired `caml_modify` to
   MMTk's *generational* barrier and M9 deleted the stock concurrent major, so no SATB path
   is wired today, but the shape is native to the runtime/codegen → re-introduce it (gated on
   the concurrent plan) rather than invent it. **First-class sub-item — lazy/SATB coverage (an
   open research question, "implement and test breakage"):** forcing a `lazy` mutates the
   suspension in place, so the SATB deletion barrier must fire on forcing (else the thunk's
   captured env is lost → dangling) and the binding's `scan_object` must survive the
   force-vs-mark tag-transition race (`Lazy`/`Forcing` → `Forward`/result) + multi-domain
   `Forcing`/`Undefined` protocol. Plan: implement, then *deliberately break it* — heavy
   multi-domain forcing under concurrent marking at a small heap with `sanity`; characterise
   each break as fixable (missing barrier) or open (protocol conflict). **Gate:** re-enable the
   disabled `lazy/…force` testsuite test. **Status: availability confirmed** (`ConcurrentImmix` is a real
   mmtk-core 0.32 `PlanSelector` with `SATBBarrier`); **SATB barrier wired (~82 lines, bytecode) and
   `lazy` is clean** (force-vs-mark + force-vs-relocate, FAQ Q2). **Q3 (continuation scan vs resume) FIXED**
   (commit `55ab6ce40b`: per-continuation lock `cont_lock.rs` + resume SATB-snapshot — deterministic crash
   gone, STW flat in fiber count). **Native: VALIDATED** — the expected native gaps were already closed
   (native reaches `caml_modify` via the out-of-line extcall; mmtk-core eager-marks acquired lines so
   allocate-black is automatic — which also makes **no-zero (RQ8) safe on ConcurrentImmix**, now enabled),
   plus a real atomics-SATB-ordering bug found + fixed (`d0c721a8b7`); sanity-clean, macOS bytecode build
   verified. **Open (perf, not correctness):** the UNLOG-bit barrier gate is **de-prioritized** — native
   characterization showed the SATB barrier is ~free (<0.1% self), so the gate is empirically a non-issue;
   the sanity-build-only ~10 MB deadlock — **FIX LANDED (2026-06-25, mmtk-core `72ee627050`, submodule
   bump `a86ce19c18`).** Root cause: the concurrent→FinalMark handoff had no self-driving trigger
   (`trigger_internal_collection_request` was `unimplemented!()`; `FIXME` at
   `concurrent/immix/global.rs:84-85`), so FinalMark was only re-armed at the allocation poll and never
   fired when the Concurrent bucket drained while all mutators were quiescent/parked. Fix: `scheduler.rs::
   respond_to_requests` self-requests `WorkerGoal::Gc` (→ `Pause::FinalMark`) on bucket-drain while all
   workers are parked, gated to concurrent plans. **Validated for safety + correctness + no-regression**
   (turing, sanity on: 4-domain continuation stressor ×5 clean; checksum identical across plans); the
   *intermittent* end-to-end deadlock could not be reproduced on demand (never captured in an rr trace),
   so the "deadlock gone" confirmation awaits a live sanity-build/rr capture. → RESEARCH_QUESTIONS RQ1;
   NOTES (2026-06-25).
   - **SEPARATE, still-OPEN ConcurrentImmix hang — GH#14 (root-caused 2026-06-25, fix not yet landed).**
     Distinct from the `#4` deadlock above: a **marker-vs-mutator livelock** at *normal/dynamic heap,
     single-domain* on `spectralnorm`/`LU_decomposition` (hot boxed-float kernels). The SATB barrier
     refills the `Concurrent` work bucket from the mutator faster than the single default GC worker
     drains it, so `work_buckets[Concurrent].is_drained()` is never true while workers are parked →
     **FinalMark is never requested** → marking never finishes → the mutator blocks forever. `#4`'s
     guard is structurally unsatisfiable here (active mutator). **Fix (ready to implement, ~10 lines):**
     a heap-pressure forced FinalMark in `ConcurrentImmix::collection_required` — when
     `concurrent_marking_in_progress()` and `gc_trigger.is_heap_full()`, return `true` *regardless of*
     `is_drained()` (the FIXME at `global.rs:85`). Land **after** the quick-panel run is captured (it
     changes ConcurrentImmix numbers). Full evidence + rr recipe in GH#14 + NOTES (2026-06-25). This is
     why ConcurrentImmix is omitted from the README quick-panel table.

9. **#18 — stock-GC dead-code tail (M9 cleanup; mostly load-bearing).** Audit (2026-06-24)
   confirms the M9 excision is structurally complete: the deletable residue is **small**, and most
   inert-looking stock-GC code is **load-bearing** — link symbols the weak/ephemeron/finaliser/
   teardown paths call, the `*_done` teardown flags (`domain.c:2127,2136`), the major-slice
   **epoch** record that stops the bytecode mutator spinning in `caml_poll_gc_work`
   (`major_gc.c:1064-1069`), frozen `caml_gc_phase` gating the stock no-op branches, the `young_*`
   fields that **alias the MMTk TLAB** (`mmtk.c:332-336`), the `caml_do_roots` link anchor
   (`mmtk.c:49`), and the dependent-memory / `caml_adjust_gc_speed` exported `CAMLextern` ABI.
   **Genuinely deletable now:**
   - `caml_final_update_first`/`caml_final_update_last` (`finalise.c:118-142`) + their
     `EV_FINALISE_UPDATE_*` spans + `finalise.h` decls — **zero in-tree callers** (live path is
     `caml_final_update_last_minor`). Trivial removal; not previously catalogued.
   - the ~8 phantom `runtime_events` spans wrapping no-ops (`EV_MINOR`/`EV_MAJOR`/`EV_C_MAJOR_*`/
     opportunistic-mark) — this is perf-backlog **#R3**; removing them is what stops olly
     reporting fictional pauses, so it doubles as a measurement unblock (Risk: LOW).
   - collapse `caml_compactions_count` (`major_gc.c:108`, written nowhere) to literal `0` at its
     two `Gc.stat` reads (`gc_ctrl.c:77`), then drop the symbol.
   **The big deletion is gated on #3c, not independent:** the whole OCaml minor-STW rendezvous
   (`caml_empty_minor_heaps_once`/`caml_try_empty_minor_heap_on_all_domains`/neutered
   `caml_empty_minor_heap_promote`/minor barriers/`caml_minor_cycles_started`) is inert *as
   collection* but is the live `Domain.spawn`/terminate STW rendezvous — retiring it needs MMTk's
   STW to become the sole rendezvous, **the same rework as #3c (item 1)**. → NOTES 2026-06-24.
   **Not a local cleanup — it is an architecture decision** (which generation the framework owns) that must
   be **reconciled structurally with how other runtimes do minor collection** (OCaml ParMinor, GHC local
   heaps, Erlang per-process heaps, the Julia/CRuby MMTk bindings), and weighed against the **inverse**
   option — keep stock's *scalable* minor and use MMTk **major-only**. Both directions, and the empirical
   motivation (the fork's multi-domain anti-scaling), are written up as **RESEARCH_QUESTIONS RQ10** /
   `SCALABILITY.md`.

### Research & measurement workstreams (M8 / RQ-driven)

The active research/measurement threads behind the M8 milestone — the index; depth in `gc/mmtk/NOTES.md`,
`PERFORMANCE.md`, and `RESEARCH_QUESTIONS.md` (RQ-numbers below).

- **Benchmark vehicles (the M8 measurement plumbing).**
  - **`ocaml-bench/macro-benches`** — the authoritative DaCapo-style cross-runtime *macro* suite (±flambda),
    driven via **`running-ng`** — adopted as the M8 measurement vehicle (rather than building our own harness).
    The headline throughput/RSS campaign runs here. (See `PERFORMANCE.md` §1/§3.)
  - **Quick GC-decision bench panel** (on the `benchmarks` orphan branch, `quick/`; ~5 min/variant; sequential
    + parallel) — the **fast inner-loop complement** to the macro suite and the **no-zero (RQ8) A/B vehicle**.
- **RQ8 — no-zero allocation (CONFIRMED + LANDED on mainline, ~15–22% on alloc-bound code).** MMTk's eager
  zero-fill is redundant for OCaml (vanilla's minor heap is never zeroed); removing it recovers ~15–22%
  (spectralnorm +21.9%) with GC count/time/copies unchanged — a pure mutator win. **LANDED** via a **runtime
  plan-gate** (an `alloc_zeroed` flag set 0 by `runtime/mmtk.c`): no-zero is **universal** — ON for **all**
  plans, **including ConcurrentImmix** (verified allocate-black → safe, RQ9). The `gc/mmtk-core` fork
  (`0.32-ocaml`) is now the **mainline** mmtk dependency (submodule). → RESEARCH_QUESTIONS RQ8/RQ9; FAQ Q10.
- **GH#5 — generational-minor weak/ephemeron liveness (a GC-scheduling + `Gc.major_collections`-accounting
  bug, NOT clear-too-early).** Flipping the default to GenImmix surfaced `weaklifetime.ml`. Direct
  instrumentation **disproved** the hypothesized clear-too-early (freshly-promoted referent); the real failure
  is **clear-too-LATE**: at the test heap the generational plans run **no full GC**, so mature-*dead* weaks are
  never cleared, and **`Gc.major_collections` counts nursery GCs**, so the test's major-count window is
  unsatisfiable (same binary passes at a 16 MB heap). The generational-aware liveness shim
  (`ephe_is_reachable` treats non-nursery referents as live during a nursery GC) is
  **correct-but-doesn't-close-the-test** — **LANDED 2026-06-25 (`2a05e10846`)** as the sound soundness
  half (prevents mis-clearing a LIVE mature weak on a minor GC; weak-ephe-final finaliser/weaktest
  byte-match, Immix unchanged). **STILL OPEN** (the test's clear-too-LATE half): schedule a **full GC
  under mature pressure** + make **`Gc.major_collections` count only full GCs** — but counting-only-full
  ALONE would *hang* `weaklifetime`'s `while major_collections < 20` loop, so it is held until the
  companion full-GC trigger is designed. `finaliser_handover` SIGSEGV is the separate #55 sub-bug.
  → FAQ Q11; NOTES 2026-06-25, 2026-06-24; GitHub #5.
- **RQ7 — `Bactrian` hybrid (flagship research direction).** The faithful MMTk realization of
  OCaml's collector: copying nursery (GenImmix) + concurrently-marked, STW-evacuated Immix mature
  (ConcurrentImmix) + SATB barrier. Both halves are landed natively; composing them with a (near-)non-moving,
  incremental mature is the open mmtk-core-fork work. → RESEARCH_QUESTIONS RQ7.
- **LXR integration — RQ1's read-barrier-free, low-latency vehicle (PLAN, 2026-06-25).** **LXR** (Zhao,
  Blackburn & McKinley, PLDI'22) is reference counting on a hierarchical Immix heap + occasional concurrent
  SATB backup tracing for cycles, with **no read barrier** and a cheap **coalescing field-logging write
  barrier** — exactly the design RQ1 predicts OCaml's immutable-by-default heap makes unusually cheap (most
  stores are initialising writes through `caml_initialize`, which take no barrier; only genuinely-mutable
  fields hit `caml_modify`). It lives in a separate fork, **`wenyuzhao/mmtk-core` branch `lxr`**.
  - **Headline (de-risks it): same base, `mmtk-core 0.32.0`** as our `0.32-ocaml` fork, and our fork is
    *already on the LXR lineage for the concurrent-marking half* (the `concurrent/` plan, `Pause`,
    `SATBBarrier`, `ConcurrentPlan` are shared). What we lack is the **reference-counting half**: `src/args.rs`,
    `src/util/rc.rs` (the `RC_TABLE` side-metadata + `RefCountHelper`), the `FieldBarrier`
    (`src/plan/lxr/barrier.rs` — coalescing per-slot unlog bit, deferred `ProcessIncs`/`ProcessDecs`), the RC
    `WorkBucketStage`s, the LXR plan (`src/plan/lxr/`), and **RC hooks woven through `policy/immix/immixspace.rs`
    (~17 sites) + LOS** — the deepest, least-modular part.
  - **The "incompatible API changes" (enumerated, vs upstream/our 0.32):** LXR adds, to the **VMBinding traits**,
    new *required* `ObjectModel` methods (`dump_object_s`, `get_class_pointer`) + a per-slot
    `GLOBAL_FIELD_UNLOG_BIT_SPEC`; a **two-arg `SlotVisitor::visit_slot`** + `scan_object_with_klass` +
    `ObjectKind`/obj-array hooks (`Scanning`); a `RootKind` arg on `create_process_roots_work`; an extra
    `current_gc_should_unload_classes` arg on `stop_all_mutators`; and `Slot::to_address`. Several are
    **signature-breaking** for an existing 0.32 binding — but most are OpenJDK-shaped (class-unloading, klass
    pointers) and our OCaml binding can stub them (`get_class_pointer` → `Address::ZERO`, ignore `klass`, pass
    `false` for out-of-heap, no class unloading).
  - **A git merge is OUT (tested 2026-06-25):** `git merge lxr/lxr` into `0.32-ocaml` = 3 conflicts
    (`space.rs`/`immix_allocator.rs`/`options.rs`, where our no-zero/trigger deltas overlap LXR) + **125 files**
    of LXR's divergent core pulled in — the lxr branch is **1690 commits** past the shared v0.32.0 root (ours
    +5), so it is a full research fork (binding-breaking VM-trait changes + 1690 commits of API evolution that
    won't build against our 0.32.0 surface). One vendored branch is still the goal, but via an **additive,
    ADAPTED port** of just the RC pieces (re-written against our 0.32.0 API — not copyable verbatim), gated.
  - **Strategy: additively VENDOR + adapt LXR's RC half into `0.32-ocaml`, gated behind `MMTK_PLAN=LXR`** (every
    existing plan stays byte-identical). *Not* a merge/rebase onto `wenyuzhao/lxr` — that would force LXR's
    OpenJDK-shaped trait churn
    across the whole core and conflict with our `SpaceOverheadTrigger`/`no_zero_alloc`/FinalMark-trigger deltas;
    *not* a minimal cherry-pick — RC is not modular (the `immixspace.rs` hooks). Conflict map: HIGH on
    `gc_trigger.rs` (our SpaceOverheadTrigger vs LXR's survival-predictor triggers) and `immixspace.rs`/LOS (the
    RC trace variants — the bulk of the work + the main correctness risk under our moving Immix); MEDIUM on
    `spec_defs.rs` (the global side-metadata budget — `RC_TABLE` 2-bit + field-unlog 1-bit/word compete with our
    `GLOBAL_LOG_BIT`), `work_bucket.rs`, the VM traits; LOW on `barriers.rs`/`concurrent/` (purely additive).
  - **Phased plan (each phase builds; GenImmix stays the untouched default throughout):** **P1** mmtk-core
    scaffolding — vendor `args.rs`, `rc.rs`, RC `spec_defs`/`WorkBucketStage`s, `FieldBarrier`,
    `BarrierSelector::FieldBarrier`, `Pause::RefCount` (~3–5 d, low risk). **P2** Immix-policy RC hooks +
    LOS RC (~1–2 wk, **highest risk** — moving-GC correctness; lean on the `sanity` feature at small heaps).
    **P3** port `plan/lxr/`; wire `MMTK_PLAN=LXR` in `api.rs` (~1 wk). **P4** OCaml binding + runtime barrier —
    `GLOBAL_FIELD_UNLOG_BIT_SPEC`, the new `ObjectModel`/`Scanning`/`Slot` methods, `mmtk_ocaml_field_barrier`
    wired into `caml_modify` (mirror the existing SATB path, also pre-store/slot-granular) (~1 wk happy path;
    **+1–2 wk** for ephemerons/finalisers-under-RC and the `caml_initialize`/RC-0 interaction). **P5** bring-up:
    `sanity` at small heap, CLBG correctness gate, testsuite under `MMTK_PLAN=LXR`. **~5–7 weeks** to a correct
    single-domain LXR. **The RQ1 measurement** (does OCaml's immutable-store profile make LXR's barrier nearly
    free; does RC beat GenImmix on tail latency at memory parity) **is reachable after P1–P4** — it does not
    need the full correctness tail, consistent with prioritising the research question over the
    completionist grind. → RESEARCH_QUESTIONS **RQ1** (flagship); the LXR-fork study + the enumerated API diff +
    the touch-set are in `gc/mmtk/NOTES.md` (2026-06-25).
- **Scalability gap (open M8 work).** No multicore speedup-vs-cores data: the parallel/multidomain
  macro-benches are disabled, so there is no throughput-vs-domains curve. Re-enable them (or stand up the
  quick-panel/Sandmark-style scaling harness — task #36). Micro-benches already show ~2× on parallel
  alloc-heavy. → PERFORMANCE.md §1/§6; GitHub #7.

### Shipped (done — one line each; depth in NOTES)

- **bug #2** — native unmarshalling allocated off-heap (intern path was `#ifndef NATIVE_CODE`); un-guarded so native interns via MMTk. (CI ocamldoc SIGSEGV resolved; manpage repro 0/12.)
- **bug #3** — MMTk STW stop barrier was a no-op (blocking-section counter underflowed `usize`); fixed by passing the domain to `caml_mmtk_enter/leave_blocking`.
- **bug #3b** — multidomain spawn/STW deadlock: replaced the global stop-counter with an MMTk-native per-mutator RUNNING set (`stop_all_mutators` waits for `running.is_empty()`); also fixed an MMTk-STW × OCaml-minor-STW deadlock + a terminate corruption; removed `park_terminating`. native `domain_dls` 14/30 hang → 30/30.
- **bug #4** — `gc_regs` bucket leak on OOM-raise inside `caml_call_gc` (RESTORE_ALL_REGS skipped); `caml_mmtk_recycle_gc_regs_bucket()` before the raise. 25/25→0/25.
- **bug #1** — `is_forwarded` read forwarding-bits metadata that non-moving plans don't map; register that spec only for `moves_objects && !needs_forward_after_liveness`.
- **moving-GC root cause** — forwarding-pointer / `Infix_tag` collision in `slot.rs` `classify` (consult forwarding-bits side metadata before trusting the header).
- **GC-mid-`intern_rec`** — unmarshaller ran a GC through raw un-rooted C pointers; suppress collection across unmarshal via `is_collection_enabled`.
- **#3 native write barrier** — `caml_modify`/`caml_initialize` were `#ifdef NATIVE_CODE` no-ops; now unconditional, self-gated on `caml_mmtk_generational`.
- **#6 minor-heap arena removed** — `allocate/free/reallocate_minor_heap_arena` deleted; `young_*` bootstrapped from MMTk via `caml_mmtk_refill_tlab`.
- **#7/#7b Gc.stat** — heap fields reimplemented on MMTk stats; collection counts via forced MMTk GC; `Gc.minor_words` accounting fed from the alloc fast paths.
- **#8 Is_young reservation retired** — `Is_young` always-false under TLAB; folded to a constant; reservation + STW machinery removed; header-colour audit (no live liveness read; `memprof.c:1558` flagged).
- **caml_mmtk_enabled removed** — ~35 dual-path sites collapsed to unconditional MMTk (153 lines).
- **shared_heap.c / .h deleted** — −1665 lines; colour-machinery/`caml_atom`/`caml_compactions_count` relocated to `major_gc.{c,h}`; `mmtk.c` link anchor keeps `roots.o` (`caml_do_roots`) linking.
- **#15 — non-Immix plans wired (bytecode)** — `SemiSpace`/`GenCopy`/`MarkCompact`/`PageProtect` added behind the generic forwarding-spec gate (taking the bytecode total to 9 at the time; ConcurrentImmix later via #8 → 10 wired, 1 deferred); CLBG byte-identical.
- **#16 — native GenImmix + GenCopy** — generalized the TLAB refill to alias the copy-nursery `BumpPointer` (not just an in-place Immix block); the moving-root fixup was reused from the major/defrag path (no minor-vs-major root path → 2-file change). GenImmix = the stock-faithful generational default. old→young pointer A/B + `sanity` (3.05M copied, 0 Invalid) clean.
- **linux-O0 debug runtime** — stale stock-GC asserts removed; a real domain-terminate lock-drop race fixed (`caml_mmtk_park_terminating` parks without dropping `domain_lock`).
- **opam relocatability** — `libmmtk_ocaml.a` referenced as `-lmmtk_ocaml` (symlinked into `stdlib/`, installed into `$(LIBDIR)`); DWARF build-root stripped via `--remap-path-prefix`. `test-in-prefix` exit 0.
- **CI hygiene** — x86-64 Build green; CLBG cross-plan correctness gate green; all-plans testsuite workflow (deliberately red — surfaces per-plan breakage).

---

## GC plans

The binding is *moving-ready* (forwarding-pointer spec, pinning bit, updatable slots,
`copy`/`copy_to`) and **generic over the plan**: `mmtk_ocaml_init` passes `MMTK_PLAN`
straight to mmtk-core (no hardcoded allowlist); the forwarding-bits side-metadata spec
is registered **iff** `moves_objects && !needs_forward_after_liveness`. So wiring a new
bump-pointer plan is mostly validation, not trait code. mmtk-core 0.32 offers 11 plans;
**10 are wired**, 1 deferred (Compressor). The `Testsuite (all GC plans)` CI workflow
(`.github/workflows/testsuite-plans.yml`) runs the suite under all 11 to surface
per-plan breakage; CLBG `run.sh validate` is the byte-identical cross-plan gate.

| Plan | Kind | Moving | Wired up? |
|------|------|:------:|:---------:|
| `NoGC` | bump-pointer, no collection | no | ✅ (byte + native; native moot — never reclaims) |
| `MarkSweep` | free-list mark-sweep | no | ✅ (bytecode) |
| `Immix` | mark-region w/ opportunistic defrag | yes | ✅ (byte + native) |
| `StickyImmix` | Immix + sticky mark-bit (gen, in-place nursery) | yes | ✅ (byte + native) |
| `GenImmix` | generational, copying nursery + Immix mature | yes | ✅ **default** (byte + native) |
| `SemiSpace` | classic copying (two spaces) | yes | ✅ (byte + native) |
| `GenCopy` | generational, copying nursery + SemiSpace mature | yes | ✅ (byte + native) |
| `MarkCompact` | Lisp-2 mark-compact | yes | ✅ (bytecode; native **infeasible** — VO bit + reserved header word) |
| `PageProtect` | debug — page-granularity alloc | no | ✅ (bytecode, manual — exceeds CI time cap) |
| `Compressor` | bitmap mark-compact | yes | ❌ **deferred** (unified obj-ref model) |
| `ConcurrentImmix` | concurrent non-moving Immix, SATB | no | ✅ (**byte + native**) — SATB; `lazy`-clean; **Q3 fixed** |

**Native** runs **7 plans** — `Immix`/`StickyImmix`/`ConcurrentImmix` (in-place Immix-block TLAB),
`GenImmix`/`GenCopy` (copy-nursery `BumpPointer` TLAB), and `SemiSpace`/`NoGC` (also `BumpPointer` Default) —
i.e. every plan whose Default allocator is a bump/Immix region the inlined TLAB can alias; the moving-root fixup is reused
from the major path. `MarkSweep` (free-list), `MarkCompact` (per-object VO bit + reserved Lisp-2 header
word the gapless TLAB can't produce), and `PageProtect` abort at startup on native (bytecode-only).
`GenImmix` is the stock-faithful generational plan and the **default** (it replaced Immix as default — OCaml's
short-lived-allocation profile favours a copying nursery; see `PERFORMANCE.md`).

**`Compressor` (deferred) and `ConcurrentImmix` (landed bytecode, open work #8):**

- **`Compressor` — needs a unified object-reference model.** Its bitmap mark-compact
  assumes the object reference *is* the object start; OCaml puts the value reference at
  field 0 with the header one word before (`OBJECT_REF_OFFSET = WORD_SIZE`), so there is
  no single "reference == object start" identity. Supporting it means redesigning the
  object model. (The all-plans CI gate greps for its `requires a unified object
  reference` abort.) → NOTES #15 (2026-06-23).
- **`ConcurrentImmix` — SATB write barrier (RQ1 flagship; landed bytecode + native).** The high-value
  low-latency line (`RESEARCH_QUESTIONS.md` RQ1). The SATB deletion barrier is wired (~82 lines)
  by re-using OCaml's *stock* SATB-shaped barrier (its major barrier is already Yuasa) via
  mmtk-core's slot-granularity `memory_region_copy_pre`, gated on the concurrent plan — inert
  off it. **`lazy` is clean**, and **Q3 (continuation scan vs resume) is fixed** (per-continuation lock +
  resume SATB-snapshot, commit `55ab6ce40b`; FAQ Q2/Q3). **Native validated** — the expected gaps were
  already closed (native uses the out-of-line `caml_modify` extcall; mmtk-core auto-allocate-blacks acquired
  lines — which also makes **no-zero (RQ8) safe + enabled on ConcurrentImmix**), plus an atomics-SATB-ordering
  bug found + fixed (`d0c721a8b7`). **Open (perf):** the UNLOG-bit gate is **de-prioritized** (native
  characterization: the SATB barrier is ~free, <0.1% self — empirically a non-issue); the sanity-build ~10 MB
  deadlock (`rr`) remains.
  → open work #8; RESEARCH_QUESTIONS RQ1; FAQ Q1–Q4; NOTES (2026-06-23).

---

## Key implementation map (pointers for a cold start)

- **Binding** (`gc/mmtk/`):
  - `binding/src/{lib,api,object_model,active_plan,collection,scanning}.rs` —
    VMBinding impls + C ABI (`mmtk_ocaml_*`) + domain registry.
  - `common/src/{header,object_model,scanning,slot}.rs` — version-independent value
    layout, `scan_ocaml_object`, `FieldSlot`.
  - Moving correctness lives in `common/src/slot.rs`: `FieldSlot` caches an `info`
    (NOT_TRACEABLE / 0 / infix byte-offset) at scan time so `store` can re-derive an
    interior pointer as `new_parent + offset` after the parent is forwarded; `classify`
    consults forwarding-bits side metadata before trusting an `Infix_tag` header.
  - STW coordination: `collection.rs` — a per-mutator **RUNNING set** (`stop_all_mutators`
    waits for `running.is_empty()`); STOPPED→RUNNING via `caml_mmtk_become_running`, which
    parks cooperatively via the backup thread if a GC is active; a terminate fence on
    deregister. (Replaced the old global stop-counter — bug #3b.)
- **Runtime glue** (`runtime/`): `mmtk.c` + `caml/mmtk.h` (init, alloc, STW poll/park,
  blocking-section + termination hooks, native TLAB refill), allocation redirection in
  `caml/memory.h` (`Alloc_small`) and `memory.c` (`alloc_shr`), TLAB refill / minor-GC
  bypass in `minor_gc.c` and `domain.c` (`caml_poll_gc_work`), root publishing in
  `interp.c`, per-domain mutator + STW in `domain.c`, blocking sections in `signals.c`,
  unmarshaller routing in `intern.c`. `mmtk.c` compiles into both runtimes.
- **Build**: GNU make (top-level `Makefile`); `Makefile.mmtk` builds the Rust staticlib
  and links it. `make -j` for parallel builds.

## Known caveats / lessons

- Every allocation path must reach MMTk (the M2 torture crash was the unmarshaller
  bypassing the hooks → globals untraced → swept-and-reused; the bug #2 native intern
  off-heap crash was the same class).
- `FieldSlot::load` filters pointers outside MMTk spaces (atoms, code, pre-enable
  objects) via `is_in_mmtk_spaces`.
- Every `Is_young`-gated barrier-elision in the runtime was suspect under MMTk
  (`Is_young` is identically false) — the `array.c` `caml_uniform_array_make` barrier was
  one such latent bug (fixed); the reservation is now retired (#8).
- GC worker `tls = Address::from_usize(1)` sentinel is fine for now (`is_mutator` uses
  registry lookup); revisit only if it bites a copying plan.
- Architecture: **MMTk owns the entire heap** (all-MMTk). There is no separate OCaml
  minor GC — young objects live in MMTk's own heap; the only stop-the-world is MMTk's.
  The vanilla-minor + MMTk-major intermediate was superseded, not just deferred.
- Debugging: `turing` (Linux) has rr + gdb; `sanity` at a small heap for moving-GC
  bugs; `setarch -R` for the ASLR/metadata-mmap flake. See `gc/mmtk/NOTES.md` and
  `CLAUDE.md`.
