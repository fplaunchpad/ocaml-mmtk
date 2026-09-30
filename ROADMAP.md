# ocaml-mmtk roadmap

The living plan for `ocaml-mmtk`, meant to be picked up cold in a fresh session.
**Where we are:** the engineering bring-up (M0–M7) and the M9 stock-GC excision are
done; **M8 (benchmark + optimise) is the open milestone.** The correctness/perf tail
below is deferrable engineering; the agenda in
[`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) drives priority.
**Correctness status (2026-09-30).** Five fixes merged on 2026-09-29/30: the default plan's silent
corruption from native `Array.fill` and `No_sharing` unmarshalling (item 18, GH issue 28; present since
2026-06-24) is fixed on mainline as of `240b6c8f62`; LXR's wrong results (item 17, GH issue 26), the
systhreads RUNNING-set bug (item 19, GH issue 24), the dynamic heap's pending-demand growth
(item 20(a)) and the domain-creation interrupt-word race (item 29, GH issue 37, `6865b559ed`) are fixed
too. **Combined run** (re-run independently, macOS arm64, dynamic heap, tree built in place at
`da5f51ffba`): full testsuite GenImmix 1446 / 0, Bactrian 1446 / 0, Immix 1446 / 0; CI on `da5f51ffba`:
Build green including both debug jobs, all-plans green for GenImmix, Immix, StickyImmix, SemiSpace and
ConcurrentImmix, red for Bactrian (item 26), GenCopy (item 15) and LXR (item 21). **Later on 2026-09-30**
ocaml-mmtk PR 41 (merge `3fbe544984`) made explicit `Gc` requests synchronous (item 22 FIXED) and pinned
mmtk-core `95b425a27d`, which carries mmtk-core PR 4 (`37d0143dd3`, `MMTK::is_collection_requested`) and
PR 5 (GenCopy keeps worker mature copy buffers across nursery GCs; item 15 FIXED). Full testsuite on
PR 41 (macOS arm64): GenImmix 1448 / 0 (also with the debug runtime), Immix, Bactrian and
ConcurrentImmix 1448 / 0 at 512 MiB; all-plans CI on `3fbe544984` (run 36668846658): every gating plan
green, GenCopy included (first time since the September merge), except LXR on its known set (item 21).
**LXR capacity is diagnosed** (item 21) and its **retention fixes are merged** (ocaml-mmtk PR 46,
merge `6bb7a1be36`; mmtk-core PR 6, `b566d1f5b7`, pinned at `7a36bbb0b5`). **Root-caused the same
evening and FIXED (merged; GH issues closed):** the near-OOM root-scan SIGSEGV (item 13, GH issue 49; PR 51,
`ce2dd86167`) and the GC-worker free abort under domain churn (item 28, GH issue 39, with `rr`; PR 52,
`0015fbf179`). **The macOS RSS
artefact (mmtk-core memset every fresh mapping) is fixed** (mmtk-core PR 7, `5454281016`, merged as `69b5ddf663`;
ocaml-mmtk PR 53, merged, `3846019997`) and the M4 space-time sweep re-run: the fronts moved 40–66 MiB left and now meet
vanilla's; verdicts unchanged (workstreams, "Space-time curves"; `RESULTS.md`). **Multi-domain scaling
diagnosed (2026-09-30 night, item 35):** the gap is per-pause cost, not the STW rendezvous — ≈ 1.36 µs
per copied object on chameneos (≥ 2× stock; one `malloc` per copy from narrow 2–4-object work packets) and
0.63 µs on binarytrees (wide packets; the per-copy writes), plus the E1 barrier-study counters contending
across domains (removed by PR 58, merged `ee744db495`: chameneos d=8 21.8 → 10.3 s). **First lever landed,
opt-in (2026-09-30 late):** a worker-local nursery closure (`MMTK_LOCAL_NURSERY_TRACE=1`, GenImmix) cuts
chameneos nursery pause time 29 % (d=1) and binarytrees' 4 %; full testsuite run pending. **Open:** LXR capacity and failures (items 21, 31 =
GH issue 44, 32 = GH issue 45), fork (item 25, GH issue 33), the Bactrian CI out-of-memory (item 26, GH
issue 36), one-off multi-domain crashes on CI (item 30, plausibly item 28), and item 28's
process-exit residual. Order: items 21/31, 26, 25, 30, 20(c), 32, 23, 24.

Companion docs: [`README.md`](README.md) (overview + build/run),
[`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) (what this platform is *for*),
[`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) (dated design notes, deferred investigations,
known-failure repros — **newest first; the depth behind every item below lives
there**), [`fork-handoff.md`](fork-handoff.md) (original rationale).

**Project shape.** This repo *is* the OCaml fork (base `5.5.0` final, branch
`5.5+mmtk`), distributed as `ocaml-mmtk`. The MMTk binding is in-tree at
[`gc/mmtk/`](gc/mmtk) and depends on `mmtk-core` 0.32 via our fork, the git submodule
`gc/mmtk-core` (`fplaunchpad/mmtk-core`, branch `0.32-ocaml`), which `gc/mmtk/Cargo.toml`
substitutes for the crates.io `mmtk` through `[patch.crates-io]`.
MMTk is **always-on and the only collector** — no opt-out; the stock minor *and*
major GC have been excised (M9). `MMTK_PLAN` selects the plan (default `GenImmix`).
Native code uses TLAB nursery-aliasing onto an MMTk bump/Immix region, so it requires a
plan whose Default allocator is a bump/Immix region (the nine:
`Immix`/`StickyImmix`/`ConcurrentImmix`/`LXR`, `GenImmix`/`GenCopy`/`Bactrian`, `SemiSpace`/`NoGC`); bytecode runs under any plan.
**`LXR`** is our reference-counting **research plan** (PLDI'22 RC-on-Immix): single-domain validated
(correct, sanity-clean, at memory parity with Immix; the field barrier is near-free on OCaml's
init-write-dominated code; a backup trace reclaims cycles) — **experimental; single- AND multi-domain
validated** (par_binarytrees D=1..32) as of 2026-07; **its results are provisional:** the silent
wrong-results bug (item 17, GH issue 26) is fixed on mainline since 2026-09-29 (PR 30, `c59f7851c3`),
but the fix exposed a capacity problem (diagnosed 2026-09-30; the retention fixes merged the same day,
PR 46, while block-granularity retention remains), and LXR still fails a set of tests and is unsafe for
`Weak`/`Ephemeron` (open items 21, 31 and 32), so every LXR time/RSS number must be re-measured; the post-merge abort (item 14) is fixed; requires a
pinned `MMTK_HEAP_SIZE_MB`; it runs in the all-plans testsuite workflow (`testsuite-plans.yml`) but is
**not** in the byte-identical CLBG cross-plan gate. Design/status in `gc/mmtk/NOTES.md`. Run
knobs: `MMTK_PLAN`, `MMTK_HEAP_SIZE_MB` (pins a **fixed** heap; the default is now a
**space-overhead** heap — `heap = live × 2.2` after each full GC, à la stock's `Gc.space_overhead`,
clamped 32 MiB..RAM; replaced MemBalancer, whose sqrt rule under-provisioned big live sets — binarytrees
3.5× → 1.27× slower than stock. Floor raised 16→32 MiB (GH#6): a 16 MiB floor let a nursery GC fire during
matmul's matrix-build phase, promoting the half-built result matrix → the O(n³) compute loop then paid the
generational write barrier on every write (matmul-768 19.6s → 3.1s once the build stays in-nursery);
tunable via `MMTK_MIN_HEAP_MB`), `MMTK_RC_RETAIN` (LXR retention accounting, default off), `MMTK_NURSERY` (default bounded 2–16 MiB × live domain count; the max was 64 MiB until
2026-08-12, `e41c5383b2`), `MMTK_VERBOSE`;
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
| M8 | **Benchmark + optimise** vs. the stock GC — **the open milestone.** First native sweep: parity-or-better on 5 of 6 CLBG benchmarks (~1.5× faster on parallel alloc-heavy), one structural outlier (spectralnorm ~1.74× — MMTk's eager zero-fill double-write). Obvious-removal levers ~neutral (only C1-sftbound ~+1%); GenImmix-default validated. Method: `PERFORMANCE.md`. (These first-sweep figures are historical. In the 2026-07-02 quick panel (in `README.md` up to `358ea7958c`, section "Performance (quick panel)"; a 2026-07-02 build, before PR 23 and the 16 MiB nursery default), spectralnorm was 0.96× vanilla under GenImmix; in the 2026-09-30 M4 re-baseline (`RESULTS.md`) it is 1.00× the time at 74 vs 5 MiB RSS.) | 🟡 **open milestone** |
| M9 | **MMTk-only: excise the stock GC** — always-on; stock minor + major GC deleted; `shared_heap.c`/`.h` deleted (−1665 lines, live colour-machinery relocated to `major_gc.{c,h}`); per-domain minor-heap arena removed; `Gc.stat` reimplemented on MMTk stats; `Is_young` reservation retired. `ocaml-mmtk` is a single-GC runtime. **Complete** bar #11 (weak-clear semantics) + the flagged `memprof.c` colour read. Stage/bug depth: `gc/mmtk/NOTES.md`. | 🟢 done |
| — | Parallel collection: verified correct; marking scales ~8.4× on 16 threads (parallel-friendly heaps) | ✅ |
| — | **GC plans:** 12 wired (bytecode: 10 stock mmtk-core plans + our `Bactrian` and `LXR`), 9 native, 1 deferred (Compressor) — see the GC plans table below | 🟢 |

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
   (`weak-ephe-final/weaklifetime.ml` weak-clear timing under the generational plans
   is FIXED — GH#5, NOTES 2026-06-29.) → NOTES M6 entries.

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
     uprobes until #R1–#R4 land (GH#20). Host `turing`: set governor=performance + `opam install
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
     gone; the residual 1.27× is the per-collection copy cost, not the heap). RSS ≈ 4× live was
     read as Immix *fragmentation* (a separate defrag lever) — **corrected 2026-09-30:** on macOS most of
     that gap was mmtk-core's zero-filled side metadata (fixed; workstreams, "Space-time curves"). Deeper finding: **MMTk's per-minor-GC cost ≈ 4× stock's**
     (~1.2 ms vs 0.3–0.4 ms/collection at a 2 MiB nursery) — every nursery collection goes
     through the full STW + GC-worker + work-packet machinery vs stock's inline on-mutator
     Cheney copy. Being profiled on turing to attribute the floor. The nursery should be an
     **adaptive survival-rate (~10%) controller with a per-GC-cost amortization floor** (not
     heap-proportional, not a fixed cap) — future work, isolated as an opt-in *mode* (a
     trigger/policy feature, not a new plan); keep a frozen baseline config. → NOTES 2026-06-24.
   - **GC worker pool — default is `nproc` (force-1 tried 2026-06-24, then REVERTED).** The `nproc` default
     makes every worker park/wake on every collection (~82% of GC-worker CPU in futex contention), and forcing
     **1 worker** is ~1.37× faster on single-domain minor GC — but that was reverted as a band-aid: worker
     count does **not** fix multi-domain throughput scaling (per-pause cost, not pool-bound; item 35), so the default stays
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

9. **#18 — stock-GC dead-code tail (M9 cleanup; mostly load-bearing).** A second, adversarially-verified
   audit (2026-06-29, 118 agents) found **~650 removable lines** (zero-caller remembered-set machinery,
   dead major-GC/opportunistic-slice fns, runtime_events phantom spans, the dune subsystem); full
   item-by-item report + the load-bearing keep-list in [`gc/mmtk/STOCK_REMOVAL_AUDIT.md`](gc/mmtk/STOCK_REMOVAL_AUDIT.md);
   the low-risk clusters are being deleted on branch `remove-dead-stock`. The earlier audit (2026-06-24)
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
   **#31/GH#3 UPDATE (2026-06-29, partial fix MERGED):** the `Finished(...)` result is now published into a
   `ml_values->result` *generational global root* (scanned by every collection, park-timing-immune) + a bounded
   promote-retry — making promotion reliable where the prior coalescing `caml_mmtk_collect()` could return
   un-promoted (~9%→~4% on the 28-core storm, 0 hang; 40/40 local clean, goldens + weaklifetime green). **But
   instrumentation proved the residual is a SEPARATE, deeper bug:** the result is provably promoted+published
   (mature `v` written, `state_before=0x1`) yet the joiner still reads `term_sync->state==0x400` — a post-publish
   corruption of the correctly-published mature slot (cross-domain `caml_modify` + GC mis-forward/remembered-set).
   **#31/GH#3 CLOSED (2026-07-02, `aa60e04407` / mainline `33ae0009f8`):** the residual was **remembered-set
   buffers lost at mutator deregistration** — the dying domain's un-flushed modbufs dropped old→young edges, so
   the next minor GC swept live young objects (the earlier exhaustive termination full-GCs had *masked* it: a
   full trace needs no remset). Fix: flush the mutator before deregistering. 20/20 joinstorm clean; unmasked by
   (and landed with) the allocation-paced-trigger work. → NOTES 2026-07-02.
   Phase 3 **structurally eliminated the bug#3c/dual-STW deadlock class** (no second barrier for a
   terminating RUNNING domain to lead). The separate ConcurrentImmix chameneos continuation-scan hang is
   **also FIXED** (GH#4 + GH#14 closed; mmtk-core `88ab2f5ea5` lost-wakeup fix at the Concurrent→FinalMark
   bucket boundary + `72ee627050` + cont_lock yield-spin; fork bump `3f6f10072`) — re-verified 2026-07-02
   (~30 chameneos runs clean incl. 955 concurrent GCs at 16 MiB, byte-identical checksum), so ConcurrentImmix
   is correctness-ready. (The residual effect/fiber bug is now **LXR-only**: an unguarded RC slot-unlog on
   mmap'd fiber-stack slots, `plan/lxr/rc.rs:221`/`:621` — see NOTES 2026-07-02.) Kept (as plain
   spawn/terminate state, not barriers): `all_domains_lock` +
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

10. **#19 — Testsuite triage: fix every failure, or disable it with a greppable marker (DONE — 0 untriaged).**
    **Complete (2026-06-29):** a full-clone GenImmix run (1700 pass) triaged every failure with evidence
    (isolated re-run + actual-vs-reference diff). **58 markers** total now (30 baseline + 28 new); final
    **GenImmix/Immix = 0 non-flaky failures** (2 left enabled as load-timing-flaky, pass in isolation). The 28
    categorize as: 11 `runtime_events` `[unsupported]` (GH#20), 9 finaliser/weak/ephemeron deferral `[semantic-timing]` (GH#21),
    3 `Gc.stat`/minor-counter `[stock-counter]`, 2 signal-poll `[behavioral-diff]`, 1 alignment, 1
    gdb-worker-threads infra, 1 slow-timeout. **The only genuine MMTk semantic gaps** are the **2 deterministic
    signal-delivery poll-point diffs** (`callback/signals_alloc.ml`, `lib-unix/kill/unix_kill.ml` — a signal
    lands at a later safepoint, not lost; no correctness gap, `sanity` unaffected) → one low-priority follow-up
    (align signal poll/safepoint with stock; GH#19). Per-test evidence + categories in **`gc/mmtk/TESTSUITE_TRIAGE.md`**.
    Original triage protocol below.

    Work
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

11. **#20 — retire the per-domain backup-thread machinery — DONE 2026-06-29.** The backup thread +
    interruptor STW-answering path are deleted (−416 lines); blocking sections do a plain domain-lock
    release/acquire, the binding's RUNNING set being the thread-state flag MMTk's `stop_all_mutators`
    already honours. Validated (turing A/B): clean `world.opt`, testsuite + spawn/join/systhreads burst
    stress clean (0 hangs at `MMTK_THREADS`∈{1,2,28}); the lone `sigwait` native flake is pre-existing
    (worse on baseline). → NOTES 2026-06-29. *(Original design notes below.)*
    Vanilla OCaml gives every domain a **backup thread**
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
    - **(a) [SUPERSEDED 2026-08-12: the default max went the other way, 64→16 MiB per domain (`e41c5383b2`,
      SHAPE.md rounds 26–28), trading binarytrees' surplus for the LU outlier.]**
      Raise the default cap 64→256 MiB — DEFERRED; helps only with a fixed large heap, MOOT under the
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
    - **(c) The dynamic-heap floor under-provisions low-live/high-alloc workloads — PARTIALLY FIXED (GH#6),
      floor raised 16→32 MiB; structural residual is RQ2.** The space-overhead heap (live×2.2) is sized to the
      *live set*, so a low-live workload gets a tiny heap → tiny nursery → constant collection (spectralnorm-3500
      did 52× more GCs at the dynamic heap than a fixed 4 GiB). For GenImmix the cost is worse than the GC count:
      the dominant pathology (GH#6 / matmul) is a **bimodal cliff** — perf-stat (matmul-768) shows a *single*
      nursery GC during the matrix-build phase PROMOTES the half-built result matrix into mature space, after
      which the O(n³) compute loop pays the generational **write barrier** on every `res.(i).(j) <- _` write
      (instruction count **5.8×**: 1.40e12 → 8.19e12; 3.1s → 19.6s; cache-misses ~equal, so it is NOT locality).
      Raising the floor to **32 MiB** (api.rs, gated by `MMTK_MIN_HEAP_MB`) keeps the canonical panel workloads'
      transient build footprint in the nursery (matmul-768: 0 GCs, **18.6s → 3.2s**; LU 6.1→4.8s; spectralnorm
      740→364 GCs) at negligible RSS cost (+5–7 MB; tiny programs never commit the floor — it is a LIMIT not a
      reservation, nbody/fannkuch/mandelbrot stay 8 MB). **Residual (RQ2):** a fixed floor only *moves* the cliff
      to a larger live set — size-1024 matmul still trips one promoting GC at 48 MiB → 51s. The structural fix is
      survival/age-driven promotion (don't promote an actively-mutated young object) OR a write-barrier fast path
      for freshly-promoted objects, NOT a higher floor. (An adaptive churn-escalating floor was prototyped and
      rejected: it cannot fix the cliff — the penalty is locked in by the first GC, before any churn signal — and
      it overshot RSS 2–5×; see ~/gh6-progress.md on turing.) **Age-driven promotion was since tried for
      Bactrian** (survivor aging, `MMTK_NURSERY_AGE`, NOTES 2026-08-08): a measured negative on binarytrees,
      then **disabled as unsound** (mmtk-core `a9b553a486`, 2026-09-08 — the remembered set does not persist
      across aging minors; the knob is now ignored with a warning). So this residual is still open.
    - **NOT a mainline bug: "degenerate default install / 913 GCs"** was a **church `fix/bug3c-cross-stw` build
      artifact** — clean mainline (local + turing) gives 114 GCs (correct 64 MiB). Check before merging that
      branch; not a mainline issue.
    - **Note the limit:** the nursery is a *level* lever, not a *slope* fix — even at a large fixed heap,
      GenImmix-256 MiB still regresses d4→d8, so the residual multi-domain sublinearity is the per-collection STW
      cost, addressed by off-STW marking (ConcurrentImmix), not the nursery. → RQ10; NOTES 2026-06-25.

13. **Known failures recorded in NOTES but not otherwise tracked (open).**
    - **Near-OOM SEGV** — just above the true OOM point, `binarytrees 20` at a fixed 36–52 MiB heap
      segfaults in `ScanMutatorRoots` nondeterministically instead of raising `Out_of_memory` (GenImmix
      mostly, Bactrian once). Not investigated further; an `rr` candidate. Related to item 3. **Same
      signature seen again (2026-09-30):** a SIGSEGV in the GC worker's root scan (`caml_scan_stack` <-
      `caml_do_roots` <- `ScanMutatorRoots::do_work`, invalid address 0x18) in a tight-heap Bactrian
      compile of `tformat.ml` (item 26). → NOTES "Near-OOM SEGV" (2026-08-06), 2026-09-30 (later).
      Tracked as **GH issue 49**; the 2026-09-30 space-time sweep reproduces it on 14 binarytrees grid
      points (6 GenImmix, 8 Bactrian; `RESULTS.md`). **ROOT-CAUSED 2026-09-30 (evening); FIXED by PR 51
      (merged, `ce2dd86167`; GH issue 49 closed).** The bug-4 fix (`caml_mmtk_recycle_gc_regs_bucket`) set `Caml_state->gc_regs = NULL`
      before `caml_raise_out_of_memory` inside `caml_call_gc`'s saved-registers window; `caml_raise` runs
      pending actions before unwinding, the GC poll re-attempts the TLAB refill, under the generational
      plans that starts a collection while the `caml_call_gc` frame is still top of stack, and the root
      scan reads that frame's register roots through `gc_regs == NULL` (lldb: `scan_stack_frames`
      `fiber.c:305`, `regs == 0`). Every plan is exposed (a catch-and-retry OOM loop crashed on GenImmix,
      Bactrian, Immix, StickyImmix and GenCopy; Immix rarely collects on the retry). Recycling the live
      bucket was also unsound on its own (a callback in the window could pop it; on arm64 `gc_regs` points
      16 bytes into the bucket). Fix: `caml_mmtk_ensure_free_gc_regs_bucket` pushes a fresh bucket and
      leaves `gc_regs` alone (stock's semantics; one `Wosize_gc_regs` block, ~460 B, abandoned per caught
      `Out_of_memory`, as in stock). New test `gc-roots/oom_in_call_gc.ml`: exit 139 before, passes on 5
      plans; `binarytrees 20` at 32 MiB is a clean `Out_of_memory` on GenImmix and Bactrian. The Bactrian
      tight-heap crash of item 26 (fault 0x18) is the same bug. Repro before the fix: `binarytrees 20` at
      `MMTK_HEAP_SIZE_MB=32`, GenImmix. → NOTES 2026-09-30 (evening).
    - **fragmed OOMs under Bactrian** — `fragmed` at 64 MiB with 4 workers (NOTES 2026-08-13, "needs its own
      look") and `fragmed-300` at 192 MiB (NOTES 2026-08-12, "a pacing-tightness item, not corruption").
      Whether the September pacing rework changed either is not recorded.
    - **Unmeasured:** the rotating deterministic pads (`MMTK_ALLOC_JITTER=24/25`) were never actually
      exercised before 2026-09-28 (a parser bug made them mode 5), so SHAPE.md round 11's verdict on them
      is void. → NOTES 2026-09-28.

14. **LXR release-counter underflow abort (regression from the PR 23 merge) — FIXED 2026-09-29**
    (mmtk-core PR 2, merge `045f121143`, fix `085bc3be48`; GH issue 25). **Symptom:** after the
    merge, 65-69 tests under `MMTK_PLAN=LXR` aborted in CI with `pending_release_packets is still
    18446744073709551615` (`epilogue.rs:11`), in runs 36534216189 and 36537448938 (0 times in the
    pre-merge baseline run 28657254889). **Cause:** the binding enabled `marksweep_as_nonmoving`,
    and LXR never armed the mark-sweep space's release counter (`common.release(tls, false)`), but
    each mutator still decremented it (`is_nursery_gc()` is false for LXR). **Fix:** LXR prepares
    and sweeps that space as a full-heap collection at Full pauses only (the only pause whose backup
    trace marks it), and runs the mutator-side free-list release only then. **Verified locally**
    (macOS arm64, `MMTK_HEAP_SIZE_MB=512`): `misc/sorts.ml` and `lazy/lazy3.ml` abort with the panic
    on the unfixed submodule and pass on the fix; the full LXR suite on the fix gives 1432 passed,
    53 skipped, 10 failed, with no panic; GenImmix on the fix gives 1442 passed, 0 failed. **Not
    validated:** the Full-only sweep with objects actually in that space
    (`MMTK_MEDIUM_NONMOVING=1`), and `cargo clippy`. The remaining LXR failures are tracked in item 21.
    → NOTES 2026-09-29 "LXR abort fix verified".

15. **GenCopy: `misc/darkening_work.ml` (native) fails since the merge — FIXED 2026-09-30** by
    mmtk-core PR 5 (merge `95b425a27d`, fix `1eab027050`), pinned by ocaml-mmtk PR 41 (merge
    `3fbe544984`). The fix is described at the end of this item; the history below is kept.
    Output `error: writes caused 4 more cycles` instead of `ok` (the test compares
    `major_collections` with and without no-op writes and tolerates a difference of 2). Seen in
    both runs on the merged GC code: run 36534216189 (4 more cycles) and the rerun 36537448938 on
    `9631db07fd` (3 more cycles), so it is not a one-off flake. GenCopy passed the whole suite at `cbc66e3efd` and on 9 of the 10
    earlier runs checked (the other failed on `publish.ml`); GenImmix/StickyImmix/Bactrian pass it.
    **Diagnosis (2026-09-29; a pacing artefact, not corruption).** A diagnostic with
    no-write/write/no-write phases counts 20/29/21 major collections with the default GC workers,
    5/5/5 with `MMTK_THREADS=1`, 5/5/5 with `MMTK_BUMP_BLOCK_KB=32`, 5/5/5 with a 64 MiB nursery
    (which hides the difference behind the cadence trigger) and 8/13/8 with `MMTK_MATURE_FLOOR_MB=64`
    (verified from the diagnostic's report, not re-run). Every automatic full GC is triggered by
    mature pressure, and the table stayed intact (`table_bad=0`). *Source reading:* each GC worker's
    copy allocator is rebound at every nursery pause and abandons its block tail (mmtk-core
    `plan/generational/copying/global.rs` ~105, `policy/copyspace.rs` ~367, `util/alloc/bumpallocator.rs`
    ~97); blocks are 512 KiB since mmtk-core `2c9e516786` (2026-08-09); the pressure law counts
    reserved pages. *Inferred, not instrumented:* the remembered-slot packets from the writes change
    how many workers copy in a pause, so more partly-used 512 KiB blocks are reserved. One instance of
    the reserved-pages pacing class, item 20. **Disposition:** keep open; do not raise the test's
    tolerance; do not change the block size globally.
    **CI update (2026-09-29, from the all-plans run logs):** intermittent, not persistent. Of the 11
    completed runs of the day (mainline and topic branches, all after the PR 23 merge) it failed 8 and
    passed 3: the mainline runs at `cd162e8496`, `c59f7851c3` and `240b6c8f62` (36565968550,
    36569985297, 36570516849). A topic-branch run between them (36569800854) failed it.
    Repro: `env MMTK_PLAN=GenCopy MMTK_HEAP_SIZE_MB=4096 make -C testsuite one
    TEST=tests/misc/darkening_work.ml TIMEOUT=120`, then the built binary under `MMTK_PACE_DEBUG=1`.
    **CI update (2026-09-30, from the run logs):** of the later runs it failed on the PR 35 branch
    (`7383bb3c93`) and on mainline `da5f51ffba`, and passed on `e351966d59` and the PR 38 branch
    (`d963811a74`). (On `e351966d59` GenCopy failed a different test, `parallel/tak.ml`: item 30.)
    **Fix (2026-09-30).** The cause was as diagnosed: workers abandoned a 512 KiB mature copy-block tail
    at every nursery GC, and the pressure law reads reserved pages. `GenCopy::prepare_worker` now rebinds
    the worker copy allocator only when the GC is not a nursery GC. *As reported by the fix's author:*
    majors per phase 21/30/21 -> 5/5/5 (reversed phase order 29/20/30 -> 5/5/5), 153 collections per phase
    unchanged, mature-reservation growth per minor 2.9-5.4 MiB -> 0.3 MiB, `sanity` builds pass.
    *Verified (re-run independently, macOS arm64):* `darkening_work` under GenCopy at 4096 MiB passes 3/3
    (fails 3/3 without the fix); GenImmix, StickyImmix and Bactrian pass it; full GenCopy suite 1448 / 0,
    GenImmix 1447 / 0. All-plans CI on `3fbe544984`: GenCopy green. *Safety argument (source reading):*
    GenCopy mutators never allocate in the mature copy spaces; the worker copy selector is a separate
    namespace; no release path assumes an empty worker buffer. The test's tolerance and the global block
    size are unchanged. → NOTES 2026-09-29, 2026-09-30, 2026-09-30 (later).

16. **Immix / ConcurrentImmix: `weak-ephe-final/weaklifetime.ml` times out at CI's fixed 4 GiB heap
    (pre-existing, not caused by the merge) — FIXED 2026-09-29 by ocaml-mmtk PR 27 (merge `69b81065c7`).** Exit -9 (ocamltest's 120 s SIGKILL) in run
    36534216189 and in the baseline run 28657254889; it has failed this way on every run since
    `20d3ad42ed` except one Immix pass. In the rerun 36537448938 it timed out again on Immix
    (bytecode) but passed on ConcurrentImmix, so the result depends on runner speed. The test loops until 20 major collections, and a
    non-generational plan collects only when the fixed heap fills, so at 4 GiB it is slow. A
    test/CI-configuration matter (heap size or per-test timeout for these plans), not a GC bug as far
    as the logs show. Repro: `env MMTK_PLAN=Immix MMTK_HEAP_SIZE_MB=4096 make -C testsuite one
    TEST=tests/weak-ephe-final/weaklifetime.ml TIMEOUT=600`, and time it. **Fix:** ocaml-mmtk PR 27
    (`ad6632c3c4`, merged as `69b81065c7`) pins `MMTK_HEAP_SIZE_MB=128` for this test via an
    ocamltest `set`; the test body is unchanged. **Confirmed by CI:** Immix and ConcurrentImmix passed
    `weaklifetime.ml` on all 7 all-plans runs of 2026-09-29 whose tree contains `69b81065c7` (checked
    from the run logs). Under LXR the test still fails at the 128 MiB pin (item 21). → NOTES 2026-09-29.

17. **LXR silently computes wrong results (GH issue 26; pre-existing) — FIXED 2026-09-29** by
    ocaml-mmtk PR 30 (merge `c59f7851c3`, fix `c10cf38244`; binding only). GH issue 26 is closed.
    A probe that keeps 1000 arrays (64 words each) in a 1000-word table while
    allocating 3,000,000 short-lived arrays should print 1501500000 (GenImmix does); LXR printed
    2813004516 at a 64 MiB heap and 1842208410 at 512 MiB, and so did builds from before the merge
    (superproject `cbc66e3efd`, submodule `ed02eafc6b`). **Cause (source reading, confirmed by the
    fix):** the LXR field barrier runs before the store and buffers the slot; `FieldSlot` cached the
    OLD value's classification, and `FieldSlot::load` returned `None` for a cached not-traceable
    value without reading the current one, so a store that overwrote an immediate or an out-of-heap
    static lost the new value's RC increment and the object could be freed while reachable. The
    table size is irrelevant (100 to 4000 slots fail alike). **Fix:** pre-write-barrier slots are
    classified at load time, not from the cached old value (a `DEFERRED` sentinel,
    `FieldSlot::from_address_deferred`, and `OCamlMemorySlice::from_slots_deferred` used by
    `mmtk_ocaml_satb_barrier`; branch commit `69b429828a`, merged as `c10cf38244`), with the regression
    test `testsuite/tests/misc/mutation_old_value.ml`. **Verified (re-run independently, macOS arm64,
    512 MiB):** the probe prints 1501500000 under LXR at 64 and 512 MiB, native and bytecode; the
    regression test fails under LXR before the fix and passes after; the full LXR testsuite gives 1433
    passed / 10 failed with 0 GC panics, the same 10 test files as before the fix (`churn`,
    `domain_parallel_spawn_burn`, `domain_parallel_spawn_burn_gc_set`, `forbidden`,
    `gc_mark_stack_overflow`, `publish`, `test`, `weak_array_par`, `weaklifetime`, `weaktest`); full
    GenImmix 1443 / 0. The generational remembered set is
    NOT affected (source reading and probes): its barrier pushes the slice and classifies slots at GC
    time, after the store. **Consequence:** the earlier LXR (RQ1) results were validated with the
    binarytrees/kb checksums, which do not exercise this pattern; with the fix applied LXR also runs
    out of memory where it previously appeared to succeed (item 21). LXR results stay provisional
    until item 21 is resolved and they are re-measured. (An earlier version of this item said an explicit `Gc.full_major ()` produces no
    collection under LXR; that was wrong — see item 22.) → NOTES 2026-09-29.

**Items 18-27 (added 2026-09-29), 28-35 (added 2026-09-30).** Items 17, 18, 19 and 20(a) were fixed
and merged on 2026-09-29, item 27 with the docs of PR 35, item 29 on 2026-09-30, and items 15 (=
20(b)) and 22 later on 2026-09-30 (ocaml-mmtk PR 41, mmtk-core PRs 4 and 5); item 21's retention
fixes merged that evening (PR 46, mmtk-core PR 6). The OPEN work, in
priority order (item 28, GH issue 39, a GC worker freeing a pointer it does not own under domain
churn, and item 13, GH issue 49, are FIXED: PRs 52 and 51, merged, issues closed; item 28's
process-exit residual remains): (1) item 21, LXR capacity (diagnosed; retention fixes merged; block-granularity
retention open) and the remaining LXR failures, with item 31, LXR's unsound weak references, which blocks
line reuse (together they keep LXR's results provisional); (3) item 26, GH issue 36, the
intermittent Bactrian out-of-memory in Linux CI (the explicit-request part is closed by PR 41; the
rest is open); (4) item 25, GH issue 33, a forked child cannot run a collection (explicit requests
are now no-ops there; an allocation-triggered collection spins); (5) item 30, one-off multi-domain
crashes on Linux CI (possibly item 28); (6) items 34 and 35 — the residual maxRSS gap and the multi-domain scaling gap, the two headline gaps after the macOS memset fix; item 35 is DIAGNOSED (per-copy nursery cost: narrow packets on chameneos, per-copy writes on binarytrees; the E1 counters, PR 58, merged) and its first lever, the opt-in worker-local nursery closure (`MMTK_LOCAL_NURSERY_TRACE=1`), has landed; the per-copy writes are the next lever and item 34's binarytrees time lever; item 33's per-pause log is done (PR 57, merged); (7) item 33, the RQ7 pause-time measurement that decides whether Bactrian stays; (8) the unexplained July growth (20(c)); (9) item 32, an LXR
reference-count anomaly in kb; (8) item 23, a possible concurrent-marking infix race; (9) item 24,
two pre-existing side findings from the store-path audit. Evidence is labelled *verified*
(reproduced by running), *source reading*, *inferred* or *unknown*.

18. **Two bulk-store paths skipped the generational barrier: silent corruption under the generational
    plans, including GenImmix, the default (GH issue 28) — FIXED 2026-09-29** by ocaml-mmtk PR 31
    (merge `240b6c8f62`, fix `2a59d9f89e`). GH issue 28 is closed. The default plan was affected from
    2026-06-24 until `240b6c8f62`.
    - **Native `Array.fill`.** *Source reading:* the array-fill primitive (`runtime/array.c` ~773-777)
      calls `caml_mmtk_region_barrier` only under `#ifndef NATIVE_CODE`, so native `Array.fill`
      stores into a mature array are never remembered. *History (git):* `b0dc6460e3` (2026-06-19)
      added the fill barrier inside that guard; `af6afd77ba` (06-21) kept it bytecode-only;
      `16373f099c` (06-22) wired the native barrier into `caml_modify`/`caml_initialize` but missed
      this third site; `49588c70ae` (06-24) made GenImmix the default. So the default plan has been
      affected since 2026-06-24. *Verified:* a probe that uses `Array.fill keep j 1 a` to store young
      64-word arrays into a 1000-slot old array while allocating 3,000,000 arrays finds 962/1000
      slots wrong under native GenImmix, 961 under StickyImmix, 962 under GenCopy and 949 under
      Bactrian; 0 under Immix and under bytecode GenImmix; `keep.(j) <- a` and `Array.blit` give 0
      on the generational plans. A 2026-07-03 build (`cbc66e3efd`) gives 878/1000 under GenImmix.
    - **`No_sharing` unmarshalling, both runtimes.** `Marshal.from_string` / `input_value` of data
      marshalled with `No_sharing`. *Source reading:* the barrier pass added by `451b4ebaf0`
      (2026-09-23) walks `intern_obj_table`, which is allocated only when the data records shared
      objects, so for `No_sharing` data the pass does nothing. *Verified:* 100/100 corrupted under
      Bactrian, native and bytecode, where medium pretenuring is on by default (since `16169a12a0`,
      2026-08-10); GenImmix/StickyImmix/GenCopy only with `MMTK_MEDIUM_NONMOVING=1` (exit 139).
    - **Related, UNVERIFIED (source reading only, no reproducer; present at mmtk-core `045f121143`,
      not introduced by any 2026-09-29 fix):** `intern_rec` (`runtime/intern.c` ~534-540) suppresses
      collection with `caml_mmtk_disable_collection` while it holds raw pointers into half-built
      objects, but that counter is consulted only through `is_collection_enabled`, which a forced
      request skips (mmtk-core `util/heap/gc_trigger.rs` ~180, `if force || ...`). The binding's
      explicit-GC entry points pass `force = true` (`gc/mmtk/binding/src/api.rs` ~673 and ~692). So a
      custom-block deserialize callback that forces a GC during unmarshalling could collect in the
      middle of `intern_rec`.
    - **Store-path audit (source reading):** no other gap. Every other store goes through
      `caml_modify`/`caml_initialize` or a scanned root list (the full table is in GH issue 28), and
      ocamlopt emits every pointer store as an out-of-line `caml_modify`/`caml_initialize`
      (`asmcomp/cmm_helpers.ml` ~2279-2295).
    - **Fix:** the `#ifndef NATIVE_CODE` guard in `runtime/array.c` is removed, and `runtime/intern.c`
      lists mature blocks when there is no object table and hands them to the region barrier at the
      end of `intern_rec` (branch commit `3c4b074899`, merged as `2a59d9f89e`). Regression test:
      `testsuite/tests/gc-roots/old_to_young_bulk_stores.ml`. *Verified (re-run independently):* the
      probe gives 0/1000 for both `fill` and `blit` under GenImmix, StickyImmix, GenCopy, Bactrian
      and Immix (native); the regression test passes under GenImmix, Bactrian, StickyImmix and LXR;
      full GenImmix testsuite 1443 / 0 and full Bactrian 1443 / 0 (macOS arm64). On the PR's CI run the
      Bactrian job passed. In Linux CI under LXR, the regression test's native variant failed on the
      mainline run at `240b6c8f62` because the bytecode `ocamlopt` compiling it segfaulted (exit -11),
      not because the test failed (item 21). The *related, unverified* forced-GC-during-`intern_rec`
      hazard above is not addressed by this fix.
    - **Lesson:** the testsuite had no old-to-young test for bulk primitives, which is why GenImmix
      was green; a barrier split between the native and bytecode runtimes needs a test per runtime.
    → NOTES 2026-09-29.

19. **GH issue 24: systhreads master-lock handoff vs MMTk's per-domain RUNNING set — FIXED
    2026-09-29** by ocaml-mmtk PR 32 (merge `e351966d59`; fix `ed61273ae9`, detector `e08718a7be` and
    `af66508981`, tests `610d6449cd` and `ba187bdd07`). GH issue 24 is closed. The diagnosis below
    was made on branch `test/systhreads-running-set` (detector `475ae50838`, tests `ffd5fc5011`) and
    is posted on GH issue 24. *Verified before the fix (macOS only; Linux not run):*
    - Thread-exit hang: a GC worker waits on `running.is_empty()` (`gc/mmtk/binding/src/collection.rs`
      ~848) while domain 1 stays in the RUNNING set with its master lock free.
    - Yield wake-up after a blocking-section release, with heap corruption: with the check off, the
      stress workload crashed 6/6 release-runtime runs and 5/8 debug-runtime runs (including
      `fiber.c:295 Assertion failed: d`); the control without blocking sections passed 14/14. Timer-tick
      preemption in the upstream `testpreempt` test triggers it too.
    - The STOPPED-after-unlock race occurs several times per run with no artificial delay.
    - The restore-before-RUNNING window is reached (~200 times per run); harm not shown.
    - The enter-blocking retry loop was not reproduced.
    - New: the native main domain is never marked RUNNING until its first blocking section or GC park.
    - Re-run of both tests: `gh24_thread_exit` is killed by the 30 s timeout (exit -9) and the stress
      test aborts at the check (exit -6), bytecode and native.
    *Source reading:* a worker-concurrent plan can start FinalMark from a GC worker without a mutator
    poll (mmtk-core `scheduler.rs` ~500-537), so these gaps matter even for single-domain programs
    under ConcurrentImmix. Root cause of the class: the backup-thread retirement (NOTES "GH#20",
    2026-06-29) kept a per-domain flag but made it safety-critical without tying it to master-lock
    ownership.
    **Fix (merged).** Only the master-lock holder changes the domain's RUNNING state: release marks
    STOPPED only when no thread waits, under the master lock's mutex, before unlocking (upstream's
    `st_bt_lock_release(waiters == 0)` rule); an acquire from STOPPED marks RUNNING before
    `restore_runtime_state`; hand-offs inherit RUNNING; the default blocking-section hooks do the
    marking themselves; the main domain is marked RUNNING at startup. It ships the RUNNING-set check
    (`MMTK_CHECK_RUNNING=1|warn|lost`; on and aborting by default in the debug runtime;
    `MMTK_CHECK_RUNNING_STW=1` counts STW-mutex acquisitions) and enables the two tests
    `lib-systhreads/gh24_thread_exit.ml` and `gh24_running_set_stress.ml`.
    *Verified (re-run independently, macOS arm64):* both new tests pass (they hung / aborted before);
    `testfork` and `testpreempt` pass; full testsuite GenImmix 1444 / 0, Immix 1444 / 0, and the debug
    runtime (`USE_RUNTIME=d`, `s=4096`, check aborting) 1444 / 0. On CI the Linux `extra (debug)` and
    `extra (debug-s4096)` jobs pass with the check on. *As reported by the fix's author:* the 8-domain
    stress with the check off went from crashing 6/6 (release) and 5/8 (debug) to 20/20 clean each;
    zero check violations across 43 threaded/parallel/signal tests in both runtimes; 7/7 per plan under
    Immix, StickyImmix, Bactrian and ConcurrentImmix; STW-mutex acquisitions per contended blocking
    section fell from 2 to about 0.1 (single thread: 2 -> 2).
    **Not covered:** Windows (the shared `st_bt_lock_*` code is not compiled there), Linux local runs,
    and `fork` (out of scope; see item 25, GH issue 33).
    **Risks recorded:** code that replaces the blocking-section hooks must now do the RUNNING marking
    itself; the debug CI jobs now fail on any future RUNNING-set violation (intended).
    → NOTES 2026-09-29.

20. **Pacing laws fed by reserved pages (open; a class with two verified instances, both FIXED:
    (a) 2026-09-29, (b) 2026-09-30; (c) unexplained).** Stock OCaml
    paces major work by allocated words; these laws instead read MMTk's page-reservation accounting.
    - **(a) Dynamic heap grows on an all-garbage program (Immix).** *Verified:* 1,000,000 discarded
      64-word arrays with an 8 MiB floor and a 128 MiB cap retain 23 pages after every GC, yet the
      heap limit goes 2048 -> 4521 -> 9950 -> 21905 -> 32768 pages (the cap); in every GC
      `target_pages == demand_target_pages` = the pre-GC poll reservation x 2.2, while the live-based
      target is 50 pages. Forced defrag and bytecode give the same sequence. A pinned 32 MiB heap
      runs 16 GCs at 86.6 MiB max RSS; the dynamic heap runs 7 GCs at 175.2 MiB. *Source reading:*
      mmtk-core `util/heap/gc_trigger.rs` ~603-612 records the whole reservation at a heap-full poll
      as `pending_demand_pages`, and ~528-530 sizes the heap to it; introduced by mmtk-core
      `032100ea2a` (2026-08-30) to admit allocations larger than the limit (that case verified
      working: a 64 MiB allocation from an 8 MiB floor). GenImmix on the same probe does not grow
      (330 GCs at the 8 MiB floor, 66 at 32 MiB; its collections are nursery-triggered) — a
      difference for this probe, not general immunity. **FIXED 2026-09-29** by mmtk-core PR 3
      (`fplaunchpad/mmtk-core`, merge `44d02b65a9`, fix `f0afe6ed00`, only
      `src/util/heap/gc_trigger.rs`), pinned by ocaml-mmtk PR 34 (merge `724ca4068a`; pin
      `045f121143` -> `44d02b65a9`): the demand target is sized from the largest actual pending
      request instead of the whole pre-GC reservation. *Verified (re-run independently on `240b6c8f62`,
      one GC worker, max RSS from `/usr/bin/time -l`, macOS arm64; all 40 probe runs exit 0 with
      correct checksums).* All-garbage probe, 128 MiB cap, collections and max RSS before -> after:

      | plan, floor | before | after |
      |---|---|---|
      | Immix, 8 MiB | 7, 175.2 MiB | 67, 63.0 MiB |
      | Immix, 32 MiB | 5, 175.1 MiB | 16, 86.9 MiB |
      | SemiSpace, 8 MiB | 10, 150.7 MiB | 165, 30.8 MiB |
      | SemiSpace, 32 MiB | 9, 150.8 MiB | 33, 54.8 MiB |
      | GenImmix, 8 MiB | 330, 59.0 MiB | 330, 59.3 MiB |
      | GenImmix, 32 MiB | 66, 63.1 MiB | 66, 63.2 MiB |
      | Bactrian, 8 MiB | 340, 59.1 MiB | 340, 59.2 MiB |
      | Bactrian, 32 MiB | 68, 63.1 MiB | 68, 63.2 MiB |

      A 64 MiB allocation from an 8 MiB floor, a ~64 MiB growing-live builder and a four-domain
      16 MiB-each probe pass on all four plans after the fix, with collection counts within one of
      before. Full testsuite with the dynamic heap (no `MMTK_HEAP_SIZE_MB`): GenImmix 1444 / 0,
      Immix 1444 / 0. **SemiSpace was affected too** (not previously recorded). **Consequence:** RSS
      figures for Immix and SemiSpace measured under the dynamic heap with `032100ea2a` present (on
      mainline: between the PR 23 merge, which brought it in, and this pin) are inflated; July 2026
      figures predate `032100ea2a` and are not affected by it. **Limits:** the macro-benchmarks that motivated
      `032100ea2a` (decompress, ydump, sedlex) were not run; Linux was not tested; mmtk-core's fork
      has no CI on pull requests, and the all-plans workflow did not run on the pin bump (item 27).
    - **(b) GenCopy `darkening_work.ml`** — item 15; FIXED 2026-09-30 by mmtk-core PR 5 (worker copy
      buffers are no longer abandoned at nursery GCs, so the reserved pages stop inflating).
    - **(c) Unknown, possibly a third instance:** on a 2026-07-03 build (`cbc66e3efd`, before
      `032100ea2a`), Immix with the dynamic heap grew to 12-14 GB RSS on `weaklifetime` (a single
      measurement). Hypothesis, not instrumented: reserved pages track block occupancy, not live
      bytes.
    → NOTES 2026-09-29; RESEARCH_QUESTIONS RQ7.

21. **LXR capacity, and the remaining LXR failures (open; second priority, after item 28; capacity
    DIAGNOSED 2026-09-30; retention fixes MERGED 2026-09-30, PR 46; block-granularity retention open).** The capacity problem was
    exposed by the item-17 fix (merged 2026-09-29); the first capacity figures below are as reported by
    the fix's author, not re-run independently. The 2026-09-30 diagnosis follows them.
    - With increments applied, the item-17 probe runs out of memory at 32 MiB with a ~0.5 MiB live
      set: used pages after RC pauses climb 364 -> 9068, about one 32 KiB block per kept 520-byte
      array.
    - `chameneos_redux 500000` raises `Out_of_memory` at 64 and 128 MiB and completes at 256 MiB.
      Before the fix it "completed" at 64 MiB while applying ~7.5k increments instead of ~6.39M, so
      the earlier LXR `chameneos_redux` numbers are invalid.
    - *Source reading:* `ImmixSpace::get_reusable_block` returns `None` whenever `rc_enabled`
      (mmtk-core `policy/immix/immixspace.rs` ~1061-1072, commented "Throughput cost only"). With
      in-place promotion and no nursery evacuation, every block that holds a survivor stays whole.
      The out-of-memory path also did not wait for an armed backup trace (the item-22 early return;
      fixed on mainline by PR 41).
    - binarytrees and kb apply the same RC increment totals with and without the fix (281802 and
      1558258), so their outputs were not affected; their memory behaviour still needs re-measuring.
    - **Consequence:** all LXR time and RSS numbers (RQ1) must be re-measured now that the fix is on
      mainline.
    - **Local failures (macOS arm64, 512 MiB, re-run independently on the fix):** 1433 passed / 10
      failed, the same 10 test files before and after the fix: `churn`, `domain_parallel_spawn_burn`,
      `domain_parallel_spawn_burn_gc_set`, `forbidden`, `gc_mark_stack_overflow`, `publish`, `test`
      (not triaged), `weak_array_par`, `weaklifetime`, `weaktest`. The compilers SIGSEGV when run
      under LXR with the default dynamic heap (pre-existing, not investigated).
    - **Linux CI failures (all-plans workflow, 4 GiB pinned heap; read from the run logs of
      2026-09-29).** On all 7 runs after the item-14 fix, LXR fails `memory-model/forbidden.ml`,
      `memory-model/publish.ml`, `misc/gc_mark_stack_overflow.ml`, `weak-ephe-final/weaklifetime.ml`
      (at its 128 MiB pin: native `Assertion failed` at line 66, bytecode `out of memory in uncaught
      exception handler`) and `weak-ephe-final/weaktest.ml` (`Invalid_argument("index out of
      bounds")`); all five were already failing in the July baseline run 28657254889. New since the
      PR 23 merge: `lib-marshal/intext_par.ml` (`Invalid_argument("Marshal.from_bytes")`) fails on 10 of the 11 runs of the day, on Linux CI
      only — the local macOS runs pass it. Intermittent: `weak_array_par.ml` (`Assertion failed` at
      line 20), `parallel/domain_parallel_spawn_burn.ml` / `_gc_set.ml`, `lf_skiplist/test_parallel.ml`,
      and compile-time failures where the bytecode `ocamlopt`, itself running under LXR, segfaults
      (exit -11) while compiling a test (`gc-roots/old_to_young_bulk_stores.ml` at `240b6c8f62`,
      `lib-systhreads/gh24_running_set_stress.ml` on the PR 32 branch).
      On PR 41's CI run LXR failed `intext_par`, `weak_array_par`, `weaklifetime` and `weaktest`; on
      mainline `3fbe544984` (run 36668846658) the compile segfaults of `old_to_young_bulk_stores.ml`
      and `gh24_running_set_stress.ml`, `lf_skiplist/test_parallel.ml`, `forbidden`, `publish`,
      `gc_mark_stack_overflow`, `weak_array_par`, `weaklifetime` and `weaktest` — all within the set.
    - **Diagnosis (2026-09-30; research branches `research/lxr-capacity` in both repositories, pushed,
      not merged: superproject `744053f961`, mmtk-core `392e41fb28`).** *Verified* (macOS arm64,
      `MMTK_HEAP_SIZE_MB` pinned, new `MMTK_RC_RETAIN` accounting):
      - *Block granularity.* The item-17 probe (1000 kept 520-byte arrays, ~0.5 MiB live) at 32 MiB:
        457 KiB live in 884 held blocks (28 MiB); held is 62x live, 1.47x from line granularity and
        42x from block granularity. The backup trace ran 104 times and reclaimed 0 (each held block
        holds a live object; 0 cycles). Cause: the port never reuses lines under RC
        (`get_reusable_block` returns `None` when `rc_enabled`) and never evacuates the nursery.
      - *`chameneos_redux 500000` fails for four separate reasons, largest first.* (1) 2-bit sticky
        counts pin chains of popped `Queue` cells (`Queue.pop` leaves `next` set, so one stuck cell
        keeps every later cell): 10-20 MiB per epoch; the dead-but-counted set at the next Full is
        ~229k each of `Queue` cells, closures and continuations, with rc=1 and 49 sticky; 4-bit counts
        remove it (*inferred* for the exact pin; *verified* for the counts). (2) The mutator raises
        `Out_of_memory` before the armed Full backup pause runs — the item-22 early return; fixed by
        waiting for the pending request, now on mainline via PR 41. (3) A sweep guard ("mutator is
        reusing this block") misfires under the port's single-bump epoch and refuses dead blocks: up
        to 1585 held blocks with no live object (50 MiB), 1606 refusals counted in one run; it also
        appears without effects (qchurn: 28 MiB held with 8 KiB live; 96 KiB with the guard off).
        (4) Resuming a promoted continuation never decrements what its stack referenced (~38 KiB;
        balanced once fixed); the fix needs a hook on resume (`caml_mmtk_cont_resumed` in
        `runtime/fiber.c`, a prototype).
      - *With all four and 4-bit counts:* the probe runs at 32 MiB (55 pauses, 0 Full);
        `chameneos_redux` completes at 64/128/256 MiB with 0 Full, and increments equal decrements
        (20,720,186 vs 20,721,337).
      - *Line reuse* (prototype `MMTK_RC_LINE_REUSE`) runs the probe at 32 MiB but is NOT safe to
        enable: LXR never counts or clears weak referents, so `Weak.get` can return freed memory
        (item 31).
      - *Reference LXR (source reading, `lxr/lxr-v0.32.0` `603e29bb13`):* it reuses lines under RC
        (`rc_get_next_available_lines`), evacuates the nursery by default (`RC_NURSERY_EVACUATION`),
        defragments mature at Full, and maps emergency and user requests to Full. The port dropped
        all of these in its "minimal in-place cut" (NOTES 2026-06-29/30).
      - *RSS tax:* LXR runs 50-100 MiB above Immix at the same pinned heap (probe at 32 MiB: 142-146
        MiB vs 89 MiB; `chameneos_redux` at 64 MiB: 250 vs 145).
      - *Testsuite under LXR before/after the prototype:* `mutation_old_value`, `lazy3`,
        `ephe_custom`, `ephe_infix`, `ephetest_par`, `finaliser`, `weaklifetime2`,
        `weaktest_par_load` and the effects tests (24/24) pass in both; `sorts` at 64 MiB fails
        because the bytecode compiler itself segfaults or runs out of memory under LXR (known).
      - **Status:** the resume-decrement hook and the sweep-guard fix (the two safe fixes) and a
        4-bit default are **MERGED** (below). Open follow-ups: item 31 (weak references, GH issue 44)
        and item 32 (a kb reference-count anomaly, GH issue 45).
      - *RSS tax caveat:* measured on macOS while mmtk-core zero-filled every mapped metadata chunk
        (workstreams, "Space-time curves"; fixed by mmtk-core `5454281016`, pinned by PR 53, merged); likely
        partly that artefact; re-measure (Linux, or macOS after the fix).
      - **RQ1 consequence:** "barrier ~free" survives (it is mutator-side). "In-place RC wins at
        memory parity" is unproven: the earlier parity was heap-size parity, LXR carries the RSS
        tax, and every effects/queue workload was fiction before GH issue 26. Re-measure at RSS
        parity with 4-bit counts, the guard off, resume decrements, the pending-request wait, and
        line reuse off.
    - **Retention fixes MERGED (2026-09-30):** ocaml-mmtk PR 46 (merge `6bb7a1be36`) and mmtk-core
      PR 6 (`b566d1f5b7` on `0.32-ocaml`, pinned at `7a36bbb0b5`). (1) The RC sweeps no longer refuse
      dead mature blocks as "being reused" (the epoch-timed refusal misfired on blocks promoted in
      place; safe because this port has no mutator block reuse and every decrement and sweep is STW; a
      comment in `block.rs` says a lazy/concurrent path must restore a real ownership guard).
      (2) Resumed promoted continuations decrement their stack referents (`caml_mmtk_cont_resumed`,
      `mmtk_ocaml_lxr_continuation_resumed`, 256-entry batches, plan-flag gated, within 2 % on a
      GenImmix perform/continue loop); `caml_continuation_borrow` keeps `Effect.*.get_callstack` from
      double-decrementing (that crashed, SIGBUS, under LXR — found while landing). (3) 4-bit reference
      counts by default (`LOG_REF_COUNT_BITS` 2; `lxr_rc_bits_2`/`lxr_rc_bits_8` remain; RC table 1/16
      of the heap instead of 1/32). (4) `MMTK_RC_RETAIN` accounting, default off (`cont_resume_decs`,
      a `cycle_dead_rc` histogram, `marked_rc0` with `[RC-SANITY]` lines). New test
      `tests/effects/resume_counts.ml`. *Verified:* `chameneos_redux 500000` at 64/128/256 MiB: 0 Full
      pauses (was 20/2/0), max used after a pause 0.73 MiB (was 62.9/125.8/0.9); qchurn holds 0.16 MiB
      instead of 31; 2-bit vs 4-bit in isolation: 11/4/0 Full pauses with 2-bit. Cost: 5–10 % wall
      under LXR at pinned heaps (binarytrees 18@32 MiB 0.33 → 0.36 s, kb 50@32 1.13 → 1.21 s,
      chameneos@64 5.46 → 5.72 s), RSS unchanged, GenImmix unchanged. CI on PR 46, LXR job vs mainline:
      fixed `gh24_running_set_stress` and `old_to_young_bulk_stores`; `intext_par` failed in that run
      (the known flaky multi-domain set); `forbidden`, `gc_mark_stack_overflow`, `publish`,
      `test_parallel`, `weak_array_par`, `weaklifetime`, `weaktest` are pre-existing. **Remaining:**
      the h probe still runs out of memory at 32 MiB (block-granularity retention; needs line reuse or
      nursery evacuation; line reuse is blocked on GH issues 44 and 45); every LXR number is still to
      be re-measured. → NOTES 2026-09-30 (evening).
    - **Process consequence:** the LXR job is a gating job and is red on every PR. A red LXR job is
      treated as explained only when all its failing tests are within the set above; any other
      failure needs triage. The same rule applies to Bactrian (item 26): a red Bactrian job counts as
      explained only if its only failures are `ocamlopt.opt` `Out of memory` compile failures (GH issue
      36); anything else, including a test program's own `Out of memory`, needs triage.
    → NOTES 2026-09-29, 2026-09-30 (later), 2026-09-30 (evening).

22. **Explicit `Gc` requests returned before the collection ran (GH issue 21's mechanism) — FIXED
    2026-09-30** by ocaml-mmtk PR 41 (merge `3fbe544984`; binding `00d1dcc001`, `67213e10d4`, runtime
    `e3d803b4b9`, `7b9595f4cc`, tests `3108064b59`, `588e96b4a5`, `d0485c1342`), with mmtk-core PR 4
    (merge `37d0143dd3`, `MMTK::is_collection_requested`) pinned through `95b425a27d`. GH issue 21 stays
    open (below).
    **Before the fix** (*verified*, a 60-run matrix on `358ea7958c`, 64 MiB heap, `MMTK_THREADS=1`,
    GenImmix/Immix/LXR, native and bytecode): after `Gc.major`, `Gc.full_major` or `Gc.compact` the
    collection counters read 0/0 immediately after the call and 1/1 after a 20 ms blocking wait;
    `Gc.minor` produced no collection even after the wait. *Source reading:* `park_until_resumed`
    returned at once while `gc_active` was still false, and `Gc.minor` never requested an MMTk
    collection. (The earlier unmerged topic commits, mmtk-core `885ed5a880` and ocaml-mmtk
    `6fc57da0f8`, are superseded by PR 41.)
    **Fix.** `block_for_gc`'s `park_until_resumed` waits while a collection is requested or active,
    bounded by an epoch counter that `resume_mutators` bumps under the STW lock, so a parker returns
    at the first pause that completes after it parked. On the generational STW plans an explicit major
    request is repeated until a full-heap pause completes (a request can coalesce onto another domain's
    already-decided nursery pause). `Gc.minor` requests a non-exhaustive collection and waits.
    `GC_COUNT` is published before mutators resume. In a forked child (GH issue 33, item 25) explicit
    requests are no-ops and the wait ignores the request flag (a `pthread_atfork` child handler sets a
    flag). New tests: `gc-roots/explicit_gc.ml`, `gc-roots/explicit_gc_domains.ml`.
    **Guarantee on return, per plan** (*verified* with probes, 64 MiB, native and bytecode):
    - Immix, SemiSpace (and MarkSweep/MarkCompact, bytecode): one whole-heap pause for every call.
    - GenImmix, StickyImmix, GenCopy: `Gc.minor` one nursery pause; the major calls a full-heap pause.
    - Bactrian: `Gc.minor` one nursery pause; the major calls one pause, a Full only if no concurrent
      cycle was in flight.
    - ConcurrentImmix: one pause, possibly only an InitialMark; no whole-cycle guarantee.
    - LXR: one RC pause, no backup trace.
    - NoGC, and any plan in a forked child: no-op.
    **Differences from stock:** stock `Gc.full_major`/`Gc.compact` run three major cycles and drain
    pending actions between them; here it is one pause, so a value alive only until its `Gc.finalise`
    callback is reclaimed by a later collection.
    **Cost** (*verified*): each explicit request is now a real STW pause of about 0.5 ms, fixed at any
    heap size (2000 `Gc.compact` calls take about 1 s at 64, 512 and 4096 MiB, native and bytecode).
    `regression/pr9853/compaction_corner_case.ml` (25001 compactions) is disabled under MMTk with the
    greppable marker: its bytecode variant exceeded the 120 s CI timeout on every plan (native
    passed), and it targets a stock-compactor corner case. Domain termination, which already requested
    a collection per termination, now waits for it (*verified*, 3 runs each, macOS):
    `par_binarytrees 20` at 8 domains 157-169 GCs / 1.8-2.1 s before, 173-181 / 1.75-1.8 s after; 2000
    spawn-join rounds 1964-1999 GCs / 1.0 s before, 2000 / 0.9-1.0 s after.
    **GH issue 21:** eight of its nine disabled tests now pass on every STW plan (10/10 GenImmix; once
    each under Immix, StickyImmix, Bactrian, SemiSpace and GenCopy) but fail on ConcurrentImmix and
    LXR, which are gating plans, so they stay disabled with markers naming those plans.
    `c-api/alloc_async` still times out (its stub waits on a stock counter MMTk never updates).
    **Verification:** full testsuite (macOS arm64) GenImmix 1448 / 0, GenImmix debug runtime 1448 / 0,
    Immix, Bactrian and ConcurrentImmix 1448 / 0 (512 MiB). CI on PR 41: every gating job green except
    LXR (its known set, item 21), GenCopy green for the first time since the September merge; the
    all-plans run on the merge `3fbe544984` gives the same picture (checked from the run).
    The `lib-dynlink-domains/main.ml` regression once attributed to the earlier fix is **refuted**:
    10/10 passes in a tree built in place. `testfork` passes (explicit requests are no-ops in the
    child).
    **Also noted:** a forked child that allocates past its trigger spins at 100% CPU in
    `alloc_slow_inline` (pre-existing; GH issue 33, item 25).
    → NOTES 2026-09-29, 2026-09-30 (later).

23. **Possible concurrent-marking infix race (open question; inferred, not probed).** Under
    ConcurrentImmix/Bactrian concurrent marking, a slot created at scan time is loaded later without
    trust: `load` re-reads the value but reuses the infix offset cached at scan time. A field that
    switches between an ordinary pointer and an infix pointer in that window would yield a wrong
    object start. Unverified; needs a probe or an argument that the window cannot occur.
    → NOTES 2026-09-29.

24. **Two pre-existing side findings from the store-path audit (open; not fixed; unchanged by the
    item-18 fix).**
    - Native StickyImmix: `Obj.Ephemeron.set_data` into an aged ephemeron loses 199/200 data blocks
      while the key is live; bytecode is fine. *Inferred:* weak/ephemeron processing rather than a
      barrier.
    - Immix with the non-default `MMTK_MEDIUM_NONMOVING=1` segfaults on large-array probes.
    → NOTES 2026-09-29.

25. **GH issue 33: a forked child cannot run a collection (open; added 2026-09-29; explicit requests
    made no-ops in the child 2026-09-30).** MMTk's GC worker
    threads do not survive `fork()`. *Verified:* on a tree where `Gc.minor` requests an MMTk
    collection and waits (the first, unmerged item-22 fix), `lib-systhreads/testfork.ml` hangs in the child at
    `Gc.minor ()` and leaves an orphaned process. *Source reading:* the child has no GC workers and no
    scheduler thread, a copy of the binding's stop-the-world state (the RUNNING set, `gc_active`) and of
    its mutex (possibly held by a worker at the fork), and nothing re-initialises MMTk in
    `caml_thread_reinitialize` or the atfork path. **Update (2026-09-30, PR 41):** explicit `Gc`
    requests in a forked child are now no-ops (a `pthread_atfork` child handler sets a flag that the
    request path and the wait consult), so `testfork` passes with synchronous explicit requests. **Still
    open:** a forked child that allocates past its trigger spins at 100% CPU in `alloc_slow_inline`
    (observed while verifying PR 41; pre-existing, not caused by it) — the allocation-triggered case
    that was only expected from the source reading before. *Unknown:* what the sibling MMTk bindings do at fork.
    **Fix direction:** re-create the workers and reset the binding's STW state in the child (in the
    same child handler or `caml_atfork_child`), or fail cleanly on the child's first collection.
    `fork` was out of scope of the item-19 fix.
    → NOTES 2026-09-29 (landings entry), 2026-09-30 (later).

26. **GH issue 36: intermittent Bactrian out-of-memory on Linux CI (open; added 2026-09-29; CI cause
    unknown; a tight-heap reproduction partly diagnosed 2026-09-30).** In the all-plans testsuite workflow (4 GiB pinned heap), `ocamlopt.opt`, itself
    running under `MMTK_PLAN=Bactrian`, stops with `Fatal error: exception Out of memory` (exit 2) while
    compiling a test; the test is then counted as failed. *Verified from the run logs:*
    - Failed: `lib-string/test_string.ml` (run 36542738150, mainline `89d0602e12`), `misc/sorts.ml`
      (36558162128, mainline `358ea7958c`), `lib-format/tformat.ml` and `lib-scanf/tscanf.ml`
      (36563475743, PR 29's branch, i.e. mainline code at `69b81065c7`), `tformat.ml` (36563792450,
      PR 30's branch), `test_string.ml` (36569800854, PR 32's branch, based on `cd162e8496`),
      `memory-model/forbidden.ml` (36579401670, mainline `e351966d59`), `tscanf.ml` (36581329531, PR
      35's branch), `tformat.ml` (36583101914, mainline `da5f51ffba`), `sorts.ml` (36595437886, PR 38's
      branch).
    - **Not only the compiler:** on `e351966d59` the bytecode test program `parallel/churn.ml` itself
      died with `Fatal error: exception Out of memory` (exit 2) under Bactrian, besides the
      `forbidden.ml` compile failure.
    - Passed: 36534216189 and 36537448938 (the first two post-merge runs), 36563812717 (PR 31's
      branch), 36565968550 (`cd162e8496`), 36569985297 (`c59f7851c3`), 36570516849 (`240b6c8f62`).
    **Refuted (2026-09-30):** the earlier hypothesis that the item-18 bugs caused this. The failure
    recurs on `e351966d59`, `da5f51ffba` and the PR 38 branch, all of which contain the item-18 fix.
    Only Bactrian shows it; the other gating plans pass on the same commits. *Unknown:* the cause (a
    compiler out of memory in a 4 GiB heap suggests pacing or accounting — a collection not
    triggered, or reserved pages not returned — more than a live-set problem, but nothing is
    measured), and whether it depends on the GC worker count or the runner's memory. Not reproduced
    locally (full Bactrian on macOS arm64, 512 MiB and dynamic heap: 1446 / 0 on `da5f51ffba`).
    **Next:** run `ocamlopt.opt` on `tformat.ml` repeatedly under `MMTK_PLAN=Bactrian
    MMTK_HEAP_SIZE_MB=4096` on Linux with `MMTK_VERBOSE=1` and `BACTRIAN_TRACE=1`, and record heap
    occupancy and the size of the failing request. The CI triage rule is in item 21.
    **Progress (2026-09-30; not resolved).** *Verified* on macOS arm64 with a flambda compiler built to
    match CI:
    - At 4 GiB, no reproduction: 60/60 `tformat.ml` compiles at 1, 2 and the default GC worker count;
      `tscanf`, `test_string` and `sorts` also pass.
    - At 32 MiB, two workers, `MMTK_IMMIX_ALWAYS_DEFRAG`, compiling `tformat.ml` under Bactrian:
      instrumentation shows `block_for_gc` returning with a collection requested but not active, then
      the allocator rejecting the request at attempt 2 in emergency mode with the request still
      pending — the premature-return path that PR 41 closes (item 22). The last Full had retained
      8097/8192 pages and the binding had requested compact-all next.
    - A/B with only the pending-request wait: 2 successes, then a third run failed after real
      collections (attempt 4, no request pending, 7014/8192 pages) with a SIGSEGV in the GC worker's
      root scan (`caml_scan_stack` <- `caml_do_roots` <- `ScanMutatorRoots::do_work`, invalid address
      0x18) — the same signature as item 13's near-OOM SEGV, and (root-caused 2026-09-30 evening)
      the same bug: item 13's `gc_regs == NULL` scan, fixed by PR 51 (merged).
    - With `MMTK_COMPACT_OVERHEAD_PCT=0`: 3/3 successes. Repeated compaction requests contribute to
      the tight-heap failure (not a fix).
    **Open question:** after a Full, does `note_swept_baseline` repeatedly force compact-all without
    admitting the allocation, with those pauses charged as failed allocation attempts? **Next
    discriminator:** Linux CI history from `3fbe544984` onward — does the 4 GiB out-of-memory recur
    with PR 41 in? (Bactrian was green on PR 41's CI run and on the merge's all-plans run 36668846658.)
    The `parallel/churn.ml` test-program out-of-memory on `e351966d59` (above) is part of this item.
    → NOTES 2026-09-29 (landings entry), 2026-09-30, 2026-09-30 (later).

27. **The all-plans testsuite workflow did not run on a submodule bump — FIXED 2026-09-29** by
    `7383bb3c93` (merged with PR 35, `da5f51ffba`). `.github/workflows/testsuite-plans.yml` ran on
    `push` with `paths:` `gc/mmtk/**`, `runtime/**` and the workflow file; the mmtk-core submodule is
    the gitlink `gc/mmtk-core`, which `gc/mmtk/**` does not match, so a pure pin bump got no all-plans
    run: PR 34 (`e788d8bdb5`, merged as `724ca4068a`, only `gc/mmtk-core` changed) had none, and
    mmtk-core's fork has no CI on pull requests. **Fix:** `'gc/mmtk-core'` added to `paths:`. Not yet
    exercised by a pure pin bump (PR 41 bumped the pin but also changed `gc/mmtk/`). (Docs-only edits to `gc/mmtk/*.md` still trigger the workflow.)

28. **GH issue 39: a GC worker aborts freeing a work packet under domain churn — FIXED 2026-09-30**
    by ocaml-mmtk PR 52 (merge `0015fbf179`; GH issue 39 closed; added 2026-09-30, ROOT-CAUSED with
    `rr` the same evening; was high priority — memory safety on the default plan; the process-exit
    residual below remains). *Symptom:* SIGABRT (exit 134),
    no output. The macOS crash report shows thread `mmtk-gc-worker` in `abort` <- `malloc_report`
    (`pointer being freed was not allocated`) <- `drop_in_place<Box<dyn GCWork<OCamlVM>>>` <-
    `GCWorker::run` <- `start_worker` <- `VMCollection::spawn_gc_thread` (one crash report's stack read
    independently). *Reproduction:* debug runtime, bytecode, default plan, `OCAMLRUNPARAM=v=0`, macOS
    arm64; 200 rounds, each spawning 6 domains that each allocate, spawn a child domain and join it,
    while one more domain allocates throughout. *As reported by the investigation that found it:*
    about 1-3% of runs (5 aborts in roughly 500), on `358ea7958c`, on `da5f51ffba` and with the item-29
    fix applied, so independent of GH issue 37; at the abort, domains were parked for a collection,
    some terminating, others in `caml_mmtk_domain_terminate`. *Unknown:* the cause (a double free of a
    work packet or corruption of its box); whether it occurs on the release runtime, in native code or
    on Linux (see item 30 for Linux CI crashes that may or may not be the same bug); whether it
    involves mutator deregistration at domain termination (the remembered-set flush at deregister,
    or packets that reference a terminated mutator). **Next:** `rr record -c <N>` on Linux, varying N.
    **ROOT-CAUSED (2026-09-30, evening; `rr` on godel in a build that widens the window with a
    `yield_now()` inside `VectorQueue::take`; traces `~/i39w/rrtraces/w_natd_c10000/run1` and `run2`).**
    A terminating domain leaves RUNNING in `caml_domain_terminate` but stays in the mutator registry
    until `caml_mmtk_domain_terminate` deregisters it, and deregistration flushes its barrier buffers.
    A pause that froze the mutator set in that window has a `ScanMutatorRoots` packet for the same
    mutator, which flushes the same buffers on a GC worker. Both `take` the same `region_modbuf`
    buffer (replay: same pointer, len 1), each wraps it in a `ProcessRegionModBuf` packet, and dropping
    the second packet frees it again (the abort); the SIGSEGV variant processes an already-freed buffer
    (`ProcessModBuf::do_work`). So: release runtime, native code and Linux are all affected. **Fix
    (PR 52, merged):** `caml_mmtk_domain_terminate` holds a binding slot (`caml_mmtk_try_begin_bind`
    … `caml_mmtk_end_bind`) across `mmtk_ocaml_deregister_domain`; a slot is granted only while no
    collection is active and `stop_all_mutators` waits for held slots, so no pause flushes this mutator
    concurrently and the next pause no longer includes it (also closing the window where a pause missed
    those remembered-set entries). No new lock order. *Verified:* godel GenImmix stress, native release
    6/200 → 0/200, native debug 10/200 → 0/200, widened build 50/50 → 0/100; macOS 0/65; godel full
    testsuite 1446 / 1 (`weaklifetime` at the 120 s timeout, host speed). *Residual:*
    `caml_mmtk_deregister_domain`, used when the main domain force-cancels peers at process exit, still
    flushes without a slot (waiting there could deadlock with a running main domain).
    → NOTES 2026-09-30, 2026-09-30 (evening).

29. **GH issue 37: a new domain's interrupt word was published after the bind loop — FIXED
    2026-09-30** by ocaml-mmtk PR 38 (merge `6865b559ed`, fix `d963811a74`; `runtime/domain.c`). GH issue
    37 is closed. *Symptom:* `Assertion failed: has_interrupt_word` (`runtime/domain.c:201`,
    `check_stw_domains`), seen once on CI in job `extra (debug)`, test `lib-dynlink-domains/main.ml`,
    on `724ca4068a`. *Cause:* `domain_create` parks a slot under `all_domains_lock`; since `eb7d21a683`
    (2026-09-28, "Domain creation no longer overlaps MMTk collections") it may drop that lock in the
    MMTk bind loop while a collection is active, and it published the slot's `interrupt_word` only
    after the loop. A second creator could then park and activate a later slot, putting an active
    domain above a slot with no interrupt word and breaking the prefix invariant that
    `check_stw_domains` asserts. Not caused by the item-19 fix: it reproduces on `358ea7958c`.
    *Release-runtime effect during the window (source reading):* `caml_interrupt_all_signal_safe`
    stops at the unpublished slot, so signal handling is delayed for domains above it (domains below,
    including domain 0, are still interrupted); `caml_find_index_of_running_domain` returns -1 for a
    running domain above the slot, so `caml_c_thread_register_in_domain` can fail spuriously; MMTk
    pauses are unaffected (pause poisoning goes through the mutator registry). **Fix:** publish the
    interrupt word in the same lock hold that parks the slot, as upstream effectively does.
    *Verified (re-run independently, macOS arm64, debug runtime, domain spawn/join stress under load):*
    10 assertion hits in 80 runs on `358ea7958c`, 0 in 40 on the fix run at the same time; full
    testsuite on the fix GenImmix 1446 / 0, debug runtime 1446 / 0. CI on the PR: `extra (debug)`,
    `extra (debug-s4096)` and `normal` pass. *As reported by the fix's author:* 10/100 and 7/50 hits
    before, 0/200 after. → NOTES 2026-09-30.

30. **One-off multi-domain crashes on Linux CI (open; added 2026-09-30; cause unknown; possibly item
    28).** *Verified from the all-plans run logs of 2026-09-29* (release runtime, 4 GiB pinned heap),
    each seen once:
    - ConcurrentImmix, `parallel/prodcons_domains.ml` (bytecode), run 36581329531 on the PR 35 branch
      (`7383bb3c93`, same code as `e351966d59`): glibc `double free or corruption (!prev)`, exit -6.
    - GenCopy, `parallel/tak.ml` (native, built by `ocamlopt.byte`), run 36579401670 on mainline
      `e351966d59`: SIGSEGV (exit -11), no output.
    *Inferred, not established:* the `double free` is the same class as item 28 (a free of memory the
    allocator does not own, in a multi-domain program), which would place item 28 on Linux and the
    release runtime. The two logs do not identify the freeing thread. **Update (2026-09-30, evening):**
    item 28 is root-caused and does occur on Linux and the release runtime. The GenCopy `tak` SIGSEGV
    is plausibly the same bug (GenCopy failed the item-28 stress 2/100 before the fix; *inferred*, `tak`
    not reproduced). The ConcurrentImmix `prodcons_domains` double free is the same class by source
    reading (the SATB flush, `flush_satb`, at deregistration); 0/100 on godel, so PR 52 is untested
    there. → NOTES 2026-09-30, 2026-09-30 (evening).

31. **LXR weak references are unsound: `Weak.get` can return freed memory (open; added 2026-09-30; GH
    issue 44).** *Source reading:* LXR never counts or clears weak referents
    (`WeakRefClosure` is disabled, and `process_weak_refs` never runs for LXR), so a weakly held
    object whose count drops to zero is freed while the weak slot still points at it. *Verified*
    (bytecode, 512 MiB, research branch of item 21): `weak-ephe-final/weak_array_par.ml` fails 8/20 on
    mainline, 20/20 with the line-reuse prototype and 0/16 with `MMTK_WEAK_REFS=0`. *Inferred:* line
    reuse overwrites the freed referent sooner, hence the higher failure rate. This explains LXR's
    known `weak_array_par` failure (item 21) and blocks enabling line reuse under RC. Weak references
    and ephemerons under deferred RC are a design gap in the port. Filed as **GH issue 44** (mechanism,
    fix options); with the merged retention fixes `weak_array_par` fails 5/20 (native, 512 MiB), 0/20
    with `MMTK_WEAK_REFS=0`. LXR is unsafe for programs that use `Weak` or `Ephemeron`; this blocks line
    reuse. → NOTES 2026-09-30 (later), 2026-09-30 (evening).

32. **LXR: objects reached by the Full trace with a zero reference count in kb (open; added 2026-09-30;
    anomaly, cause unknown; GH issue 45).** *Verified* (research branch of item 21, the
    `sanity` feature's dead-object poisoning, every pause forced Full): in `kb 20`, 495-570 objects
    reached by the Full trace have rc=0, deterministically; pre-existing. The item-17 probe,
    `binarytrees 16`, `chameneos_redux 50000` and qchurn show 0. *Unknown:* the cause, and whether RC
    could free any of these objects while they are reachable. Filed as **GH issue 45**: with the merged
    fixes (4-bit counts), `kb 20` at 32 MiB gives 558 such objects in 6 of 41 Full pauses, deterministic;
    sampled ones are small blocks in blocks promoted in place; output correct. It matters because
    `rc_dead` block freeing and line reuse trust a zero count, so it blocks line reuse with item 31.
    → NOTES 2026-09-30 (later), 2026-09-30 (evening).
33. **RQ7 decision measurement — pause-time distributions, GenImmix vs Bactrian vs vanilla (open;
    added 2026-09-30).** Decides whether Bactrian stays. On throughput the 2026-09-30 sweep and panel put
    Bactrian's fronts on top of GenImmix's on every bench (binarytrees 101 MiB / 1.74 s vs 94 / 1.64; kb
    22 / 0.48 vs 23 / 0.49; LU identical; chameneos 3.3 vs 3.0 s; par_matmul slightly better at 2–4
    domains, equal at 8), so throughput does not justify a second plan; but Bactrian's claim is pause
    time (stock's sliced marking inside nursery pauses, incremental sweep), and pause time has not been
    measured — the panel and sweep report wall and RSS only, `MMTK_VERBOSE` prints totals. Task:
    (1) **DONE (2026-09-30 night):** a per-pause log in the binding behind `MMTK_PAUSE_LOG=1` (PR 57,
    merged `4b7b1d8b50`) and a `--pauses` mode in the quick drivers (`benchmarks` PR 59, merged `b316229cc1`)
    reporting max, p99, mean, count and the number of Full-heap pauses; vanilla's
    `OCAMLRUNPARAM=v=0x400` gives counts only, not per-GC times; (2) GenImmix vs Bactrian vs vanilla on binarytrees, kb, par_binarytrees
    d=8 and chameneos_redux at matched heaps (the sweep's front points), 3 reps, M4 and godel;
    (3) decide: a materially lower max/p99 pause at equal RSS answers RQ7 yes — Bactrian stays and
    item 26 (issue 36) and its residual bugs get fixed; otherwise RQ7 is answered no with a number (a
    publishable negative: stock's incremental-marking architecture buys nothing on MMTk's generational
    Immix) and Bactrian is frozen — code and SHAPE.md kept as the record, dropped from the default
    panel, CI gating and the README plan table, no further maintenance. Not to be deleted before the
    measurement exists. → the RQ7 workstream bullet; RESULTS.md; RESEARCH_QUESTIONS RQ7.
34. **Residual maxRSS gap vs vanilla after the macOS memset fix (open; added 2026-09-30; the first of
    the two headline gaps).** After mmtk-core `5454281016` the fronts meet vanilla's, but MMTk's front
    still starts 8–21 MiB above vanilla's on kb, LU, matmul and chameneos (GenImmix/Bactrian; Immix 11–37;
    binarytrees 26–37, where the gap is collector speed rather than memory — RESULTS.md). Known
    components (NOTES 2026-09-30 evening): the nursery bound of 16 MiB per domain (vanilla's minor heap is
    2 MiB per domain, 8× smaller), GC work-packet vectors (~30 MiB on binarytrees, ~0 on kb; vanilla's mark
    stack is bounded and pruned), one 4 MiB chunk per space, and no page return on macOS (every release
    path is Linux-only; `MADV_FREE_REUSABLE` is the macOS call). Research question: what is the
    irreducible footprint tax of a framework collector over a bespoke one — side metadata + nursery + work
    queues — and can it be brought under ~10 % at equal time? Task: (1) a vmmap budget at each bench's
    front point on the fixed build; (2) three levers, each plotted as a space-time front rather than
    adopted as a default: nursery bound 16→8→4→2 MiB per domain, bounded/reused work packets, page return;
    (3) the matmul drop (GenImmix 48–71 MiB, 0.62–0.66 s everywhere except a 32 MiB nursery with heap
    ≥96 MiB: 27 MiB / 0.58 s) explained. Godel with reps for the Linux cross-check. **Note (2026-09-30
    night, corrected late):** binarytrees' residual is item 35's per-object nursery cost (0.63 µs per
    copied object), but not its packet term: binarytrees' scan packets carry 2,759 objects and it makes
    0.0027 malloc per copy (NOTES 2026-09-30 late). The opt-in local closure gains it only 4 %; its time
    lever is item 35's per-copy writes (the T12 list) — a slower collector needs more heap for the same
    time.
35. **Multi-domain scaling decomposition (DIAGNOSED 2026-09-30 night; first lever LANDED opt-in
    2026-09-30 late; added 2026-09-30; the second headline gap).** The 2026-09-30 panel: par_binarytrees GenImmix 2.5× at 8 domains vs
    vanilla 4.4×; par_spectralnorm 3.4× vs 5.2×; par_matmul 5.2× vs 6.1×; chameneos_redux anti-scales, 3.0 s
    at 1 domain → 13.4 s at 8 (vanilla 1.4 → 0.4 s). Decomposed on godel (14 cores of one NUMA node,
    GenImmix, `MMTK_THREADS` = domains, medians of 3) and cross-checked on the M4 with the new pause log;
    full tables, commands and the chain of evidence: NOTES 2026-09-30 (night); the mechanism correction
    and the first lever: NOTES 2026-09-30 (late). **The gap is per-pause cost,
    not the rendezvous and not the scaling shape.** On par_binarytrees 20 the mutator scales 8.9 → 1.9 s
    (4.7×) and the pause 12.3 → 4.5 s (2.8×, vanilla's whole-program ratio is 2.85×), but 242 pauses cost
    12.3 s at d=1 while vanilla's entire run is 10.35 s. Three terms, ranked:
    1. **Per-object nursery cost** (*verified*). A nursery pause costs ≈ 1.36 µs per copied object on
       chameneos (20.2 M copies, 27.5 s) and 0.63 µs on binarytrees, vs at most ≈ 0.6 µs for vanilla's
       whole chameneos run, mutator included, at the same promoted volume. Two different terms:
       - *chameneos — narrow packets.* An `LD_PRELOAD` counter shows one malloc per copied object
         (chameneos 200000: 7,837,308 copies, 7,861,282 mallocs; vanilla 110). A packet census by an
         independent review corrects the mechanism first inferred (one-object packets): nursery scan
         packets average **3.001 objects, 99.95 % of width 2–4** (2,611,387 packets / 7,836,637 nodes;
         edge packets 2,617,336 / 36,758,365 slots), and the malloc count is several allocations per
         small packet (node vector, slot vector, boxed edge work), not one packet per copy.
         `VectorQueue` buffers are allocated fresh per packet; `EDGES_WORK_BUFFER_SIZE` 4096 → 65536
         changes nothing (packets are never full). Profile share (d=1, 1 worker): copying 17 %, packet
         machinery 25–30 %, side-metadata traffic next.
       - *binarytrees — per-copy writes.* 2,759 objects per scan packet (5,828 packets / 16,079,827
         nodes), 0.0027 malloc per copy: its 0.63 µs is **not** packet overhead but the copy/metadata
         path. Single-worker GenImmix already takes the UP path (`collection.rs:921-942`, no
         forwarding-claim CAS), VO bits are off; what remains per promoted object is the header+payload
         `memcpy`, the allocator cursor, the destination mark bit, a whole destination unlog byte, the
         source forwarding pointer, one line-mark byte per spanned 256 B line, plus a relocated-slot
         store per edge (the T12 list). Why more GC workers slow single-domain binarytrees (M4: 1790 ms
         of pause with 12 workers vs 630 with 1) is **open**: with 5,828 wide packets,
         packet hand-off is an unlikely explanation.
       After term 2, chameneos is 68 % (d=1) to 82 % (d=8) pause, almost all in nursery pauses (420 ×
       64 ms at d=1; 68 full pauses total 0.7 s).
    2. **The E1 barrier-study counters** (*verified*; **fixed by PR 58, merged `ee744db495`**). Plain
       non-atomic global counters bumped from every domain on every `caml_modify` / `caml_initialize` /
       barrier call: 68 % of chameneos d=8 CPU was in the write barrier. Removed: chameneos d=8 21.8 →
       10.3 s, 160 → 68 CPU-s, speedup 1.9× → 4.0×; d=1 and par_binarytrees unchanged. Most of the
       panel's chameneos anti-scaling was this (*inferred* for the M4 panel, not re-run there).
    3. **The STW rendezvous** (*verified*, M4, single rep): time-to-stop 0.04 → 26 ms of a 674 ms pause
       total at d=8. Not a factor; the "STW-bound" reading (SCALABILITY.md updates 3 and 5) is
       corrected to "per-pause cost".

    **First lever — LANDED, opt-in (2026-09-30 late; mmtk-core `3acfce3465`, pinned `441a0b80b3`).** A
    worker-local nursery closure: with `MMTK_LOCAL_NURSERY_TRACE=1`, GenImmix's
    `GenNurseryProcessEdges::flush` scans each copied object into one worker-local slot buffer and
    processes those slots until the frontier is empty, reusing the frontier and slot vectors across waves;
    above `MMTK_LOCAL_NURSERY_SPILL` (default 4096, min 2) half the frontier is published as a scan packet
    for stealing; continuations still go through the ordinary packet; line marks (`post_scan_object`)
    preserved; off under `extreme_assertions`, `count_live_bytes_in_gc` and non-GenImmix plans. godel,
    3 reps, `MMTK_THREADS` = domains: chameneos_redux 500000 d=1 GC 27.8 → 19.6 s (−29 %, wall 40.9 →
    32.7), d=8 8.6 → 7.5 s (−12 %); binarytrees 20 12.0 → 11.5 s (−4 %); par_binarytrees 20 d=8 4.35 →
    4.0 s (−8 %, noisier); kb 50 3.49 → 3.17 s (−9 %). An independent review (one worker, same-binary
    off/on): chameneos 200000 total pause −31 %, mallocs 7.86 M → 27.7 k. `sanity`-clean on five
    benches at small heaps (1–4 workers, spill 16 to force spilling), 12/12 goldens at 4 domains, six
    focused fiber/ephemeron/finaliser/barrier tests; **full testsuite run pending**. Open:
    - (i) *Research question:* when should tracing stay local versus publish work, given the frontier
      width? The spill threshold is the local-vs-parallel knob, set by hand; chameneos (narrow) and
      binarytrees (wide) bracket it.
    - (ii) Default-on after the testsuite run; (iii) extend to GenCopy and Bactrian (UP-oldify,
      `MMTK_UP_OLDIFY=1`, SHAPE.md round 30, is the single-worker Bactrian precedent).
    - (iv) **Next lever: the per-copy writes (T12 list)**, especially for binarytrees — *inferred*,
      risk-scoped candidates: skip the unlog-byte store per copy when the object barrier is unused
      (OCaml uses the region barrier) or initialise unlog over exclusive allocation ranges; fold repeated
      line-mark stores over monotonic promotion ranges; batch destination mark initialisation. Upstream
      mmtk-core master (tracing moved to `src/plan/tracing/gc_work/closure.rs`) still creates fresh
      queues and alternates slot/node packets — nothing to borrow.
    Measured as a domain-sweep speedup curve with RSS, and on item 34's binarytrees front.
    - (a) *Quick partial test (superseded by the closure):* pooled `VectorQueue` buffers, or mimalloc as
      the Rust global allocator, to size the malloc share alone.
    - (b) *Young-target barrier filter* (branch `exp/barrier-filter`, **not landed**): the generational
      post-write barrier (mmtk-core's region path, `memory_region_copy_post`) records every mature store
      without checking the stored value, where stock's `caml_modify` checks `Is_young(val)`. A
      stock-style filter (binding exports the nursery range via `GenerationalPlan::nursery_range`;
      GenImmix/GenCopy; `MMTK_NO_YOUNG_FILTER=1` opt-out) is `sanity`-clean but had no measurable effect
      on chameneos (its mature stores are of young values, which stock records too). Kept as an unproven,
      stock-faithful option pending a mature-mutation workload; not merged.
    → NOTES 2026-09-30 (night), 2026-09-30 (late); SCALABILITY.md update 7; RESEARCH_QUESTIONS RQ7; RESULTS.md.

### Research & measurement workstreams (M8 / RQ-driven)

The active research/measurement threads behind the M8 milestone — the index; depth in `gc/mmtk/NOTES.md`,
`PERFORMANCE.md`, and `RESEARCH_QUESTIONS.md` (RQ-numbers below).

- **Benchmark vehicles (the M8 measurement plumbing).**
  - **`ocaml-bench/macro-benches`** — the authoritative DaCapo-style cross-runtime *macro* suite (±flambda),
    driven via **`running-ng`** — adopted as the M8 measurement vehicle (rather than building our own harness).
    The headline throughput/RSS campaign runs here. (See `PERFORMANCE.md` §1/§3.)
  - **Quick GC-decision bench panel** (on the `benchmarks` orphan branch, `quick/`; ~5 min/variant; sequential
    + parallel) — the **fast inner-loop complement** to the macro suite, the **no-zero (RQ8) A/B vehicle**, and
    the **LXR (RQ1) measurement vehicle** (LXR runs both sequential AND the parallel domain sweep at a pinned
    heap; not in the byte-identical CLBG gate). **Parallel finding:** RC does *not* rescue the multi-domain
    anti-scaling — LXR scales like the tracing plans (on par with GenImmix on compute benches, weak on
    alloc-heavy par_binarytrees), well short of stock OCaml; the bottleneck is the MMTk↔OCaml integration, not
    the collector algorithm (see `gc/mmtk/NOTES.md` 2026-07-01 and the 2026-07-02 quick panel, in `README.md` up to
    `358ea7958c`, section "Performance (quick panel)" — a 2026-07-02 build, before PR 23 and the 16 MiB
    nursery default). The tracing plans were re-measured on 2026-09-30 (M4 re-baseline below); LXR was
    not, so its quick-panel figures are still the July ones. These LXR numbers are also
    provisional: they predate the item-17 fix (merged 2026-09-29), the capacity problem it exposed
    (open item 21) and the retention fixes (merged 2026-09-30, PR 46), so they must be re-measured — at RSS parity, not heap-size parity (item 21's RQ1
    consequence).
  - **godel re-baseline (2026-09-30; cross-check host, not the representative one).** The quick panel,
    vanilla 5.5.0 (released, non-flambda) vs GenImmix, Bactrian and Immix, dynamic heap, fork
    `6865b559ed` / mmtk-core `44d02b65a9` (before PR 41), 14 physical cores of one NUMA node on godel
    (56-core Xeon Gold 5120, `powersave` governor). Full table, configuration and caveats: `benchmarks`
    branch, `quick/RESULTS-godel-2026-09-30.md` (raw: `quick/godel-2026-09-30-dynamic.ndjson`).
    *Verified on that host; observations, all host-caveated:* compute-bound controls are flat
    (nbody, fannkuchredux, mandelbrot within 0.96-1.03x); GC-heavy benches are worse relative to
    vanilla than in the July M4 panel (binarytrees GenImmix 1.40x -> 1.66x, kb 1.20x -> 1.47x,
    matrix_multiplication 0.87x -> 1.13x; the 16 MiB nursery default is the suspect, unverified);
    Bactrian's binarytrees RSS is 404 MiB (4.4x vanilla, 2x GenImmix; 257 MiB in July), unexplained
    (candidates: the compaction law, sliced marking, the new pacing); `chameneos_redux` is the worst
    result (GenImmix 1.75x vanilla at 1 domain and 4x at 8; Immix is fastest at 1 domain, 0.44x, but
    anti-scales). A worker-count check (`MMTK_THREADS` 4/14/56, binarytrees and kb, GenImmix) showed
    no improvement. LXR was excluded (it needs a pinned heap and its results are provisional). The
    like-for-like M4 Pro run (next bullet) does not reproduce the regression vs July or the Bactrian
    RSS, so both are godel-specific or fixed between `6865b559ed` and `3fbe544984` (not separated).
  - **M4 re-baseline (2026-09-30; the representative host).** The same panel on the Apple M4 Pro (8P+4E
    cores, 24 GiB, macOS, machine quiet), vanilla 5.5.0 (released, non-flambda) vs GenImmix, Bactrian and
    Immix, dynamic heap, fork `b714bb86d3` (tree identical to mainline merge `3fbe544984`, PR 41) /
    mmtk-core `95b425a27d`, 1 GC worker per sequential cell (N for an N-domain cell), no pinning, 3 reps
    (median) + 1 warmup. Tables and caveats: `RESULTS.md` "Quick panel (screening)"; raw:
    `benchmarks` branch `quick/m4-2026-09-30-dynamic.ndjson`, `quick/m4-2026-09-30.log`,
    `quick/graphs_m4/`. It supersedes the 2026-07-02 panel for the tracing plans. *Verified on that
    host; a dynamic-heap screening, not a verdict (next bullet):* compute-bound controls flat (0.99-1.01x
    time; RSS = each plan's startup floor, 26 MiB GenImmix/Bactrian vs 2 vanilla); no plan wins on both
    coordinates on a GC-heavy bench — binarytrees GenImmix 0.78x the time at 2.6x the RSS (238 vs 92 MiB),
    matrix_multiplication 0.87x at 5.9x (112 vs 19), kb 1.15x at 91 vs 8 MiB; `chameneos_redux`
    anti-scales on every MMTk plan (GenImmix 3016 ms at 1 domain vs vanilla 1421, 13372 at 8 vs 419;
    Immix fastest at 1 domain, 1067 ms, then 5423 at 8); par_spectralnorm and par_matmul scale less than
    vanilla (3.2-3.4x vs 5.2x; 5.0-5.2x vs 6.1x at 8 domains); Immix anti-scales on par_binarytrees.
    Versus godel, time ratios do not transfer (binarytrees GenImmix 1.66x vs 0.78x, kb 1.47x vs 1.15x,
    matrix_multiplication 1.13x vs 0.87x); vanilla's RSS matches where the live set dominates (binarytrees
    92 MiB on both) but MMTk's does not (GenImmix binarytrees 187 vs 238 MiB, kb 29 vs 91), so each
    host is compared only with its own vanilla.
  - **Space-time curves (method of record from 2026-09-30).** GC performance is a space-time curve, so a
    single dynamic-heap cell (wall ratio + RSS) is a *screening* result, not a verdict — e.g. the 2026-09-30 M4
    panel has GenImmix at 0.78× vanilla's time on binarytrees at 2.6× its RSS, a point vanilla's front beats
    (0.92 s at 116 MiB). Driver `quick/spacetime.py` on the `benchmarks` branch, after Jane Street's allocator
    showdown method: each GC-sensitive bench (binarytrees, kb, matrix_multiplication, LU_decomposition,
    chameneos at 1 domain) once per grid point, x = max RSS, y = wall, one front per configuration; vanilla
    `OCAMLRUNPARAM` `o` × `s`, each MMTk plan `MMTK_HEAP_SIZE_MB` {32…256} + dynamic × `MMTK_NURSERY`
    {default, 4 MiB, 32 MiB}. A plan "wins" only where its front lies below-and-left of vanilla's.
    **First sweep DONE (2026-09-30, M4 Pro, 440 points, one run each; its RSS carried the macOS memset
    artefact, see below; the re-sweep in `RESULTS.md` supersedes it):** no plan dominates vanilla on any GC-heavy bench. binarytrees, kb and LU are dominated
    for all three plans (MMTk's lowest RSS is 60–90 MiB above vanilla's; kb/LU times near parity there,
    binarytrees ~2×), and GenImmix/Bactrian on chameneos; matrix_multiplication and Immix on chameneos are "faster only at higher RSS" (2–5×
    and 3× vanilla's RSS). **Open questions it raised:** (a) decompose the RSS floor (side metadata, nursery
    outside the pinned heap, chunk-granularity mapping, the ~26 MiB startup floor, Immix fragmentation;
    GenImmix matrix_multiplication drops from ~120 to 41 MiB with a 32 MiB nursery, so it is not static);
    (b) why the generational plans take 2.6–3.1× Immix's time on chameneos (continuation stacks vs the
    promotion path); (c) GenImmix/Bactrian segfault in `caml_scan_stack` (item 13, "Near-OOM SEGV")
    instead of raising `Out_of_memory` at small pinned heaps (binarytrees, 32–48 MiB; Bactrian also 64
    and 96 MiB with a 32 MiB nursery; GH issue 49; Immix fails cleanly) — **root-caused and fixed by PR 51,
    merged after both sweeps** (item 13); (d) compute benches are not
    bracketed — vanilla's `o`/`s` do not move matrix_multiplication off 19 MiB and no MMTk configuration reaches it, so
    that comparison is a floor comparison, not a front one; (e) repeat on godel with reps (dispersion,
    Linux RSS accounting).
    **RSS floor decomposition (question (a)): DONE 2026-09-30 — macOS zero-fill. macOS memset fix:
    mmtk-core PR 7 (`5454281016`, merged as `69b5ddf663`) / ocaml-mmtk PR 53 (merged, `3846019997`). Re-sweep DONE 2026-09-30,
    `RESULTS.md`.**
    *Source reading:* mmtk-core's `dzmmap`/`dzmmap_noreplace` (`src/util/memory.rs`) call `zero()` under
    `#[cfg(not(target_os = "linux"))]` and the chunk mmapper maps whole 4 MiB chunks, so on macOS every
    heap and side-metadata chunk is fully resident once mapped; every page-return path
    (`blockpageresource.rs`, `freelistpageresource.rs`, `memory.rs`) is Linux-only. *Verified* (paused
    `vmmap`, GenImmix, `MMTK_THREADS=1`): startup floor 27 MiB = 12 metadata + 4 nursery chunk + 2.4
    malloc + ~7 text (Immix 44); binarytrees 20 at heap 64 = mature 60 (live ≈ 48) + nursery 16 +
    metadata 60 (15 chunks; 20 MiB from the Immix data base straddling a 256 MiB metadata boundary) +
    malloc 29–36 (GC work-packet vectors) = 174 vs max RSS 172; kb 50 at heap 32 = metadata 60 + mature
    12 + nursery 8 + malloc 2.5 = 82.6 (vanilla 8). Metadata actually needed: 5.7 MiB (binarytrees),
    1.4 MiB (kb). Nursery counted twice in the heap budget; the trigger estimates metadata at ~8 % while
    RSS pays 60 MiB. Linux cross-check: godel kb GenImmix 29 MiB vs M4 91. **Consequence:** the first M4
    fronts carried a 40–66 MiB/point OS artefact in the RSS coordinate; the "RSS ≈ 4× live is
    fragmentation" reading is wrong on macOS (≈73 % of kb's RSS was zero-filled metadata). **Fix:**
    mmtk-core `5454281016` (fplaunchpad/mmtk-core PR 7, "memory: do not memset fresh mappings on
    macOS"), pinned by ocaml-mmtk PR 53 (merged, `3846019997`); `sanity` (binarytrees 18 @ 64 MiB, kb 30 @
    32 MiB; GenImmix/Bactrian/Immix; 0 invalid), 12/12 quick-panel goldens, wall unchanged; spot RSS
    nbody 25→9 MiB, kb@32 90→31, binarytrees@64 164→119 (GenImmix), kb@32 Immix 86→43. **Re-sweep
    (same grid, same vanilla binaries; `benchmarks` `3e150aa0dc`, `quick/spacetime-m4-nz.*`):** front
    starts moved 40–66 MiB left (binarytrees GenImmix 156→94 MiB, kb 82→23, LU 86→26, matmul 41→27,
    chameneos 118→58) and the fronts now meet vanilla's; verdict classes unchanged; the same 20 points
    fail (GH issue 49; PR 51, merged later, not in the build). On binarytrees the gap is now collector speed (at equal
    RSS vanilla is 1.15–1.54× faster; its curve falls faster with memory); on kb/LU a 9–16 MiB floor
    plus 3–22 % time. **Open (residual floor, 8–21 MiB above vanilla on kb/LU/matmul/chameneos for GenImmix/Bactrian,
    Immix 11–37):** (1) the nursery (bounded up to 16 MiB, outside the pinned heap, counted twice in the heap
    budget; nursery `Bounded` max as a front, not a default); (2) GC work-packet memory (29–36 MiB of
    malloc on binarytrees; bounded/reused packet vectors — vanilla's mark stack is bounded and pruned);
    (3) one chunk per space/metadata spec; (4) `MADV_FREE_REUSABLE` page return on macOS; (5)
    GenImmix matrix_multiplication drops 48–71 → 27 MiB with a 32 MiB nursery and a large heap (why?);
    (6) re-measure LXR metadata (Linux, or macOS after the fix); (7) collector speed at a given heap on
    binarytrees (per-collection cost; a 4 MiB nursery costs 0.4–0.8 s). *Research angle (RQ4-adjacent;
    claimable once a Linux cross-check confirms the residual):* side-metadata RSS scales with spaces ×
    specs × mmap granularity wherever the OS does not demand-zero — a framework-vs-bespoke tax.
    → NOTES 2026-09-30 (evening).
    **Pending driver fixes (`quick/spacetime.py`, `benchmarks` branch; from a review):** `classify`
    lacks the symmetric "smaller but slower" (lower-memory trade-off) case (no effect on this dataset);
    the front comparison is over common memory budgets (each best point extended rightwards), not
    matched measured RSS — only binarytrees had raw RSS overlap in the first sweep; results are observed fronts over this
    grid, one run per point, one GC worker, no dispersion; the `t = a + b/(H − c)` fit is a descriptive
    overlay, not a validated model.
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
  byte-match, Immix unchanged). **FIXED 2026-06-29** (the test's clear-too-LATE half): `resume_mutators`
  schedules a **full GC under mature pressure** (mature grew past 1.2x the post-full-GC baseline, OR a
  bounded 8-nursery-GC cadence backstop — the cadence is load-bearing for steady-state-live programs
  where mature never grows) and **`Gc.major_collections` now counts only full GCs** (new `FULL_GC_COUNT`).
  Both shipped together (counting-only-full ALONE *hangs* `weaklifetime`'s `while major_collections < 20`
  loop). Binding-only via the public `GenerationalPlan` trait. `weaklifetime.ml` passes byte+native on
  GenImmix/StickyImmix/GenCopy; weak-ephe-final 10/14 on all four of GenImmix/Immix/StickyImmix/GenCopy
  (GenImmix was 8/14); binarytrees throughput at Immix parity. `finaliser_handover` SIGSEGV was the
  separate #55 sub-bug (now passing). → FAQ Q11; NOTES 2026-06-29; GitHub #5.
  **RETUNED 2026-07-02 (`33ae0009f8` — allocation-paced trigger):** the 2026-06-29 trigger's fixed floor +
  flat cadence *manufactured* domain-scaled full-GC work (spectralnorm d24: 807 fulls vs vanilla's 0;
  SCALABILITY.md UPDATEs 4–5). Now: pressure floor `max(32 MiB, nursery)` of newly-promoted pages, cadence
  `8 × ndomains` (d=1 semantics unchanged — `weaklifetime` still green), and domain termination runs a
  *minor* (not exhaustive) collection — which unmasked GH#3 (fixed, see #16/#31). After: fulls 807→39,
  par_binarytrees d8/d24 wall −22/−25%, GenImmix S(8) 0.89→1.02.
- **RQ7 — `Bactrian` hybrid (flagship research direction) — v1 LANDED (2026-07-02, branch `bactrian`, since merged to mainline).**
  The stock-architecture MMTk plan (architecture-matched, not implementation-matched — gc/mmtk/BACTRIAN.md): copying nursery (GenImmix) + concurrently-marked,
  STW-evacuated Immix mature (ConcurrentImmix) + slot-granular SATB deletion barrier, composed as
  `MMTK_PLAN=Bactrian` in the mmtk-core fork (developed on submodule branch `bactrian`, now in `0.32-ocaml`). Every pause except `Full` is
  nursery-anchored (InitialMark = minor GC + snapshot seeding; FinalMark = minor GC + remark + sweep);
  GH#5 mature pressure starts a concurrent cycle, user GCs stay STW Full. Validated: testsuite 1441
  passed / 2 failed with both failures shared with the GenImmix baseline (zero plan-specific);
  sanity+vo_bit clean. **First RQ7 readout: quick panel has Bactrian within ~8% of vanilla on 7/8
  sequential benches (binarytrees 1.08× vs GenImmix's 1.37×)** — most of the previously-measured gap
  was algorithmic (STW major vs concurrent major), not MMTk abstraction overhead; the quantified
  residual framework costs are the per-minor-GC pause floor (kb 1.21×), the RSS premium, and
  multi-domain STW coordination. **Closing-step 1 (allocation-paced cycle trigger) DONE 2026-07-02**
  (`33ae0009f8`; SCALABILITY.md UPDATE 5): manufactured majors de-manufactured, GenImmix
  par_binarytrees anti-scaling eliminated (S(8) 0.89→1.02), Bactrian no longer permanently mid-cycle
  (8-domain RSS 1689→504 MiB, though its S(8) is 0.67 — it now pays paced full pauses instead of
  floating garbage). **W-parity campaign (2026-08-06..10, branch `shape/tweaks` + shape-bench
  `quick/campaigns/wnight-20260808/`): UP-trace single-tracer plain-op mode (locked RMWs/object
  5.7→1.1; bt whole-process 0.90× vanilla), poll-trap-livelock fix, allocation-denominated full-GC
  backstop, and Max_young_wosize pretenuring DEFAULT-ON (≥ 2056 B born mature + overflow-block
  line-phase rotation + remset immediate filter → matmul 1.03× vanilla, nursery-independent; was a
  1.25–1.89× alignment lottery) — W-instruction parity certified panel-wide, W/D2-pause-count/
  mark-cadence matched at the 2 MiB stock-parity nursery ([`gc/mmtk/SHAPE.md`](gc/mmtk/SHAPE.md) rounds 1–23). Rounds 24–26
  (2026-08-11/12): G economics landed (thin LTO, header-sentinel forwarding = stock oldify's
  value-range protocol, UP direct-trace closure → GC −23 %, bt 0.84×); **sliced-STW marking
  DEFAULT-ON** (stock's mark slices as in-pause quanta → bt@2M max pause 8.0 ms vs vanilla's
  15.2, D3 tail matched); the residual-gap mystery resolved into two named mechanisms — the
  **JCC-erratum layout lottery** (matmul DSB 99 %→2 % on a 16-byte draw; assembler mitigation on
  both toolchains → matmul 0.98×, kb 0.98×, fannkuch 1.00×, and vanilla's own fannkuch build was
  a victim) and **allocation-frontier warmth** (LU/spectralnorm store-side RFO: L2-warm arena /
  LLC-warm nursery / DRAM-cold tiers).** **Rounds 27–32 + the September pacing rework
  (2026-08-12..09-28; merged to mainline 2026-09-29 as PR 23 `ddc53f4007` + mmtk-core PR 1 `892056da7a`):**
  nursery default **64→16 MiB per domain** (`e41c5383b2`; round 27 found warmth benches and survivor
  benches want opposite sizes — 16 MiB is the balanced point); **D2 period calibrated** (pressure margin
  120%→14% `f35a1ed592`, then 150% once incremental sweep moved the baseline to post-sweep `e5fd83a11b`;
  pressure floor 32→8 MiB `970a2721ce`; early-trigger clamp `MMTK_CONC_TRIGGER_PCT`=80 `d7e8b6d9ff`);
  **incremental sweep** — FinalMark's mature chunk sweep drained as budgeted quanta in later nursery pauses,
  on by default with sliced marking (mmtk-core `c01edca806`, round 29); a mature-direct pacing tick for
  pretenure/LOS-heavy programs (`29d16b434c`); a **mature-compaction law** (`MMTK_COMPACT_OVERHEAD_PCT`,
  default 100 → compact-all Full; `8a7c7d4ec8`/`16505c1235`, mmtk-core `7ddf1ed2bf`, round 30d);
  **UP-oldify** (opt-in `MMTK_UP_OLDIFY=1`: the binding walks the nursery closure natively via a new
  mmtk-core hook, `Scanning::up_oldify_packet`; `33bb7e4cbc`, mmtk-core `32d8057efa`, round 30); the
  mark-sweep nonmoving space made sound under generational/concurrent collection, with the free-list band
  (`MMTK_MEDIUM_TO=freelist`) kept opt-in (mmtk-core `c9d9a4af5b`, round 31); fused-metadata plain ops under
  the UP window (mmtk-core `9cda6a4816`, round 32); then the plan took over slice sizing — slice a cycle iff
  the predicted monolithic Full is too long, slices paced from the runway frozen at InitialMark and the
  measured mark rate, the binding only requests cycles (`22ade70f20`; mmtk-core `50f56f5987`, `4660d08769`,
  `22351d1644`, `9372c33c01`, `abd1879f6f`); LOS counted toward mature pressure (mmtk-core `a7b10f3d85`)
  and off-heap custom-block memory wired into pacing (`d9816ef9db`, `e16d20e0ae`). **Survivor aging** was
  tried and is **disabled** as unsound (mmtk-core `a9b553a486`). **Open RQ7 sub-questions:** the STW
  minor-pause rendezvous floor; the round-28 D5 finding that vanilla's pareto front dominates Bactrian's on
  bt/kb/LU/sp (constant metadata floor, space overhead on the front, bt's per-minor wall floor), with
  block-reuse ordering proposed there as the next lever (its outcome is not recorded); and the adversarial
  benches' gaps (mature_mutation, fragmed — round 32 attributes fragmed to missing in-place reuse for
  multi-line objects and names a pool-class fast allocator, not policy, as the cure).
  → RESEARCH_QUESTIONS RQ7; BACTRIAN.md; NOTES 2026-07-02, 2026-08-09/10/12/13/14, 2026-09-29; SHAPE.md
  rounds 23–32.
  - **RQ7 DECISION MEASUREMENT — pause-time distributions (OPEN, decides whether Bactrian stays; added
    2026-09-30).** On throughput the 2026-09-30 sweep and panel put Bactrian's fronts on top of GenImmix's
    on every bench (binarytrees 101 MiB / 1.74 s vs 94 / 1.64; kb 22 / 0.48 vs 23 / 0.49; LU identical;
    chameneos 3.3 vs 3.0 s; par_matmul slightly better at 2–4 domains, equal at 8), so throughput does not
    justify a second plan. Bactrian's claim was never throughput: stock's architecture (sliced marking inside
    nursery pauses, incremental sweep) is meant to buy **pause time**, and pause time has not been measured
    — the panel and the sweep report wall and RSS only; `MMTK_VERBOSE` prints totals. Task: (1) add a
    `--pauses` mode to `quick/spacetime.py` / `quickbench.py` that records every pause's duration (max,
    p99, mean, count, and the count of Full-heap pauses) from a per-pause start/end line emitted by the
    binding (behind `MMTK_PAUSE_LOG=1`; `BACTRIAN_TRACE` already has the hook points), with vanilla's
    per-GC timing alongside (`OCAMLRUNPARAM=v=0x400`); (2) run GenImmix vs Bactrian vs vanilla on
    binarytrees, kb, par_binarytrees d=8 and chameneos_redux at matched heaps (the sweep's front points),
    3 reps, on the M4 and on godel; (3) decide: if Bactrian's max/p99 pause is materially lower at equal
    RSS, RQ7 is answered yes — it stays and issue 36 and its residual bugs get fixed; if not, RQ7 is
    answered no with a number (a publishable negative: stock's incremental-marking architecture buys
    nothing on MMTk's generational Immix) and Bactrian is frozen — code and SHAPE.md kept as the record,
    dropped from the default panel, CI gating and the README plan table, no further maintenance. Do not
    delete it before this measurement exists. Cost to keep meanwhile: the plan with the most open bugs
    (issue 36; the extra small-heap segfaults in the sweep were issue 49, fixed) and a doubled CI /
    benchmark budget.
- **LXR integration — RQ1's read-barrier-free, low-latency vehicle (planned 2026-06-25; P3–P5 since LANDED
  on mainline by 2026-07-02 — `MMTK_PLAN=LXR` is wired, single- and multi-domain validated at the time
  (now provisional: the wrong-results fix merged 2026-09-29, item 17, exposed a capacity problem, item 21 —
  retention fixes merged 2026-09-30, block-granularity retention open; weak references unsound, item 31), see the intro above
  and NOTES 2026-06-30 / 2026-07-02; the phase plan below is kept as the design record).** **LXR** (Zhao,
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
    LOS RC (~1–2 wk, **highest risk** — moving-GC correctness; lean on the `sanity` feature at small heaps). **MERGED
    into `0.32-ocaml` mainline 2026-06-29** (`3998611893`; binding submodule bumped on `5.5+mmtk` @ `657c9117d2`) as
    **9 gated commits P2.0–P2.I** (`PlanConstraints.rc_enabled` gate default-false; RC per-block/per-line/per-page
    side-metadata specs registered only in the `rc_enabled` arm; gated `is_live`/`is_reachable` overrides; inert
    `trace_object_without_moving`/`mark_lines`/`post_copy` guards; `Defrag::decide_whether_to_defrag` rc threading; LOS
    read-side overlays). **Byte-identical-VALIDATED**: a fresh-clone `world.opt` builds green against the bumped
    submodule, and `par_binarytrees` (355319636), `weaklifetime` (PASS), `matmul-768` reproduce identical golden output
    under GenImmix/Immix/StickyImmix — because no plan sets `rc_enabled`, all 10 wired plans stay byte-identical. **The
    P2 write-SIDE (`RC-travels-with-copy` in `post_copy`/forwarding + the LOS write path) is deferred to P3** — it is
    non-additive (needs the `&'static LXR` plan back-pointer, `Pause`-typed dispatch, and a 1-arg→2-arg SFT
    `attempt_mark`/`initialize_object_metadata` change that breaks the trait for *every* space), so it cannot land
    byte-identically and is the real stopping point for further LXR work.
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
- **Scalability gap (open M8 work).** Speedup-vs-domains is now measured on the quick panel's stdlib-only
  `Domain.spawn` benches (`SCALABILITY.md`; the "Parallel" table of the 2026-07-02 quick panel, last in
  `README.md` at `358ea7958c`, not re-measured since PR 23), so the remaining gap (GitHub issue 7, open)
  is the parallel/multidomain *macro*-benches (merlin, lavyek), which were never ported to this fork's suite. Port them
  (or extend the quick-panel scaling harness — task #36). Micro-benches already show ~2× on parallel
  alloc-heavy. → PERFORMANCE.md §1/§6; GitHub #7.

### Shipped (done — one line each; depth in NOTES)

- **bug #2** — native unmarshalling allocated off-heap (intern path was `#ifndef NATIVE_CODE`); un-guarded so native interns via MMTk. (CI ocamldoc SIGSEGV resolved; manpage repro 0/12.)
- **bug #3** — MMTk STW stop barrier was a no-op (blocking-section counter underflowed `usize`); fixed by passing the domain to `caml_mmtk_enter/leave_blocking`.
- **bug #3b** — multidomain spawn/STW deadlock: replaced the global stop-counter with an MMTk-native per-mutator RUNNING set (`stop_all_mutators` waits for `running.is_empty()`); also fixed an MMTk-STW × OCaml-minor-STW deadlock + a terminate corruption; removed `park_terminating`. native `domain_dls` 14/30 hang → 30/30.
- **bug #4** — `gc_regs` bucket leak on OOM-raise inside `caml_call_gc` (RESTORE_ALL_REGS skipped); `caml_mmtk_recycle_gc_regs_bucket()` before the raise. 25/25→0/25. **Superseded (2026-09-30):** the recycling (which also set `gc_regs = NULL`) caused the near-OOM root-scan SIGSEGV (item 13, GH issue 49); PR 51 (merged, `ce2dd86167`) replaces it with `caml_mmtk_ensure_free_gc_regs_bucket`.
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
**10 are wired**, 1 deferred (Compressor); our fork adds two research plans, **`Bactrian`** (RQ7) and
**`LXR`** (RQ1), for **12 wired** in all. The `Testsuite (all GC plans)` CI workflow
(`.github/workflows/testsuite-plans.yml`) runs the suite under all 13 to surface
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
| `Bactrian` *(fork)* | copying nursery + SATB-marked Immix mature (sliced marking + incremental sweep) | nursery yes; mature only at `Full` | ✅ (byte + native) — RQ7; see `gc/mmtk/BACTRIAN.md` |
| `LXR` *(fork)* | reference counting on Immix + backup trace for cycles | yes | ✅ (byte + native) — RQ1, experimental; needs pinned `MMTK_HEAP_SIZE_MB`; ⚠ **results provisional:** wrong-results bug fixed and merged 2026-09-29 (item 17); capacity problem diagnosed and retention fixes merged 2026-09-30 (PR 46), block-granularity retention and remaining failures open (item 21); weak references unsound (item 31, GH issue 44); all time/RSS numbers need re-measuring |

**Native** runs **9 plans** — `Immix`/`StickyImmix`/`ConcurrentImmix`/`LXR` (in-place Immix-block TLAB),
`GenImmix`/`GenCopy`/`Bactrian` (copy-nursery `BumpPointer` TLAB), and `SemiSpace`/`NoGC` (also `BumpPointer` Default) —
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
  deadlock has a fix landed (mmtk-core `72ee627050`, open work item 8; GH#4 closed), though the intermittent
  hang was never re-captured to confirm it gone.
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
- RSS on macOS was inflated by mmtk-core itself: `dzmmap` zero-filled every mapped 4 MiB chunk
  (heap and side metadata) off Linux. **Fixed** (mmtk-core PR 7, `5454281016`; ocaml-mmtk PR 53,
  merged, `3846019997`): fronts moved 40–66 MiB left. **Residual:** a 10–20 MiB floor above vanilla (nursery,
  GC work-packet vectors, per-space chunks), and no page is returned to the OS on macOS (every return
  path is Linux-only). Figures measured on macOS before the fix (the first M4 sweep and panel, the LXR
  metadata tax) overstate MMTk's RSS (workstreams, "Space-time curves").
- Debugging: `turing` (Linux) has rr + gdb; `sanity` at a small heap for moving-GC
  bugs; `setarch -R` for the ASLR/metadata-mmap flake. See `gc/mmtk/NOTES.md` and
  `CLAUDE.md`.
