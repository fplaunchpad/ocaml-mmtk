# mmtk-ocaml roadmap

This is the living plan for bringing up an MMTk-backed garbage collector for
OCaml. It is meant to be picked up cold in a fresh session. Companion docs:
[`README.md`](README.md) (overview + build/run), [`fork-handoff.md`](fork-handoff.md)
(original rationale), [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) (dated design notes
and deferred investigations).

**Project shape.** This repo *is* the OCaml fork (base `5.5.0-rc1`, branch
`5.5+mmtk`), distributed as `mmtk-ocaml`. The MMTk binding is in-tree at
[`gc/mmtk/`](gc/mmtk) and depends on `mmtk-core` 0.32 from crates.io (not
vendored). MMTk is **opt-in** (`MMTK_ENABLED=1`); a normal build and the
compiler bootstrap still run on the stock GC. Approach: **bytecode first**
(native code inlines its allocation sequence, so it can't be swapped by
redirecting a C function); **route all bytecode allocation through MMTk**,
bypassing the minor heap.

Run knobs (as of M9, MMTk is **always-on**): `MMTK_PLAN` (default `Immix`),
`MMTK_HEAP_SIZE_MB` (default 1024), `MMTK_VERBOSE`, and a transitional
`MMTK_DISABLE=1` stock-GC escape (for benchmarking; removed at final excision).
Native uses TLAB nursery aliasing and requires an Immix-family plan. mmtk-core's
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
| M5 | **Native-code integration** — all-MMTk via TLAB/nursery-aliasing (Immix-family plans, auto-selected), **single- and multi-domain** (`Domain.spawn` clean at 16–48 MB); staticlib auto-linked via configure global-link | ✅ done |
| M6 | Runtime features: weak arrays, ephemerons, finalisers | ⏸ parked |
| M7 | Pass the OCaml testsuite — core passes under TLAB Immix: **95/96** across 14 `basic*`/`callback`/etc. dirs (ASLR off via `setarch -R`, tabled-feature tests disabled). Lone miss = a benign bytecode signal-delivery-timing diff (native passes). Broader dirs next. | 🟡 |
| M8 | **Benchmark + optimise** vs. the stock GC — first baseline: MMTk ~1.4–1.8× slower & more memory on a GC-heavy native bench (`gcbench`); structural (fixed heap, non-gen Immix re-traces live set). Optimisation levers identified (dynamic heap, generational default, bytecode fast-path inline, GC-thread count) | 🟡 started |
| **M9** | **MMTk-only: excise the stock GC** — make MMTk always-on, then delete the stock minor/major GC + shared heap; `mmtk-ocaml` becomes a single-GC runtime | 🟡 in progress |
| — | Parallel collection: ✅ verified (correct; marking ~8.4x on 16 threads) | ✅ |
| — | GC plans: 9/11 work (incl. SemiSpace, GenCopy, MarkCompact, ConcurrentImmix); PageProtect + Compressor need work — see NOTES matrix | 🟡 |
| — | Concurrent GC: `ConcurrentImmix` exists in 0.32 and runs our tests; concurrent-marking correctness unvalidated | 🟡 |

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
prioritized ahead of the remaining feature/plan items (weak/ephemeron,
Compressor, benchmarking) — fix what the suite finds first.

Weak/ephemeron + finaliser processing is parked. Note the current constraint:
the conservative interim (rooting `ephe_info`) keeps them alive safely **only
under non-moving MarkSweep**. Under moving plans (Immix opportunistically,
GenImmix/StickyImmix always) the interior field slots it reports go stale when
the ephemeron block is relocated → weak/ephemeron programs can crash/hang there.
Proper fix (MMTk weak-reference processing) is the unpark task. See
`gc/mmtk/NOTES.md`.

What works today: NoGC, MarkSweep, and Immix back all bytecode allocation under
`MMTK_ENABLED=1`. MarkSweep and Immix collect correctly single- and
multi-domain; Immix relocates objects (validated: ordinary blocks, closures,
**infix/interior pointers**, and the multi-domain + moving combination all
produce correct results after forced defrag — a 150-iteration soak under
`MMTK_IMMIX_ALWAYS_DEFRAG`+`DEFRAG_EVERY_BLOCK` was clean). Heap exhaustion
raises a catchable OCaml `Out_of_memory` (not an abort). Collections are
**parallel** (multiple GC worker threads) and **stop-the-world**.

---

## GC plan tiers — what each costs

MMTk offers a ladder of plans. The binding was written *moving-ready*
(forwarding pointer spec, pinning bit, updatable slots, `copy`/`copy_to` all
present), so the plans differ mostly in *mutator-side* machinery and validation,
not in new trait code.

1. **Non-moving (NoGC, MarkSweep, PageProtect) — free.** PageProtect needs the
   same VM contract as MarkSweep, so it's selectable today as a debugging plan.

2. **Moving, non-generational (SemiSpace, Immix, MarkCompact, Compressor) —
   free in code, work in validation.** The object model + slot abstraction
   already do copy/forward/pin and are selected by name. To be *correct*, every
   reference into the MMTk heap must be a precise, updatable slot, and any raw
   `value` held by C that isn't a registered root must be pinned (else it
   dangles after a move). That validation/hardening across the runtime and C FFI
   is the real work — which is why Immix is its own milestone, not a flag flip.
   Immix is the gentlest of the four: it moves only opportunistically and falls
   back to in-place marking. (**Done for Immix**; SemiSpace/MarkCompact/Compressor
   should "just work" but are unvalidated.)

3. **Generational (GenCopy, GenImmix, StickyImmix) — not free.** They need:
   - a GC **write barrier** invoked from the OCaml mutator (`caml_modify`,
     bytecode `SETFIELD`/`SETVECTITEM`, `caml_initialize`) — currently nothing
     calls MMTk's barrier;
   - the `MemorySlice` impl for array-copy barriers (currently
     `UnimplementedMemorySlice`, panics);
   - they sit on a moving mature space, so they inherit tier-2 validation.
   - Upside: the global log-bit metadata is already reserved, so that plumbing
     is done.

4. **Concurrent (e.g. a concurrent Immix) — the most work, and not in 0.32.**
   Needs a SATB (snapshot-at-the-beginning) write barrier, concurrent GC-worker
   coordination, and extending the multi-domain stop-the-world handshake to
   concurrent marking phases. **MMTk 0.32's released plans are all
   stop-the-world**; concurrent collection is research-stage upstream (e.g. LXR)
   and not exposed in the crate we depend on. Treat as upstream-dependent /
   long-horizon.

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

### D. Concurrent collection — more accessible than first thought
`ConcurrentImmix` **is** present in mmtk-core 0.32 and **runs** our tests
(torture/retain/infix) with no corruption. Remaining: verify it actually does
concurrent marking (vs. STW fallback), and that the SATB / snapshot-at-the-
beginning write barrier and concurrent-marking races are handled (our barrier
may be a no-op for it). If it holds up, concurrent GC is far closer than the
"upstream-dependent" framing in the GC-plan-tiers section above (which predates
this finding).

### E. Runtime feature support
OCaml semantics MMTk must preserve:
- **Weak arrays & ephemerons** (`Weak`, `Ephemeron`, `Weak.Make`, …) —
  interim `caml_mmtk_scan_ephe_roots` roots the `domain->ephe_info` lists so they
  don't dangle, **but only safe under non-moving MarkSweep**: it reports interior
  field slots of the ephemeron blocks, which go stale when those blocks are
  relocated under a moving plan (Immix/GenImmix/StickyImmix) → crash/hang. A
  non-moving-allocation attempt regressed MarkSweep/NoGC and was reverted. Proper
  fix = MMTk weak-reference processing (register ephemerons, clear dead keys/data,
  update under moving). Still TODO; weak refs also never clear yet. See NOTES.
- **Finalisers** — first-class (`Gc.finalise`) and last-ditch
  (`Gc.finalise_last`). Don't run yet — the root scan passes `do_final=1` to keep
  finalisable values alive. Wire MMTk's finalizable processing.
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
  MMTK_ENABLED=1 MMTK_PLAN=Immix MMTK_HEAP_SIZE_MB=2048 make -C testsuite one
  DIR=tests/<dir>` (native nursery mode auto-selected from the plan).
- **Definitive result (ASLR off, tabled tests disabled): 95/96** across `basic`,
  `basic-float`, `basic-more`, `basic-io`(+2), `basic-manyargs`, `basic-modules`,
  `basic-multdef`, `basic-private`, `array-functions`, `callback`, `runtime-errors`,
  `exotic-syntax`, `extension-constructor`, `letrec`, `letmodule`, `comparisons`.
  The single miss is `callback/signals_alloc.ml` **bytecode** variant: a signal is
  delivered one allocation-step differently under MMTk (`01243` vs `01234`) — the
  signal *is* handled; benign timing, and the **native** variant passes. Every
  other sampled "failure" across the journey was tabled-feature, missing-`testing.cma`,
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
2. **Generational plan (StickyImmix) — faster but NOT yet usable as default.**
   `gcbench` 5.4 s vs Immix 7.0 s (≈1.4× stock), TLAB-compatible — but its
   **bootstrap SEGVs** (deterministically, compiling `parsing/parser.cmo` during
   `coreboot`). So it stays unselected. **Crucially, this is the same latent
   moving-GC correctness bug that causes the rare ocamldoc crash — and StickyImmix
   triggers it reliably.** Root-causing it via the StickyImmix `parser.cmo` repro
   is now the **top next task**: it unblocks *both* the StickyImmix perf win *and*
   the always-on merge (the ocamldoc crash). Immix (non-generational, only
   opportunistic moving) stays the default — it bootstraps cleanly.
3. **Inline the bytecode allocation fast path** — bytecode all-MMTk calls
   `mmtk_ocaml_alloc` per object (vs stock's inlined bump); inline a bump fast path.
4. **GC-thread count** — default is `nproc` (28) *per process* (a big chunk of the
   slow self-hosting bootstrap); a smaller default helps short programs but a
   long GC-heavy run wants parallel marking — needs a balanced default.
5. Immix defrag/policy tuning; reduce TLAB-refill overhead; revisit LOS threshold.

### I. M9 — MMTk-only: excise the stock GC
Goal: remove OCaml's stock garbage collector entirely so `mmtk-ocaml` is a
single-GC runtime — no `MMTK_ENABLED` opt-in, no dual code paths, no stock
minor/major GC. This deletes the per-allocation `caml_mmtk_enabled` branch and the
maintenance tax of keeping two GCs correct side by side.

**Gate (prerequisite): a full self-hosting bootstrap must run on MMTk.** Being
validated now — a from-scratch `make` under `MMTK_ENABLED=1 MMTK_PLAN=Immix`
(the compiler compiling itself + the world on MMTk); then `make bootstrap` (the
strict self-hosting cycle, regenerating `boot/`). Stdlib already rebuilds clean
under MMTk; the M7 testsuite already runs the compiler under MMTk.

Stages (each independently buildable + testable):
1. **Always-on.** Drop `MMTK_ENABLED`/`caml_mmtk_wanted`; MMTk inits
   unconditionally at startup. Default plan becomes a *collecting* one (Immix) —
   NoGC can't sustain the runtime. Remove the `if (caml_mmtk_enabled …)` gates in
   `Alloc_small`/`caml_alloc_shr`/write barrier/dispatch/domain-init — they become
   unconditional MMTk. Stock-GC code is now dead.
2. **Delete the stock minor GC** (`minor_gc.c`): oldify/promotion,
   `caml_empty_minor_heap*`, the remembered-set tables (`major_ref`/`ephe_ref`/
   `custom`), the stock path of `caml_alloc_small_dispatch`. Young region is purely
   the TLAB; the write barrier keeps only MMTk's generational form.
3. **Delete the stock major GC + shared heap** (`major_gc.c`, `shared_heap.c`):
   mark/sweep/slices/mark-stack/pool/LOS. `caml_alloc_shr`/`caml_alloc_small` go
   straight to MMTk.
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
