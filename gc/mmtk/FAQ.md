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

## Q3. Can the concurrent marker race a **continuation resume** while scanning its fiber stack? — **OPEN**

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

**Status:** real, and in the current ConcurrentImmix prototype **unhandled and untested** — the SATB work
targeted the write barrier and `lazy`; no effect/continuation workload has been run under ConcurrentImmix.

**Likely fix.** Defer continuation-stack scanning to a STW phase: during concurrent marking, mark the
continuation object and its fiber reference but **queue** the stack; scan all queued stacks at **FinalMark**
with mutators stopped. (Alternative: a resume↔scan handshake / per-fiber claim so a resume waits for, or
forces re-scan after, an in-progress concurrent scan.) Cost: STW work proportional to live continuations.
Validate with a multi-domain effect stressor that resumes continuations *during* concurrent marking, at a
small heap under `sanity`. Ties to `RESEARCH_QUESTIONS.md` RQ3 (effect handlers as a GC workload).

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
