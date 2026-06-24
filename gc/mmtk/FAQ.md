# FAQ — MMTk-OCaml correctness & concurrency hazards

Mechanism-level answers to the subtle *"is this actually correct?"* questions that recur about the
MMTk integration — especially concurrency hazards under the moving and concurrent plans. These are
questions a passing stressor **cannot** settle (a rare race slips a checksum test); each answer is
grounded in the source, not in test evidence.

Distinct from its siblings: `NOTES.md` is the dated design log, `ROADMAP.md` is the plan,
`RESEARCH_QUESTIONS.md` is the research agenda. **This file is the standing Q&A of *why* a hazard is or
isn't a problem.** Add an entry whenever a tricky correctness question is raised and reasoned through.
Each is tagged **SETTLED** (with the mechanism that closes it + source refs) or **OPEN** (a real hazard
not yet resolved or tested).

---

## Q1. Does ConcurrentImmix's marker race the mutator on the object *header word*? (e.g. `lazy` retag vs the mark bit) — **SETTLED**

**The worry.** In a concurrent collector, if the GC thread sets a mark bit *in the header word* while the
mutator changes the same word (lazy forcing flips the tag; any header mutation), a non-atomic
read-modify-write on either side loses the other's update — a dropped mark (→ a live object swept) or a
lost retag. Vanilla OCaml keeps the GC colour *in* the header and defends with a **CAS on both the GC and
mutator side**, and avoids bit-level writes to dodge word tearing.

**Why it cannot happen here: all GC status bits are in side metadata, not the header.**
- `LOCAL_MARK_BIT_SPEC = side_after(...)` and `GLOBAL_LOG_BIT_SPEC = side_first()`
  (`gc/mmtk/binding/src/object_model.rs:17-27`) — the mark bit and the SATB unlog bit are **side metadata**.
- `gc/mmtk/common/src/header.rs:5`: the header's colour bits (9..8) are *"managed by OCaml's own GC; MMTk
  uses side metadata"* — MMTk never writes them, and `make_header` zeroes them.

So the two writers touch **disjoint words**: the marker sets a bit in a separate side-metadata region; the
mutator's lazy retag writes the header's *tag byte*. No shared word ⇒ no lost update. The marker only ever
**reads** the header (for tag/wosize to drive scanning), and a header write is a single aligned word, so it
reads either the old (`Lazy`) or new (`Forward`) tag — both scanned correctly. Among GC threads,
side-metadata bit-sets are atomic in mmtk-core; the mutator never touches mark metadata. This is exactly why
the fork needs no CAS-on-header dance — vanilla shares the colour word, the fork does not.

**Caveat (the one in-header GC field).** `LOCAL_FORWARDING_POINTER_SPEC = in_header(0)`
(`object_model.rs:20-21`) overwrites the header with a forwarding pointer — but **only during STW Full
pauses** (concurrent phases are non-moving). Mutator stopped ⇒ no concurrent header write. A
*concurrent-compacting* plan would write it concurrently with mutator header writes and **reopen exactly
this race**, forcing a header-resident, CAS'd mark like vanilla (and the load/read barrier OCaml refused).

---

## Q2. Does `lazy` forcing break under ConcurrentImmix? — **SETTLED (for this collector)**

No, in two independently-tested regimes; depth in `RESEARCH_QUESTIONS.md` RQ1.
- **force-vs-mark** (concurrent marking, non-moving): a multi-domain force stressor yields a checksum
  identical to Immix/StickyImmix.
- **force-vs-relocate** (forcing during a defragging STW Full pause, `objects_copied > 0`): also identical
  checksum, 0 sanity errors.

**Mechanism.** The only edge-deletion in forcing — clearing the thunk's field (`Obj.set_field b 0 ()`) —
routes through `caml_modify` and is SATB-covered pre-store. The lazy *retag* only CASes the header tag
(deletes no edge), and mark state is side-metadata (Q1), so it can't corrupt the marker. Liveness (SATB)
and movement (forwarding) are **disjoint paths that never combine**: concurrent phases are strictly
non-moving (`ConcurrentTraceObjects::trace_object` asserts `object == new_object`); relocation is STW-only
with the SATB barrier deactivated. The hard hazard — a lazy forced *during a concurrent relocation* —
does not exist because the collector never moves concurrently.

---

## Q3. Can the concurrent marker race a **continuation resume** while scanning its fiber stack? — **FIXED (bytecode; 2026-06-23)**

**The hazard.** A continuation is a heap object pointing at a captured fiber stack. During concurrent
marking a GC thread traces the continuation and scans that fiber stack as roots (`scan_object` →
`continuation_stack`, `gc/mmtk/binding/src/scanning.rs` / `gc/mmtk/common/src/scanning.rs`). Concurrently,
another domain may `continue k` — switching execution onto that fiber stack and **running on it**. The
marker is then reading a stack a mutator is actively pushing/popping/mutating → a stale or half-written slot
→ a missed root (→ dangling) or a wild pointer (→ crash). One-shot continuations bound this to the single
resume but do not remove the race.

**Why this one bites and Q1/Q2 don't.** Stacks are not single words guarded by side metadata; they are
mutable regions the marker must walk slot-by-slot. Vanilla OCaml scans stacks only at **STW safepoints**;
this binding scans continuation fiber stacks **inline during concurrent heap tracing** — that is the gap.

**Status: FIXED (2026-06-23, bytecode; commit `55ab6ce40b`).** Confirmed real first — a multi-domain
effect/continuation stressor **deterministically SIGSEGV'd** under ConcurrentImmix (Immix/STW clean). The
per-continuation lock below was implemented (`gc/mmtk/binding/src/cont_lock.rs` + `scan_object` try-lock +
resume lock/snapshot in `fiber.c`, gated on `caml_mmtk_concurrent`): the stressor is now clean (checksum ==
Immix/StickyImmix), and **STW stays flat ~1 ms as live continuations grow 0→6000** (vs Immix's 2.7→314 ms) —
STW does NOT scale with fiber count, the whole goal. Inert outside the concurrent plan (no regression). Native
ConcurrentImmix remains the separate documented native-SATB gap. **One residual, debug-only:** under the
`sanity` build (full-heap re-trace) at ~10 MB the run intermittently *deadlocks* (28 threads in `futex_wait`)
— a sanity-instrumentation × cont-lock interaction, **not** a production miss (the production collector is
clean at ≤12 MB, 18/18); needs `rr` + a lock-ordering audit, tracked separately.

**Fix — mirror vanilla's per-continuation lock (defer-to-STW is REJECTED).** The naive fix — queue every
fiber stack and scan them all at FinalMark (STW) — is wrong: with many live fibers it dumps an unbounded
scan into the pause, making STW time scale with *fiber count* and defeating the whole low-pause point.
Vanilla OCaml already solved this (its major GC is concurrent *and* has first-class continuations): it
**locks the continuation object for the duration of marking its stack**; a concurrent mutator trying to
*resume* (switch to) that continuation is **blocked** on the lock until the scan finishes; and other
concurrent GC threads that reach the same continuation simply **skip** it (already locked / being marked).
Mirror that:
- **GC scan** (`scan_object` → continuation): **try-lock**; if not acquired (held by a resume or another
  worker) → **skip** the stack scan; if acquired → scan the fiber stack → unlock. Scanning a *suspended*
  fiber is safe and concurrent (no STW cost) — the common case.
- **Resume** (`caml_continuation_use*` / runstack switch): acquire the lock **blocking** before switching.
- **Resumed case (as implemented):** mmtk-core's ConcurrentImmix FinalMark does *not* re-scan mutator roots
  (`new_no_scan_roots`), so a resumed continuation can't rely on being picked up there. Instead the resume
  path takes an **SATB stack-snapshot before switching** (`caml_mmtk_cont_snapshot`) — greying the
  about-to-run stack's roots once, which SATB then keeps. So no roots are lost, **and STW does not scale with
  fiber count** — suspended fibers are scanned concurrently; only the (bounded) per-resume snapshot cost is
  borne by the mutator.

The stock lock primitive lives in `runtime/fiber.c` (`git show 5.5.0:runtime/fiber.c`); if M9's neutering
disconnected it from the deleted stock marker, re-connect it to the MMTk marker + resume path rather than
invent one. Validate: resume-during-concurrent-marking is correct (checksum == Immix) **and** FinalMark
time stays flat with many live but un-resumed fibers. Ties to `RESEARCH_QUESTIONS.md` RQ3 (effect handlers
as a GC workload).

---

## Q4. Is wiring an SATB barrier major new engineering? — **SETTLED (no)**

OCaml's *stock* mostly-concurrent major GC is **already** an SATB (Yuasa deletion) collector — `caml_modify`
greys the old referent on overwrite. Bug-#3's work repointed `caml_modify` at MMTk's *generational*
(slot-remembering) barrier and M9 deleted the stock concurrent major, so no SATB path is wired by default;
but the *shape* is native to the runtime. ConcurrentImmix re-introduces it (gated on `caml_mmtk_concurrent`)
by reusing OCaml's existing **slot-granularity** barrier `(start,count)` via mmtk-core's
`memory_region_copy_pre` — *not* the object-granularity `object_reference_write_pre` (which needs the src
object `caml_modify` lacks). ~82 lines; `needs_prepare_mutator` required zero binding work.

---

## Q5. Why can't MarkCompact run native? — **SETTLED (infeasible via TLAB aliasing)**

Native code allocates by an inlined bump into a TLAB aliased onto the plan's Default allocator. MarkCompact
needs per-object bookkeeping the inlined bump cannot produce: a **reserved header word before every object**
(its Lisp-2 forwarding slot) and a **per-object VO bit** set via `post_alloc` that `compact` relies on to
linear-scan the space. The native fast path bump-fills objects gaplessly into raw `[start,end)` and bypasses
`post_alloc`, so an aliased MarkCompact region is unscannable/uncompactable — the slide would corrupt the
heap (confirmed empirically: first compaction panics *"does not have a forwarding pointer"*). MarkSweep
(free-list, no bump region) is native-infeasible for the related reason. Both stay **bytecode-only**;
SemiSpace/NoGC/GenImmix/GenCopy/StickyImmix/Immix run native.

---

## Q6. Which GC plans break OCaml's C-API "no read barrier" assumption? — **SETTLED (none of the current plans; it gates future concurrent compactors)**

**The constraint.** OCaml's C API lets C code read OCaml values *raw* (`Field(v,i)`, `Bytes_val`, …) with no
read/load barrier, and the runtime depends on it. *Retrofitting Parallelism onto OCaml* (ICFP'20) chose the
STW `ParMinor` over the concurrent `ConcMinor` **specifically to avoid a read barrier** — adding one would
force every C-API user to change their code, violating their R1 feature-compatibility requirement. So a plan
that **moves objects concurrently with the mutator** and uses a read barrier to forward reads would silently
break every C stub that reads a field raw (stale / from-space data).

**Which plans does it affect? None of mmtk-core 0.32's.** Every available plan moves objects *only at STW*:
Immix/StickyImmix/GenImmix/GenCopy/SemiSpace/MarkCompact/Compressor defrag/copy/compact at a stop-the-world
pause; **ConcurrentImmix marks concurrently but moves only at its STW Full pause** (Q3 / RQ1
force-vs-relocate). At STW, pointers are fixed up once and every mutator — including a domain running C — is
stopped at a safepoint (and a domain in a *non-blocking* C section keeps the world from stopping until it
yields; the bug-#3 RUNNING-set protocol). So C never observes a move mid-read, and mmtk-core 0.32 carries
only *write*-side barriers (None / ObjectBarrier / SATBBarrier) — **no plan uses a read barrier.** The whole
current menu is C-API-safe on this axis.

**What WOULD break it:** a *concurrent-compacting / on-the-fly copying* collector (the ZGC / C4 / Shenandoah
load-barrier family, or a future concurrent-*evacuating* Immix). 0.32 ships none. Note **LXR is deliberately
read-barrier-free** (write barrier + RC), so an LXR-style plan would *stay* C-API-compatible — which is
exactly why it's RQ1's vehicle. Supporting a true read-barrier plan would require **evolving the C API**:
either a read barrier in the accessor macros (breaks all existing stubs) or object **pinning** across FFI
calls (the VO-bit machinery the Julia/CRuby reports needed and OCaml has so far avoided — RQ4). Whether
that's worth doing is `RESEARCH_QUESTIONS.md` RQ6.

---

## Q7. What is the LXR plan, and is it in MMTk? — **reference: not in mmtk-core 0.32 (research branch); the read-barrier-free low-latency candidate for OCaml**

**What it is.** LXR (Zhao, Blackburn & McKinley, *Low-Latency, High-Throughput Garbage Collection*, PLDI'22)
is a collector built on the MMTk framework over a hierarchical **Immix** heap. It combines **reference
counting** (coalescing RC via a cheap field-logging *write* barrier, ~1.6% mutator overhead) for prompt
incremental reclamation, an **infrequent concurrent backup trace** (SATB mark) to collect cycles and correct
RC's conservatism, and **bounded Immix evacuation** (defrag) at short STW pauses. Defining choice: **no read
barrier** — it bets stores are ~an order of magnitude rarer than loads, so a write barrier beats a read
barrier; low latency comes from RC's promptness + bounded pause work + the *non-moving* concurrent backup
trace, not from concurrent read-barrier evacuation (the ZGC/C4/Shenandoah route LXR avoids). Reported: tight
heap 7.8× throughput + 10× better p99.99 tail latency vs Shenandoah; moderate heap +4% vs G1, +43% vs
Shenandoah.

**Is it in MMTk?** It was implemented *in* MMTk (the paper's artifact, on OpenJDK) but is **NOT a plan in
mainline / released mmtk-core** — 0.32's `PlanSelector` has 11 plans (NoGC, MarkSweep, Immix, GenImmix,
StickyImmix, SemiSpace, GenCopy, MarkCompact, PageProtect, Compressor, ConcurrentImmix); LXR is not among
them, and there is no RC/refcount plan module. It lives on a research branch/fork. So `MMTK_PLAN=LXR` does
not work against crates.io mmtk 0.32 — using it on OCaml means **porting the LXR plan + its RC write barrier**
into our binding (real work, flagged by RESEARCH_QUESTIONS RQ1).

**Why it's the read-barrier-free low-latency candidate for OCaml.** Read-barrier-free ⇒ C-API-compatible
(Q6). Its core bet — low store/mutation frequency keeps the write barrier cheap and lets you skip the read
barrier — is *exactly* OCaml's immutable-by-default regime, only more so; and RC suits OCaml's profile
(immutable objects set their references once at init → cheap RC; short-lived objects die promptly at RC=0).
Versus the `ConcurrentImmix` we have (trace-based: concurrent mark + STW move), LXR is RC-based (prompt
incremental reclamation + an infrequent backup trace), so it should deliver more *consistent* low latency.
Both are read-barrier-free; LXR is RQ1's second vehicle alongside ConcurrentImmix.

---

## Q8. Which plan is closest to vanilla OCaml's GC, and is GenImmix incremental? — **reference (closest = GenImmix; no full match; the true match is a research problem)**

Vanilla OCaml 5: per-domain **copying** minor heap (bump-pointer, STW-parallel, survivors promoted) over a
**shared, non-moving, mostly-concurrent / incremental mark-and-sweep** major, with a **SATB** write barrier, **no
read barrier**, and an optional STW compaction.

**Closest single plan: `GenImmix`** — it matches the defining feature, a **copying nursery that promotes into a
separate mature space**. (`StickyImmix` is generational too, but its nursery is *in-place* — sticky mark-bit, no
copy/promote — so it's less faithful.) GenImmix differs on two axes: its mature is **moving** (Immix defrag)
where vanilla's is non-moving, and its collection is **STW** where vanilla's major is mostly-concurrent.

**Is Immix / GenImmix incremental? No.** Immix is full STW (mark + defrag in one pause). GenImmix is
**generational but not incremental**: short STW *minor* pauses (bounded by the small nursery) + a full STW
*major*. Generational ≠ incremental — the low *average* pause comes from frequent small nurseries, not from
slicing a collection across the mutator; the *worst-case* pause is still a whole-heap STW major. **`ConcurrentImmix`**
is the one with concurrent (incremental-in-spirit) *marking* — but its evacuation is still STW. Vanilla's major
is genuinely incremental + mostly-concurrent (and non-moving, so no evacuation pause at all).

**The true match** — generational + copying-minor + incremental/concurrent + (near-)non-moving major + SATB +
read-barrier-free — **is not an mmtk-core 0.32 plan, and building it is a research problem** (`RESEARCH_QUESTIONS.md`
RQ7); it coincides with the RQ1 low-latency target.

---

## Q9. Why is GenImmix the default plan (it replaced Immix)? — **SETTLED**

OCaml allocates a torrent of small, short-lived objects with very low nursery survival — the textbook
generational regime. GenImmix gives each domain a copying nursery promoting into an Immix mature, so a minor GC
over near-entirely-dead young objects copies almost nothing: native GenImmix reaches parity-or-better with
vanilla 5.5.0 on 5/6 CLBG benches and wins ~1.5× on parallel alloc-heavy (binarytrees). It fires more GCs than
plain Immix (spectralnorm 45 vs 23) but copies few objects — the generational hypothesis empirically holds for
OCaml. It is also the most stock-faithful plan (vanilla's minor is a copying nursery). Caveat: the flip surfaced
a generational-minor weak-clear regression — GH#5 (see Q11). The one structural loss is spectralnorm (~1.6×, vs
Immix's 1.74×) — Immix mature-space sweep cost, the lever RQ8/RQ7 target.

---

## Q10. Is MMTk's eager zero-fill correct to remove for OCaml (no-zero allocation)? — **SETTLED — no-zero LANDED on mainline, enabled for all plans**

mmtk-core 0.32 zero-fills every recyclable line/region before the mutator fills it, so OCaml writes every word
twice (~20% of spectralnorm cycles). Existence proof it's redundant: vanilla OCaml's minor heap is never zeroed
— it bump-allocates into uninitialized young memory, relying on "no GC between caml_alloc and field-fill".
Removing it (the 0.32-ocaml fork's no_zero_alloc) is SAFE for STW plans: the scanner is header-driven (header
always written), native Ialloc polls before the field stores with no safepoint between, no-scan tags
(String/Bytes/Double/Custom) read zero fields anyway. Load-bearing case: Closure_tag (scanner reads field 1
closinfo for start_env; OCaml already forbids a GC before closinfo is set — issue #11482). Measured recovery:
spectralnorm +21.9%, alloc/mutate/binarytrees +2–10%, nbody +0.0% (compute control), GC count/time/copies
identical — a pure mutator win. **LANDED on mainline** (tip `338cce723`) and **enabled for all plans —
including ConcurrentImmix** — via a *runtime* gate (an `alloc_zeroed` flag forwarded to the two zeroing sites,
set 0 by `runtime/mmtk.c`), so one binary is correct across every `MMTK_PLAN`; the `gc/mmtk-core` fork
(`0.32-ocaml`) is now the mainline mmtk dependency. The concurrent-marker subtlety (→ RQ9) is **resolved**:
ConcurrentImmix is **allocate-black** (the marker eager-marks acquired lines and never traces a
newly-allocated object's fields), so it never reads the header-written/fields-unwritten window — no-zero is
verified safe there too, so it is enabled rather than gated off. The instructive point: *which* invariant
carries the proof differs STW (the safepoint property) vs. concurrent (allocate-black non-scanning).

---

## Q11. Does the GenImmix default clear weak references / ephemerons too early? — **OPEN (GH#5; fix in progress)**

Yes — flipping the default to GenImmix surfaced a real regression: weak-ephe-final/weaklifetime.ml (native +
bytecode) asserts a weak is CLEARED while its block is still reachable. Root cause: process_weak_refs DOES run
on nursery GCs, but the clear-vs-keep predicate is ObjectReference::is_reachable() → (ImmixSpace doesn't
override SFT::is_reachable) → is_live → is_marked() against a mark_state advanced ONLY on full GCs. An object
freshly promoted to mature during this very minor GC has no current mark bit → is_reachable()==false → its
still-held weak is wrongly cleared. MMTK_WEAK_REFS=0 makes it worse (6→9; never-clear breaks the positive-clear
asserts). Fix: make the predicate generational-aware — on a nursery GC treat any non-nursery-resident referent
as live, clear only dead nursery objects (stock OCaml's minor rule); needs a small public shim to the
0.32-ocaml mmtk-core fork. Full GCs unaffected (Immix byte-identical, preserves 10/14). A multi-domain
orphan-finaliser handover SIGSEGV (finaliser_handover.ml) — a use-after-free of un-rooted orphaned
finaliser values — is **FIXED** (`a1971a465d`: root `orph_structs` every GC, 3/3 crash → 0/62). A
low-frequency adoption-routing residual remains (some adopted finalisers get queued on one domain and never
drain — under-execution at small heaps, not a crash). The weak-clear (GH#5) item above is the remaining
default-plan tail for Weak/Ephemeron users.
