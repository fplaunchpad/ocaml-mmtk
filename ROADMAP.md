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

1. **bug #3c — rare burn-pattern hang (after the bug #3b STW rearchitecture).** A rare
   hang (~2/30 bytecode burn; ~0–1/20 native burn; `dls`/stress are 30/30) remains
   **only** in the `burn` pattern (3 driver domains hammering `Gc.minor`/`Gc.major` +
   25-way spawn bursts) — a *separate* race from bug #3b (now fixed): a GC during the
   tight `Gc.minor` OCaml-minor-STW loop and/or during `caml_mmtk_refill_tlab` at domain
   init (child holds `all_domains_lock`, no backup thread). Fix: route `Gc.minor` to MMTk
   so it doesn't run OCaml's own minor STW, and/or suppress collection around the
   init-time refill. → NOTES `bug #3b` residual (2026-06-23).

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

6. **#16 — native bump-pointer plans.** **GenImmix + GenCopy native: DONE** (2026-06-23) —
   the copy-nursery `BumpPointer` TLAB aliasing; the anticipated "lots of issues" didn't
   materialise because the moving-root fixup is already general (no minor-vs-major root path —
   reused from the major/defrag path), so it was a 2-file change. **GenImmix is the
   stock-faithful generational native default.** → Shipped / NOTES (2026-06-23).
   **`SemiSpace` + `NoGC` native: DONE (2026-06-23)** — they already worked via the `BumpPointer`
   generalization (their Default allocator is a bump pointer); `SemiSpace` is `sanity`-clean (0 Invalid,
   3M+ objects copied). The native set is now **6**: Immix/StickyImmix/GenImmix/GenCopy/SemiSpace/NoGC.
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
     write barrier (a no-op *call* under the default Immix plan, `cmm_helpers.ml:2290`).
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
   - Earlier first-round levers (heap sizing — small-min `DynamicHeapSize` *regressed*;
     StickyImmix closes much of the gap; GC-thread-count `nproc` oversized). → full ranked
     backlog in `PERFORMANCE.md` Appendix A; NOTES `Workstreams archive`.

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
   allocate-black is automatic), plus a real atomics-SATB-ordering bug found + fixed (`d0c721a8b7`);
   sanity-clean, macOS bytecode build verified. **Open (perf, not correctness):** an UNLOG-bit barrier gate
   + the sanity-build-only ~10 MB deadlock (`rr`). → RESEARCH_QUESTIONS RQ1;
   NOTES (2026-06-23).

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
- **#15 — 9 plans wired (bytecode)** — `SemiSpace`/`GenCopy`/`MarkCompact`/`PageProtect` added behind the generic forwarding-spec gate; CLBG byte-identical.
- **#16 — native GenImmix + GenCopy** — generalized the TLAB refill to alias the copy-nursery `BumpPointer` (not just an in-place Immix block); the moving-root fixup was reused from the major/defrag path (no minor-vs-major root path → 2-file change). GenImmix = the stock-faithful generational native default. old→young pointer A/B + `sanity` (3.05M copied, 0 Invalid) clean.
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

**Native** runs **6 plans** — `Immix`/`StickyImmix` (in-place Immix-block TLAB), `GenImmix`/`GenCopy`
(copy-nursery `BumpPointer` TLAB), and `SemiSpace`/`NoGC` (also `BumpPointer` Default) — i.e. every plan
whose Default allocator is a bump/Immix region the inlined TLAB can alias; the moving-root fixup is reused
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
  lines), plus an atomics-SATB-ordering bug found + fixed (`d0c721a8b7`). **Open (perf):** an UNLOG-bit gate +
  the sanity-build ~10 MB deadlock (`rr`).
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
