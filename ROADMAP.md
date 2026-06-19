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
| M3 | **Immix** (moving): copy/forward, infix-pointer fixup, updatable roots | 🟡 core done |
| — | Clean `Out_of_memory`; pinning validation across runtime/FFI | ⬜ M3 remainder |
| M4 | Generational plans (GenImmix / StickyImmix) — needs write barrier | ⬜ |
| M5 | Runtime features: Lazy, finalisers, weak arrays, ephemerons | ⬜ |
| M6 | Native-code integration | ⬜ |
| M7 | Pass the OCaml testsuite (modulo unsupported features) | ⬜ |
| M8 | Benchmark MMTk plans vs. the stock GC | ⬜ |
| — | Verify/tune parallel collection; concurrent collection (upstream-dependent) | ⬜ |

What works today: NoGC, MarkSweep, and Immix back all bytecode allocation under
`MMTK_ENABLED=1`. MarkSweep and Immix collect correctly single- and
multi-domain; Immix relocates objects (validated: ordinary blocks, closures,
**infix/interior pointers**, and the multi-domain + moving combination all
produce correct results after forced defrag). Collections are **parallel**
(multiple GC worker threads) and **stop-the-world**.

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
- **Clean `Out_of_memory`.** Today `copy_object` asserts (aborts) if evacuation
  can't allocate, and the mutator alloc path doesn't handle a null result.
  Implement `VMCollection::out_of_memory` to raise OCaml's `Out_of_memory`.
  Caution: the OCaml raise longjmps; do it from a C frame (set a flag / return
  from the Rust hook, then `caml_raise_out_of_memory` in `caml_mmtk_alloc*`),
  not by longjmping through MMTk's Rust frames.
- **Pinning validation.** Audit places that hold a raw `value`/interior pointer
  across a potential GC without it being a registered, updatable root —
  especially C primitives and the FFI. Pin where needed (`object_pinning` is
  enabled, pinning bit reserved). No pin has been required by the tests so far,
  but this is the tier-2 "real work."
- **Soak** the moving + multi-domain combination under forced defrag (the
  gdb-as-parent hang catcher / gauntlet scripts used for M2 multi-domain apply).

### B. Parallel collection (works; verify + tune)
Collections already run on multiple GC worker threads. Explicitly test
correctness + scaling across `MMTK_GC_THREADS`, confirm determinism with a
single worker, and document the knob/defaults.

### C. Generational plans (GenImmix / StickyImmix)
Implement the GC **write barrier** on the mutator side and the `MemorySlice`
impl. Wire `caml_modify` / bytecode `SETFIELD`/`SETVECTITEM` / `caml_initialize`
to MMTk's object barrier. Then validate as a moving plan (tier 2).

### D. Concurrent collection
Upstream-dependent (see tier 4). Scope only once a concurrent plan is available
in mmtk-core, or decide to track/contribute upstream.

### E. Runtime feature support
OCaml semantics MMTk must preserve:
- **Lazy values** (`Lazy`, forcing, `Obj.*` shape changes).
- **Finalisers** — both *first-class* (`Gc.finalise`) and *last-ditch*
  (`Gc.finalise_last`); MMTk has a finalizable-processing path we currently
  bypass (root scan passes `do_final=1` to keep finalisable values alive).
- **Weak arrays** and **ephemerons** (`Weak`, `Ephemeron`) — need MMTk's weak
  reference processing wired to OCaml's weak/ephemeron tables.
  Currently none of these are integrated; they're kept alive conservatively or
  unsupported. Each needs MMTk's corresponding VM hook implemented.

### F. Native-code integration
The hard part deferred from day one: native code inlines a bump-pointer
allocation sequence at every allocation site (`asmcomp`/`emit`). Strategy:
major-heap routing first, then nursery aliasing. This is a large milestone.

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
