# MMTk-OCaml design notes & deferred investigations

Running notes on design decisions and things we have deliberately deferred.
Each entry is dated and self-contained. Newest first.

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

## Native TLAB / nursery-aliasing — feasibility study & blueprint

*2026-06-20*

Goal: all-MMTk for native (MMTk owns the nursery too), so there's no OCaml minor
GC and the nested-STW hazard can't occur. Studied the mmtk-core 0.32 allocator
internals; **it is feasible** with this design:

**Design — OCaml's young region IS an MMTk Immix block.**
- On refill (where native currently does a minor GC), the binding hands OCaml a
  fresh Immix block: drive the mutator's **Default** `ImmixAllocator` (its
  `bump_pointer` is `pub`: `cursor`/`limit`/`reset`) and return `[cursor, limit)`.
  Set `young_end = limit`, `young_ptr = limit`, `young_limit = cursor`. OCaml's
  inlined fast-path bumps `young_ptr` **down**, filling the block top-down.
- When `young_ptr` reaches `young_limit` (= block start), the block is full →
  refill again (advance the allocator's `cursor` to `limit`, get a new block).
- **No OCaml minor GC, no promotion** — the block's objects are MMTk objects.
  MMTk traces them from roots (native frame descriptors → our scanner), marks
  Immix lines, and reclaims unmarked lines. Bump direction is irrelevant to
  Immix's mark-region GC (it follows pointers, marks lines per live object).

**Feasibility findings (the de-risking):**
- ✅ No per-object `post_alloc` needed: Immix's `post_alloc` only `set_vo_bit`s
  *under the `vo_bit` feature* (we don't enable it). So OCaml bump-filling a block
  without calling post_alloc is fine for non-vo_bit Immix.
- ✅ Comballoc is fine: multiple objects per young region all live in the one
  MMTk block, each traced individually from roots (this only breaks the
  alternative "always-slow-path, alloc a region per comballoc" approach).
- ✅ MMTk exposes the pieces: `BumpPointer{cursor,limit,reset}` pub;
  `Plan::get_allocator_mapping()` and `Allocators::get_allocator(_mut)` pub.
- ⚠️ One plumbing wrinkle: `Mutator::allocators` is `pub(crate)`, so the binding
  reaches the active allocator via an offset/unsafe accessor (the standard
  MMTk VM fast-path mechanism, as mmtk-julia/ruby do) — not a blocker, just
  plumbing.
- ⚠️ `young_limit` dual role (GC trigger + STW-interrupt poison): set the "real"
  limit = block `cursor`; the existing interrupt poison/`caml_reset_young_limit`
  path still works.

**Implementation steps:** (1) binding `mmtk_ocaml_refill_tlab(mutator, min) ->
{start,end}` that drives the ImmixAllocator (slow-path a block, return bounds,
sync cursor); (2) C glue to reset `young_*` from it; (3) call it from
`caml_alloc_small_dispatch` in place of the minor GC; (4) disable the stock minor
GC for native-all-MMTk; (5) global `native_c_libraries` link. Validate single-
then multi-domain, then the testsuite. This is research-grade plumbing but no
longer an unknown.

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
