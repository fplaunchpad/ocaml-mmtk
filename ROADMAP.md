# ocaml-mmtk roadmap

This is the living plan for bringing up an MMTk-backed garbage collector for
OCaml. It is meant to be picked up cold in a fresh session. Companion docs:
[`README.md`](README.md) (overview + build/run), [`fork-handoff.md`](fork-handoff.md)
(original rationale), [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) (dated design notes
and deferred investigations).

**Project shape.** This repo *is* the OCaml fork (base `5.5.0-rc1`, branch
`5.5+mmtk`), distributed as `ocaml-mmtk`. The MMTk binding is in-tree at
[`gc/mmtk/`](gc/mmtk) and depends on `mmtk-core` 0.32 from crates.io (not
vendored). MMTk is **always-on and the only collector** (no opt-out; the stock
minor *and* major GC have been excised — M9). `MMTK_PLAN` selects the plan. The
bring-up approach (historical) was **bytecode first** (native code inlines its
allocation sequence, so it can't be swapped by redirecting a C function), routing
all allocation through MMTk; native now uses TLAB/nursery-aliasing.

Run knobs (as of M9, MMTk is **always-on** and the only collector — no opt-out):
`MMTK_PLAN` (default `Immix`), `MMTK_HEAP_SIZE_MB` (default 1024), `MMTK_VERBOSE`.
Native uses TLAB nursery aliasing onto an MMTk Immix block, so it requires `Immix`
or `StickyImmix` (the plans with an Immix nursery allocator); `GenImmix`'s copying
nursery has no Immix Default allocator and aborts at startup on native. Bytecode
runs under any plan (`Immix`/`StickyImmix`/`GenImmix`/`MarkSweep`/`NoGC`). mmtk-core's
own `MMTK_*` options are honoured (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, e.g.
`MMTK_IMMIX_ALWAYS_DEFRAG=true MMTK_IMMIX_DEFRAG_EVERY_BLOCK=true`).

---

## Status

| Milestone | Description | Status |
|-----------|-------------|--------|
| M0 | Build skeleton: in-tree binding links into the bytecode runtime | ✅ done |
| M1 | MMTk **NoGC** backs every bytecode allocation | ✅ done |
| M2 | **MarkSweep**: precise root scanning + stop-the-world (real collection) | ✅ done |
| M2+ | **Multi-domain** stop-the-world (`Domain.spawn` programs) | ✅ done |
| M3 | **Immix** (moving): copy/forward, infix-pointer fixup, updatable roots, clean `Out_of_memory` | ✅ done |
| — | Pinning: validated under forced defrag (broaden via M7); evacuation-time OOM assert remains | 🟡 |
| M4 | **Generational plans (GenImmix / StickyImmix)** — mutator write barrier | ✅ done |
| M5 | **Native-code integration** — all-MMTk via TLAB/nursery-aliasing (`Immix`/`StickyImmix` — the plans with an Immix nursery allocator), **single- and multi-domain** (`Domain.spawn` clean at 16–48 MB); staticlib auto-linked via configure global-link | ✅ done |
| M6 | Runtime features: weak arrays, ephemerons, finalisers — `process_weak_refs` **on by default** (`MMTK_WEAK_REFS=0` opts out to the memory-safe interim, transitional). Does weak-clear, ephemeron-release, `Gc.finalise`/`finalise_last`, **and custom-block finalizers** (via MMTk's finalizer queue, incl. unmarshalled blocks) under Immix **and** StickyImmix. `pr3612` + `pr5233` pass; no regressions (the testsuite weak/finaliser "failures" were parallel-harness flakes — pass in isolation); full bootstrap clean with weak-clearing live. pr5233 needed a plan fix: `Gc.full_major` now requests an *exhaustive* MMTk GC (generational user GCs were nursery-only → mature/LOS weak refs never cleared). **Cross-domain finaliser handover fixed**: orphaned finalisers from a terminated domain are now adopted into a live domain (`caml_mmtk_adopt_orphaned_finalisers`) at the start of `process_weak_refs` — the stock `adopt_orphaned_work` was deleted in M9 stage 3. (Orphaned *ephemerons* have the same gap — tracked TODO.) | 🟢 done (default-on) |
| M7 | Pass the OCaml testsuite — full bytecode suite under StickyImmix: **1476/1524 pass** (`setarch -R`, per-dir 120s cap). 47 non-pass are unsupported features (weak/finaliser → fixed by `MMTK_WEAK_REFS`; `Gc.stat`/memprof/runtime-events) — none crash; 1 multidomain regression (`parallel/domain_parallel_spawn_burn_gc_set`, bug #3) — **FIXED 2026-06-22** (the MMTk stop-the-world barrier counter underflowed; see Phase 3 #10). **Moving-GC bug (#2) — the linux-arm64/CI `ocamldoc` crash — ROOT-CAUSED + FIXED (2026-06-22).** Native unmarshalling allocated unmarshalled objects **outside MMTk spaces**: in `intern.c` the MMTk allocation path (per-object `caml_mmtk_try_alloc_shr` + the bulk `Alloc_small` skip) was wrapped in `#ifndef NATIVE_CODE` (bytecode-only), so native `intern_alloc_obj` fell through to the stock `caml_shared_try_alloc(d->shared_heap, …)`, which under M9 allocates in a non-MMTk `caml_stat`/malloc region. MMTk's root-scan/`scan_object` pointer filter drops those objects, so their fields are never traced and anything reachable only through the unmarshalled graph (the loaded ocamldoc module/info records) is collected → dangling pointer → SIGSEGV in `odoc_man.ml` walking the doc tree. **Fix:** remove the `#ifndef NATIVE_CODE` guards so native intern allocates via MMTk too (+ `CAMLassert(!caml_mmtk_enabled)` on the now-dead stock branch). **Verified:** from-scratch `make clean && make -j world.opt` (incl. the `ocamldoc Stdlib.3o` manpage step) succeeds; manpage repro **0/12** (was 12/12) under default Immix. The earlier bytecode-only fixes — GC-mid-`intern_rec` suppression (`is_collection_enabled`, fixed the `parser.ml` repro) and the `array.c` barrier — stand but were different/`#ifndef NATIVE_CODE` bugs. Full details: `gc/mmtk/NOTES.md`. | 🟢 |
| M8 | **Benchmark + optimise** vs. the stock GC — first baseline: MMTk ~1.4–1.8× slower & more memory on a GC-heavy native bench (`gcbench`); structural (fixed heap, non-gen Immix re-traces live set). Optimisation levers identified (dynamic heap, generational default, bytecode fast-path inline, GC-thread count) | 🟡 started |
| **M9** | **MMTk-only: excise the stock GC** — always-on (st.1) ✅, stock **minor** GC deleted (st.2) ✅, stock **major** GC made inert then bodies deleted (st.3) ✅, **`shared_heap.c` + `caml/shared_heap.h` deleted entirely** (st.5, −1665 lines; live colour-machinery/`caml_atom`/`caml_compactions_count` relocated to `major_gc.{c,h}`; heap-size/stats consumers rewired to MMTk; a link anchor in mmtk.c keeps `roots.o` linking) ✅, `Gc.stat` heap fields reimplemented on MMTk stats ✅ (collection-count semantics for `Gc.counters` + the custom-block-pacing-driven tests still partial) 🟡, **per-domain minor-heap arena removed** (domain create/terminate bootstrap `young_*` from MMTk via `caml_mmtk_refill_tlab`; `allocate/free/reallocate_minor_heap_arena` deleted; the address-space reservation is KEPT only because it still bounds `Is_young`) ✅. `ocaml-mmtk` is a single-GC runtime. Remaining: header/metadata reconciliation (incl. retiring the `Is_young` reservation) | 🟢 mostly done |
| — | Parallel collection: ✅ verified (correct; marking ~8.4x on 16 threads) | ✅ |
| — | **GC plans:** `Immix` (default), `StickyImmix`, `GenImmix`, `MarkSweep`, `NoGC`. All five validated on **bytecode**; **native** runs `Immix` + `StickyImmix` only (TLAB needs an Immix nursery allocator — `GenImmix`/`MarkSweep`/`NoGC` abort at startup on native). Collecting plans collect single- and multi-domain; moving plans relocate. (Bug #1: non-moving `MarkSweep`/`NoGC` had regressed — `is_forwarded` read forwarding-bits metadata they don't map; fixed by registering that spec only for moving plans.) CI: the `Testsuite (all GC plans)` workflow (`.github/workflows/testsuite-plans.yml`) runs the full testsuite under all 11 mmtk plans on x86-64 (5 wired + 6 unwired) to surface per-plan breakage (deliberately red — shows what still needs to work); CLBG `run.sh validate` is the byte-identical correctness gate on the known-good set. | 🟢 |
| — | **CI `Build` workflow — remaining red after bug #2 fix** (separate, pre-existing; surfaced once the x86-64 `build` job stopped crashing and the matrix stopped fast-failing). (a) **i386**: MMTk staticlib won't build — `Makefile.mmtk:36 mmtk-lib` Error 127 (32-bit cargo/target unsupported). (b) **linux-O0** (debug runtime): stock-GC debug asserts + a domain-terminate lock-drop race — **fixed** (see Phase 1 item 1); the bug #3 STW-barrier crash behind the residual `tests/parallel` reds is now **fixed** too (Phase 3 #10), leaving the pre-existing spawn-burn hang/harness-contention timeouts (bug #3b). (c) **opam installation**: `test-in-prefix` (relocatability) — **fixed 2026-06-22**: `configure.ac` baked the absolute build-dir path to `libmmtk_ocaml.a` into config (+ a second leak — Cargo `debug=true` embedded the build root in the archive's DWARF, propagated into every binary). Now referenced relocatably as `-lmmtk_ocaml` (symlinked into `stdlib/` during build, installed into `$(LIBDIR)`, like `libasmrun.a`), and `--remap-path-prefix` strips the DWARF build root. Verified: fresh build + install + `test-in-prefix` exit 0 + post-install compile with the build tree moved away. The x86-64 `build` job (the bug-#2 site) is **green**. Only `i386` (deliberately skipped — 32-bit MMTk unvalidated) remains. | 🟢 |

## Next steps (prioritised)

Execution order — correctness before performance; dependencies noted. Detail for each
item is in the workstreams / M9 stages below.

**Phase 1 — flag cleanup**
1. 🟢 `linux-O0` debug-runtime fix (essentially done): the debug runtime (`USE_RUNTIME=d`)
   asserts stock-GC invariants MMTk doesn't maintain. Removed (all DEBUG-only):
   `Debug_free_minor` (memory.h), `caml_gc_phase != Phase_sweep_main` (domain.c:2313 +
   `caml_orphan_ephemerons` major_gc.c:391), `young_ptr == young_end` (minor_gc.c:439) +
   its poison, the `Caml_state` vs `Caml_state_opt` bug in `caml_mmtk_enter/leave_blocking`
   (see item 2), and the two `check_minor_heap` stock-arena asserts (domain.c:520) that
   broke every native domain-spawning `tests/parallel` test. **Plus a REAL race fixed:**
   `caml_domain_terminate`'s MMTk park released `domain_lock`, letting a fresh domain
   reuse a slot mid-teardown → debug `memprof == NULL` assert, release
   `bind_mutator … already registered` panic. Fix: `caml_mmtk_park_terminating()` parks
   without dropping the lock (safe — the domain has left the OCaml STW set). Disabled
   `major_gc_wait_backup` (needs the stock backup thread). Remaining `tests/parallel`
   reds are NOT this fix: the GC-burn `domain_*_spawn_burn*` crash is bug #3 (item 10,
   now **fixed**; the remaining spawn-burn hang is bug #3b), and `tak`/`churn` timeouts are
   harness contention. (Build-CI red 1 of 3 → now just the i386 + opam reds.)
2. ✅ **Removed `caml_mmtk_enabled`** (~35 sites collapsed to unconditional MMTk; 153
   lines deleted). No init-reordering was needed — no OCaml *value* allocation happens
   pre-init (the `domain_create` allocs are C/`caml_stat`/mmap), and the region barrier
   self-gates on `caml_mmtk_generational`. The one real pre-init hazard the build+run
   caught: `caml_mmtk_enter/leave_blocking` (reached via `caml_open_descriptor_in` at
   startup with `Caml_state` still NULL) now guards `Caml_state_opt != NULL` — the raw
   thread-local, NOT the `Caml_state` macro, which is
   `(CAMLassert(Caml_state_opt != NULL), Caml_state_opt)` and would trip its own assert
   in the debug runtime in exactly the NULL case being guarded (see item 1). Verified: clean
   `make world.opt` (bytecode + native self-host) + bug-#2 ocamldoc repro + native
   Immix/StickyImmix runs.

**Phase 2 — native generational correctness + finish the stock-GC excision (M9 st.2–5)**
3. ✅ Native `caml_modify`/`caml_initialize` MMTk write barrier wired (was a no-op under
   `#ifdef NATIVE_CODE`). Self-gates on `caml_mmtk_generational`, so non-generational plans
   pay only a branch. Verified A/B on turing with a targeted old→young test (mature array
   reachable only via a global): native StickyImmix returned 511500 (wrong — young tuples
   reclaimed) without it, 1000000 (correct) with it; native Immix 1000000 either way.
   Unblocks correct native generational (StickyImmix now; GenImmix TLAB aliasing in M8).
4. Land the stock-major-GC body deletion (merge `m9-stage3-delete`).
5. Finish stock-minor-GC remnants (`major_ref`/`ephe_ref` structs, `caml_minor_collection`,
   `caml_alloc_small_dispatch`'s stock path).
6. ✅ Removed the per-domain minor-heap **arena**: `allocate/free/reallocate_minor_heap_arena`
   deleted; domain create/terminate + `caml_set_minor_heap_size` + `stw_resize` no longer
   commit/free a stock arena. The domain's `young_*` are bootstrapped from MMTk
   (`caml_mmtk_refill_tlab` via `caml_mmtk_domain_init`) — verified nothing allocates an OCaml
   value in the create window before that. `minor_heap_wsz` is set to the nominal size (for
   Gc.stat/Gc.get + minor-table sizing). The address-space **reservation** is KEPT (it still
   bounds `Is_young`, address_class.h) — retiring it is part of #8. Verified on turing:
   multidomain spawn/terminate (Immix+StickyImmix), gc-roots, moving-GC, simple programs.
7. Reimplement `Gc.stat`/`quick_stat`/counters/`allocated_bytes` on MMTk stats — the
   **prerequisite for deleting `shared_heap.c`**. The stock shared heap is now a
   permanently *empty* heap (MMTk does all allocation), but its lifecycle + stats are
   still wired into 7 files: `domain.c` (`caml_{init,free,orphan}_shared_heap`,
   `caml_adopt_all_orphan_heaps`, `caml_assert_shared_heap_is_empty`, `caml_finalise_heap`,
   the `d->shared_heap` field), `gc_stats.c` (`caml_collect_heap_stats_sample` /
   `caml_accum_orphan_heap_stats`), `custom.c`/`major_gc.c`/`sys.c`/`gc_ctrl.h` macros
   (`caml_heap_size`/`caml_heap_blocks`/`caml_top_heap_words`), `startup_aux.c`
   (`caml_finalise_freelist`). The empty heap reports size/stats ~0 — which is why
   `Gc.stat` heap fields and the `subarraystub` test read 0 ("Not enough GC cycles").
   ✅ DONE (the deletion): heap-size consumers rewired to MMTk (`caml_mmtk_heap_size_bytes`
   → `mmtk_ocaml_total_bytes`; peak/blocks stubbed), the empty-heap lifecycle dropped from
   domain.c/startup_aux.c/gc_stats.c, and `shared_heap.c`/`.h` deleted (the
   `caml_domain_state.shared_heap` field kept NULL to avoid the native-codegen struct-offset
   ripple — remove it later in an ABI-aware pass). ✅ Collection counts: confirmed
   `Gc.major`/`full_major`/`compact` already FORCE a real MMTk collection and increment
   `major_collections` (+5/+5/+5, native+bytecode) — `caml_mmtk_collect →
   handle_user_collection_request(force=true) →` scheduler bumps `GC_COUNT`; no code change
   needed, just documented the per-field MMTk semantics in gc_ctrl.c. Disabled 3
   stock-GC-pacing-dependent tests (`subarraystub` custom-block pacing; `test_compact_full`
   compact=+3major+1compaction; `boundscheck` loops `minor_collections<1000` → would hang).
   🟡 **REMAINING (new follow-up): `Gc.minor_words` allocation accounting** — the MMTk alloc
   fast paths (`caml_mmtk_alloc_small`, `caml_mmtk_refill_tlab`) don't feed `stat_minor_words`,
   so `Gc.minor_words`/`Gc.counters` under-report (fails `gcwords`/`opaque`/`with_tag`). Fix in
   the alloc paths (accumulate words per allocation).
8. Header/metadata reconciliation, in two parts:
   (a) **stock color/mark header bits vs MMTk side metadata** — audit what still reads the
   stock header colour and reconcile with MMTk's mark state.
   (b) **retire the `Is_young` address-space reservation** (kept by #6). Finding (2026-06-22):
   under TLAB nursery-aliasing nothing is ever allocated in `[caml_minor_heaps_start,
   caml_minor_heaps_end)` (young objects live in MMTk Immix blocks outside it), so
   **`Is_young(v)` is always false** (already noted at `array.c:240`). The ~8 consumers
   (`weak.c`, `finalise.c`, `memprof.c`, `globroots.c`, `obj.c`, `intern.c`, `fiber.c`,
   `minor_gc.c`) therefore run their always-false branch — each must be audited to confirm
   that is the MMTk-correct behaviour (vs needing MMTk's own nursery notion) before the
   reservation + macro can go. Entangled with #11 (`weak.c`/`finalise.c` Is_young checks are
   part of the weak/finaliser-during-collection path), so do alongside it. Needs per-consumer
   reasoning + sanity-at-small-heap verification — not a mechanical delete.

**Phase 3 — correctness (testsuite-driven)**
9. Triage the all-plans testsuite CI (all 11) and fix the per-plan failures it surfaces.
10. bug #3 — GC-burn `parallel/domain_*_spawn_burn*` SIGSEGV / MMTk panic
    `cannot trace object 0x1 / 0x11 …`. **ROOT-CAUSED + FIXED (2026-06-22).** The MMTk
    **stop-the-world barrier was a no-op**: `caml_mmtk_enter_blocking` (called after the
    blocking-section hook nulled `Caml_state`) skipped its `+1` while `leave` still did the
    `-1`, so the safe-stopped counter **underflowed** (`usize`; `stopped >= n` always true)
    and the GC scanned domains that had not stopped — tracing their live, mutating stacks
    where a slot held a tagged immediate. Fix: capture the domain in
    `caml_enter/leave_blocking_section` and pass it to `caml_mmtk_enter/leave_blocking(dom)`
    so the count balances (`signals.c`, `mmtk.c`, `mmtk.h`; no binding change). Verified on
    turing: 0 immediate crashes / 20 runs (pristine 100%), bytecode + native regress clean.
    **bug #3b (residuals — separate, pre-existing, NOT this bug):** a rare (~1/30) `cannot
    trace` with a *wild* (non-immediate) value = a stale-root-slot race (freed fiber stack /
    reused `gc_regs` / terminating-domain teardown); and a ~30% spawn-burn **hang** in OCaml's
    own domain-spawn/STW machinery (not MMTk's path). The native-`ocamlopt` SIGSEGV was split
    out as **bug #4** (item 12b) and is now **fixed**. See `gc/mmtk/NOTES.md`.
11. Weak refs — fix `process_weak_refs` resurrection ordering (`pr5233`) + the orphaned-ephemeron
    gap; **then retire the transitional `MMTK_WEAK_REFS` flag** (make `process_weak_refs`
    unconditional, like `caml_mmtk_enabled`). Until that fix, `MMTK_WEAK_REFS=0` is the safety
    fallback (conservative never-clear), so it stays.
12. Evacuation-time OOM — graceful `Out_of_memory` in `copy_object` instead of asserting.
12b. bug #4 — native `ocamlopt` SIGSEGV under a tight Immix heap — **FIXED 2026-06-23**
    (rr-confirmed). The `Out_of_memory` raised from `caml_alloc_small_dispatch` on a failed
    TLAB refill unwinds *through* `caml_call_gc` (skipping RESTORE_ALL_REGS), leaking the
    single gc_regs bucket; the compiler's `try_finally` catches the OOM and the next
    `caml_call_gc` faults on the now-NULL free-list. Fix: `caml_mmtk_recycle_gc_regs_bucket()`
    pushes the in-use bucket back before the raise. Verified 25/25→0/25 at Immix 64 MB
    (+20/20 re-run), `world.opt` clean, multidomain OK. (The earlier "corruption" read was a
    stale-binary instrumentation artifact — see `gc/mmtk/NOTES.md`.)

**Phase 4 — breadth + platform**
13. Build-CI: `opam` `test-in-prefix` relocatability — **done** (configure.ac relocatable
    `-lmmtk_ocaml` + stdlib symlink/install + `--remap-path-prefix` DWARF strip). Remaining:
    `i386` (32-bit MMTk staticlib won't build; the job is deliberately skipped for now).
14. macOS native linking (always-on native is Linux-only today).
15. Unwired plans (bytecode-only — none has a native Immix nursery), per CI triage:
    `PageProtect`/`SemiSpace` likely cheap; `MarkCompact`/`Compressor` medium; `GenCopy` ≈
    GenImmix; `ConcurrentImmix` = a SATB write barrier (high-effort, the only high-value one).

**Phase 5 — performance (M8)**
16. **Native GenImmix — copy-nursery TLAB aliasing**: the stock-faithful native generational
    model (vanilla OCaml's minor heap *is* a bump-allocated copying nursery). Point
    `refill_tlab` at the CopySpace nursery bump allocator and let nursery-full drive MMTk's
    nursery GC; reuse the moving-root + post-GC `young`-reset machinery. Depends on #3.
    Candidate native default.
17. Benchmark native configs — GenImmix vs StickyImmix vs Immix → choose the native default;
    plus dynamic heap sizing, inline the bytecode alloc fast-path, GC-thread-count + LOS tuning.

---

**Architecture decision (2026-06-20): MMTk owns the ENTIRE heap (all-MMTk); the
minor↔MMTk coordination fix is SUPERSEDED, not just deferred.**

The proper integration is all-MMTk: no separate OCaml minor GC, so young objects
live in MMTk's *own* heap and the only stop-the-world is MMTk's. This dissolves
the nested-STW hazard by construction — the minor↔MMTk coordination fix would be
throwaway work on the vanilla-minor intermediate, so **we will not do it**.
Bytecode all-MMTk already works (the default mode); **native all-MMTk now works
single-domain too** via TLAB/nursery aliasing (`MMTK_TLAB=1` — the inlined
fast-path bumps an MMTk Immix block; see workstream F + `gc/mmtk/NOTES.md`). The
**vanilla minor + MMTk major** mode (`MMTK_VANILLA_MINOR=1`, the native default
and the fallback when TLAB's Immix `Default` allocator isn't available) is kept as
a *validated fallback*.

**Native all-MMTk = TLAB / nursery aliasing — DONE single-domain
(`MMTK_TLAB=1`).** MMTk owns the nursery: the inlined downward `young_ptr` bump
fills an MMTk Immix block the binding hands over (`mmtk_ocaml_refill_tlab`); on
exhaustion the runtime refills another block instead of running a minor GC. No
OCaml minor GC, no promotion ⇒ no nested STW. The thorns resolved cleanly: bump
direction is irrelevant to Immix's mark-region GC; no `post_alloc` needed (we
don't enable `vo_bit`); `young_limit`'s dual role still works; a post-GC
young-region reset handles relocation under moving plans. Validated across
Immix/StickyImmix, forced defrag (heavy relocation), and clean OOM — see
`gc/mmtk/NOTES.md`. **Multi-domain: DONE** — `Domain.spawn` (`multidom8`, 8
domains) is clean at 16–48 MB (was 11/12 hang at 32 MB). Fix: keep the all-domains
minor-empty STW rendezvous, but neuter the per-domain promotion in TLAB mode (skip
oldify, just reset the young region — refill deferred outside the STW). The global
link is now committable (`configure.ac`); both items done.

**Then → run the OCaml testsuite under MMTk (M7), the primary unknown-bug
surfacer.** It exercises far more object shapes, C primitives, and edge cases
than our handful of programs. Note: the **bytecode** testsuite needs no native
(bytecode all-MMTk works today) so it can run *now* and surface bytecode-path
bugs immediately; the **native** testsuite follows the TLAB work. This is
prioritized ahead of the remaining items (benchmarking and tuning) — fix what the
suite finds first.

Weak/ephemeron + finaliser processing is **done** (M6, on by default): MMTk-native
`Scanning::process_weak_refs` clears weak refs, releases ephemeron data on dead keys,
and runs `Gc.finalise`/`finalise_last` + custom-block finalizers (the old conservative
`ephe_info`-rooting scheme is the `MMTK_WEAK_REFS=0` fallback). The dedicated
weak/ephemeron/finaliser/lazy testsuite dirs (tabled during bring-up) have been
re-enabled: under default Immix, `weak-ephe-final` 14/0, `lazy` 10/0, `lib-lazy`
2/0 all pass (`ephe-c-api` is `skip;` upstream). Fixing them surfaced and closed
the cross-domain finaliser-handover bug (orphaned-finaliser adoption, above); two
minor-heap-specific tests (`finaliser2`, `minor_major_force`) are re-tabled as
incompatible-by-design with an in-file reason. See `gc/mmtk/NOTES.md`.

What works today (MMTk is **always-on**, default plan Immix): every allocation —
bytecode and native — goes through MMTk; the stock minor and major GC are gone.
Immix/StickyImmix/GenImmix collect correctly single- and multi-domain and relocate
objects (validated: ordinary blocks, closures, **infix/interior pointers**, weak
arrays/ephemerons, **continuation/fiber stacks**, multi-domain + moving after forced
defrag). Heap exhaustion raises a catchable `Out_of_memory`. Collections are
**parallel** and **stop-the-world**. The full bytecode compiler bootstraps on MMTk.

---

## GC plans

The binding is *moving-ready* (forwarding-pointer spec, pinning bit, updatable
slots, `copy`/`copy_to`), so plans differ in mutator-side machinery, not trait code.

mmtk-core 0.32 offers 11 plans; **5 are wired up in our binding**, the other 6 are not
(yet). The `Testsuite (all GC plans)` CI workflow (`.github/workflows/testsuite-plans.yml`)
runs the testsuite under all 11 to surface exactly that.

| Plan | Kind | Moving | Wired up? |
|------|------|:------:|:---------:|
| `NoGC` | bump-pointer, no collection | no | ✅ |
| `MarkSweep` | free-list mark-sweep | no | ✅ |
| `Immix` | mark-region w/ opportunistic defrag | yes | ✅ (default) |
| `StickyImmix` | Immix + sticky mark-bit (generational, no copying nursery) | yes | ✅ |
| `GenImmix` | generational, copying nursery + Immix mature | yes | ✅ |
| `SemiSpace` | classic copying (two spaces) | yes | ❌ |
| `GenCopy` | generational, copying nursery + SemiSpace mature | yes | ❌ |
| `MarkCompact` | Lisp-2 mark-compact | yes | ❌ |
| `Compressor` | Compressor-style bitmap mark-compact | yes | ❌ |
| `PageProtect` | debug — page-granularity alloc, protects dead pages | no | ❌ |
| `ConcurrentImmix` | concurrent non-moving Immix using SATB | no | ❌ |

The five wired-up plans, all implemented and validated:

- **Non-moving:** `MarkSweep` (collects, single- and multi-domain) and `NoGC`
  (bump-only, never reclaims — short programs / bring-up only).
- **Moving, non-generational:** `Immix` (default) — moves opportunistically (defrag),
  else marks in place. Every heap reference is a precise updatable slot; raw C-held
  values that aren't registered roots are pinned.
- **Generational:** `StickyImmix` (in-place nursery) and `GenImmix` (copying nursery)
  — the mutator write barrier (`caml_modify`, bytecode `SETFIELD`/`SETVECTITEM`,
  `caml_initialize`) and the `MemorySlice` array-copy barrier are implemented; the
  log-bit metadata is reserved.

**Native** allocates from a TLAB aliased onto an MMTk Immix block, so it needs an
Immix nursery allocator: only `Immix` and `StickyImmix` run native. `GenImmix`
(copying nursery), `MarkSweep`, and `NoGC` are bytecode-only and abort at startup on
native. Concurrent collection is not available — mmtk-core 0.32's released plans are
all stop-the-world.

---

## Workstreams (not strictly ordered)

### A. Finish M3 (Immix)
- ✅ **Clean `Out_of_memory`** (commit). `VMCollection::out_of_memory` returns
  instead of panicking; `mmtk_ocaml_alloc` propagates the null, and the C alloc
  wrappers raise `caml_raise_out_of_memory` from a C frame (raising through
  MMTk's Rust frames would be unsound). Validated under MarkSweep and Immix.
- ✅ **Soak** done: 150 iterations of multi-domain + infix under forced defrag,
  clean (see `gc/mmtk/NOTES.md`).
- **Pinning validation** — *largely covered, broaden later*. OCaml's stock GC
  already moves objects, so correct C code already roots its values; the residual
  risk is unrooted raw pointers to assumed-immovable old objects. Forced
  defrag-every-block (moves everything) is the stress test and currently passes;
  broaden via the testsuite (workstream G). Pin explicitly only if a real failure
  surfaces (`object_pinning` is enabled, pinning bit reserved).
- **Evacuation-time OOM** still asserts in `copy_object` if `alloc_copy` fails
  mid-defrag (Immix reserves headroom to avoid it); convert to a graceful path
  if it ever bites.

### B. Parallel collection — ✅ verified
Collections run on multiple GC worker threads (`MMTK_GC_THREADS`). Verified
correct across 1..16 threads (both plans) and that marking scales — ~8.4x at 16
threads on a high-parallelism live set; flat on linked lists (sequential,
latency-bound). See `gc/mmtk/NOTES.md`. GC pause time/count are now instrumented
(`mmtk_ocaml_gc_count`/`_gc_time_ms`, reported under `MMTK_VERBOSE`), which also
feeds workstream H. Remaining: tune default thread count; broader benchmarking.

### C. Generational plans (GenImmix / StickyImmix) — ✅ done
Mutator write barrier implemented. OCaml's `caml_modify` gets only a field
address, not the object, so the object-remembering barrier doesn't fit; we use
MMTk's **region barrier** (`memory_region_copy_post`) to remember the modified
*slot* — matching OCaml's own slot-based remembered set — via a real
`OCamlMemorySlice` (`MemorySlice`). Wired from `write_barrier` (covers
`caml_modify`/`caml_modify_field`/atomics/bytecode `SETFIELD`/`SETVECTITEM`),
`caml_initialize`, and `caml_uniform_array_fill`; gated by `caml_mmtk_generational`
so it's a no-op for non-gen plans. Validated: aged-array ← young-tuple survives
nursery GCs (checksum matches stock) under GenImmix and StickyImmix; the full
moving/multidomain/oom battery passes too. Caveat: weak/ephemeron unsafe under
moving plans (see E).

### E. Runtime feature support
OCaml semantics MMTk must preserve:
- **Weak arrays & ephemerons** (`Weak`, `Ephemeron`, `Weak.Make`, …) — two schemes,
  selected by `MMTK_WEAK_REFS`:
  - *default (off)* — interim `caml_mmtk_scan_ephe_roots` roots the
    `domain->ephe_info` lists, pinning each ephemeron block and reporting its fields
    as updatable root slots. Memory-safe under moving plans, but keeps the whole
    ephemeron graph alive: **weak refs never clear**.
  - *on* — **`Scanning::process_weak_refs` (M6, partial)**: ephemeron mark fixpoint
    (retain data iff all keys reachable) + clean pass (clear dead keys/data, forward
    survivors). Basic weak-clear works in smoke tests, but the weak+finaliser
    resurrection ordering is wrong (`pr5233`) — a value resurrected only for its
    finaliser must still read cleared through a weak pointer.
- **Finalisers** — three mechanisms, not one:
  - `Gc.finalise` / `Gc.finalise_last` (OCaml finaliser table). *Default (off)*: don't
    run (root scan `do_final=1` keeps values alive). *`MMTK_WEAK_REFS` on*: **run** —
    `process_weak_refs` retains+enqueues dead `finalise` values and enqueues dead
    `finalise_last` as unit; root scan uses `do_final=0`. Basic case validated (smoke).
  - **Custom-block finalizers** (`Custom_operations.finalize` — Bigarray, `Int64`,
    channels, …): **implemented** via MMTk's finalizer queue (`add_finalizer`/
    `get_finalized_object`) — register at `caml_alloc_custom` *and* the unmarshal path
    (`intern.c`), drain + run finalize at a safepoint (`caml_mmtk_run_custom_finalizers`).
    `pr3612` passes. Replaces the stock `shared_heap`-sweep finalization for stage 3.
- **Lazy values** — work today (ordinary mutable blocks; no special GC support
  needed). Verified under MarkSweep + Immix.
- **`Gc` module** — ✅ `Gc.major`/`full_major`/`compact`/`major_slice` now route
  to MMTk (`caml_mmtk_collect` → `handle_user_collection_request`) instead of the
  stock major-GC machinery. The stock path ran `caml_finish_major_cycle` on the
  bypassed stock heap and **corrupted state under TLAB Immix** (channel mutex
  pointer clobbered → SIGSEGV; repro `Array.init 300…; Gc.full_major ()`). Still
  TODO: `Gc.stat`/`allocated_bytes`/counters report stock numbers (meaningless
  under MMTk, but not a crash) — map to MMTk stats.

### F. Native-code integration — 🔜 active
Native inlines a downward bump-pointer alloc in a dedicated register
(`ALLOC_PTR`=`young_ptr`), slow path via `caml_call_gc` → `caml_garbage_collection`
→ `caml_alloc_small_dispatch`, roots via frame descriptors.

**Done — vanilla-minor native (fallback, validated):** keep the stock minor heap,
redirect promotion (`alloc_shared`) + `caml_alloc_shr` to MMTk, native roots via
`caml_do_roots`. Single-domain works (allocation/promotion/major-GC/roots correct,
1/2/3 GCs at 48/32/24 MB). Multi-domain runs but hangs intermittently on the
nested-STW hazard — **not being fixed** (superseded by all-MMTk below). Linked for
validation with `-cclib -Wl,--whole-archive <libmmtk_ocaml.a> …`.

**Done — all-MMTk = TLAB / nursery aliasing (`MMTK_TLAB=1`, single-domain).**
MMTk owns the nursery: `mmtk_ocaml_refill_tlab` drives the mutator's Default Immix
allocator to hand OCaml a block as its young region (read `bump_pointer.{cursor,
limit}` via `allocator_impl_mut::<ImmixAllocator>`, then set `cursor = limit` to
eject it so direct `caml_alloc_shr` allocs don't collide). `caml_alloc_small_dispatch`
refills instead of doing a minor GC; `caml_poll_gc_work`/`caml_empty_minor_heaps_once`
skip minor work; `caml_mmtk_uninterrupt` resets the young region post-GC (handles
relocation under moving plans). The open problems dissolved: bump direction is
irrelevant to Immix; no `post_alloc` (no `vo_bit`); `young_limit` dual role
preserved. Bug found+fixed: a >line-size probe took Immix's `overflow_alloc` →
inaccessible `large_bump_pointer` → SEGV; fixed by probing one word and sizing the
region up. Validated Immix/StickyImmix incl. forced defrag; clean OOM.
**Multi-domain: DONE** (was 11/12 hang at 32 MB; now 0 hangs across 16–48 MB, 8
domains). The deadlock was terminating domains spinning in `caml_domain_terminate`
because the TLAB short-circuit of `caml_empty_minor_heaps_once` removed the
all-domains minor-empty STW rendezvous. Fix: keep that STW, but in
`caml_empty_minor_heap_promote` skip the oldify (a `goto`, EV-balanced) and just
reset the young region (`young_ptr = young_start`; refill deferred outside the STW,
so no nested GC). The global link is now committable via `configure.ac`.
Vanilla-minor native remains the fallback when a plan has no Immix Default allocator.

### G. Testsuite — primary unknown-bug surfacer (high priority, after native)
Run OCaml's own testsuite under MMTk — the broadest validation we have, and the
fastest way to flush out bugs our ad-hoc programs miss. Plan:
- **Prerequisite (discovered): the MMTk staticlib must be on the global link
  line.** `ocamltest` is built `-custom`, and `-custom`/native test exes link
  `libcamlrun.a`/`libasmrun.a` — which now contain the glue (`mmtk.c`) and so
  reference `mmtk_ocaml_*`. `make ocamltest` fails to link them today. A per-target
  `-cclib <staticlib>` does **not** work: `ocamlc -custom` places `-cclib` flags
  *before* the runtime lib, so a single-pass linker misses it (and `--whole-archive`
  hits the reverse dependency — the staticlib references `caml_mmtk_scan_ephe_roots`
  back in the runtime lib). The correct fix is the **global link**: add the
  staticlib (+ its native libs) to `bytecomp_c_libraries` and `native_c_libraries`
  via `configure.ac`, which `ocamlc`/`ocamlopt` place *after* the runtime lib — the
  same ordering that makes the standard `ocamlrun` link resolve. This is the
  deferred "global `native_c_libraries` link" item; doing it needs a full-world
  rebuild + a bootstrap/`.opt`-tools check (and a macOS `--whole-archive` ⇒
  `-all_load` equivalent). Do this first, then:
- Run a slice with `MMTK_ENABLED=1 MMTK_PLAN=MarkSweep` (NoGC can't sustain the
  compiler) in the environment so the test programs use MMTk.
- **Bytecode suite** (`MMTK_PLAN=MarkSweep`/`Immix`) is independent of native; the
  **native suite** runs under `MMTK_TLAB=1` (Immix/StickyImmix), now including
  `Domain.spawn` tests (multi-domain TLAB fixed — workstream F).
- Triage failures into *known unsupported feature* (weak/ephemeron clearing,
  finalisers, `Gc.*` semantics, mixed blocks) vs *real bug* — fix the real bugs,
  feature-gate/skip the rest. Track which suites are gated on which feature.
- Consider a dedicated "mmtk" ocamltest variant for repeatability.

**Findings (2026-06-20, global-link applied on the build box):**
- **Run the native suite under TLAB** (`MMTK_TLAB=1`, Immix/StickyImmix) — it runs
  the native compiler; vanilla-minor crashes the compiler (Buffer corruption in
  `asmlink` — a vanilla-minor remembered-set bug, moot if we standardize on TLAB).
- **`testing.cma` must be built** (`make ocamltest` + `make testsuite/lib/testing.cmxa`);
  `make one` doesn't, so dirs beyond `tests/basic` that `open Testing` all fail to
  compile without it. This — not weak/ephemeron — was what tanked the first broad
  sweep.
- With the lib built: **core passes under TLAB Immix** — `tests/basic` 33/40,
  `basic-more` 20/22; the failures are **tabled features** (lazy, finalisers, weak,
  ephemerons), being disabled (remove their `(* TEST *)` block + a comment).
- Fixed: `Gc.major/full_major/compact/major_slice` corrupting state under TLAB
  (now route to MMTk).
- **Weak tables made memory-safe under moving**: `caml_mmtk_scan_ephe_roots` pins
  each ephemeron/weak block (`mmtk_ocaml_pin_object`) so its reported interior-slot
  roots stay valid; the compiler's internal weak hashtables now survive a
  compile-time moving GC. (Memory safety only; weak *semantics* stay tabled — E.)
- **Testsuite flakiness root-caused (ASLR vs MMTk metadata mmap), not correctness.**
  ~10% of tests flaked with a *different* set each run; every one sampled has
  byte-identical stock-vs-MMTk output. The real failure is at startup: MMTk
  sometimes aborts `failed to mmap meta memory: File exists (os error 17)` because
  ASLR drops something into the fixed range it maps side-metadata into. **Run the
  suite under `setarch $(uname -m) -R`** (ADDR_NO_RANDOMIZE, inherited) → flakiness
  gone (0/40). A binding-side deterministic-metadata fix is a follow-up.
- **Run recipe**: build `testing.{cma,cmxa}`, then `setarch $(uname -m) -R env
  MMTK_PLAN=Immix MMTK_HEAP_SIZE_MB=2048 make -C testsuite one DIR=tests/<dir>`
  (MMTk is always-on; native nursery mode auto-selected from the plan). For a full
  bytecode run set `native_compiler=false`/`native_dynlink=false` in
  `ocamltest/ocamltest_config.ml` and use a per-dir timeout cap.
- **Definitive result — full bytecode suite under StickyImmix (2026-06-21): 1476/1524
  pass.** All core dirs pass. The 48 non-pass: 1 multidomain regression (bug #3,
  `parallel/domain_parallel_spawn_burn_gc_set` SIGSEGV — **fixed 2026-06-22**, the MMTk STW
  barrier counter underflowed; see `gc/mmtk/NOTES.md`) + 47 unsupported-feature failures (weak/finaliser, now fixed by
  `MMTK_WEAK_REFS=1`; `Gc.stat`/memprof/runtime-events). The earlier narrow run
  (95/96 across 14 `basic*`/`callback` dirs under TLAB Immix) is superseded.
  One benign known diff: `callback/signals_alloc.ml` **bytecode** variant delivers a
  signal one allocation-step differently under MMTk (`01243` vs `01234`) — the signal
  *is* handled; **native** passes. Every other sampled "failure" was tabled-feature, missing-`testing.cma`,
  or ASLR-flake — never an MMTk correctness difference (output byte-identical to stock).
  Next: extend to more dirs (`lib-*`, `typing-*`, `effects` [fibers], `tool-*`).

### H. Benchmarking **and optimisation** (first-class workstream)
Benchmark MMTk plans against the stock OCaml GC — throughput, pause time, memory —
on representative workloads, then *optimise* (this is where MMTk-OCaml currently
loses to stock, so it must be tracked as real work, not an afterthought).

**First baseline (2026-06-20, `gcbench` native, single-domain, large persistent
live set ~192 MB + heavy churn — a GC-heavy worst-ish case):**

| Config | wall | RSS |
|---|---|---|
| stock GC | **3.85 s** | 440 MB |
| MMTk Immix 512 MB | 14.1 s | 524 MB |
| MMTk Immix 1024 MB | 7.0 s | 1.0 GB |
| MMTk Immix 2048 MB | 5.9 s | 2.1 GB |
| MMTk StickyImmix 1024 MB | **5.4 s** | 1.25 GB |

So today MMTk is **~1.4–1.8× slower and uses more memory** here. Two structural
reasons (not bugs): (1) **fixed heap** — MMTk reserves the whole `MMTK_HEAP_SIZE_MB`
(RSS ≈ heap; tight heaps thrash: 512 MB → 14 s); stock auto-sizes. (2) stock is
**generational**, so its frequent collections don't re-trace the old set; non-gen
**Immix re-traces the whole 192 MB live set every GC**. A *generational* MMTk plan
(**StickyImmix**) already closes much of the gap, and more heap headroom helps.

**Optimisation levers + first-round results (2026-06-20):**
1. **Dynamic heap sizing — TRIED, REGRESSED, reverted.** Switched `gc_trigger` to
   `DynamicHeapSize:32M,cap`. On `gcbench` it *thrashed* — a single run took >190 s
   (vs 5.9 s fixed) because it starts at 32 MB with a ~192 MB live set and mmtk
   0.32's grow heuristic ramps too slowly. So a small-min dynamic heap is worse,
   not better, for large-live-set programs. Reverted to `FixedHeapSize`. Future:
   either a much larger/auto min, or investigate mmtk's MemBalancer trigger.
2. **Generational plan (StickyImmix) — faster; StickyImmix bootstrap SEGV + CI bug #2 both fixed.**
   `gcbench` 5.4 s vs Immix 7.0 s (≈1.4× stock), TLAB-compatible. The
   deterministic `parsing/parser.cmo` bootstrap SEGV was **root-caused and fixed**
   (`gc/mmtk/common/src/slot.rs`, `FieldSlot::classify`): a forwarding pointer
   stored in the header word (`in_header(0)`) whose low byte coincides with
   `Infix_tag` (`0xf9`/249) was misread as a real infix header, so a field was
   silently never forwarded. `classify` now consults MMTk's forwarding-bits side
   metadata before trusting the header (mirrors vanilla `oldify_one` checking
   "already forwarded" before `Infix_tag`; the binding injects the one spec at init)
   — see `gc/mmtk/NOTES.md`. A from-scratch **bytecode** `make all` under
   StickyImmix builds the compiler (843 compile steps, 0 crashes). **CI bug #2 (the
   native `ocamldoc Stdlib.3o` SIGSEGV under default Immix) is now also fixed (2026-06-22):**
   it was a *separate* bug — native `intern.c` allocated unmarshalled objects off-heap via
   the stock `caml_shared_try_alloc` (the MMTk path was `#ifndef NATIVE_CODE`), so they were
   untraced and their referents collected → dangling → SIGSEGV in `odoc_man.ml`. Fixed by
   making native intern allocate via MMTk; verified by a from-scratch `make clean && make -j
   world.opt` (incl. manpages) + manpage repro 0/12. Immix stays the default. See
   `gc/mmtk/NOTES.md`.
3. **Inline the bytecode allocation fast path** — bytecode all-MMTk calls
   `mmtk_ocaml_alloc` per object (vs stock's inlined bump); inline a bump fast path.
4. **GC-thread count** — default is `nproc` (28) *per process* (a big chunk of the
   slow self-hosting bootstrap); a smaller default helps short programs but a
   long GC-heavy run wants parallel marking — needs a balanced default.
5. Immix defrag/policy tuning; reduce TLAB-refill overhead; revisit LOS threshold.

### I. M9 — MMTk-only: excise the stock GC
Goal: remove OCaml's stock garbage collector entirely so `ocaml-mmtk` is a
single-GC runtime — no `MMTK_ENABLED` opt-in, no dual code paths, no stock
minor/major GC. This deletes the per-allocation `caml_mmtk_enabled` branch and the
maintenance tax of keeping two GCs correct side by side.

**Gate (prerequisite): a full self-hosting build must run on MMTk — MET.** A from-scratch
`make clean && make -j world.opt` (the CI build, including the native `ocamldoc Stdlib.3o`
manpage step) runs clean under MMTk on the default Immix (and bytecode `make all` on Immix +
StickyImmix — 843 compile steps, 0 crashes). The CI bug #2 that SIGSEGV'd the native ocamldoc
manpage step is fixed (native `intern.c` off-heap allocation, 2026-06-22 — see
`gc/mmtk/NOTES.md`). Stdlib rebuilds clean; the M7 testsuite runs the compiler under MMTk. (`make bootstrap` to a strict fixpoint is
still fiddly for build-system / tree-state reasons — an aborted run leaves `ocamlc`
missing — not GC correctness.)

Stages (each independently buildable + testable):
1. **Always-on — ✅ done.** Dropped `MMTK_ENABLED`/`caml_mmtk_wanted`; MMTk inits
   unconditionally at startup; default plan is Immix (a collecting plan — NoGC can't
   sustain the runtime); the `caml_mmtk_vanilla_minor` native mode is removed.
   `caml_mmtk_enabled` has been **removed** (2026-06-22): no OCaml *value* allocation
   happens pre-init, so the alloc/barrier paths are unconditional MMTk; only the
   blocking-section notify (`caml_mmtk_enter/leave_blocking`) guards `Caml_state != NULL`
   for the early-startup window. The stock-GC code paths are dead — deleted in the stages
   below.
2. **Delete the stock minor GC** (`minor_gc.c`) — 🟡 mostly done. Removed: the
   `MMTK_DISABLE` escape (MMTk is the only collector); the oldify/promotion machinery
   (`oldify_one`, `oldify_mopup`, `oldify_scanning_flags`, `alloc_shared`,
   `try_update_object_header`, the promote oldify body); `ephe_clean_minor` and
   `custom_finalize_minor`; and **all stock minor remembered-set population** — the
   dead `Ref_table_add`/`caml_darken` fallbacks in `write_barrier`, `caml_initialize`,
   and `caml_array_fill` (so `major_ref`/`ephe_ref` are now never written). ~560 lines
   gone; verified across bytecode + native, StickyImmix + Immix, multi-domain,
   custom blocks, and full `make all`/`world.opt`. Remaining (vacuous, lower-value):
   the now-empty `major_ref`/`ephe_ref` table *structs* + `domain_clear`'s clearing of
   them, the `caml_minor_collection` entry, and `caml_alloc_small_dispatch`'s stock
   path — a coordinated struct change, deferred. **Kept:** the all-domains
   minor-empty STW skeleton (domain spawn/terminate rendezvous) and the `custom`
   table (still used by the parked finalizer tracking). **Note:** native `caml_modify`
   doesn't call the MMTk barrier (pre-existing gap; fine for default Immix, a TODO for
   native StickyImmix — see `gc/mmtk/NOTES.md`).
3. **Delete the stock major GC + shared heap** (`major_gc.c`, `shared_heap.c`):
   mark/sweep/slices/mark-stack/pool/LOS. M6 is done (the prerequisite). Done via an
   *inert-first* approach — guard the stock collector to no-op under MMTk, then delete
   the dead bodies. **🟡 inert step MERGED to `5.5+mmtk`** (`caml_darken` / slice
   drivers / `caml_finish_*` no-op under MMTk; finish_* still set
   `marking_done`/`sweeping_done` so `caml_domain_terminate` exits). Validated: clean
   Immix+StickyImmix bootstrap, 25×4 `Domain.join` battery, effects 23/0. The earlier
   `nested_fiber` "blocker" was a *separate* pre-existing bug (the binding never scanned
   continuation fiber stacks — fixed, see `gc/mmtk/NOTES.md`), not the inert step. The
   stock major GC no longer runs. **🟡 deletion step on branch `m9-stage3-delete`**:
   the seven inert entry points are now thin stubs and the dead mark/sweep/slice/cycle
   bodies are removed — `major_gc.c` 2540→1002 lines, `shared_heap.c` 1677→1476 lines
   (dead `caml_sweep`/`large_alloc_sweep`/`verify_swept`/`caml_redarken_pool`/
   `caml_cycle_heap*`; the pool allocator is kept). Builds `world`+`world.opt` clean;
   validated 25×4 `Domain.join` battery + spot-check (gc-roots/effects/basic, callback
   `nested_fiber` passes). Kept: pacing, `caml_orphan_ephemerons`/`_finalisers` and the
   ephemeron machinery they use, `Gc`-stat/phase helpers.
4. **Domain + `Gc` module cleanup**: remove the minor-heap arena
   (`allocate/free_minor_heap_arena`, the reservation) — the nursery comes from
   MMTk; reimplement `Gc.stat`/`quick_stat`/counters/`allocated_bytes` on MMTk
   stats instead of stock counters.
5. **Header/metadata reconciliation**: the stock color/mark bits in object headers
   vs MMTk's side metadata — repurpose/retire the unused header fields.

Risks / notes:
- `boot/` runs on `runtime/ocamlrun`, so once always-on it bootstraps on MMTk;
  regenerate `boot/` via `make bootstrap`.
- macOS native linking is still deferred — always-on there needs the native link
  sorted (currently Linux-only).
- Weak/ephemeron/finalisers (M6) stay tabled; ensure their stubs don't depend on
  stock-GC internals as those are deleted.
- `Gc.stat` semantics shift to MMTk numbers — expect some testsuite reference
  churn.

---

## Key implementation map (pointers for a cold start)

- **Binding** (`gc/mmtk/`):
  - `binding/src/{lib,api,object_model,active_plan,collection,scanning}.rs` —
    VMBinding impls + C ABI (`mmtk_ocaml_*`) + domain registry.
  - `common/src/{header,object_model,scanning,slot}.rs` — version-independent
    value layout, `scan_ocaml_object`, `FieldSlot`.
  - Moving correctness lives in `common/src/slot.rs`: `FieldSlot` caches an
    `info` (NOT_TRACEABLE / 0 / infix byte-offset) at scan time so `store` can
    re-derive an interior pointer as `new_parent + offset` after the parent is
    forwarded.
  - STW coordination + the OCaml-STW/MMTk-STW deadlock fix: `collection.rs`
    (`caml_mmtk_park` hands a parked domain's OCaml-STW participation to its
    backup thread — see `gc/mmtk/NOTES.md`).
- **Runtime glue** (`runtime/`): `mmtk.c` + `caml/mmtk.h` (init, alloc, STW
  poll/park, blocking-section + termination hooks, native TLAB refill), allocation
  redirection in `caml/memory.h` (`Alloc_small`) and `memory.c` (`alloc_shr`),
  TLAB refill / minor-GC bypass in `minor_gc.c` (`caml_alloc_small_dispatch`,
  `caml_empty_minor_heaps_once`) and `domain.c` (`caml_poll_gc_work`), root
  publishing in `interp.c`, per-domain mutator + STW in `domain.c`, blocking
  sections in `signals.c`, unmarshaller routing in `intern.c`. `mmtk.c` compiles
  into both runtimes; MMTk paths are gated at runtime (`caml_mmtk_enabled` /
  `_vanilla_minor` / `_tlab`), not by `#ifndef NATIVE_CODE`.
- **Build**: GNU make (top-level `Makefile`); `Makefile.mmtk` builds the Rust
  staticlib and links it. `make -j` for parallel builds.

## Known caveats / lessons
- Every allocation path must reach MMTk (the M2 torture crash was the
  unmarshaller bypassing the hooks → globals untraced → swept-and-reused).
- `FieldSlot::load` filters pointers outside MMTk spaces (atoms, code,
  pre-enable objects) via `is_in_mmtk_spaces`.
- GC worker `tls = Address::from_usize(1)` sentinel is fine for now
  (`is_mutator` uses registry lookup), revisit only if it bites a copying plan.
- Debugging: `turing` (Linux) has rr + gdb; attach via gdb-as-parent under
  `ptrace_scope=1`. macOS lldb needs `sudo DevToolsSecurity -enable`.
