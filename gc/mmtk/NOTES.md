# MMTk-OCaml design notes & deferred investigations

Running notes on design decisions and things we have deliberately deferred.
Each entry is dated and self-contained. Newest first.

---

## M6 design: MMTk-native weak/ephemeron/finaliser via `Scanning::process_weak_refs`

*2026-06-21*

This is the unblock for M9 stage 3 (see the stage-3 audit below). Concrete,
implementable design pinned against mmtk 0.32 + the OCaml 5 ephemeron layout.

**Mechanism — `process_weak_refs`, not `ReferenceGlue`.** OCaml ephemerons are
key→value weak structures (data is retained iff *all* keys are reachable); they do
not map onto MMTk's `ReferenceGlue` (Java Soft/Weak/Phantom) model, so
`reference_glue.rs` stays a stub. The right hook is
`Scanning::process_weak_refs(worker, tracer_context) -> bool` (mmtk
`src/vm/scanning.rs`): called *after* the strong transitive closure, may retain
objects, and if it returns `true` MMTk reruns it after draining the `VMRefClosure`
bucket — exactly the fixpoint iteration ephemerons need. mmtk-ruby uses this hook
for the same reason.

**Tools the hook gives us** (all on `ObjectReference`):
- `object.is_reachable()` — did strong tracing reach it?
- `object.get_forwarded_object()` — new address if a moving plan relocated it
  (Immix defrag / StickyImmix nursery). Use this to *update* surviving weak slots.
- `tracer_context.with_tracer(worker, |t| { t.trace_object(o) })` — *retain*
  (resurrect) an unreachable object and get its (possibly forwarded) address. One
  `with_tracer` per pass, many `trace_object` inside (it batches the closure).

**OCaml ephemeron layout** (`runtime/caml/weak.h`): block fields are
`link@0, data@1, keys@2..`; `caml_ephe_none` is the cleared/empty marker; a weak
array is an ephemeron whose `data == caml_ephe_none`. Ephemerons are threaded on
per-domain lists `domain->ephe_info->{todo, live}` via `Ephe_link`.

**Algorithm** (mirrors stock `major_gc.c` ephemeron marking, run inside
`process_weak_refs`):
1. Walk each domain's `todo`+`live` ephe lists. For each ephemeron `e`:
   - if `!e.is_reachable()` → `e` itself is dead: **unlink it from the list** (so the
     list never dangles) and skip; it gets reclaimed.
   - else for each key slot `k = Ephe_key(e,i)`: if the referent `is_reachable()`,
     update the slot to `get_forwarded_object()`; else the key is dead → set the slot
     to `caml_ephe_none` and mark `e` "incomplete".
   - if `e` had no dead key → retain `data`: `trace_object(data)` and store the
     forwarded ref into `Ephe_data(e)`. If incomplete → set `Ephe_data(e)` to
     `caml_ephe_none`.
2. Return `true` if any data was retained this pass (retaining data may make another
   ephemeron's keys reachable → re-run to fixpoint); else `false`.
3. **Finalisers** (`finalise.c`): a finalisable value that is now unreachable is
   `trace_object`-retained for one more cycle and pushed to the domain's
   `to_do`/`final_fun` run queue (the root scan currently passes `do_final=1` to keep
   *all* finalisable values alive — that conservative flag goes away here).

**C glue, not Rust-walks-OCaml.** The list/slot walking is far cleaner in C with the
`Ephe_*` macros, so add `caml_mmtk_process_ephemerons(is_reachable_cb, retain_cb,
forward_cb, domain)` in `runtime/mmtk.c`; `process_weak_refs` provides the three
callbacks (closing over the tracer) and invokes it per registered domain.
`is_reachable_cb(v) -> int`, `forward_cb(v) -> value` (new addr; identity if not
moved), `retain_cb(v) -> value` (trace + new addr).

**Removal cascade once this lands** (the actual M9-stage-3 payoff):
- Delete `caml_mmtk_scan_ephe_roots` + the `mmtk_ocaml_pin_object` interim pinning in
  `scanning.rs`/`api.rs`/`mmtk.c` (the conservative "keep the whole ephemeron graph
  as strong roots" scheme this replaces).
- Drop the `do_final=1` keep-alive in `scan_roots_in_mutator_thread`.
- `caml_domain_terminate`'s `caml_orphan_ephemerons/finalisers` + the stock
  `caml_finish_*` cycle can then go; the stock major slice drivers can be guarded
  under MMTk; `major_gc.c`/`shared_heap.c` mark/sweep/slice/pool/LOS become dead.

**Landing safely.** Implement behind `MMTK_WEAK_REFS=1` (default = today's
conservative scheme, which is memory-safe but never clears weak refs and leaks
finalisable values). Validate the new path opt-in against the testsuite's
weak/ephemeron/finaliser/lazy tests (the ones tabled in M7) on StickyImmix at a small
heap with `sanity` on — those tests *are* the acceptance spec — then flip the default
and remove the interim scheme. See the stage-3 audit below and [[mmtk-ocaml-bringup-plan]].

---

## M9 stage 3 audit: the stock *major* GC is still load-bearing under MMTk — gated on M6

*2026-06-21*

Stage 2 (stock minor GC) is done and validated (clean `make all` + `world.opt`,
bytecode + native, Immix + StickyImmix, multi-domain). The next stage on paper is
"delete the stock major GC + shared heap." Audit result: **it is not dead code —
it is exercised on every run under always-on MMTk**, so it cannot be guarded-off and
deleted without M6 (MMTk-native weak/ephemeron/finaliser). Mapping:

**What still drives the stock major slice under MMTk** (`caml_request_major_slice`
sets `requested_major_slice` directly → the auto-driver at `domain.c` ~2154 runs
`caml_major_collection_slice`):
- `caml_adjust_gc_speed` (`memory.c`) — custom-block off-heap accounting; fires once
  `extra_heap_resources > 0.2`.
- `advance_global_major_slice_epoch` (`domain.c`) — advances `caml_major_slice_epoch`
  when a domain burns half its (TLAB) minor arena; the epoch check at `domain.c`
  ~2141 then requests a slice.
- (the `alloc_shr` stock path at `memory.c` ~458 is dead — after the
  `if (caml_mmtk_enabled) return caml_mmtk_alloc_shr(...)` early-return.)

**What runs the full cycle machinery under MMTk** — `caml_domain_terminate`
(`domain.c` ~2300, reached at **process exit** for the last domain and on every
`Domain.join`): `caml_finish_sweeping()` → `caml_finish_major_cycle(0)` (last
domain) → `caml_finish_marking()` → `caml_orphan_ephemerons` / `caml_orphan_finalisers`,
looping until no marking/sweeping work remains.

**Why it works today and why a naive cut breaks:** the stock shared heap holds only
the pre-init handful of `caml_alloc_shr` objects (everything after init is an MMTk
object). The slice/cycle marks from roots via `caml_darken` (still called from
`weak.c` ×4 and `finalise.c`), then sweeps. So:
- Making the *slice* inert but leaving `caml_darken` live → `caml_darken` keeps
  pushing to a mark stack nobody drains; `num_domains_to_mark`/`marking_done`
  invariants drift.
- Making `caml_darken` inert but leaving the *slice/cycle* live → mark phase marks
  nothing, sweep frees the live pre-init stock objects → **use-after-free**; and
  termination's mark/orphan logic breaks.
- Making both inert → termination's `caml_orphan_ephemerons/finalisers` + the
  finish-cycle still expect a coherent stock heap state.

**Conclusion / order of work.** Stage 3 is **blocked on M6**, not on a clever guard.
M6 = real MMTk weak-reference processing (a `Scanning::process_weak_refs` pass in the
binding that, after transitive closure, queries `is_reachable` to clear dead weak
slots / queue finalisable values), replacing today's conservative
`caml_mmtk_scan_ephe_roots` (which keeps the entire ephemeron graph alive as strong
roots). Once weak/ephemeron/finaliser no longer route through `caml_darken` and the
stock cycle, `caml_domain_terminate` can drop the `caml_finish_*`/orphan calls, the
slice drivers can be guarded under MMTk, and `major_gc.c`/`shared_heap.c` mark/sweep/
slice/pool/LOS become genuinely dead and deletable. See [[mmtk-ocaml-bringup-plan]].

---

## M9 stage 2 scoping: excising the stock minor GC — entanglement + the bridge it burns

*2026-06-21*

The self-hosting gate is **met** (clean `make all` under MMTk on Immix *and*
StickyImmix after the moving-GC fix), so M9 stage 2 (delete the stock minor GC) is
unblocked on correctness. But it is **not** an isolated removal — audit of what's
reachable under always-on:

- `caml_minor_collection` — called from `array.c` and within `minor_gc.c`.
- `caml_empty_minor_heap*` — woven through `domain.c` (STW handlers, domain
  teardown, `caml_empty_minor_heaps_once`).
- `caml_alloc_small_dispatch` — called from `signals_nat.c` (native alloc slow path)
  and referenced by `mmtk.c`/`domain.c`.
- `oldify_one`/`oldify_mopup` — internal to `minor_gc.c`.
- `Ref_table_add` / remembered set — `array.c` and the *stock fallback* of the write
  barrier in `memory.c` (after the `if (caml_mmtk_enabled) { region_barrier; return; }`
  early-return — dead under always-on, modulo the caveat below).

**Couplings noted while scoping:**
1. **`MMTK_DISABLE` was the stock GC — now removed.** `MMTK_DISABLE=1` used to flip
   `caml_mmtk_enabled` off and run the *stock* collector, which served as the M8
   benchmark baseline. Decision (2026-06-21): benchmark MMTk vs stock by installing
   a **separate vanilla OCaml 5.5 opam switch** instead — no need to carry the stock
   GC in this tree — so `MMTK_DISABLE` and `caml_mmtk_wanted` are deleted (first step
   of the excision). The stock GC is now reachable *only* in the pre-init window
   (caveat below).
2. **`caml_mmtk_enabled` is also the pre-init readiness guard** (brief startup window
   before MMTk init — a handful of pre-init allocations take the stock alloc path).
   The stock fallbacks can't be fully removed until MMTk-init-before-first-alloc is
   done (a separate step); until then the pre-init window can still reach them.

**Proposed deletion order** (each independently buildable + testable; do it on the
checkpointed `5.5+mmtk` head, build + `sanity` + regression each step):
  0. **✅ done** — remove the `MMTK_DISABLE` escape + `caml_mmtk_wanted` so MMTk is
     unconditional; the stock GC is now reachable only in the pre-init window.
  - **MEASURED (2026-06-21): the stock minor heap is never used under always-on, so
    the stock minor GC is vacuous → deletable.** A diagnostic at the MMTk-enable point
    (`caml_mmtk_domain_init`) prints `young_ptr == young_end`, **used = 0 bytes**:
    zero pre-init small allocations. The minor heap is set up but empty when MMTk
    takes over, and post-init every small alloc goes to MMTk — so `oldify` /
    `caml_empty_minor_heap*` / `caml_minor_collection` only ever run on an **empty**
    heap. There is nothing to promote, so deleting the minor GC can't break
    correctness, and **no pre-init-window elimination is needed for the minor GC**
    (my earlier worry that pre-init objects get promoted via `oldify` was wrong —
    they don't exist). The deletion is mechanical (progress):
      • ✅ neuter `caml_empty_minor_heap_promote` for bytecode too (skip under
        `caml_mmtk_enabled`, not just `caml_mmtk_tlab`).
      • ✅ `array.c` `Is_young(init)` branch dropped (dead).
      • ✅ delete the oldify/promotion machinery — promote oldify body, `oldify_one`,
        `oldify_mopup`, `oldify_scanning_flags`, `alloc_shared`,
        `try_update_object_header` (−483 lines, build warning-clean; verified under
        StickyImmix + Immix: parser.ml, multi-domain, Array.make).
      • ✅ delete `ephe_clean_minor` (guarded by `prom.locked_ephemerons`, always
        false now) and `custom_finalize_minor` (body fully `Is_young`-gated → vacuous;
        custom finalization is MMTk's job, parked). Verified incl. a custom-block
        test (Int64 + channels).
      • TODO (interwoven — do as a coordinated change, with native + finalizer +
        write-barrier tests): `caml_empty_minor_heap_domain_clear` + the remembered-set
        tables (`major_ref`/`ephe_ref`/`custom`) + the stock write-barrier fallback in
        `memory.c` (Ref_table/darken — dead under always-on but the write barrier is a
        hot path used by native too) + the custom-table population
        (`add_to_custom_table` in `custom.c`/`intern.c`) + the `caml_minor_collection`
        entry + the stock path of `caml_alloc_small_dispatch`. Then stage 3
        (major GC + `shared_heap.c`).
      KEEP: the all-domains minor-empty STW skeleton (`caml_empty_minor_heaps_once`
      etc.) — the domain spawn/terminate rendezvous.
    Build + boot (`ocamlc`) + multi-domain after each step. (Full `make all` under
    StickyImmix re-validated after the oldify deletion: 0 crashes, 0 errors.)

    **Investigation for the remaining cluster (2026-06-21):**
    - The stock remembered set (`major_ref`/`ephe_ref`) is **dead under MMTk** —
      grep shows MMTk's root scan (`mmtk.c`/`roots.c`/binding) never reads it. It is
      populated by the write-barrier fallback, cleared by `domain_clear`, and never
      consumed. So it (and the fallback that fills it) is safe to delete — but as a
      coordinated change, since the write barrier is hot.
    - **Native `caml_modify` does NOT call the MMTk barrier**: the
      `caml_mmtk_region_barrier` call in `write_barrier` is under `#ifndef
      NATIVE_CODE` (bytecode only); native emits a `caml_modify` Cextcall that falls
      through to the (now-vacuous) stock fallback. This is a *separate* pre-existing
      gap: native StickyImmix has no working generational write barrier via
      `caml_modify` (the default Immix is non-generational, so it doesn't need one).
      Wiring native `caml_modify` → `caml_mmtk_region_barrier` is a prerequisite if
      native StickyImmix is ever to be generationally correct — and should be done
      *before* deleting the stock fallback, or jointly.
  - **Stage 2 essentially done** (2026-06-21): the stock minor GC's active machinery
    is gone — promotion/oldify, ephe/custom minor cleaning, the whole minor
    remembered-set (`major_ref` field + all its populators). What remains is *not*
    dead-but-vacuous code: `caml_minor_collection` is still reached by `Gc.minor`
    (`gc_ctrl.c:240`, runs the neutered STW empty), `caml_alloc_small_dispatch`'s
    stock path handles TLAB refill, and `ephe_ref`/`custom` are still populated by
    weak/custom ops. Those are kept until M6 (weak/ephemeron/finaliser) and the
    dispatch are addressed.
  - **Stage 3 scoping (major GC + `shared_heap.c`) — INTERWOVEN, do as a coordinated
    effort:**
    - `caml_finish_major_cycle` is already prevented under MMTk — `Gc.major`/
      `full_major`/`compact` route to `caml_mmtk_collect` (mmtk.c), which triggers a
      real MMTk collection instead (the stock cycle "corrupts the bypassed shared
      heap"). The *auto* major slice (`domain.c:2154`) still fires if
      `requested_major_slice` is set — need to confirm whether anything sets it under
      MMTk (most setters are in the bypassed `caml_alloc_shr` stock path).
    - `caml_darken` is **still called** from `weak.c` + `finalise.c` (the parked M6
      features), so it (and the mark machinery it drives) cannot be deleted until
      weak/ephemeron/finaliser are reworked on MMTk.
    - `caml_shared_try_alloc` (the stock shared heap) is used by `intern.c`
      (unmarshalling) and the dead `caml_alloc_shr` stock path. Measure pre-init
      large allocations (do any land in the stock shared heap?) before deleting
      `shared_heap.c`; those (if any) need an MMTk home or pre-init-window removal.
    Net: stage 3 is gated on M6 (weak/ephemeron/finaliser) and the intern path — a
    bigger coordinated change than the minor-GC excision.
  a. Reroute/neuter stock call sites in `domain.c` STW + `array.c` so always-on never
     invokes stock minor collection (MMTk drives collection).
  b. Remove the stock write-barrier fallback + `Ref_table` machinery
     (`memory.c`/`array.c`), keeping only MMTk's generational barrier.
  c. Delete oldify/promotion + `caml_empty_minor_heap*` + `caml_minor_collection`
     from `minor_gc.c`.
  d. `caml_alloc_small_dispatch` → MMTk refill only.
Then stage 3 (major GC + shared heap), stage 4 (domain/`Gc` module on MMTk stats),
stage 5 (header color/mark-bit reconciliation).

---

## ROOT-CAUSED + FIXED: the moving-GC bug — forwarding-pointer / `Infix_tag` collision

*2026-06-20*

The latent moving-GC correctness bug (the deterministic StickyImmix `parser.cmo`
SEGV from the entry below, and almost certainly the rare ocamldoc `Lexing.engine`
crash) is **root-caused and fixed**. Fix: `gc/mmtk/common/src/slot.rs`
(`FieldSlot::classify`).

**Root cause.** `classify()` reads the *pointee's* header word `(addr - 8)` to
detect an interior (infix) pointer (`Tag == Infix_tag`, 249). During a moving GC the
pointee may already be **forwarded**, and MMTk stores the forwarding pointer **in the
header word** (`LOCAL_FORWARDING_POINTER_SPEC = in_header(0)`; status bits live in
side metadata). So the word read can be a *forwarding pointer*, not an OCaml header —
and its low byte can equal `Infix_tag` purely by coincidence of the destination
address (observed: forwarding word `0x…dbcf9`, new copy `0x…dbcf8 | status 1`, low
byte `0xf9` = 249). `classify` then computed a garbage infix offset (`wosize` of an
address ≈ 2 billion words), `load()` returned `raw − garbage = ` an unmapped
"parent", `trace_object` no-oped on it, and `store()` wrote the garbage back —
**so the field was silently never forwarded**, leaving a dangling pointer to the
old (now-forwarded) location. Classic order-dependent bug: only bites when the
pointee is forwarded *before* a referencing slot is processed **and** the forwarding
address's low byte happens to be `0xf9`.

**Fix.** Mirror vanilla `oldify_one`, which checks "already forwarded" (`hd == 0`)
*before* testing `Infix_tag` (`runtime/minor_gc.c:268`). MMTk's equivalent of
`hd == 0` is the **forwarding-bits side metadata** (`LOCAL_FORWARDING_BITS_SPEC`:
`0b00` not-triggered / `0b10` being-forwarded / `0b11` forwarded). So in `classify`,
when the header looks like `Infix_tag`, first consult that state: if `addr` is
forwarded, the header word is a forwarding pointer (not a real header) → treat the
slot as an ordinary reference (`info = 0`) so the trace follows the forwarding
pointer and `store` rewrites the slot. For a genuine infix pointer `addr` is
interior to a closure (never an object start), so its bits read not-triggered and we
use the real `Infix_tag` header — forwarding bits are only ever set at object
starts, the same invariant vanilla relies on.

This is checked authoritatively: reading the forwarding bits needs only the concrete
`SideMetadataSpec`, not the `VM` type (the `<VM>` on `object_forwarding::is_forwarded`
only *fetches* the spec). The binding injects that one spec into `common` at MMTk
init (`set_forwarding_bits_spec`), and `classify` does a single side-metadata load on
the rare `Infix_tag` branch — no FieldSlot/scanning/barrier changes, no new feature.
*(An earlier version of this fix inferred "forwarded" from the offset magnitude —
a genuine infix offset is small so `addr − offset` stays in committed space, a
collision's is ~address/128 so it lands in uncommitted memory. That worked for the
heaps we run but was config-dependent — it assumed `heap_base/128 > committed_span`,
which a ≳17 GB or low-mapped heap would break — so it was replaced with the
side-metadata check above.)*

**Result.** StickyImmix went from **crashing at every heap size** to **completing
`ocamlc -c parsing/parser.ml` at 96 MB → 1024 MB** (96 MB: 149 GCs / 2.6 M copied;
1024 MB: 1 GC). No regression on Immix. The fix is in `common`, so it covers every
moving plan (Immix defrag, GenImmix, StickyImmix).

**Full-build validation.** A from-scratch `make clean && make all` under
`MMTK_PLAN=StickyImmix` (2048 MB, `setarch -R`) **completes cleanly — 843 `ocamlc`/
`ocamlopt` steps, 0 crashes, 0 make errors** — and notably builds
`api_docgen/.../build/man/Stdlib.3o`, the **exact ocamldoc `Lexing.engine` manpage
step that was the original intermittent crash** (the documented blocker for merging
always-on MMTk to `5.5+mmtk`). Since StickyImmix relocates far more aggressively
than the default Immix, this clean build means **the always-on merge is unblocked**
on the correctness front. (`make bootstrap` to a fixpoint is still fiddly for
unrelated build-system/tree-state reasons — an aborted run leaves `ocamlc` missing —
but the GC no longer crashes anywhere in the compile.)

**How it was cracked.** Enabled mmtk's `sanity` feature (full-heap re-trace after
each GC) at a deliberately **small heap** — small heaps force frequent + full GCs so
`sanity` actually runs, and it caught the dangling edge deterministically (`Invalid
reference` panic). Then `rr record` + `rr replay` (forward `continue` and
`reverse-continue` to breakpoints; **hardware watchpoints trip an rr/gdb async
"target is running" bug**, so avoid them) pinned the offending slot, the forwarding
word, and the `0xf9`/`Infix_tag` collision.

**STILL OPEN — bug #2 (separate, narrower; tight heaps only).** At a *very tight*
heap (64 MB; heavy copy pressure → frequent + full GCs) StickyImmix still SIGSEGVs
(deterministically). **`sanity` does *not* flag it** (no `Invalid reference` panic
across runs, with the fix in place) — so the *heap* is consistent after every GC;
the bad value is in a **root the GC scan misses**, not a heap field. The crash is
**corrupted control flow**, not a single dangling data pointer: at the fault the
bytecode `pc` is a tiny garbage value (`0x1`/`0x5`) and the `RETURN` frame is bogus
(`sp[0]` = the int `0` where a saved code pointer belongs, `interp.c:623`), while
`accu`/`env` still look valid (`env` → a live tag-3 block). That signature means an
*earlier* wrong jump (most likely `pc = Code_val(accu)` in an `APPLY`-family opcode
on a stale/garbage closure) propagated into a bogus dispatch — i.e. a value live
across a GC in an interpreter slot the root scan doesn't cover, used after its
target moved. 96 MB+ is unaffected (the gap is latent unless the missed root's
target actually relocates, which heavy 64 MB copy pressure makes near-certain — same
"latent vs. reliably-triggered" relationship as Immix↔StickyImmix for bug #1).

Default stays **Immix**; StickyImmix is now viable at practical heap sizes but not
yet at the tightest. Root-causing needs reverse execution from the *first* garbage
`pc` back to the unscanned slot — non-trivial because the corruption manifests far
downstream, and rr **hardware watchpoints trip the async bug** here (reverse-continue
to *breakpoints* works; a software-watchpoint reverse or a binary-search on GC count
is the likely route). Deterministic repro saved: `~/.local/share/rr/strcrash` on the
dev box (`MMTK_PLAN=StickyImmix MMTK_HEAP_SIZE_MB=64`, single-threaded).

---

## M9 stage 1: MMTk always-on (vanilla GC removed as a mode)

*2026-06-20*

MMTk is now this fork's GC by default — `MMTK_ENABLED` is gone; the only escape is a
transitional `MMTK_DISABLE=1` (kept so the benchmarking phase can still measure the
stock GC; to be deleted at final excision). Default plan flips to **Immix** (NoGC
can't sustain an always-on runtime). The `caml_mmtk_vanilla_minor` mode is deleted:
native is always TLAB nursery-aliasing and a non-Immix-Default plan is now a fatal
error (Immix/StickyImmix/GenImmix are the native set). `caml_mmtk_enabled` is kept
purely as the MMTk-init-readiness guard for the brief pre-init startup window, so
the per-allocation branch remains (removing it needs MMTk-init-before-first-alloc —
a separate perf step, not part of "remove the vanilla GC"). Work on branch
`m9-mmtk-only`.

**Validated:** the **whole compiler builds and self-hosts under always-on MMTk** —
`make bootstrap` reached its fixpoint earlier (under MMTk), and a from-scratch
always-on `make` builds runtime + stdlib + bytecode and native compilers cleanly.

**Open blocker for merging to `5.5+mmtk`: a rare intermittent SEGV in `ocamldoc`.**
The always-on `make world.opt` failed once at the **manpage** step
(`build/man/Stdlib.3o`): `ocamldoc.opt` segfaulted in `Stdlib.Lexing.engine`
(via odoc's `odoc_ocamlhtml` source-highlighting lexer). It is **intermittent** —
5/5 direct re-runs pass across stock, Immix 1024/4096, StickyImmix, with/without
ASLR; it only bit once under the parallel `-j16` build. So it's a rare latent
moving-GC correctness bug (lexbuf/lex-table corruption under Immix), surfacing under
parallel load — NOT the ASLR metadata-mmap flake (that's a start-up abort; this is a
runtime SEGV in OCaml code) and NOT `Gc.*`/weak/tabled. The compiler's *own* lexer
runs fine under MMTk, so it's data/timing-specific. **Deferred (needs rr to root-
cause a rare repro); always-on stays on `m9-mmtk-only` until it's understood, since
it intermittently breaks `make world.opt`.** Everything else (compiler, bootstrap,
core testsuite) is solid always-on.

**UPDATE — a *deterministic* repro of this same bug found via StickyImmix.**
`MMTK_PLAN=StickyImmix make bootstrap` **reliably** SEGVs during `coreboot`
compiling `parsing/parser.cmo` (`coreboot Error 2`). This is almost certainly the
*same* latent moving-GC correctness bug as the rare ocamldoc `Lexing.engine` crash
— StickyImmix's always-relocating nursery just triggers it on every run instead of
once-in-a-while. That makes StickyImmix the **right vehicle to root-cause it** (no
rr-on-a-flake needed; it's reproducible). Consequence: StickyImmix is *faster*
(gcbench 5.4 s vs Immix 7.0 s) but **cannot be the default until this is fixed** —
it can't even self-host. **Immix stays the default** (bootstraps cleanly; only
opportunistically moves, so it dodges the bug almost always). Root-causing via the
StickyImmix `parser.cmo` repro is the single highest-value next task: it unblocks
*both* the StickyImmix perf win *and* the always-on merge. A clean gdb backtrace
still needs a from-clean rebuild (the failed bootstrap leaves the tree half-built —
`ocamlc` missing — and `boot/ocamlc` can't stand in without the full `.cmi` set).

---

## Testsuite (M7) bring-up: global link, and first two bugs surfaced

*2026-06-20*

Started running OCaml's own testsuite under MMTk. Findings so far:

**Global link (prerequisite, validated).** `ocamltest` is built `-custom`, and
`-custom`/native test exes link `libcamlrun.a`/`libasmrun.a`, which now contain
`mmtk.c` and reference `mmtk_ocaml_*`. They only link if the MMTk staticlib is on
the link line. The fix that works: add the staticlib to **`bytecomp_c_libraries`
and `native_c_libraries`** (config) — `ocamlc`/`ocamlopt` place these *after* the
runtime lib, the same ordering that makes the standard `ocamlrun` link resolve
(plain, no `--whole-archive`; this also obsoletes the `--whole-archive` dance in
the native test-compile script). Per-target `-cclib` does *not* work (it lands
before the runtime lib). Validated by editing `utils/config.generated.ml` directly
on the build box + rebuilding the compilers (incl. `ocamlc.opt`/`ocamlopt.opt`,
which embed config). **Still to do: make it committable via `configure.ac`** (the
staticlib's absolute build path + the bootstrap/`.opt` double-link check + a macOS
branch). NB a stale bytecode `mmtk.b.o` (missing `caml_mmtk_scan_ephe_roots`) sent
me down a wrong path first — rebuild `libcamlrun.a` after glue changes.

**`tests/basic` (40 tests; this dir does NOT need `testing.cma`): TLAB Immix
33/40; vanilla-minor 19/40.** Two bugs found; the `Gc.*` one fixed. (The residual
`tests/basic` failures are a mix of tabled-feature tests and a couple of
expect-test diffs — see the correction below; my first read that they were
"weak/ephemeron in the compiler" was largely wrong, that was the missing-lib
artifact in *other* dirs.)

1. **`Gc.major`/`full_major`/`compact`/`major_slice` ran the *stock* major-GC
   machinery** (`caml_finish_major_cycle`) on the bypassed stock heap — harmless
   under non-moving vanilla-minor, but **corrupts state under TLAB Immix**
   (observed: stdout channel's mutex pointer overwritten with an MMTk-heap address
   → SIGSEGV in `caml_channel_lock` on the next `Printf`). Minimal repro:
   `Array.init 300 …; Gc.full_major ()`. **Fixed:** under `caml_mmtk_enabled` these
   route to `caml_mmtk_collect` (→ `mmtk_ocaml_handle_user_collection_request`, a
   real MMTk STW collection); `major_slice` is a no-op (MMTk is whole-heap STW).
   `Gc.stat` still reads stock counters (meaningless but not a crash — separate
   audit item).

2. **The native compiler `ocamlopt.opt`/`ocamlc.opt` SEGVs under *vanilla-minor*
   MMTk** (MarkSweep and GenImmix-fallback) — corrupt `Buffer` field, garbage
   index in `CamlinternalFormat.strput_acc` during `asmlink.make_startup_file`.
   Reliably reproducible compiling any Printf-using program (enough link-time
   symbols). **It does NOT reproduce under TLAB Immix** — i.e. the all-MMTk/TLAB
   path runs the native compiler correctly where the vanilla-minor intermediate
   does not. Strong validation of the all-MMTk direction; the vanilla-minor bug is
   a promotion/remembered-set correctness issue (not yet root-caused, needs rr).

**To run the native testsuite, use TLAB (`MMTK_TLAB=1`, Immix/StickyImmix)**, not
vanilla-minor.

**CORRECTION (later same day): the bad `tests/basic-more`-style numbers were a
missing testsuite support lib, not MMTk.** `make one DIR=…` does **not** build
`testsuite/lib/testing.{cma,cmxa}` (the `testing` helper), which most dirs beyond
`tests/basic` `open`. Without it every such test fails to *compile* ("file not
found in include path: testing.cma") — that, not weak/ephemeron, is what tanked
the broader sweep. Build it once with `make ocamltest` (+ `make
testsuite/lib/testing.cmxa`; the full `ocamltest` target errors at the end on the
`--enable-ocamltest` config flag, but the lib/tools build before that). With it
built: **`tests/basic-more` 20/22 under TLAB Immix**, and the *only* 2 failures are
**tabled features** — `pr10338` (lazy) and `simplif_under_lambda` (`Gc.finalise_last`).
So the core runs correctly under MMTk TLAB; failures concentrate in the
tabled set (weak refs, ephemerons, finalisers, lazy — per the user, deferred).

**Weak tables made memory-safe under moving (pinning).** `caml_mmtk_scan_ephe_roots`
now `mmtk_ocaml_pin_object`s each ephemeron/weak-array block during the root scan,
so the interior-slot-roots it reports stay valid under a moving plan (the block
won't relocate; its field targets still get forwarded). This is *memory safety*
only — weak-reference *semantics* (clearing dead keys) remain tabled — but it's
what lets the compiler's internal weak hashtables survive a compile-time moving GC.
(`object_pinning` is enabled; Immix honors the pin bit.)

**Plan: disable the tabled-feature tests** (remove their `(* TEST *)` block + a
`disabled under MMTk` comment) so they don't run, then the rest of the suite
should pass under TLAB Immix. Done for the dedicated dirs (`weak-ephe-final`,
`lazy`, `lib-lazy`, `ephe-c-api`) + scattered finaliser/lazy tests
(`simplif_under_lambda`, …).

**Testsuite flakiness ROOT-CAUSED: ASLR vs MMTk's fixed-address metadata mmap.**
A clean sweep showed ~10% of tests "failing", but with a *different* set each run
and **byte-identical stock-vs-MMTk program output** for every one sampled — i.e.
not correctness. The actual failure is at process **startup**: MMTk occasionally
aborts with `failed to mmap meta memory: File exists (os error 17)` →
`fatal runtime error: failed to initiate panic`. MMTk maps its side-metadata at
addresses derived from the heap layout; under ASLR some library/stack/mmap
randomly lands in that range → `EEXIST`. At ~2.5%/process × 4 compiler invocations
× ~100 tests, that's the ~10 spurious fails/sweep (it hits the *compiler* runs —
the test programs themselves run 40/40 clean). **Fix: run the suite under
`setarch $(uname -m) -R` (ADDR_NO_RANDOMIZE, inherited by children) → 0/40
failures.** This is an MMTk-on-Linux init issue, independent of TLAB/GC; a proper
binding-side fix (reserve metadata deterministically / handle the collision) is a
follow-up, but `setarch -R` is the reliable run recipe.

**Run recipe for the native testsuite:** build `testing.{cma,cmxa}` (`make
ocamltest` + `make testsuite/lib/testing.cmxa`), then
`setarch $(uname -m) -R env MMTK_ENABLED=1 MMTK_PLAN=Immix MMTK_TLAB=1
MMTK_HEAP_SIZE_MB=2048 make -C testsuite one DIR=tests/<dir>`.

**Definitive result: 95/96** core tests pass under MMTk TLAB Immix (ASLR off,
tabled tests disabled) across 14+ `basic*`/`callback`/`runtime-errors`/… dirs. The
one miss is `callback/signals_alloc.ml` *bytecode* variant — a SIGUSR1 lands one
allocation-step differently under MMTk's alloc path (`01243` vs `01234`); the
signal is handled, it's benign timing, and the *native* variant passes. No sampled
failure across the whole effort was an MMTk correctness difference (output is
byte-identical to stock everywhere).

Remaining real caveat: multi-domain TLAB deadlocks (separate note). The
vanilla-minor native compiler SEGV (point 2 above) is moot — we standardize on
TLAB for native.

---

## Vanilla minor heap + MMTk major heap (chosen architecture)

*2026-06-20*

**Status: implemented (flag-gated), works at reasonable heaps.** Opt in with
`MMTK_VANILLA_MINOR=1` (default off = the all-MMTk bypass, unregressed). The
stock minor heap + minor GC run; `alloc_shared` (promotion) routes to
`caml_mmtk_alloc_shr`; `Alloc_small`/`write_barrier`/`caml_initialize`/array-fill
take the stock path in this mode (so the minor remembered set is maintained).
Validated: large-heap runs (retain/torture/infix/varied) all correct, and
`retain@32MB` did **12 MMTk major GCs** cleanly. **The nested-STW hazard is real
and confirmed:** at a very tight heap (`torture@16MB`) an MMTk GC fires *during*
minor-GC promotion and SEGVs (default all-MMTk mode at 16MB is fine). So
promotion must not trigger an MMTk GC — the remaining work (see hazard note
below). Plan/choke-points unchanged:

Decision: keep OCaml's **stock minor heap + minor GC**, make **MMTk the major
heap**. Validate on bytecode first, with **MarkSweep** as the major plan (non-moving
→ no minor→major dangling, simplest). This both replaces the bytecode all-MMTk
bypass and is the route to native (the inlined native fast-path keeps bumping the
stock nursery; no compiler changes).

**Exact choke points (all in shared files, so bytecode + native get it):**
1. **Promotion → MMTk**: `alloc_shared` (minor_gc.c:152) is the single function
   the minor GC uses to allocate the promoted copy (currently
   `caml_shared_try_alloc` on the stock major heap). Redirect to
   `caml_mmtk_alloc_shr` under `caml_mmtk_enabled`.
2. **Young allocation stays stock**: revert/gate the `Alloc_small` all-MMTk
   redirect (memory.h) so it bumps `young_ptr` again; re-enable the stock minor
   GC (M1 disabled it).
3. **Direct major alloc → MMTk**: `caml_alloc_shr` already routes to MMTk.
4. **Write barrier**: re-enable OCaml's stock one (currently disabled under MMTk)
   — it maintains the minor remembered set (`major_ref`) for MMTk(major)→minor
   pointers, which the minor GC scans as roots.
5. **Disable the stock major GC** (mark/sweep slices); route `Gc.*`.

**Decision (2026-06-20): SUPERSEDED — the coordination fix will NOT be done.**
The proper integration is all-MMTk (MMTk owns the entire heap, including the
nursery), which has no OCaml minor GC and therefore no minor↔MMTk nested STW —
the hazard dissolves by construction. Bytecode all-MMTk already works (default
mode); for native, all-MMTk means TLAB/nursery-aliasing (the inlined fast-path
bumps an MMTk buffer). The flag-gated vanilla-minor mode (and native's current
vanilla-minor) is retained only as a *validated fallback* (plan B if TLAB proves
intractable); the recipe below is what plan B would implement. Do not spend on it
unless plan B is chosen.

**THE hazard — nested stop-the-world (CONFIRMED: SEGV at tight heaps).** Minor GC
runs inside an OCaml STW. If a promotion (`alloc_shared` → MMTk) finds the MMTk
heap full, `mmtk_ocaml_alloc` triggers an MMTk GC (`block_for_gc`) *inside* the
minor-GC STW → nested STW, scanning a half-promoted heap → SEGV (seen at
`torture@16MB`; the same heap is fine in all-MMTk mode). Promotion must **not**
trigger a collection.

**Concrete fix recipe (MMTk API confirmed):**
- Expose `memory_manager::free_bytes` / `handle_user_collection_request` via the
  ABI.
- **Before** a minor GC (at the `caml_alloc_small_dispatch` safepoint, *not* mid
  promotion), if `free_bytes < minor_heap_size + margin`, trigger an MMTk GC
  there. Then the subsequent promotion is guaranteed to fit without collecting.
- For that pre-minor MMTk GC to be correct, MMTk must see major objects reachable
  *only via live young objects* — so add a **minor-heap root scan**: walk
  `[young_ptr, young_end)` header-by-header (safe at a safepoint — all young
  objects are fully initialised there) and report each object's fields as roots
  (FieldSlot::classify filters the non-MMTk young targets). Over-conservative
  (keeps even dead-young's major targets until the next minor GC) but safe.
- Delicate part: the minor-heap walk (object boundaries) and getting the ordering
  exactly right. Validate at the tight heaps that currently SEGV.

**Test:** revert bytecode to stock-minor, run the existing battery; old→young and
churn must survive; compare against the all-MMTk mode. Then native.

## Native TLAB / nursery-aliasing — IMPLEMENTED (single-domain)

*2026-06-20*

**Status: working single-domain for Immix + StickyImmix.** Opt in with
`MMTK_TLAB=1`. This is all-MMTk for native: MMTk owns the nursery too, so there
is no OCaml minor GC and no promotion — the nested-STW hazard of the
vanilla-minor model cannot occur by construction. The inlined native fast-path is
unchanged; it bumps `young_ptr` down through an MMTk Immix block that the binding
hands over.

**Design — OCaml's young region IS an MMTk Immix block.**
- On refill (where native used to do a minor GC), `mmtk_ocaml_refill_tlab` drives
  the mutator's **Default** `ImmixAllocator` to acquire a fresh region and returns
  `[start, end)`. The C glue sets `young_start = young_limit-region = start`,
  `young_end = young_ptr = end`, `young_trigger = young_start`. OCaml's fast-path
  bumps `young_ptr` **down**, filling the block top-down. When it reaches
  `young_start`, `caml_alloc_small_dispatch` refills again.
- **No OCaml minor GC, no promotion** — objects in the block are MMTk objects from
  birth, traced from roots (native frame descriptors → our scanner), with Immix
  lines marked per live object and unmarked lines reclaimed. Bump direction is
  irrelevant to Immix's mark-region GC.

**How the pieces resolved (vs the blueprint):**
- ✅ Allocator access: `memory_manager::get_allocator_mapping::<VM>(Default)` →
  `AllocatorSelector`; if it's `Immix(_)`, `mutator.allocator_impl_mut::<ImmixAllocator<VM>>(selector)`
  gives a typed `&mut ImmixAllocator` whose `pub bump_pointer.{cursor,limit}` we
  read/write. (Cleaner than the `AllocatorInfo` offset path — same result, type-safe.)
- ✅ Eject the block from MMTk's bump view by setting `bump_pointer.cursor = limit`
  after taking the region. **Essential:** direct MMTk allocations (`caml_alloc_shr`
  → large arrays, etc.) share this *same* Default allocator, so without ejecting
  they'd bump into the region OCaml is filling top-down. Post-eject, the next
  direct alloc slow-paths a fresh block.
- ✅ No per-object `post_alloc` (we don't enable `vo_bit`); comballoc fine (all
  objects share the block, traced individually).
- 🐛 **bump_pointer vs large_bump_pointer (fixed).** Driving the allocator with the
  *object's* size was wrong: an Immix `alloc` larger than a line (256 B) takes the
  `overflow_alloc` path, which populates the **inaccessible** `large_bump_pointer`,
  not the `pub bump_pointer` we read → bogus region → SEGV (hit by `String.make
  1000` = 127 words; small list cells were fine). Fix: probe with a *one-word*
  alloc (always the small/`bump_pointer` path), then ensure `[result, limit)` ≥ the
  requested size, retrying past undersized recyclable-line holes until a clean
  32 KiB block satisfies it.
- ✅ **Moving GC across a held young region (the real correctness worry).** A GC
  can fire while a domain holds a partially-filled young block (e.g. a large
  `caml_alloc_shr` triggers it). Moving plans would relocate the objects already in
  the block and free/recycle its tail. Handled by **resetting the young region
  post-GC**: `caml_mmtk_uninterrupt` (called per domain from `resume_mutators`)
  sets `young_ptr = young_start` in TLAB mode, forcing a fresh refill on the next
  allocation. Live objects survived via root tracing (and had their refs fixed up
  if moved); we just stop bumping into the stale block.

**Validated (single-domain, Immix + StickyImmix):** natgc, torture, retain, infix,
gcbench, treebench — correct results under plain Immix, forced defrag
(`MMTK_IMMIX_ALWAYS_DEFRAG`+`DEFRAG_EVERY_BLOCK`, heavy relocation: 74k–300k
objects copied), StickyImmix, and tight heaps forcing many GCs. Clean
`Out_of_memory` when the live set exceeds the heap (gcbench@64MB). `String.make`
(large small-object) works. The inlined fast-path needed **no compiler changes**.

**Plan support:** TLAB requires an Immix `Default` allocator → **Immix,
StickyImmix**. GenImmix/GenCopy (copying-nursery generational; `Default` is a
nursery BumpPointer), MarkSweep (free-list), NoGC (contiguous BumpPointer) have no
Immix `Default`, so `mmtk_ocaml_refill_tlab` returns false and the runtime
**falls back to the validated vanilla-minor model** (logged under `MMTK_VERBOSE`).
(Extending TLAB to plain BumpPointer plans — NoGC — is easy; GenImmix's nursery is
GenImmix-managed and would need its own handling.)

**Multi-domain TLAB: FIXED.** `Domain.spawn` programs (`multidom8`, 8 domains) now
run cleanly under TLAB Immix at every heap size tried — 16/24/32/48 MB, 0 hangs,
correct results (`total=3599880000 OK`), where 32 MB previously hung 11/12.

The original deadlock: terminating worker domains spun in `caml_domain_terminate`'s
`while(!finished)` loop while the main thread waited in `Domain.join`. Root cause
was *our own short-circuit* — TLAB skipped `caml_empty_minor_heaps_once`, which
removed the **all-domains minor-empty STW rendezvous** that synchronizes domain
spawn/terminate, so termination never converged.

The fix (exactly the planned shape): **keep the minor-empty STW, neuter only the
promotion.** `caml_empty_minor_heaps_once` no longer short-circuits in TLAB — it
runs the real `caml_try_empty_minor_heap_on_all_domains` STW (reusing all its
battle-tested contention/barrier orchestration). Inside `caml_empty_minor_heap_promote`,
a TLAB `goto` skips the entire oldify/root-scan promotion (running it would wrongly
*copy* live MMTk objects) — the skipped region is `EV_BEGIN/END`-balanced — and
the per-domain work becomes just a young-region reset (`young_ptr = young_start`,
so the domain refills a fresh block on its next allocation, *deferred outside the
STW* → no nested MMTk GC). Live objects in the old block stay reachable via roots.
Gated on `caml_mmtk_tlab`, so non-TLAB modes (vanilla-minor, bytecode all-MMTk) are
unchanged. Validated: single-domain TLAB (incl. forced defrag) and vanilla-minor
still pass; run under `setarch -R` for the ASLR/metadata-mmap flake.

**Choke points (all gated on `caml_mmtk_tlab`):** `mmtk_ocaml_refill_tlab`
(binding) + `caml_mmtk_refill_tlab` (glue, sets `young_*`); initial refill in
`caml_mmtk_domain_init`; refill instead of minor GC in `caml_alloc_small_dispatch`;
`caml_poll_gc_work` consumes pending GC requests and returns (no minor GC / major
slice); `caml_empty_minor_heaps_once` services interrupts then returns;
`caml_mmtk_uninterrupt` post-GC young-region reset. Still uses the validation-time
`--whole-archive` link (global `native_c_libraries` link still deferred).

## GC plan support matrix (mmtk-core 0.32)

*2026-06-20*

Swept all 11 plans (bytecode, default all-MMTk mode, torture + retain + infix).
The plan-agnostic binding works for **9 of 11** with no plan-specific code:

- ✅ **NoGC, MarkSweep, Immix, GenImmix, StickyImmix** — validated earlier.
- ✅ **SemiSpace, GenCopy** — work, but copying collectors use ~half the heap, so
  they need ~2× the size or raise a (clean) `Out_of_memory`.
- ✅ **MarkCompact** (sliding compaction) — works (torture+retain+infix).
- ✅ **ConcurrentImmix** — *runs* our tests cleanly. Caveat: shows no corruption,
  but concurrent marking / SATB-barrier correctness is unvalidated (may fall back
  to STW or not be stressed). Promising for the concurrent-GC goal — note that
  concurrent IS present in 0.32 (earlier notes said otherwise).
- ❌ **PageProtect** — panics (`freelistpageresource`): a debug plan that maps one
  page per object, so it exhausts the page resource at normal heap sizes. Likely
  needs a much larger reservation; not obviously a binding bug.
- ❌ **Compressor** — panics in `compressorspace`: a Compressor-specific
  requirement (mark bitmap / offset vector layout) the binding doesn't satisfy.

## Native-code integration (M5) — WORKING for single-domain

*2026-06-20*

**Native OCaml code runs on MMTk (single-domain).** A program compiled by
`ocamlopt.opt` allocates in the stock minor heap (inlined fast-path, unchanged),
**promotes survivors into MMTk**, churns garbage, triggers **MMTk major GCs**
(verified 1/2/3 GCs at 48/32/24 MB), and produces correct results — proving
native **root scanning works** (the live set survives via frame-descriptor roots
→ `caml_do_roots` → our scanner; this was the big unknown). The vanilla-minor +
MMTk-major model carried over to native with only guard relaxations (commits
`c559cb2`, `f7347b6`); the inlined native allocation needed no compiler changes.

**Linking.** `libasmrun` references the glue, so native exes must resolve the
MMTk staticlib. For validation we link it explicitly with `--whole-archive`:
`ocamlopt.opt … -cclib -Wl,--whole-archive -cclib <libmmtk_ocaml.a> -cclib
-Wl,--no-whole-archive -cclib "-ldl -lpthread -lm"`. (Plain `-cclib <staticlib>`
fails: it lands *before* libasmrun in the link line, so the linker doesn't pull
the referenced objects. The chosen convenience path — adding the staticlib to
`native_c_libraries` — places it correctly after libasmrun, but perturbs the
compiler bootstrap, so it's deferred.)

**Multi-domain native** (`Domain.spawn`): now *runs correctly* (was a SEGV — fixed
by deregistering the MMTk mutator only after the terminate-time minor flush,
commit `06f3ae7`), but **intermittently hangs** at larger heaps (≈2/6 at 80 MB,
clean at 48/64 MB). This is the **same nested-STW coordination hazard**: a
terminating domain's `caml_empty_minor_heaps_once` is itself a multi-domain OCaml
STW, and a promotion inside it that fills MMTk triggers an MMTk GC → nested STW.
The multidom hang is **not being fixed**: it's superseded by the all-MMTk
decision (see the matrix/decision notes). Native's proper path is TLAB/nursery
aliasing (MMTk owns the nursery ⇒ no OCaml minor GC ⇒ no nested STW), which makes
this hang moot. Vanilla-minor native (single-domain solid; multidom racy) is kept
as the validated fallback (plan B). Single-domain native has no STW nesting and
is solid either way.

## Native-code integration (M5) — strategy & plan

*2026-06-19*

**The native allocation mechanism** (examined on arm64; amd64 is analogous).
The compiler *inlines* allocation at every site: a dedicated register
`ALLOC_PTR` holds `young_ptr`; the sequence is `ALLOC_PTR -= whsize; cmp
ALLOC_PTR, young_limit; b.lo caml_call_gc` — a **downward** bump. Comballoc
merges several allocations into one decrement. Slow path: `caml_call_gc` (asm,
arm64.S) saves regs and calls `caml_garbage_collection` (signals_nat.c), which
reads the **frame descriptor** at the return address to recover the allocation
count/sizes, then calls `caml_alloc_small_dispatch`; on return `young_ptr` is
valid again and the inlined code proceeds. Native **roots** also come from frame
descriptors (each return address lists live registers/stack slots);
`caml_scan_stack` already walks native frames precisely — so feeding MMTk reuses
the same `caml_do_roots` path as bytecode.

So unlike bytecode (plain C entry points we redirect), native allocation can't be
swapped by replacing a C function — the bump is inlined.

**Two strategies:**

- **A. Nursery aliasing / TLAB** — point `young_ptr`/`young_limit` at an
  MMTk-backed bump region; refill from MMTk on overflow. Keeps the inlined
  fast-path. Problems: OCaml bumps *downward*, MMTk Immix bumps *upward*; and
  MMTk needs per-object metadata (post_alloc) that the inlined bump won't set.
  Highest performance, hardest.

- **B. Keep the stock minor heap; MMTk owns the major heap (RECOMMENDED FIRST).**
  Leave the inlined fast-path and the stock minor heap **unchanged** — young
  objects allocate in the stock nursery exactly as today (no compiler change).
  Redirect only: (1) `caml_alloc_shr` → MMTk (as in bytecode); (2) the minor
  GC's *promotion* — surviving minor objects get copied into MMTk via
  `mmtk_ocaml_alloc` instead of into the stock major heap; (3) disable the stock
  major GC. This is exactly how the bdwgc fork did native, and it sidesteps the
  inlined-bump problem entirely. The existing minor-GC remembered set / write
  barrier stay (major→minor = MMTk→nursery), with promotion targets in MMTk.

**Concrete plan (Strategy B):**
1. Build `libasmrun` with the MMTk glue: today every MMTk patch is `#ifndef
   NATIVE_CODE`; selectively enable init + `caml_alloc_shr` redirection +
   promotion hook for native. Link the staticlib into native exes too
   (`Makefile.mmtk`).
2. MMTk init for the native domain (mirror `caml_mmtk_domain_init`).
3. Promotion: in the minor GC (`minor_gc.c` `oldify`/promote path), allocate the
   promoted copy via `caml_mmtk_alloc_shr` instead of the stock major heap;
   update the forwarding so references point into MMTk.
4. Roots: feed `caml_do_roots` (native stacks via frame descriptors + globals)
   to MMTk — same `scan_roots_in_mutator_thread` as bytecode (verify native
   frame scanning produces the same `FieldSlot`s).
5. STW: native already has `young_limit`-poison interrupts (`caml_call_gc` does an
   `acquire` fence for exactly this) — reuse the multi-domain STW machinery.
6. Disable the stock major GC slices; route `Gc.*` like bytecode.

**Risks:** native roots include callee-save registers and frame layouts that must
be reported precisely; promotion correctness under a moving MMTk major heap
(forwarded pointers); C FFI (`CAMLparam`) across native↔C; the `-DNATIVE_CODE`
build must stay green for the stock GC when MMTk is off. Test with a tiny native
program first (`ocamlopt`), then the moving/multidomain battery natively.

## Generational write barrier (GenImmix / StickyImmix)

*2026-06-19*

OCaml's `caml_modify(field_ptr, val)` is handed only the **field address**, not
the containing object, so MMTk's object-remembering barrier (`object_reference_write_post`,
which re-scans the remembered *object*) doesn't fit. Instead we use the **region
barrier** (`memory_region_copy_post`), which remembers the modified *slice* — for
a scalar write, a 1-slot region = the slot itself. This matches OCaml's own
remembered set, which is also slot-based (`Ref_table_add` stores field
addresses). `OCamlMemorySlice` (common/slot.rs) is the `VMMemorySlice` impl.

Wired from `write_barrier` (covers `caml_modify`, `caml_modify_field`, atomics,
and bytecode `SETFIELD`/`SETVECTITEM`), `caml_initialize`, and
`caml_uniform_array_fill` (which inlines caml_modify's logic). `caml_uniform_array_blit`'s
old-destination path already uses `caml_modify`. Self-gated by
`caml_mmtk_generational` so it's a no-op for non-gen plans (NoBarrier). Validated:
aged array ← young tuples survives nursery GCs with stock-matching checksums.

## Weak arrays & ephemerons — interim fix is MarkSweep-only (unsafe under moving)

*2026-06-19*

`caml_mmtk_scan_ephe_roots` (runtime/mmtk.c, per-domain in
`scan_roots_in_mutator_thread`) walks `domain->ephe_info->{todo,live}` and reports
every ephemeron/weak-array field (link, data, keys) + the list heads as strong
roots, so MMTk keeps the graph alive instead of letting it dangle. This fixes the
segfault **under non-moving MarkSweep**.

**It is NOT safe under moving plans.** It reports *interior field slots* of the
ephemeron blocks; when a moving plan relocates a block (Immix opportunistically,
GenImmix/StickyImmix nursery always), those slot addresses go stale and weak/
ephemeron programs crash or hang. Tried allocating ephemerons in MMTk's
non-moving space (so the blocks never move) — this regressed MarkSweep (hang)
and NoGC (NonMoving unsupported → panic), so it was reverted. The real fix is
MMTk weak-reference processing (register ephemerons, trace/update them as objects,
clear dead keys/data). Parked. Tradeoff even on MarkSweep: weak refs never clear
(everything kept alive, a leak), like finalisable values under `do_final=1`.
Original diagnosis below.

---

**Confirmed bug, not just a missing feature.** A program that creates weak
arrays / ephemerons and later triggers ephemeron processing (e.g. `Gc.full_major`,
or enough GC activity) **segfaults** under MMTk (MarkSweep and Immix), while it
runs fine on the stock GC. lldb pins the fault in `Ephe_key` (`weak.h:84`)
reading a key field of a garbage ephemeron pointer (`EXC_BAD_ACCESS`).

Root cause: OCaml links every weak array / ephemeron into per-domain lists
`domain->ephe_info->{live,todo}`, walked by the stock major GC (`major_gc.c`).
Our MMTk integration neither scans those lists as roots nor processes them, and
ephemerons/weak arrays are `Abstract_tag` (≥ NO_SCAN) so `scan_ocaml_object`
skips them. So an ephemeron/weak array reachable *only* via `ephe_info` is
treated as dead, collected (or moved) by MMTk, and left dangling in the list —
any later walk (`Gc.full_major`, the next ephemeron pass) dereferences garbage.

This affects a lot of real code: `Weak`, `Ephemeron`, weak hash tables
(`Weak.Make`, `Ephemeron.K1.Make`), memo caches, etc. So it's a priority item.

Fix options:
- *Interim (conservative, stops the crash):* scan `ephe_info->live`/`todo` as
  roots and trace the ephemeron link chain + blocks, keeping weak arrays /
  ephemerons alive and their links updated under moving. Weak refs would then
  never clear (like our finaliser handling keeps finalisable values alive via
  `do_final=1`) — semantically loose but memory-safe.
- *Proper (workstream E):* implement MMTk weak-reference / finalizable
  processing — register ephemerons with MMTk, clear dead keys/data, run
  finalisers — replacing the stock `major_gc.c` ephemeron pass.

Status of other runtime features probed at the same time (MarkSweep + Immix):
`Lazy` works; `Gc.full_major`/`minor`/`stat`/`allocated_bytes` work *in
isolation*; finalisers don't run yet (`do_final=1` keeps values alive); weak
refs read as "still alive" (not cleared). Only the weak/ephemeron-list dangling
above actually crashes.

## Parallel collection — verified (correct, and marking scales ~8x)

*2026-06-19*

MMTk runs collections on multiple GC worker threads (`MMTK_GC_THREADS`, default
from core count). Verified two things on turing (28-core), forcing ~60 GCs over a
~4M-object live set with `MMTK_STRESS_FACTOR`, `setarch -R`, measuring MMTk's own
GC pause time (`mmtk_ocaml_gc_time_ms`):

- **Correctness**: identical results across `MMTK_GC_THREADS` = 1,2,4,8,16 under
  both MarkSweep and Immix. Parallel workers do not corrupt the heap.
- **Scaling depends on live-set shape:**
  - Bushy binary tree (independent subtrees → high marking parallelism):
    GC time 34.4s → 20.1 → 10.5 → 6.3 → 4.1s for 1→2→4→8→16 threads — **~8.4x**
    at 16 threads, near-linear to 4 (then memory-bandwidth-bound).
  - Linked lists (`Array.init 400 (List.init 10000 …)`): flat ~55→65s, no
    speedup (slight regression from coordination/contention). Tracing a list is
    a sequential pointer chase and latency-bound — workload-inherent, **not** a
    binding limitation.

Takeaway: parallel marking engages and scales well for parallel-friendly heaps;
pointer-chasing-heavy heaps are latency-bound regardless of thread count. Both
correct. (Concurrent — as opposed to parallel — collection is a separate,
upstream-dependent matter; see ROADMAP.)

## Pinning under a moving plan — why OCaml's existing rooting mostly suffices

*2026-06-19*

A moving plan (Immix defrag) relocates objects, so any reference into the MMTk
heap must either be a precise, updatable root/slot or the target must be pinned.
The reassuring fact: **OCaml's stock GC already moves objects** (minor-heap
objects are relocated on promotion), so all correctly-written C code already
roots the `value`s it holds across an allocation (via `CAMLparam`/`CAMLlocal`,
which land in `caml_local_roots` and are scanned + updated). MMTk-moving inherits
that safety for free.

The residual risk is narrow: code that holds an **unrooted raw pointer to an
object it assumes won't move** — safe under the stock GC because *major*-heap
(old) objects don't move there, but unsafe under Immix, which can move any
object. Finding such spots is the "tier-2 validation" in the roadmap.

Validation strategy (in lieu of an exhaustive audit): run under
`MMTK_IMMIX_ALWAYS_DEFRAG=true MMTK_IMMIX_DEFRAG_EVERY_BLOCK=true`, which
relocates **every** live object on **every** GC — the harshest possible test for
a stale pointer. So far this passes: the torture, retain (200k-list), infix
(mutually-recursive closures), and multi-domain churn tests all run correctly
under it, and a 150-iteration soak (multidom8 + infix, alternating) was clean.
No explicit pin has been needed yet. The next broadening step is OCaml's own
testsuite under forced defrag (roadmap M7), which exercises far more C
primitives and object shapes.

## MMTk fixed-address metadata mmap can fail with EEXIST (ASLR collision)

*2026-06-19*

Intermittently, a fresh run aborts at startup with:

```
panicked at mmtk-0.32.0/src/policy/space.rs:724: failed to mmap meta memory: File exists (os error 17)
```

On Linux mmtk-core maps its side-metadata with `MAP_FIXED_NOREPLACE`
(`util/memory.rs`), which returns `EEXIST` when something ASLR placed lands in
MMTk's fixed metadata range. It is intermittent (depends on ASLR), happens at
init (not during GC), and is unrelated to our binding or the moving code — a
soak hit it roughly once per ~30 fresh processes.

Workaround for testing: run under `setarch -R` (disables ASLR), and/or retry the
process on this specific panic (our soak script does both). It still recurred
once even with ASLR off, so it's not fully eliminated. Proper fix is upstream
(mmtk-core mmap strategy); track there. Not a correctness issue for a successful
run.

## Backup threads vs. MMTk's own GC threads (deferred)

*2026-06-19*

**Context.** OCaml 5's runtime gives every domain a *backup thread*. Its job is
to participate in stop-the-world (STW) sections — `caml_try_run_on_all_domains`
— on behalf of a domain that has released its domain lock, i.e. one sitting in a
C blocking section and not running OCaml. Without it, an OCaml STW would
deadlock waiting for a blocked domain that cannot itself reach the barrier.
(State machine and rationale: `BT_*` in `runtime/domain.c`.)

We currently **rely** on this mechanism. When a domain parks for an MMTk
collection it releases the domain lock and enters `BT_IN_BLOCKING_SECTION`
(`caml_mmtk_park` in `runtime/mmtk.c`), so its backup thread answers any
*concurrent* OCaml STW. This is what fixes the MMTk-STW-vs-OCaml-STW deadlock we
hit with multi-domain programs: a domain terminating (which runs an OCaml STW
via `caml_try_run_on_all_domains`) at the same moment MMTk was stopping the world
would otherwise deadlock — OCaml waits for the parked domains to join its
barrier, MMTk waits for every domain to park. Routing the MMTk park through the
blocking-section/backup-thread path lets the two barriers coexist.

**Open question: can the backup thread be removed entirely once MMTk owns the
GC?** MMTk runs its own dedicated GC worker threads, so the *original* reason
backup threads exist — running GC STW work (major-GC slices) on behalf of blocked
domains — no longer applies under MMTk; MMTk's workers do that work. If MMTk is
the only collector, OCaml's native STW is then needed only for **non-GC**
purposes (domain spawn/terminate, `Gc.compact`/stat, a few runtime maintenance
operations). If those were re-expressed — or themselves driven through MMTk's
stop-the-world — the backup thread, and the whole *dual* STW machinery that
caused the deadlock above, might be eliminable. That would simplify the runtime
and remove a class of races by construction.

**Before acting, check what still needs OCaml STW:**

- Domain lifecycle: `domain_create` / `caml_domain_terminate` use
  `caml_try_run_on_all_domains` to mutate the global domain set.
- `Gc` stat/compact and any `caml_try_run_on_all_domains[_async]` callers that
  survive once the stock major/minor GC is gone.
- Signal handling and anything else that assumes a blocked domain can be
  represented at a STW barrier by its backup thread.

**Status.** Not urgent. Revisit once the GC plan stabilises (post-Immix), when we
can see the full set of remaining `caml_try_run_on_all_domains` callers in a
MMTk-only build and decide whether to (a) keep cooperating with backup threads
as we do now, or (b) drive all remaining STW through MMTk and drop backup
threads. Tracked here; consider promoting to a GitHub issue when we schedule it.
