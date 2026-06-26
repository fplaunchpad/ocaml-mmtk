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
   threads=8): hang **52.5% → ~1%**, `sanity` clean. **The ~1% residual is FIXED (2026-06-25,
   rr-confirmed) — and it was NOT a bug#3c rendezvous residual at all:** it is the GH#14
   `scheduler.rs` STW-only assert firing **plan-independently** at high domain count (≥8 domains:
   a second domain's alloc poll or a domain being *created* refilling its TLAB requests a GC while
   one is in progress → GC worker panics → poisoned `WorkerMonitor` → all workers die → every domain
   deadlocks in `park_until_resumed`). rr on a 28-core box made `par_binarytrees` d8/GenImmix a 100%
   repro and the backtrace showed the panic. Fix: **remove the assert for all plans** (mmtk-core
   `ec2f5079f8`; the earlier `d2e7f3493b` only gated it off for *concurrent* plans, so STW plans still
   tripped it). Validated: d8 pinned 100% HANG → 5/5 OK, checksums golden. The separate **bug #57**
   (`active_plan.rs:59` "cannot trace object") may still appear at threads=8. → NOTES (2026-06-25).
   **GH#2 (the rare burn-pattern hang issue) CLOSED 2026-06-26** — Phase 3 structurally eliminated the
   dual-STW deadlock class (no OCaml all-domains STW left); 196 burn/spawn runs across StickyImmix/GenImmix/Immix
   clean, incl. 96 of the formerly ~50%-hang repro. → NOTES (2026-06-26).

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
   - Re-enable candidates — **verified 2026-06-25 (the "not supported" reasons were stale, pre-M6 blanket
     "tabled" disables; finalisers are default-on).** Result: `callback/test_gc_alarm.ml` **RE-ENABLED**
     (passes native + bytecode, 8/8 robust). `callback/test_finaliser_gc.ml` and
     `basic-more/simplif_under_lambda.ml` **stay disabled** but with *accurate* reasons now — not "missing
     support" but **finaliser-timing diffs**: test_finaliser_gc's bytecode variant fires the finaliser later
     than stock's minor-GC point (output order differs; native matches), and simplif_under_lambda's
     `finalise_last` does not fire on the test's `Gc.full_major` under MMTk. (Lesson: the doc-based guess was
     1/3 — verify before re-enabling.)

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
   - **GC worker pool — default is `nproc` (force-1 tried 2026-06-24, then REVERTED).** The `nproc` default
     makes every worker park/wake on every collection (~82% of GC-worker CPU in futex contention), and forcing
     **1 worker** is ~1.37× faster on single-domain minor GC — but that was reverted as a band-aid: worker
     count does **not** fix multi-domain throughput scaling (STW-bound, not pool-bound), so the default stays
     mmtk-core's `nproc` (`api.rs:147`). Set `MMTK_THREADS=1` yourself for the lowest-overhead single-domain
     runs. Intended policy "workers = running domains" is a gc/mmtk-core-fork follow-up (the pool is fixed at
     init). Also pending: **#G1** (the binding does full major-root scanning every
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
   - **ConcurrentImmix scheduler-assert DEADLOCK — GH#14: assert FIXED 2026-06-25 (mmtk-core `d2e7f3493b`,
     submodule bumped); one separate chameneos remnant remains.** Distinct from the `#4` small-heap deadlock
     above. **It was a PANIC, not a livelock** (the earlier static "FinalMark-never-requested /
     heap-pressure forced-FinalMark" hypothesis was *falsified by actually running it*). On
     `spectralnorm`/`LU_decomposition`/`par_spectralnorm` it aborted with `"GC request sent to WorkerMonitor
     while GC is still in progress."` (`scheduler.rs:444`, `on_last_parked`) → poisoned `WorkerMonitor` mutex
     → all GC workers die → deadlock. **Fix (landed):** the STW-only assert is gated to non-concurrent plans
     (`get_plan().concurrent().is_none()`); the redundant `Gc` request is coalesced in `goals.requests[Gc]`
     and serviced after the in-progress GC (survives `on_current_goal_completed`) — NOT a forced FinalMark,
     STW plans byte-identical. **Validated:** before 15/15 HANG → after 15/15 OK at small heaps
     (32/64/128 MB) across spectralnorm/LU/par_spectralnorm, checksums byte-identical to golden under both
     ConcurrentImmix and GenImmix. **`#4` self-trigger EXONERATED by static trace** (not needing rr):
     `respond_to_requests` (which holds the self-trigger) runs only when `current()==None` (asserted at
     `scheduler.rs:512`), so it cannot reach the `:444` assert; the trigger is the **mutator allocation-poll**
     re-requesting a GC during the concurrent-marking window. **Remnant FIXED 2026-06-26 (mmtk-core
     `88ab2f5ea5`):** the `chameneos_redux` hang — AND the single-domain `spectralnorm`/`LU_decomposition`
     perf-size livelock the quick panel later surfaced — were the **same** bug: an **orphaned-SATB-packet
     lost-wakeup**. `schedule_concurrent_packets` disabled+closed the `Concurrent` bucket while marking was
     still active, so a resumed mutator's late SATB `add()` got no worker wakeup and `is_drained()`
     short-circuited true → FinalMark never fired → mutators parked forever. Fix: keep the bucket enabled+open
     while marking is in progress (concurrent-gated; non-concurrent plans byte-identical). Diagnosed on godel
     (the live hang is rarer than the panel estimate), integrated + validated locally: the panel's reliable
     repro — `spectralnorm 3000`/`LU 900` under ConcurrentImmix, which **reliably HUNG** at both worker counts
     — now runs **48/48 clean**. Evidence in NOTES (2026-06-26).

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
   **Verified phased plan now exists (NOTES 2026-06-25).** A workflow mapped **every** caller of
   `caml_try_run_on_all_domains`/`caml_empty_minor_heaps_once`, analysed each, and **adversarially
   refuted** each removability claim. Verdict: **`partial`** — yes, but only as ONE coordinated change
   re-homing EIGHT callers, not a grep-and-delete (the two functions are mutually load-bearing via the
   terminate participant-set contract). **Cleanly removable now (refuters 0/2):** minor-heap-resize cap
   (→ relaxed atomic) and global-major-slice (→ LOCAL `requested_major_slice`). **Conditional (only with
   the whole family):** the minor-empty/spawn/terminate trio. **Need a replacement primitive, NOT MMTk
   GC-STW:** runtime_events ring + frametables install are *legitimate non-GC* users — `stop_all_mutators`'
   callback is hard-wired to GC marking, so they need a new "stop RUNNING + run a VM closure" hook or a
   per-subsystem rwlock/epoch (exclusion = `RUNNING`). **Phases:** 0 (low) delete the two clean callers
   **— DONE 2026-06-25** (branch `excise-ocaml-stw`: `caml_update_minor_heap_max` → plain store +
   `stw_resize_minor_heaps_reservation` deleted; `caml_poll_gc_work` clears `requested_global_major_slice`
   locally + `stw_global_major_slice` deleted; bytecode-validated, par_binarytrees d1/d4/d8 checksum-stable;
   `caml_try_run_on_all_domains_async` now dead-but-retained for Phases 1/3 — NOTES 2026-06-25);
   1 (med) re-home the domain-LOCAL minor-STW bookkeeping onto the safepoint/resume path **— DONE 2026-06-26**
   (`d556fb6ea6` + `4677c9b580`: new `caml_minor_gc_domain_bookkeeping` wired at the bytecode safepoint
   (bump=1, single per-minor-GC count bump; native stays 0) + terminate (bump=0); STW handler stripped to
   `{leader cycle bump; promote}`; `promote` kept for its load-bearing `caml_reset_young_limit` (the bytecode
   safepoint poison) but stripped of stats-sample + the `minor_gc_end_barrier`; dead `caml_mark_roots_stw`
   branch removed. Re-home to the *triggering* domain only is multi-domain-safe (ephe_ref/finaliser vacuous via
   `Is_young==0`; custom table read-never + self-bounded; memprof/stats per-domain + self-refreshed). Validated:
   clean world.opt, par_binarytrees d1==d8==golden, Gc.minor exactly-once, mmtk sanity small-heap clean —
   NOTES 2026-06-26); 2 (med) re-home
   frametables + runtime_events. **Sub-steps 1+2 DONE 2026-06-26** (`3d2eef30a5` ragged-epoch
   `caml_mmtk_quiesce_running_domains` primitive — NOT a second barrier, deadlock-class-neutral; `e06e717e51`
   runtime_events start=monotonic publish / stop=clear-enabled→quiesce→munmap, closing a munmap-vs-write_to_ring
   UAF; validated: native destroy+quiesce 10/10, lib-runtime-events A/B zero new failures, golden + counter
   neutral). **Sub-step 3 (frametables) DONE 2026-06-26** (`3c55e9a6ab`): GC-cycle RCU —
   Dolan's original 2018 frametable design (`e91cea84e30`), re-keyed off the dead `caml_major_cycles_completed`
   onto `mmtk_ocaml_gc_count()` (the upstream multicore STW #11980 was a transitory perf fix that explicitly
   invited this successor). Immutable {mask,descriptors} snapshot, atomic publish, lazy chain-retire after a
   collection; closes the latent ConcurrentImmix worker-vs-installer hazard. Validated: world.opt + par_binarytrees
   golden + lib-dynlink-domains/native/initializers (GenImmix) + sanity small-heap clean. **PHASE 2 COMPLETE.**
   (Validation surfaced GH#15, a pre-existing multi-domain GenImmix deadlock, now FIXED — see below.)
   **PHASE 3 COMPLETE + MERGED to mainline `5.5+mmtk` @ `7b5ebf0934` (2026-06-26; net −478 lines).**
   3a/3b/3c deleted the whole all-domains STW family (`caml_try_run_on_all_domains*`, `stw_request`/
   `stw_leader`/the global-barrier impl, `stw_terminate_domain`, the minor-empty STW chain); MMTk's
   `stop_all_mutators` is now the **sole all-domains rendezvous**. Kept (as plain spawn/terminate state, not
   barriers): `all_domains_lock`, `stw_domains`, `young_limit`-poison, the backup thread (systhreads-entangled
   — deletion deferred to #20). **GH#15 was the gating blocker and is FIXED** (it was 3 bugs: Bug A 4-way
   lock-order deadlock `73f780c5`; Bug B root-scan UAF/TOCTOU `77ebfd3e` = B1 ml_values RCU-retire + B1′
   `FieldSlot::load` re-validation; B2 panic→abort safeguard `55007a90`). turing: 0 trace panics (was ~11/12),
   mmtk `sanity` 24/24 clean. Fixing Bug B unmasked the **pre-existing** #31/GH#3 `Domain.join` result-UAF
   (~9% on 28-core joinstorm) — equally on mainline, so Phase 3 is a strict improvement; merge shipped, #31
   tracked separately (GH#3 reopened; harden the result-handoff for high-core load). → NOTES 2026-06-26.
   Phase 3 **structurally eliminated the bug#3c/dual-STW deadlock class** (no second barrier for a
   terminating RUNNING domain to lead) — but **not** the separate ConcurrentImmix chameneos
   continuation-scan hang. Kept (as plain spawn/terminate state, not barriers): `all_domains_lock` +
   `stw_domains` (now a plain spawn/terminate mutex) and the `young_limit`-poison (MMTk's STW reuses it);
   the backup thread is systhreads-entangled, so its deletion is deferred to **#20**. The multi-domain exit
   caller (`caml_stop_all_domains`) now `remove_running`+deregisters each cancelled peer so the sole
   rendezvous never hangs on a dead thread (the unjoined-domains-at-exit path shipped in Phase 3b).
   → NOTES 2026-06-26; RQ10 pole-A; #20.
   **Not a local cleanup — it is an architecture decision** (which generation the framework owns) that must
   be **reconciled structurally with how other runtimes do minor collection** (OCaml ParMinor, GHC local
   heaps, Erlang per-process heaps, the Julia/CRuby MMTk bindings), and weighed against the **inverse**
   option — keep stock's *scalable* minor and use MMTk **major-only**. Both directions, and the empirical
   motivation (the fork's multi-domain anti-scaling), are written up as **RESEARCH_QUESTIONS RQ10** /
   `SCALABILITY.md`.
   **RQ10 pole-B (the inverse): feasibility DONE + go/no-go experiment RUN → NO-GO (NOTES 2026-06-25).**
   Feasibility: FEASIBLE against a non-generational **Immix** major with **zero mmtk-core changes** (mutator
   `Default` maps to mature Immix; promotion = `mmtk_ocaml_alloc(.., Default)`; barrier clean — under Immix
   `caml_mmtk_generational==0` so MMTk's nursery barrier is off and stock `ref_table` is reusable), and **NOVEL**
   (no MMTk binding keeps a VM nursery in front of an MMTk major). **But the experiment kills the motivation —
   the residual is MILD and pole-B doesn't fix it** (clean mainline re-run, turing 28-core, pinned,
   `MMTK_THREADS=domains`, par_binarytrees d21; NOTES 2026-06-25). On a clean build all plans are mildly
   sublinear: **GenImmix-default S(8)=1.23, StickyImmix 1.36, GenImmix-256 MiB 1.59** — not the dramatic figures
   from the first (contaminated) church run. **Two levers:** (1) **nursery size** — GenImmix-256 MiB is ~20%
   faster at every domain count + S(8) 1.23→1.59 (commit-on-demand, ~free for small programs) → the cheap win,
   #21; (2) but even 256 MiB (28–86 GCs) still **regresses d4→d8**, so the residual *slope* is the per-collection
   all-domains STW cost — fixed by **off-STW marking (ConcurrentImmix)**, NOT by pole-B (which keeps the minor
   STW). **→ Pole-B NO-GO.** A real **BUG B** also surfaced (mainline-confirmed): `MMTK_NURSERY="Bounded:2m,64m"`
   silently parse-fails (raw bytes only) → #21. (A first-pass "BUG A: degenerate default install, 913 GCs" was a
   **church `fix/bug3c-cross-stw` build artifact** — clean mainline on local + turing gives 114 GCs; check before
   merging that branch, but it is not a mainline bug.) → see item below.

10. **#19 — Testsuite triage: fix every failure, or disable it with a greppable marker (ongoing).** Work
    through the remaining testsuite non-pass (M7: ~1450/1547 pass under Immix/StickyImmix; the ~97 non-pass
    are not yet individually triaged). For **each** failing test: either **fix** it, or **disable** it with a
    single canonical marker as the file's first line, replacing the `(* TEST *)` block (so ocamltest skips
    the file) —
    ```
    (* MMTk DISABLED: <reason> *)
    ```
    so that **`grep -rn 'MMTk DISABLED' testsuite/tests`** enumerates every intentionally-disabled test and
    why. **Convention is live: 9 files normalized to the marker** (was ad-hoc `Disabled under MMTk …`).
    Current disabled set by category: **stock-GC pacing / `Gc.stat` counters** (`lib-systhreads/boundscheck`,
    `lib-bigarray/subarraystub`, `parallel/major_gc_wait_backup`); **no-stock-minor-heap finaliser/lazy
    timing** (`weak-ephe-final/finaliser2`, `lazy/minor_major_force`); **stock compaction semantics**
    (`compaction/test_compact_full`); **unsupported finalisers/alarms** (`callback/test_finaliser_gc`,
    `callback/test_gc_alarm`, `basic-more/simplif_under_lambda` — the latter three are #12c **re-enable
    candidates** now that finalisers work under `MMTK_WEAK_REFS=1`). The **bulk** of the non-pass is still
    untriaged — that is the work: run `make -C testsuite parallel` under a plan (per `CLAUDE.md`), classify
    each failure (real MMTk gap vs known-unsupported vs flaky), fix or mark. → M7; item #4 (#12c).

11. **#20 — retire the per-domain backup-thread machinery (replace STW participation with lock
    acquisition). May be tricky.** Vanilla OCaml gives every domain a **backup thread**
    (`runtime/domain.c` `backup_thread_func`, `interruptor`) whose *sole* job is: when a mutator
    **releases its domain lock** (blocking C section, park, idle), someone must still be able to answer an
    all-domains STW request on that domain's behalf. Under always-on MMTk this is largely **redundant
    already** — the binding's RUNNING set (`collection.rs`) makes a domain that released its lock STOPPED,
    so MMTk's `stop_all_mutators` does not await it (it is the GC *worker pool*, not a domain rendezvous,
    that drives the pause). The backup thread survives only because OCaml's *own* STW rendezvous
    (spawn/terminate; the neutered minor STW) is still live (ties to #18's "big deletion" + RQ10 pole-A).
    **The idea (KC):** now that we have GC threads, a cleaner mechanism than "interrupt the domain → its
    backup thread answers" is: **a domain that wants to GC simply ACQUIRES the released domain lock (and
    any other such locks) before handing over to MMTk** — a domain that has released its lock is by
    construction not mutating, so the collector just needs to *hold* that lock (so it can't re-enter) and
    proceed. This would let us **delete the whole backup-thread + interruptor machinery** (which was also
    the surface the bug#3c / GH#6 deadlocks lived on — see the rr trace's 4 idle `backup_thread_func`
    threads). **Tricky parts:** the domain-lock + STW handshake is subtle (lock-ordering vs MMTk's STW and
    OCaml's spawn/terminate rendezvous; who holds which lock across a collection; re-entry of a domain that
    re-acquires its lock mid-GC — the same STOPPED↔RUNNING edge as GH#6); and it is gated on MMTk's STW
    becoming the sole rendezvous (RQ10 pole-A / #18). → relates to #18, RQ10, GH#6; RESEARCH_QUESTIONS RQ10.
    **Cross-runtime evidence backs the deletion (NOTES 2026-06-25):** every comparable runtime
    (HotSpot/mmtk-openjdk, mmtk-julia, mmtk-ruby, GHC, Go, CoreCLR) has **ONE** GC-owned rendezvous and
    handles a blocked/native thread with a **thread-state FLAG** (`_thread_in_native`, `JL_GC_STATE_SAFE`,
    `_Gsyscall`, released-capability, preemptive-mode) the GC skips without waiting — **not** a per-domain
    backup thread. OCaml-MMTk's backup thread is the outlier; the binding's `RUNNING` set is already exactly
    that flag (a domain that released its lock is absent → not awaited; `mmtk_ocaml_try_mark_running` is the
    return-edge re-check). Deleting the backup thread = **Phase 3** of the #18 verified plan.

12. **#21 — nursery findings (spin-off of the RQ10 pole-B experiment); ONE clean fix (BUG B), the rest needs
    care.** Investigated 2026-06-25 (turing + local mainline). Findings, with the important caveat that the
    nursery-cap win is **fixed-heap-only**:
    - **(a) Raise the default cap 64→256 MiB — DEFERRED; helps only with a fixed large heap, MOOT under the
      default dynamic heap.** With `MMTK_HEAP_SIZE_MB` pinned large (turing, 4 GiB), 256 MiB beats 64 MiB on
      par_binarytrees d21 (~20% faster, S(8) 1.23→1.59, GCs 114→28). **But under the DEFAULT (dynamic
      space-overhead) heap the cap is moot** — confirmed local: binarytrees-19 gives 281 GCs @256 MiB vs 285
      @64 MiB (≈same), because the *space-overhead trigger* (live×2.2), not the nursery cap, limits collection
      frequency. So raising the cap does not help the plan as users actually run it. Implemented + reverted on a
      throwaway branch; do NOT land without the full quick-panel memory-parity validation (moderate-live
      workloads at dynamic heap could see the nursery approach the cap and raise RSS — untested).
    - **(b) BUG B — `MMTK_NURSERY` suffix syntax is broken (mainline-confirmed); the one CLEAN fix.**
      `Bounded:2m,64m` (documented in CLAUDE.md/README) silently parse-fails (*"Can't parse value. Default value
      will be used"*) → falls back to mmtk-core's default; only raw bytes (`Bounded:2097152,67108864`) parse.
      Reproduced on clean local mainline. Fix the parser to accept `k/m/g` suffixes, **or** correct the docs to
      raw-byte syntax (cheap, do this).
    - **(c) The REAL default-condition lever (open, now QUANTIFIED): the dynamic-heap floor under-provisions
      low-live/high-alloc workloads.** The space-overhead heap (live×2.2) is sized to the *live set*, so a
      low-live workload gets a tiny heap → tiny nursery → constant collection. Measured (spectralnorm-3500,
      GenImmix, local): **default dynamic heap = 7926 minor GCs / 3378 ms GC time / 25.83 s wall** vs **fixed
      4 GiB = 152 GCs / 81 ms / 22.62 s** — i.e. the dynamic heap does **52× more GCs** and adds **~13% GC
      overhead / ~12% wall** (no vanilla needed — the fixed-heap A/B isolates it; spectralnorm is otherwise
      compute-bound). The fix is a **nursery floor / minimum dynamic-heap size decoupled from the (tiny) live
      set** for high-alloc-rate workloads — NOT the nursery cap (which is heap-limited here). Real but modest;
      the clean default-relevant nursery work.
    - **NOT a mainline bug: "degenerate default install / 913 GCs"** was a **church `fix/bug3c-cross-stw` build
      artifact** — clean mainline (local + turing) gives 114 GCs (correct 64 MiB). Check before merging that
      branch; not a mainline issue.
    - **Note the limit:** the nursery is a *level* lever, not a *slope* fix — even at a large fixed heap,
      GenImmix-256 MiB still regresses d4→d8, so the residual multi-domain sublinearity is the per-collection STW
      cost, addressed by off-STW marking (ConcurrentImmix), not the nursery. → RQ10; NOTES 2026-06-25.

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
    `BarrierSelector::FieldBarrier`, `Pause::RefCount` (~3–5 d, low risk) — **DONE 2026-06-25**: branch
    **`origin/0.32-ocaml-lxr` @ `f0319fb5e6`** on `fplaunchpad/mmtk-core` (563 insertions, purely additive,
    cargo-green + 470/470 mmtk-core tests; API-adaptation bridges noted in NOTES). **OCaml-build-VALIDATED
    2026-06-25** — the binding workspace builds green against `f0319fb5e6` (P1 is purely additive, so the
    binding API surface is unaffected); held on `0.32-ocaml-lxr` (not folded into `0.32-ocaml`). **P2** Immix-policy RC hooks +
    LOS RC (~1–2 wk, **highest risk** — moving-GC correctness; lean on the `sanity` feature at small heaps).
    **P2 port plan banked (agent, 2026-06-26):** ⚠ `lxr/lxr` is a **sibling fork, NOT a superset** — same 0.32.0
    merge-base, but its `immixspace.rs` (+872/−263) interleaves RC with three *unrelated* upstream waves
    (page-resource rewrite, `generate_tasks_batched`/`Range<Chunk>`, 1-arg `attempt_mark`/cyclic-mark rework)
    that conflict with our deltas (no-zero, SpaceOverheadTrigger, 2-arg `attempt_mark`). So **do NOT lift LXR
    function bodies — hand-write ~10 gated overlays** behind `rc_enabled`/`crate::args` consts (struct fields,
    constructor, `side_metadata_specs(rc_enabled)`, read-side `is_live`/`is_reachable`, inert guards
    `post_copy`/`mark_lines`/straddle), so all 8 existing plans stay byte-identical when off. **Prereqs:**
    `PlanConstraints.rc_enabled`, 5 RC side-metadata specs (`IX_LINE_REUSE_COUNT`, `LOS_PAGE_REUSE_COUNT`,
    `Block::{LOG_TABLE,NURSERY_PROMOTION_STATE_TABLE,PHASE_EPOCH}`), a `Defrag` rc arg. **P2.5 (split out, heavy):**
    the page-resource RC API (`BlockPageResource::{rc,prepare_gc,reset,acquire_blocks,exhausted_reusable_space,…}`)
    + the work-packet reshape — required before `prepare_rc`/`release_rc`/`get_next_available_lines`. (NOTES 2026-06-26.)
    **P3** port `plan/lxr/`; wire `MMTK_PLAN=LXR` in `api.rs` (~1 wk). **Usage recipe (validated vs mmtk-openjdk
    `lxr` + `wenyuzhao/lxr-builds`, 2026-06-26):** LXR is a pure **runtime plan selection** (OpenJDK:
    `-XX:ThirdPartyHeapOptions=plan=LXR`; us: `MMTK_PLAN=LXR` → `PlanSelector::LXR`) — the binding needs **no LXR
    cargo feature** (mmtk-openjdk's `default=[]`); the `lxr_*` features are mmtk-core *build-time* tuning,
    default-on (`RC_NURSERY_EVACUATION = !cfg!("lxr_no_nursery_evac")`, etc.). **LXR requires a FIXED heap** (no
    variable sizing — OpenJDK mandates `-Xms==-Xmx`), so P3 must require pinned `MMTK_HEAP_SIZE_MB` and skip our
    default SpaceOverhead dynamic heap for `MMTK_PLAN=LXR`. mmtk-openjdk `lxr` pins mmtk-core `wenyuzhao @ 304ce69d`. **P4** OCaml binding + runtime barrier —
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
- **opam relocatability** — `libmmtk_ocaml.a` referenced as `-lmmtk_ocaml` (symlinked into `stdlib/`, installed into `$(LIBDIR)`); DWARF build-root stripped via `--remap-path-prefix`. `test-in-prefix` exit 0. **(Superseded by GH#10's object-bundling fix `35bf263b3a`: the staticlib objects are now bundled into `lib{asm,caml}run*.a`, so there is no bare `-lmmtk_ocaml` in `*_c_libraries` — GH#10 closed 2026-06-26.)**
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
