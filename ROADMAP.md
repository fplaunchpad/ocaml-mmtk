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

Run knobs: `MMTK_ENABLED` (default off), `MMTK_PLAN` (default `NoGC`),
`MMTK_HEAP_SIZE_MB` (default 1024), `MMTK_GC_THREADS`, `MMTK_VERBOSE`. MMTk's own
options are honoured from the environment too (e.g.
`MMTK_IMMIX_ALWAYS_DEFRAG=true MMTK_IMMIX_DEFRAG_EVERY_BLOCK=true` forces
movement for testing).

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
| M5 | **Native-code integration** — single-domain works via vanilla-minor (fallback); proper path = all-MMTk TLAB/nursery-aliasing | 🟡 |
| M6 | Runtime features: weak arrays, ephemerons, finalisers | ⏸ parked |
| M7 | Pass the OCaml testsuite (modulo unsupported features) | ⬜ |
| M8 | Benchmark MMTk plans vs. the stock GC | ⬜ |
| — | Parallel collection: ✅ verified (correct; marking ~8.4x on 16 threads) | ✅ |
| — | GC plans: 9/11 work (incl. SemiSpace, GenCopy, MarkCompact, ConcurrentImmix); PageProtect + Compressor need work — see NOTES matrix | 🟡 |
| — | Concurrent GC: `ConcurrentImmix` exists in 0.32 and runs our tests; concurrent-marking correctness unvalidated | 🟡 |

**Architecture decision (2026-06-20): MMTk owns the ENTIRE heap (all-MMTk); the
minor↔MMTk coordination fix is SUPERSEDED, not just deferred.**

The proper integration is all-MMTk: no separate OCaml minor GC, so young objects
live in MMTk's *own* nursery (e.g. GenImmix's) and the only stop-the-world is
MMTk's. This dissolves the nested-STW hazard by construction — the
minor↔MMTk coordination fix would be throwaway work on the vanilla-minor
intermediate, so **we will not do it**. Bytecode all-MMTk already works (the
default mode); the **vanilla minor + MMTk major** mode (`MMTK_VANILLA_MINOR=1`,
and what native uses today) is kept as a *validated fallback* — if the native
TLAB work below proves intractable, the bounded coordination fix + vanilla-minor
is plan B, doable then (recipe in `gc/mmtk/NOTES.md`).

**Current focus → native-code integration as all-MMTk = TLAB / nursery
aliasing.** The hard part: native inlines a downward `young_ptr` bump into the
stock nursery, so all-MMTk for native means redirecting that bump into an
MMTk-provided nursery buffer (TLAB). Thorns: OCaml bumps *down* vs MMTk's *up*
cursor; per-object `post_alloc` metadata the inlined code doesn't emit;
`young_limit`'s dual role (GC trigger + STW-interrupt poison). This is the
genuine "proper native integration" and removes the coordination problem
outright.

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
- **`Gc` module** (`full_major`/`minor`/`stat`/`compact`/`allocated_bytes`) —
  work in isolation but operate on the bypassed stock heap structures; audit for
  correctness/meaning under MMTk (e.g. `Gc.stat` reports stock counters).

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

**Chosen path — all-MMTk = TLAB / nursery aliasing.** Make MMTk own the nursery
too: point the inlined `young_ptr` bump at an MMTk-provided buffer, refill from
MMTk on overflow. No OCaml minor GC ⇒ no nested STW. Open problems: bump
direction (OCaml down vs MMTk up); per-object `post_alloc` metadata not emitted by
the inlined path; `young_limit`'s dual GC-trigger/STW-poison role; and the global
`native_c_libraries` link (vs the validation-time `--whole-archive`). Keep
vanilla-minor native as plan B.

### G. Testsuite
Run OCaml's own testsuite under each MMTk plan; pass modulo features not yet
supported (E/F). Track which suites are gated on which feature.

### H. Benchmarking
Benchmark MMTk plans (MarkSweep/Immix/…) against the stock OCaml GC — throughput
and pause time — on representative workloads. Stock OCaml's major GC is
incremental/mostly-concurrent with short pauses; MMTk here is parallel STW, so
pause latency is the interesting axis.

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
  poll/park, blocking-section + termination hooks), allocation redirection in
  `caml/memory.h` (`Alloc_small`) and `memory.c` (`alloc_shr`), root publishing
  in `interp.c`, per-domain mutator in `domain.c`, blocking sections in
  `signals.c`, unmarshaller routing in `intern.c`. All under `#ifndef NATIVE_CODE`.
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
