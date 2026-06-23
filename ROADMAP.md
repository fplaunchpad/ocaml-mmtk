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

**Project shape.** This repo *is* the OCaml fork (base `5.5.0-rc1`, branch
`5.5+mmtk`), distributed as `ocaml-mmtk`. The MMTk binding is in-tree at
[`gc/mmtk/`](gc/mmtk) and depends on `mmtk-core` 0.32 from crates.io (not vendored).
MMTk is **always-on and the only collector** — no opt-out; the stock minor *and*
major GC have been excised (M9). `MMTK_PLAN` selects the plan (default `Immix`).
Native code uses TLAB nursery-aliasing onto an MMTk Immix block, so it requires an
**Immix-family** plan (`Immix`/`StickyImmix`); bytecode runs under any plan. Run
knobs: `MMTK_PLAN`, `MMTK_HEAP_SIZE_MB` (default 1024, fixed heap), `MMTK_VERBOSE`;
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
| M8 | **Benchmark + optimise** vs. the stock GC — **the open milestone.** First baseline: MMTk ~1.4–1.8× slower & more memory on a GC-heavy native bench (`gcbench`); structural (fixed heap, non-gen Immix re-traces the live set). Levers + native bump-pointer plans below. | 🟡 **open milestone** |
| M9 | **MMTk-only: excise the stock GC** — always-on; stock minor + major GC deleted; `shared_heap.c`/`.h` deleted (−1665 lines, live colour-machinery relocated to `major_gc.{c,h}`); per-domain minor-heap arena removed; `Gc.stat` reimplemented on MMTk stats; `Is_young` reservation retired. `ocaml-mmtk` is a single-GC runtime. **Complete** bar #11 (weak-clear semantics) + the flagged `memprof.c` colour read. Stage/bug depth: `gc/mmtk/NOTES.md`. | 🟢 done |
| — | Parallel collection: verified correct; marking scales ~8.4× on 16 threads (parallel-friendly heaps) | ✅ |
| — | **GC plans:** 9 wired (bytecode), 2 deferred — see the GC plans table below | 🟢 |

---

## Open work (prioritized)

Correctness before performance; dependencies noted. **Depth for every item is in
`gc/mmtk/NOTES.md`** (dated, newest-first) — this list is the index, not the detail.

1. **bug #3b — MMTk-native STW rearchitecture (IN PROGRESS, not committed).**
   Retire the binding's hand-rolled global stop-counter for **per-mutator stop state
   tied to the mutator lifecycle**, so blocking-section / spawn-handshake / terminate
   windows can't desynchronise the stop barrier. Fixes the residual ~20–35%
   `domain_*_spawn_burn*` **hang** (an OCaml-STW vs MMTk-STW deadlock — a parent wedged
   in `caml_domain_spawn`'s handshake is registered-but-not-stoppable) and is expected
   to subsume the rare wild-pointer `cannot trace` stale-root race. Interim stopgap
   (not the real fix): bracket the spawn-handshake wait in
   `caml_enter/leave_blocking_section`. → NOTES `bug #3b` (2026-06-23).

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

5. **#14 / i386 — platform.** macOS native linking (always-on native is Linux-only
   today). The **i386** Build job is deliberately skipped — the 32-bit MMTk staticlib
   won't build (`Makefile.mmtk` `mmtk-lib` Error 127). Hygiene CI (`check-typo`) is red
   on whole-tree non-ASCII/long-lines — scope it to changed files, keep new C/build
   comments ASCII ≤80 col. → NOTES various; ROADMAP archive entry.

6. **#16 — native bump-pointer plans (M8).** Extend native beyond `Immix`/`StickyImmix`
   to the copying/compacting plans (`SemiSpace`/`GenCopy`/`MarkCompact`, and **native
   GenImmix** copy-nursery TLAB aliasing — the stock-faithful generational model, a
   candidate native default) via generalized TLAB nursery-aliasing. Today TLAB needs an
   Immix `Default` allocator; a copying nursery needs its own handling. Depends on #1
   (native write barrier already done). → NOTES native-TLAB entry.

7. **#17 — benchmarking + perf tuning (M8).** The open milestone; ties directly to
   `RESEARCH_QUESTIONS.md`. Levers identified (first-round results in NOTES): dynamic
   heap sizing (a small-min `DynamicHeapSize` *regressed* on large live sets — needs a
   larger/auto min or MemBalancer), generational default (StickyImmix closes much of the
   gap), inline the bytecode alloc fast-path (vs per-object `mmtk_ocaml_alloc`),
   GC-thread-count balance (default `nproc` per process), Immix defrag/LOS tuning. →
   NOTES `Workstreams archive` (benchmark baseline + levers).

### Shipped (done — one line each; depth in NOTES)

- **bug #2** — native unmarshalling allocated off-heap (intern path was `#ifndef NATIVE_CODE`); un-guarded so native interns via MMTk. (CI ocamldoc SIGSEGV resolved; manpage repro 0/12.)
- **bug #3** — MMTk STW stop barrier was a no-op (blocking-section counter underflowed `usize`); fixed by passing the domain to `caml_mmtk_enter/leave_blocking`.
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
- **#15 — 9 plans wired (bytecode)** — `SemiSpace`/`GenCopy`/`MarkCompact`/`PageProtect` added behind the generic forwarding-spec gate; CLBG byte-identical.
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
**9 are wired**, 2 deferred. The `Testsuite (all GC plans)` CI workflow
(`.github/workflows/testsuite-plans.yml`) runs the suite under all 11 to surface
per-plan breakage; CLBG `run.sh validate` is the byte-identical cross-plan gate.

| Plan | Kind | Moving | Wired up? |
|------|------|:------:|:---------:|
| `NoGC` | bump-pointer, no collection | no | ✅ (bytecode) |
| `MarkSweep` | free-list mark-sweep | no | ✅ (bytecode) |
| `Immix` | mark-region w/ opportunistic defrag | yes | ✅ **default** (byte + native) |
| `StickyImmix` | Immix + sticky mark-bit (gen, in-place nursery) | yes | ✅ (byte + native) |
| `GenImmix` | generational, copying nursery + Immix mature | yes | ✅ (bytecode) |
| `SemiSpace` | classic copying (two spaces) | yes | ✅ (bytecode) |
| `GenCopy` | generational, copying nursery + SemiSpace mature | yes | ✅ (bytecode) |
| `MarkCompact` | Lisp-2 mark-compact | yes | ✅ (bytecode) |
| `PageProtect` | debug — page-granularity alloc | no | ✅ (bytecode, manual — exceeds CI time cap) |
| `Compressor` | bitmap mark-compact | yes | ❌ **deferred** (unified obj-ref model) |
| `ConcurrentImmix` | concurrent non-moving Immix, SATB | no | ❌ **deferred** (SATB write barrier) |

**Native** runs only `Immix`/`StickyImmix` — TLAB nursery-aliasing needs an Immix
`Default` allocator; the other plans abort at startup on native (bytecode-only).

**The two deferrals:**

- **`Compressor` — needs a unified object-reference model.** Its bitmap mark-compact
  assumes the object reference *is* the object start; OCaml puts the value reference at
  field 0 with the header one word before (`OBJECT_REF_OFFSET = WORD_SIZE`), so there is
  no single "reference == object start" identity. Supporting it means redesigning the
  object model. (The all-plans CI gate greps for its `requires a unified object
  reference` abort.) → NOTES #15 (2026-06-23).
- **`ConcurrentImmix` — needs an SATB write barrier.** The only high-value unwired plan
  (the low-latency line — see `RESEARCH_QUESTIONS.md`). Our generational barrier is a
  slot-remembering region barrier, not snapshot-at-the-beginning; concurrent marking
  needs SATB. High effort, high payoff — and interesting precisely because OCaml's own
  collector is SATB and the language is immutable-by-default. → NOTES #15 (2026-06-23).

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
  - STW coordination: `collection.rs` (the global stop-counter being rearchitected to
    per-mutator state — open work #1; `caml_mmtk_park` hands a parked domain's OCaml-STW
    duty to its backup thread).
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
