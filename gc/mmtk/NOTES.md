# MMTk-OCaml design notes & deferred investigations

Running notes on design decisions and things we have deliberately deferred.
Each entry is dated and self-contained. Newest first.

---

## Excise Phase 2 step 3 (frametables): REDESIGN to GC-cycle RCU (Dolan's original); + a latent quiesce-primitive deadlock found (2026-06-26)

Studying the UPSTREAM design (per the new CLAUDE rule: `git blame` → PR → discussion) changed the step-3 plan
and surfaced a bug in the step-1 primitive.

**Upstream history of `frame_descriptors.c`:**
- Dolan 2018 (`e91cea84e30`, "Remove dependency on shared heap from frametables"): frametables were
  **GC-cycle RCU** — "no frametables are deallocated until after the end of the GC cycle in which they were
  replaced. (This is effectively RCU)." The header's "valid until the next GC" contract is from this.
- Multicore rework (Scherer `fb99258d67` 2023, "protect current_frametable update with a STW section"; +
  `5a042b04d4` single global table; `aa6d3be9e9` "delay freeing stale tables until the next registration"):
  **replaced** the cycle-RCU with an all-domains **STW** for the rebuild. `trunk` still uses the STW. The
  fork's stale `#include "caml/major_gc.h" /* for caml_major_cycles_completed */` is a vestige of the old RCU.

**Why upstream used STW (PR archaeology, confirms the redesign):** the multicore STW-ification was **PR #11980**
(`frametables-in-stw`, Scherer) + #11935 (batch dynlink) — driven by a **quadratic natdynlink slowdown** that
regressed Coq/Frama-C (perf, NOT correctness). Scherer explicitly weighed 3 options — STW vs a concurrent
hashtable vs a reader-writer lock — and picked STW **pragmatically/transitorily** ("too lazy", "a better-scaling
approach could be proposed later, much easier on top of this PR"). Dolan's objection to the rw-lock: reader
contention — the reader path (every GC root scan / backtrace / signal) **must stay sync-free**. So a better-
scaling replacement was *explicitly invited as future work*; cycle-RCU is exactly that. The "valid until next GC"
contract is **enforced by reader discipline** (each reader re-fetches `caml_get_frame_descrs()` at the top of its
walk and never holds it across a safepoint: the GC-root reader runs *inside* the GC; backtrace readers are
`CAMLnoalloc` straight-line) → one GC cycle is a sound grace period. "Delay-until-next-registration"
(`aa6d3be9e9`) is safe *only because of the STW* — remove the STW and it no longer drains readers, so we MUST
substitute a real grace period.

**Step-3 plan (REVISED):** replace the frametable STW with **GC-cycle RCU, not the step-1 quiesce.** Publish a
fresh immutable `{mask, descriptors}` snapshot via `atomic_store_release` (bundle mask+descriptors so they swap
atomically — never a new mask vs old descriptors); a writer-lock serialises installers; tag the retired old
snapshot with **`mmtk_ocaml_gc_count()`** and free it lazily (at the next registration / in `caml_get_frame_descrs`)
once that counter has advanced (a full MMTk collection elapsed). This drains BOTH mutator stack-walkers (the
"valid until next GC" contract) AND ConcurrentImmix GC workers (the cycle completes), with **no quiesce, no
`wait_collection_done`, and no new deadlock**. Dolan's original 2018 design (`frametable_version` +
`free_prev_after_cycle`, `git show e91cea84e30`) re-keyed onto the MMTk cycle. The opaque `caml_frame_descrs`
(header forward-decl only) makes the struct split internal; the zombies/unregister path (custom-block-finalizer-
reachable, under `mutex`) stays, with the removed descriptors' free also deferred to the grace period. (The
agent's earlier quiesce-based step-3 design is superseded — it had the two deadlocks below.)

**⚠ MUST-FIX COUNTER TRAP:** `caml_major_cycles_completed` is **DEAD under MMTk** — initialised 0 in
`major_gc.c:55`, only ever *read* (`sys.c:175`); the stock major-GC machinery that bumped it is bypassed. The
fork's `#include "caml/major_gc.h" /* for caml_major_cycles_completed */` (`frame_descriptors.c:23`) is a vestige.
Keying the retire on it would free the old table prematurely/never. **Use `mmtk_ocaml_gc_count()`**
(`collection.rs:324`, `+1` per `resume_mutators` = once per MMTk collection; surfaced via `caml_mmtk_gc_stats`,
`mmtk.c:631`). It counts every MMTk collection (minor+major) — conservative + fine for a grace period (Dolan even
noted a minor GC suffices).

**⚠ LATENT BUG in the step-1 quiesce primitive (`caml_mmtk_quiesce_running_domains`, merged):** its wait leaves
RUNNING via the `caml_mmtk_enter_blocking` HOOK only (marks STOPPED in MMTk's RUNNING set) — it does NOT do the
backup-thread handoff. So while it spins in the poll loop holding its domain lock, OCaml's still-present
all-domains STW (spawn/terminate/minor `caml_try_run_on_all_domains`) would await this domain → **deadlock**. It
did not manifest in step-2 validation (runtime_events destroy at exit + the native stress had no concurrent
spawn/terminate during the quiesce). Fix: use the FULL blocking-section handoff (`caml_bt_exit_ocaml` +
`caml_release_domain_lock`, à la `caml_mmtk_cooperative_park`/`caml_enter_blocking_section`) so the backup thread
answers OCaml STWs during the wait. runtime_events still NEEDS the quiesce (its reader `write_to_ring` is
per-event, not GC-cycle-tied, so cycle-RCU doesn't apply there) — so this fix matters for step-2 robustness.
A second issue: the agent's step-3 `mmtk_ocaml_wait_collection_done()` call was placed while RUNNING → also a
deadlock window; cycle-RCU avoids needing it at all.

Upstream-PR archaeology (why multicore chose STW over RCU; whether RCU was discussed) is in flight (agent) — will
refine. Until then: step 3 = cycle-RCU; the quiesce fix is a separate small commit.

---

## BUG (GH#15): multi-domain GenImmix deadlock under infinite-alloc domains — PRE-EXISTING (A/B'd to 6dd121c2ea) (2026-06-26)

Found while validating excise Phase 2 step 2 (a multi-domain runtime_events stress). **Reproduces on the
DEFAULT plan (GenImmix), bytecode**, and is **NOT** caused by the STW excision — see A/B below.

**Repro** (`scratchpad/plain_stress.ml`, macOS, no runtime_events needed):
```ocaml
let () =
  let _ds = Array.init 4 (fun _ -> Domain.spawn (fun () ->
    let r = ref [] in
    while true do r := (Array.make 10 0) :: !r; if List.length !r > 2000 then r := [] done)) in
  let r = ref [] in
  for _ = 1 to 3_000_000 do r := (Array.make 5 0) :: !r; if List.length !r > 2000 then r := [] done;
  ignore (Sys.opaque_identity r); exit 0
```
`MMTK_PLAN=GenImmix ./runtime/ocamlrun plain_stress.byte` hangs (3/3, perl-alarm 25s). 4 never-joined domains
in tight infinite alloc loops + a busy main. Native (`.opt`) did NOT hang in ~5 runs — bytecode-specific or
timing-sensitive.

**Sample (`sample <pid>`, 22 threads, ALL blocked — hard deadlock, no thread runs):**
- 13 threads in `caml_mmtk_park` → `mmtk_ocaml_stw_park` (collection.rs:172) → `__psynch_cvwait` — mutator
  domains + their backup threads parked waiting `gc_active==false`.
- GC worker threads in `mmtk::scheduler::worker_monitor::WorkerMonitor::park_and_wait` (worker_monitor.rs:221)
  — **idle, no work** (a few caught mid `ProcessEdgesWork::do_work`/`PlanScanObjects`/`visit_slot` tracing.rs:137,
  i.e. marking had been happening then drained).
- backup threads in `caml_plat_wait` (unix.c:686).
- So: a collection is in progress (every mutator parked on it), the markers have run out of work and parked,
  but the coordinator never declares the GC done → `resume_mutators` never fires → everyone waits forever.
  Classic **marker-vs-mutator / lost-progress livelock**, but on a STOP-THE-WORLD plan (GenImmix), distinct
  from the ConcurrentImmix chameneos hang (#5) and the relaxed STW assert (GH#14).

**A/B — pre-existing, not the excision:** checked out the GC/STW files at pre-excision mainline `6dd121c2ea`
(before Phase 0/1/2), rebuilt the bytecode runtime, re-ran `plain_stress.byte` → hangs IDENTICALLY (3/3). And
the repro uses no runtime_events at all. So the excision (Phase 0/1/2) did not introduce it.

**Root cause: TODO** (deferred — separate from the excision). Hypothesis: a collection where all mutators park
but the scheduler's "all workers parked + no work ⇒ GC done" condition is missed (lost wakeup / a work packet
that never gets added or a worker that parks before the coordinator observes the last unit), so the cycle never
ends. Relates to the multi-domain-deadlock class (#5/#6); needs Linux `rr`/`bpftrace` to pin the missed-progress
edge. Filed as GH#15 (https://github.com/fplaunchpad/ocaml-mmtk/issues/15).

---

## Excise OCaml STW — PHASE 2 steps 1+2 DONE (quiesce primitive + runtime_events); step 3 (frametables RCU) next (2026-06-26)

**Step 1 (`3d2eef30a5`):** the dormant `caml_mmtk_quiesce_running_domains()` ragged-epoch primitive (per-domain
`mmtk_seen_quiesce_epoch` field acked at the safepoint; caller leaves RUNNING via enter_blocking, snapshots the
RUNNING set, poisons + polls until each acks the epoch or leaves RUNNING, never un-poisons; Rust hooks
`mmtk_ocaml_snapshot_running`/`mmtk_ocaml_is_running`). Built green, behaviour-neutral (golden + counter unchanged).

**Step 2 (`e06e717e51`):** runtime_events start/stop off the all-domains STW. START = monotonic off→on release
publish under a new `runtime_events_lifecycle_lock` (no stop). STOP (`caml_runtime_events_destroy`) = publish
enabled=0 FIRST → `caml_mmtk_quiesce_running_domains()` (drain in-flight `write_to_ring` emitters) → munmap;
teardown helper renamed `runtime_events_unmap`, no longer clears enabled (callers do, before the quiesce); the
reorder closes a munmap-vs-write_to_ring UAF the old STW masked. Both `stw_*_runtime_events` callbacks deleted.

**⚠ DISCOVERY during step-2 validation:** found GH#15 (pre-existing multi-domain GenImmix deadlock) — see entry
above. NOT caused by Phase 2.

**Validated (steps 1+2):** clean world.opt; native multi-domain start/emit/**destroy+quiesce** stress 10/10 (5
before + 5 after the fresh build — the primitive's first real exercise, drains live RUNNING domains at teardown);
lib-runtime-events testsuite A/B = **identical 10 pre-existing failures, ZERO new**, no crash/hang, and every
previously-passing start/destroy/fork/cursor test still passes (92 passed). par_binarytrees d1/d8 golden +
Gc.minor counter unchanged (step 1 neutrality).

**Step 3 (next): frametables RCU** — rebuild-off-to-the-side + atomic-publish + ragged grace-period retire (Julia
world-age); reader side unchanged. The two `caml_try_run_on_all_domains` callers at `frame_descriptors.c:306,318`.
Validate with `lib-dynlink-domains` (concurrent native Dynlink.loadfile + busy domains) under sanity small-heap.
Also fixes the latent ConcurrentImmix worker-vs-frametable-installer hazard. Design below.

---

## Excise OCaml STW — PHASE 2 design (re-home frametables + runtime_events; the legitimate non-GC STW users) (2026-06-26)

Agent-designed + sibling-validated; not yet implemented. The remaining `caml_try_run_on_all_domains` call sites
(6 total): minor-GC rendezvous (`minor_gc.c:419`, Phase 1/3), spawn + terminate (`domain.c:2296`, Phase 3), and
the **two legitimate non-GC users Phase 2 retires**: frametables install (`frame_descriptors.c:306,318` →
`stw_register_frametables`) and runtime_events start/stop (`runtime_events.c:438,228`). These can't ride MMTk's
`stop_all_mutators` (its STW callback is hard-wired to GC marking), so they need a non-GC replacement.

**Key design decision — a ragged epoch, NOT a second barrier.** New common primitive
`caml_mmtk_quiesce_running_domains()` (mmtk.c, backed by collection.rs): bump a global epoch, poison every
domain's `young_limit` (reuse `caml_mmtk_interrupt`), block the caller until every domain RUNNING at call time
has passed one safepoint (new per-domain `seen_quiesce_epoch` bumped in `caml_poll_gc_work`). STOPPED domains
(parked/blocking/terminating) hold no transient reader pointer, so aren't awaited — same invariant the RUNNING
set already encodes. **No leader, no all-domains barrier** → does NOT re-create the bug#3c dual-STW seam (Phase 3's
payoff). The quiesce wait must itself be a cooperative safepoint (park if `mmtk_ocaml_stw_active()`), like the
`caml_empty_minor_heaps_once` bracketing. Pattern from Julia world-age (`jl_world_counter` + lazy revalidation,
no barrier) + `jl_gc_add_quiescent`; Ruby `rb_vm_barrier` ragged; OpenJDK reuses its GC safepoint / a lock —
all three: *one GC-owned safepoint; a true all-threads stop only for state no thread may observe stale.*

**Frametables → RCU/epoch (writer-rare, reader-hot).** Today `add_frame_descriptors` frees the old `descriptors[]`
in place and `remove_entry` moves entries within it — a UAF/torn-probe hazard for any concurrent native stack-walk
(GC root scan `fiber.c:272-294`, backtrace, signals, tsan; also ConcurrentImmix GC *workers* via
`scanning.rs:322` — a pre-existing latent hazard). Fix: rebuild a FRESH array off to the side, publish via one
`atomic_store_release` of a single immutable snapshot pointer; readers take one acquire-load snapshot per walk
(they already call `caml_get_frame_descrs()` once at top); retire the old array via grace period
(`caml_mmtk_quiesce_running_domains` + `mmtk_ocaml_wait_collection_done` to also cover GC workers) then free. A
`frame_descrs_writer_lock` serializes installers (the old STW-leader role). No global stop; only the rare
dynlink installer waits one safepoint round. Also fixes the ConcurrentImmix worker-vs-installer hazard.

**runtime_events → monotonic publish (start) + ragged drain (stop).** Each domain writes only its own ring slot
indexed by `Caml_state->id` off the single global `current_metadata` mmap — no per-domain pointer to publish.
START: under a setup lock, mmap+init, `release`-publish `current_metadata` then `runtime_events_enabled=1`;
readers self-gate on `ring_is_active()`; a missed event in the enable window is harmless (flight recorder) → **no
stop needed**. STOP (the only op needing reader quiescence — `munmap` vs in-flight `write_to_ring` UAF): reorder
to `enabled=0` FIRST (release), then `caml_mmtk_quiesce_running_domains()` (drain emitters past the
`ring_is_active()` check — `write_to_ring` is bounded straight-line, one ragged round suffices), THEN `munmap`.

**Edit order (independently buildable):** (1) add dormant primitive (validate: build + sanity, behaviour-neutral);
(2) runtime_events (validate: `lib-runtime-events/test_caml_parallel` + fork/external, under sanity + ASan/TSan
for the munmap race); (3) frametables RCU (validate: `lib-dynlink-domains` native — concurrent `Dynlink.loadfile`
+ busy domains — under sanity small-heap, every Immix plan; adversarial: loop loadfile on one domain while others
run deep native recursion at `MMTK_HEAP_SIZE_MB=32`). After Phase 2, only GC/spawn/terminate participant-set users
remain for Phase 3 to delete wholesale. **Do NOT touch the terminate/minor-empty join in Phase 2** (participant-set
contract must stay intact until Phase 3's coordinated cut). Full design: agent report 2026-06-26 (ROADMAP #18 Phase 2).

---

## Excise OCaml STW — PHASE 1 DONE: minor-cycle bookkeeping re-homed off the all-domains STW (2026-06-26)

Toward "one STW to rule them all" (MMTk `stop_all_mutators` sole rendezvous). Phase 1 moved the
**domain-LOCAL** minor-cycle bookkeeping out of the all-domains minor STW onto each triggering domain's own
safepoint, leaving the STW handler a bare shell that Phase 3 can delete. Agent-designed + multi-domain-
adversarially verified; integrated + built + validated in the main loop. Branch `excise-ocaml-stw`.

**Step 1/2 (`d556fb6ea6`):** added `caml_minor_gc_domain_bookkeeping(domain, bump_count)` (minor_gc.c) =
gc-stats sample → memprof → `caml_final_update_last_minor` → `caml_empty_minor_heap_domain_clear` →
(if bump_count) bump `caml_minor_collections_count`. Additive, behaviour-neutral.

**Step 2/2 (`4677c9b580`, the coordinated cut, one commit):**
- `caml_stw_empty_minor_heap_no_major_slice` stripped to `{leader cycle bump; promote}` — memprof/finaliser/
  table-clear/stats-sample gone; dead `caml_mark_roots_stw` branch removed (`caml_gc_mark_phase_requested`
  never set under MMTk).
- `caml_empty_minor_heap_promote` **kept** (its `caml_reset_young_limit` re-arms the safepoint poison) but
  stripped of its stats-sample + the split `minor_gc_end_barrier`; `minor_gc_leave_barrier` + the
  `minor_gc_end_barrier` global deleted.
- `caml_empty_minor_heap_setup`: no longer bumps the counter or resets the barrier.
- `domain.c` safepoint (`caml_poll_gc_work`): `caml_minor_gc_domain_bookkeeping(d, 1)` after
  `caml_empty_minor_heaps_once()` — reached only in bytecode (native early-returns), so this is the single
  collections-count bump per minor GC; native stays 0. Terminate flush: `(domain_state, 0)`.
- `caml_minor_cycles_started` bump **stays in the handler** (`participating[0]==domain`) — it must advance
  inside the STW for the driver retry loop, and the driver caller is not necessarily `participating[0]`.

**Gating hazard RESOLVED (the reason promote is kept):** promote's young-region reset (`minor_gc.c:228-245`)
is NOT dead. The native `if (caml_mmtk_tlab)` branch is redundant with `caml_mmtk_uninterrupt`, but the
**bytecode `else` branch's `caml_reset_young_limit` is load-bearing** — `young_limit` IS the bytecode
safepoint mechanism (`Caml_check_gc_interrupt` reads `young_ptr` vs `young_limit`; `caml_mmtk_interrupt`
poisons it). Dropping promote would leave the safepoint poisoned → stuck. So promote stays; only its
stats-sample + barrier were stripped (the "safer variant").

**Correctness (multi-domain-adversarial):** re-homing to only the *triggering* domain is safe because for any
domain dragged into the STW that did not itself trigger: `ephe_ref` is never populated and the finaliser
update is vacuous (both `Is_young`-gated, `Is_young==0` under MMTk); the `custom` table is never *read* under
MMTk and is self-bounded by each domain's own `extra_heap_resources_minor` minor-GC self-trigger; memprof/stats
are per-domain and refreshed at that domain's own next safepoint (every dragged-in domain runs
`caml_poll_gc_work` right after the STW callback).

**Validated:** clean `world.opt` (native+bytecode incl. ocamldoc manpages — the former deadlock site);
`par_binarytrees` native d1==d8 == golden (`checksum=355319636 long_lived_check=2097151`, domain-count-
independent); `Gc.minor` ×100 bumps 100 (bytecode) / 0 (native) — exactly-once semantics preserved; mmtk
`sanity` small-heap clean on StickyImmix+GenImmix (par_binarytrees d1/d8, Gc.minor, a 12k-def heavy compile)
— no dangling-edge panic.

**Next:** Phase 2 (re-home frametables + runtime_events off `caml_try_run_on_all_domains`), then Phase 3 (delete
the now-shell minor STW so MMTk `stop_all_mutators` is the sole all-domains rendezvous; backup-thread removal;
multi-domain-exit `caml_stop_all_domains` must remove_running/deregister cancelled peers).

---

## Excise OCaml STW — PHASE 0 DONE: retired the two clean `caml_try_run_on_all_domains` callers (2026-06-25)

First cut of the pole-A excision (the verified plan below). The two callers the adversarial verify had cleared
at refuters-0/2 are gone. Design + re-verification by a Workflow (10 agents: map → per-caller design → 3
adversarial verifiers each → synthesis); integrated + built + validated in the main loop. Branch
`excise-ocaml-stw` (off `5.5+mmtk`). 5 edits, all in `runtime/domain.c` (+1 in `runtime/caml/domain.h`), net −24 lines.

- **minor-heap-resize:** `caml_update_minor_heap_max` (domain.c) now does a **plain store** of
  `caml_minor_heap_max_wsz` instead of an all-domains STW; `stw_resize_minor_heaps_reservation` **deleted**.
  Under MMTk the cap sizes no arena (TLAB owns the nursery), so the STW's only observable effect was the scalar
  store — now done directly. Strengthens the `gc_ctrl.c:262` `CAMLassert(newminwsz <= cap)`.
- **global-major-slice:** `caml_poll_gc_work` now **clears `requested_global_major_slice` locally** instead of
  broadcasting via `caml_try_run_on_all_domains_async`; `stw_global_major_slice` **deleted**. The broadcast ran
  the inert `caml_major_collection_slice` on every peer under MMTk; the requesting domain's slice already fired
  in the block just above.
- **Newly-dead but RETAINED** (non-static / no header prototype ⇒ no `-Wunused`, no build break; reused by
  Phases 1/3): `caml_try_run_on_all_domains_async`, `caml_empty_minor_heap_no_major_slice_from_stw`. *(The
  adversarial verify caught the design's false claim that the async runner "retains six callers" — it becomes
  the SOLE, now-dead async caller. Good catch.)*

**Validated (bytecode, this branch):** `make runtime` clean (C compiles); `Gc.set` raising minor_heap_size works
(plain-store path); `par_binarytrees` d1/d4/d8 under GenImmix/StickyImmix/Immix all return checksum 682198264
with no hang; moving-GC stress (GenImmix + StickyImmix at 64/128 MiB fixed heaps) clean. Native `world.opt` +
a native d8 run [in flight]. **Remaining `caml_try_run_on_all_domains` callers (the *sync* runner) → Phases 1–3:**
terminate (domain.c:2308), frametables (frame_descriptors.c:306/318), runtime_events (228/438). NB stale mentions
of `stw_resize_minor_heaps_reservation` elsewhere in this file predate the deletion.
## LXR integration — sibling references cloned + P2 port plan + usage recipe + P4 barrier reference (2026-06-26)

Set up + validated the LXR integration against the actual sibling implementations (per the "validate against
siblings" rule). **References now local in `_references/` (read-only/gitignored):** `mmtk-openjdk-lxr` (the
`wenyuzhao/mmtk-openjdk` **`lxr`** branch — a real binding that USES LXR), `lxr-builds` (the build/usage recipe).
The LXR **GC fork** is the `lxr` remote in the submodule (`gc/mmtk-core` → `wenyuzhao/mmtk-core` branch `lxr/lxr`).

**`lxr/lxr` is a SIBLING FORK, not a superset of ours** (agent-verified): same 0.32.0 merge-base, but its
`immixspace.rs` is a near-rewrite (+872/−263) interleaving RC with three *unrelated* upstream waves
(page-resource rewrite, `generate_tasks_batched`/`Range<Chunk>`, 1-arg `attempt_mark`/cyclic-mark rework) that
conflict with our deltas (no-zero, SpaceOverheadTrigger, 2-arg `attempt_mark`). **So P2 = hand-write ~10 gated
overlays** behind `rc_enabled`/`crate::args` consts (struct fields, ctor, `side_metadata_specs(rc_enabled)`,
read-side `is_live`/`is_reachable`, inert guards `post_copy`/`mark_lines`/straddle) — **NOT** lift LXR bodies —
so all 8 existing plans stay byte-identical when off. Prereqs: `PlanConstraints.rc_enabled` + 5 RC side-metadata
specs + a `Defrag` rc arg. **P2.5** (split out, heavy): the page-resource RC API + work-packet reshape. **P3:**
port `plan/lxr/` + wire `MMTK_PLAN=LXR`. Full region-by-region plan in the ROADMAP LXR entry.

**Usage recipe (validated vs mmtk-openjdk `lxr` + lxr-builds):** LXR is a pure **runtime plan selection** —
OpenJDK `-XX:ThirdPartyHeapOptions=plan=LXR`; us `MMTK_PLAN=LXR` → `PlanSelector::LXR`. The binding needs **no
LXR cargo feature** (mmtk-openjdk's `default=[]`); the `lxr_*` features are mmtk-core *build-time* tuning,
default-on. **LXR requires a FIXED heap** (no variable sizing — OpenJDK mandates `-Xms==-Xmx`) → P3 must pin
`MMTK_HEAP_SIZE_MB` for LXR and bypass our SpaceOverhead dynamic heap. mmtk-openjdk `lxr` pins mmtk-core
`wenyuzhao @ 304ce69d`.

**P4 barrier reference (mmtk-openjdk-lxr/mmtk/src/api.rs):** LXR's `FieldBarrier` (a coalescing per-slot
field-logging write barrier, `BarrierSelector::FieldBarrier`) is driven by `mmtk_object_reference_write_pre`
(:426) / `_post` (:441) / `_slow` (:456) → `mutator.barrier().object_reference_write_pre/post/slow(src, slot,
target)`, plus `mmtk_object_probable_write` (:511). **Our P4** wires the equivalent into `caml_modify` — a
pre/post slot-granular store barrier, mirroring our existing `caml_mmtk_satb_barrier` path (RQ1's bet: OCaml's
immutable-by-default heap makes most stores initialising writes through `caml_initialize`, which take NO barrier,
so the LXR field barrier is unusually cheap for OCaml).

---

## ROOT-CAUSED + FIXED: the ocamldoc/world.opt deadlock = `resume_mutators` allocating → self-deadlock on the worker-monitor lock (fix `cd62bd47f9`) (2026-06-26)

Root-caused with **gdb on a turing core** (the deadlock reproduces deterministically on Linux too, not just
macOS). It is a **SELF-DEADLOCK** in our binding — **our bug, not mmtk-core** — fixable on our side.

**The chain (thread 17 in the core — the stuck GC worker):**
`park_and_wait` (holds `WorkerMonitorSync` lock) → `on_last_parked` → `on_gc_finished` (scheduler.rs:623) →
binding `resume_mutators` (collection.rs:280) → `caml_mmtk_uninterrupt` (mmtk.c:849) → **`caml_mmtk_refill_tlab`
(allocates!)** → `BumpAllocator::alloc` → `Space::acquire` → `GCTrigger::poll` decides another GC is needed →
`request_schedule_collection` → **`WorkerMonitor::make_request` re-takes the same lock** → the worker blocks on
a lock it already holds. Core state: `GC_ACTIVE=1`, `GC_COUNT=0`, parker `{worker_count:28, parked_workers:28}`,
`goals.current = Some(Gc)`, sync mutex `futex=2` (held). 27 workers wait on the condvar; the mutator waits on
`gc_active` (never cleared). 

**The bug:** `caml_mmtk_uninterrupt` eagerly refilled the TLAB at resume as an fft poll-trap micro-optimization,
with the comment *"driving the allocator is safe here, all mutators are stopped."* That is safe w.r.t. mutators
but NOT w.r.t. mmtk-core's scheduler lock: `resume_mutators` is a VM hook MMTk calls from `on_gc_finished` while
holding `WorkerMonitorSync`, and the allocator's GC-request path re-takes it.

**The fix (follow the siblings).** Verified in `_references/`: **mmtk-openjdk, mmtk-julia, mmtk-ruby all
`resume_mutators` WITHOUT touching the allocator** — they only unblock mutators (+ stats/flags). The standard
MMTk model resets each mutator's allocator in the GC's `Release` phase and the mutator re-acquires a block on
its own next allocation (a normal safepoint, outside any GC lock). So we **removed the eager refill** from
`caml_mmtk_uninterrupt`; the young region stays collapsed and the mutator refills itself. (`cd62bd47f9`.)

**Validated on turing:** the ocamldoc man-gen now **COMPLETES** (was a 100% deterministic hang); par_binarytrees
d1/d4/d8 GenImmix/StickyImmix checksums unchanged (682198264). This unblocks `make world.opt` on both platforms.
**Follow-up:** the fft poll-trap perf the eager refill addressed must be re-homed to the mutator's OWN resume
path (`caml_mmtk_become_running`, mutator context, no lock) — TODO (ROADMAP). The investigation that found it is
below.

---

## `make world.opt` deadlocks at ocamldoc man-gen — a DETERMINISTIC single-domain MMTk deadlock (pre-existing on clean mainline; NOT the STW excision) (2026-06-25)

A clean `make -j world.opt` on macOS (M4 Pro) **hangs** at the ocamldoc man-page generation step. The build
target chain: `world.opt → opt.opt → (if build_libraries_manpages=true) make manpages → make -C api_docgen man`
(Makefile:818-819 / 858-859 / 2158-2160). The hung process is `ocamldoc.opt -man -d build/man …` loading ~150
`.odoc`, single-domain, **parked at 0% CPU**. `sample`d stack:
- **mutator (main thread):** `caml_call_gc → caml_alloc_small_dispatch → caml_mmtk_refill_tlab →
  Space::acquire → caml_mmtk_park → mmtk_ocaml_stw_park → _pthread_cond_wait` — i.e. an alloc-slow path
  triggered a GC and the mutator parked waiting for it to finish.
- **GC worker:** `WorkerMonitor::park_and_wait → _pthread_cond_wait` — **idle, no work scheduled.**

Mutator parked waiting for a collection that never runs, GC worker idle = **the #5/#6/bug#3c single-domain
GC-scheduling deadlock class** (a collection is requested but the work never reaches the worker pool).

**PRE-EXISTING — not the Phase-0 STW excision.** Confirmed three ways, gold standard last: (1) the Phase-0
(excise-ocaml-stw) world.opt hung here; (2) an A/B that reverted `domain.c`/`domain.h` to mainline, rebuilt
`libasmrun.a`, relinked `ocamldoc.opt` → **still hung**; (3) a **clean full mainline build** (`5.5+mmtk`,
`make clean` + `./configure` + `world.opt`, **2205 compile steps, domain.c = 0 edits**) → **still hung at the
same step.** So the excision is exonerated.

**Significance.** This is a **DETERMINISTIC** repro of the single-domain MMTk deadlock class — far more useful
than the intermittent `par_binarytrees`/chameneos ones for debugging #5/#6. ocamldoc man-gen is a long-lived,
heavy single-domain allocator that reliably wedges the alloc-slow→GC-schedule path. **Worth its own rr/core-dump
investigation** (it deterministically reproduces what the deadlock-class fix must address).

**Workaround for builds/testing:** `make world.opt` reaches it only when `build_libraries_manpages=true`. The
testsuite does not need man pages, so configure/build with manpages disabled (or build the compiler core
without `manpages`) to get a working world for `make -C testsuite parallel`. (macOS-observed; check whether
Linux/CI hits it too — if CI builds docs, it would. Filed as the build blocker behind Phase-0's testsuite gate.)

---

## RQ10 pole-B GO/NO-GO — pole-B NO-GO; the multi-domain residual is MILD (S(8)≈1.2–1.6), nursery size is one lever, off-STW marking the other; + a real MMTK_NURSERY parser bug. (Earlier church run was a CONTAMINATED build — corrected here) (2026-06-25)

**⚠️ Correction — read first.** The first pass of this experiment ran on **church**, which was on branch
`fix/bug3c-cross-stw` with a build that **silently ran an ~8 MiB default nursery** (913 GCs on par_binarytrees
d21/d1) instead of the intended 64 MiB. I wrote that up as "BUG A: degenerate default install degrades the
default GenImmix everyone runs" with a dramatic S(8)=1.13 vs StickyImmix 2.86 discriminator. **A clean rebuild on
TWO mainline hosts refutes it:** local macOS and **turing** (clean `5.5+mmtk`, even sharing church's mmtk-core
`0fe660bb9c`) both give **114 GCs** at the default (= the correct 64 MiB; = church's *explicit*-64 MiB). **BUG A
is a church branch/build artifact, NOT mainline, NOT mmtk-core.** The dramatic church scaling numbers were
contaminated by it. Lesson banked: verify a bench host's branch + clean rebuild before trusting its numbers.

**The clean mainline picture (turing, 28-core, pinned, `MMTK_THREADS=domains`, fixed 4 GiB, par_binarytrees d21,
3 reps, min wall-time, verbose GC counts):**

| config | d1 | d2 | d4 | d8 | **S(8)** | GCs d1→d8 |
|---|---|---|---|---|---|---|
| **GenImmix-default** (64 MiB, *correct*) | 9.32 | 7.44 | 6.45 | 7.60 | **1.23** (peaks 1.45@d4, regresses) | 114→165 |
| **GenImmix-256 MiB** | 7.78 | 5.85 | 4.71 | 4.88 | **1.59** | 28→86 |
| **StickyImmix-default** | 10.41 | 8.03 | 6.72 | 7.65 | **1.36** | 109→163 |

The real residual is **mild and similar across plans** (S(8)≈1.2–1.6), not the contaminated 1.13-vs-2.86 cliff.
Two findings:

**Finding 1 — nursery SIZE is a lever, but FIXED-HEAP-ONLY (moot under the default dynamic heap).** With a
**pinned large heap** (the table above, `MMTK_HEAP_SIZE_MB=4096`) GenImmix at a 256 MiB cap is ~20% faster at
every domain count (d1 7.78 vs 9.32; d8 4.88 vs 7.60), drops d1 GCs 114→28, lifts S(8) 1.23→1.59. **BUT under
the DEFAULT dynamic (space-overhead) heap the cap is moot** — confirmed local: binarytrees-19 gives **281 GCs
@256 MiB vs 285 @64 MiB** (≈same), and at a fixed 4 GiB the *same* bench gives 6 vs 25. The reason: under a
dynamic heap (live×2.2) the **space-overhead trigger**, not the nursery cap, gates collection frequency, and a
low-live workload gets a tiny heap → tiny nursery regardless of cap. So raising the cap does **not** help the
plan as users actually run it (dynamic heap). → ROADMAP #21(a) DEFERRED (fixed-heap-only); needs the full
memory-parity panel before landing. The genuinely-default-relevant lever is the **dynamic-heap floor** (#21c,
now quantified): spectralnorm-3500 at the default heap = **7926 GCs / 3378 ms GC / 25.83 s** vs fixed-4 GiB =
**152 GCs / 81 ms / 22.62 s** — the dynamic heap (live×2.2, tiny for a low-live workload) does **52× more GCs**,
~13% GC overhead / ~12% wall. The fix is a nursery floor / min-heap decoupled from the live set, NOT the cap.

**Finding 2 — but the nursery does NOT fix the slope; the residual is per-collection STW cost.** Even
GenImmix-256 MiB — only 28–86 GCs across d1→d8 — still **regresses d4→d8** (4.71→4.88). So the nursery shifts the
*level* (fewer/cheaper collections) but not the upward *slope*: the per-collection all-domains STW + root-scan
cost grows with domains regardless of frequency. This **vindicates SCALABILITY §10.2 / UPDATE-1** (my church
"nursery fixes scaling" overclaim is corrected) and is exactly what off-STW marking (ConcurrentImmix / pole-A
direction), **not** pole-B, addresses.

**BUG B (real on mainline) — `MMTK_NURSERY` suffix syntax is broken.** `Bounded:2m,64m` (documented in
CLAUDE.md/README) → *"unable to set MMTK_NURSERY… Can't parse value. Default value will be used"* → silent
fallback to mmtk-core's default. Reproduced on **clean local mainline** (not just church). Only **raw bytes**
(`Bounded:2097152,67108864`) parse. A real usability bug: anyone tuning the nursery via the documented syntax
silently gets the wrong nursery. Fix the parser to accept `k/m/g` suffixes, or correct the docs.

**BUG A — church-only build artifact (NOT a mainline bug).** On `fix/bug3c-cross-stw`/church the default-nursery
install ran ~8 MiB (913 GCs). The intervening mmtk-core commits (`0fe660bb9c`→`ec2f5079f8`) are all
scheduler/FinalMark — none touch nursery code — and turing at `0fe660bb9c` is clean, so it is the **church branch
api.rs or a stale church build**, not mmtk-core. **Worth checking before merging `fix/bug3c-cross-stw`** (it
would be a real perf regression if it carries this), but it does NOT affect mainline.

**Pole-B verdict: firmly NO-GO.** The multicore residual is mild (S(8)≈1.2–1.6) and has two in-framework levers:
nursery size (Finding 1, cheap) and off-STW marking (Finding 2 → ConcurrentImmix). Pole-B (a VM-ParMinor rebuild)
keeps the all-domains minor STW, so it would not fix the Finding-2 slope, and the Finding-1 level is already a
config knob. No VM-ParMinor rebuild / moving-GC promotion crux / second collector is justified. **→ Do NOT build
pole-B.**

**Caveats:** one alloc-heavy bench (par_binarytrees) at one depth, turing 28-core with ~1.1 background load; a
second workload + a `MMTK_THREADS` sweep would harden it. The 256 MiB win and the BUG B parser bug are clean and
reproduced; the residual-slope (Finding 2) is consistent across all three plans and with §10.2.

---

## RQ10 pole-B (VM-owned ParMinor + MMTk major-only): feasible & novel, but motivation deflated — run the go/no-go experiment BEFORE building (2026-06-25)

The inverse of pole-A: keep stock OCaml's **per-domain ParMinor** (private minor arenas) and use MMTk for the
**major heap only**, promoting survivors into MMTk's mature space. Feasibility analysis (agent, code-verified):

**FEASIBLE — but only against a non-generational (Immix/StickyImmix/ConcurrentImmix) major, with ZERO
mmtk-core changes.** The split is decided by which major plan you pick:
- **Immix major = open to the mutator.** `AllocationSemantics::Default` maps straight to the mature Immix
  space (`gc/mmtk-core/src/plan/immix/mutator.rs:39,53`), so VM-driven promotion just calls
  `mmtk_ocaml_alloc(mutator, wosize, tag, Default)` (`binding/src/api.rs:216-251`, which does `alloc`+`post_alloc`)
  and the survivor lands in mature Immix as a first-class object. StickyImmix/ConcurrentImmix identical.
- **Generational major = CLOSED to the mutator.** For GenImmix/GenCopy `Default` maps to the nursery
  `CopySpace` (`plan/generational/mod.rs:76,86`); mature is reachable **only** from a GC worker via
  `GCWorkerCopyContext::alloc_copy(.., PromoteToMature)` (`util/copy/mod.rs:75`). No `AllocationSemantics`
  means "pre-tenured into mature" (`plan/global.rs:934-958`). A generational-major-under-stock-minor would
  need an mmtk-core fork (the inverse of RQ7 Bactrian). **→ Pin pole-B's baseline to Immix-major.**

**Barrier story is clean (the lowest-risk part).** Under an Immix major `caml_mmtk_generational == 0`
(`runtime/mmtk.c:147-149`), so MMTk's nursery/region barrier is **already a no-op** (`mmtk.c:657-662`) — nothing
to disable. And stock OCaml's `ref_table` remembered set is **still in the tree, only neutered**
(`runtime/caml/minor_gc.h:53,108`; `caml_alloc_table` minor_gc.c). So pole-B **re-wires `caml_modify` to the
stock `Ref_table_add`** and the VM owns the inter-generational barrier; MMTk's nursery barrier is dormant by
construction. (ConcurrentImmix additionally keeps `caml_mmtk_satb_barrier` — orthogonal, tracks major deleted
edges.) **Promotion path:** un-delete the M9-excised per-domain minor arena + real `Is_young`
(`address_class.h:50-64` hardwires it to 0 today), restore stock `oldify_one`/`oldify_mopup` but redirect the
promotion alloc to `mmtk_ocaml_alloc(.., Default)`. The forwarding-pointer/field-copy correctness is the
moving-GC crux — validate with mmtk `sanity` at a tiny Immix heap. Today `caml_empty_minor_heap_promote`
(`minor_gc.c:203-256`) is fully neutered ("MMTk owns the heap, nothing to promote").

**NOVEL — no precedent.** All three sibling MMTk bindings (openjdk/julia/ruby) delegate the **whole** heap to
MMTk and contribute only the VM *safepoint*; none keeps a VM-managed nursery in front of an MMTk major. Pole-B
(VM-private off-heap nursery → promote across the boundary via the binding alloc API, against MMTk's
whole-heap-ownership assumption) is unattested in the ecosystem. Publishable framing: **"which generation
should a retrofitted tracing framework own?"** (RQ10).

**BUT the motivation has largely deflated — this is the key caveat.** The "S(8)=0.64 anti-scaling cliff" was
**substantially retracted** (SCALABILITY.md UPDATE, 2026-06-25): it was mostly `nproc`-worker oversubscription
+ the GH#6 scheduler assert (both now fixed). Controlled (pinned, workers=domains, assert-fixed): `par_matmul`
S(8)=4.71 ≈ vanilla 5.72; `par_binarytrees` GenImmix **S(8)=1.36** — a *mild* sublinear residual, not a cliff.
And the in-tree mechanism evidence (SCALABILITY Exp 2–3) attributes the residual to **in-pause root-scan +
trace/copy cost that grows with stopped domains** — the cure that *worked* was moving the trace **off** the STW
(ConcurrentImmix). Pole-B keeps an all-domains minor STW with full per-domain root-scan, so it would **not**
obviously move that residual. **Risk: pole-B is a large, partially-reverting build justified by a hypothesis
the existing data already leans against.**

**GO/NO-GO experiment (needs ZERO pole-B code; run it FIRST).** Discriminate the two hypotheses:
- H_nursery: the residual is MMTk's **single shared copy-nursery** (contention + survivors-copied-in-one-pause) → pole-B (per-domain arenas) fixes it.
- H_stw: the residual is the **all-domains STW + root-scan** itself → pole-B keeps both → won't help.
Run `par_binarytrees` (d21) + boxed-float `par_spectralnorm` at **d1/d2/d4/d8, pinned, MMTK_THREADS=domains,
assert-fixed**, comparing **GenImmix** (shared *copy*-nursery) vs **Immix/StickyImmix** (in-place, *no* separate
copy-nursery — survivors are just recent Immix lines). **If the multi-domain residual persists on Immix just as
on GenImmix (and doesn't grow with domains on GenImmix relative to Immix), the shared copy-nursery is NOT the
lever → H_stw → do NOT build pole-B.** Immix is the cheapest available proxy for "remove the shared copy-nursery"
without writing the ParMinor reversal.

**Recommendation (agent + concur):** (1) do **pole-A's deadlock elimination regardless** — subtractive,
low-collector-risk, fixes a real reproducible deadlock class (anti-scaling-orthogonal). (2) **Do NOT build
pole-B yet** — run the experiment above first. (3) If pursuing the "which generation" paper, pole-B is the novel
vehicle but frame it around the *measurement* (the experiment is the paper's first figure), not a presumed
anti-scaling fix. Pole-A and pole-B are **mutually exclusive in spirit** (pole-A deletes the minor STW; pole-B
keeps + strengthens it) though the non-GC rendezvous re-homing (runtime_events/frametables) is shared plumbing.

---

## Retire OCaml's STW rendezvous — verified caller-by-caller plan (RQ10 pole-A / #18 / #20) (2026-06-25)

Companion to the cross-runtime study below. Question: *can we delete `caml_try_run_on_all_domains` +
`caml_empty_minor_heaps_once` and make MMTk's `stop_all_mutators` the sole all-domains rendezvous, and
is there a good reason to keep them?* A multi-agent workflow mapped **every** caller, analysed each, and
**adversarially refuted** each removability claim (2 skeptics/claim). Verdict: **`partial` — yes in
principle, but only as ONE coordinated change that re-homes EIGHT callers; not a grep-and-delete.**

**Why not symmetric.** `caml_empty_minor_heaps_once` (`minor_gc.c:482`) is **GC-dead** under MMTk
(`caml_empty_minor_heap_promote`, `minor_gc.c:203`, promotes nothing — just resets the young region), so
it's retirable *once its per-domain bookkeeping is re-homed*. But `caml_try_run_on_all_domains`
(`domain.c:1780`) is a **generic all-domains barrier with SIX further live, non-GC users**, so it can only
go after each is re-homed. The two are **mutually load-bearing**: the terminate flush loop
(`domain.c:2128-2201`) must positively answer every *other* domain's in-flight rendezvous before it leaves
`stw_domains` (participant-set contract, `domain.c:1582-1614`) — so while **any** `caml_try_run_on_all_domains`
user survives, `caml_empty_minor_heaps_once` cannot be deleted in isolation without re-deadlocking.

**Per-caller verdict (adversarially verified; refuters/2):**
| caller | site | removable | refuters | note |
|---|---|---|---|---|
| minor-heap-resize | `domain.c:511-541` (`Gc.set`) | **yes** | 0/2 | cap → one relaxed atomic; no STW, no MMTk |
| global-major-slice | `domain.c:1913-1922` | **yes** | 0/2 | dead in native; bytecode → set LOCAL `requested_major_slice` |
| minor-empty rendezvous | `minor_gc.c:482` | conditional | 2/2 | retirable only with the whole family (Phase 3) |
| domain spawn/terminate | `domain.c:1293/2113` | conditional | 2/2 | participant-set contract; the bug#3c seam |
| runtime_events ring | `runtime_events.c:438/228` | conditional | 1/2 | needs a generic "stop RUNNING + run VM closure" hook — MMTk STW has none |
| frametables install | `frame_descriptors.c:306/318` | conditional | 2/2 | sole caller = native dynlink; engineering economy only |

**Good reasons to keep (all *conditional* — they block isolated deletion, not the coordinated retirement):**
- **TERMINATE participant-set obligation** (`domain.c:2128-2201`). Real deadlock-avoidance, but evaporates
  once the LAST `caml_try_run_on_all_domains` user is re-homed.
- **runtime_events + frametables** need a "stop all RUNNING domains and run a VM closure" primitive that
  MMTk's `stop_all_mutators` does **not** expose (its callback is hard-wired to GC marking on the worker
  pool, `collection.rs:272-274`). They are *legitimate non-GC uses* of OCaml's rendezvous — but if (1) is
  retired wholesale they must instead get a per-subsystem scheme (rwlock/epoch; the exclusion they need is
  exactly `RUNNING`) or a new MMTk VM-work hook. This is the key finding the two refuters surfaced: **MMTk
  GC-STW is not a drop-in for the non-GC callers.**
- Minimal-diff fidelity to stock 5.5.0 (diff-cosmetics, weak).

**Phased removal (each phase builds + is behaviour-preserving):**
- **Phase 0 (low):** delete the two refuters-0/2 callers. minor-heap-resize → `caml_minor_heap_max_wsz`
  becomes a relaxed atomic (or drop the cap; under MMTk it sizes no arena). global-major-slice →
  `caml_request_major_slice(1)` sets the LOCAL flag (already consumed at `domain.c:1977-1984`).
- **Phase 1 (med):** move the domain-LOCAL bookkeeping the minor-STW carries (minor-table clear, finalisers
  `caml_final_empty_young`/`_update_last_minor`, `caml_memprof_after_minor_gc`, gc-stats sample, the
  bytecode `caml_minor_collections_count` odometer) onto the triggering domain's safepoint / the
  `caml_mmtk_uninterrupt` resume path (`mmtk.c:807`). Drop the dead `caml_mark_roots_stw` branch + the
  minor-cycle barrier. Both functions stay live.
- **Phase 2 (med):** re-home frametables + runtime_events off `caml_try_run_on_all_domains` — either a new
  binding "request MMTk STW to run this VM closure" (drained inside `stop_all_mutators` after
  `running.is_empty()`, before `mutator_visitor`) or a per-subsystem rwlock/epoch. *ConcurrentImmix caveat:*
  GC workers read `descriptors[]` concurrently (`scanning.rs:322`), so the frametable swap needs RCU/epoch
  retire — but that hazard **already exists today** (OCaml STW stops mutators, not MMTk workers); surfaced,
  not introduced.
- **Phase 3 (high, the payoff):** with every non-GC user re-homed, delete the AUTO minor-empty call
  (`domain.c:1974`, bytecode-only — native already early-returns), the `Gc.minor()` promotion semantics, and
  the TERMINATE call (`domain.c:2131`); terminate keeps only the bare participant-set departure fenced by
  the **existing** MMTk deregister fence (`caml_mmtk_domain_terminate`: `mmtk_ocaml_deregister_domain` +
  `mmtk_ocaml_wait_collection_done`, `mmtk.c:901-923`), retaining the `!caml_incoming_interrupts_queued()`
  drain *during* the transition. Then delete `caml_empty_minor_heaps_once` /
  `caml_try_empty_minor_heap_on_all_domains` / `caml_empty_minor_heap_promote`, then
  `caml_try_run_on_all_domains[_async/_with_spin_work]`, `stw_leader`/`stw_request`, the
  `all_domains_lock`-as-STW-barrier, the interruptor/**backup-thread** machinery (#20), and the
  `domain_create` `stw_leader` spin-wait. Keep the parent/child `p.status` 2-party handshake (bug#3b) and
  `register_mutator`/`RUNNING` ordering. **Validate:** build + `sanity` (small heap) + the bug#3c
  deterministic repro (par_binarytrees 20, d8 pinned, `MMTK_THREADS=8`) at 0% hang + chameneos + full
  testsuite on every STW plan.

**Deadlock class this eliminates (and what it does NOT).** Phase 3 **structurally** kills the
bug#3c-residual / dual-STW deadlock class: the STOPPED→RUNNING re-entry edge
(`caml_mmtk_become_running` → `mmtk_ocaml_try_mark_running`, gated only on `!gc_active`) currently lets a
terminating domain L re-mark RUNNING and lead a *new* OCaml all-domains STW while RUNNING, forming the
4-way cycle (`stop_all_mutators` waits `running.is_empty()`→L; L holds `all_domains_lock` waits peers;
peers parked for `gc_active==false`; `gc_active` clears only when running empties→needs L). Delete the
OCaml STW and there is **no second barrier for L to lead → the cycle cannot form.** Two caveats: (1) the
*specific* GH#6 d8 instance was rr-refuted as the `scheduler.rs:444` assert (already fixed), **not** the
dual-STW — so this doesn't "fix #6," it removes a sibling class; (2) **chameneos under ConcurrentImmix has
a SEPARATE still-open deadlock** (continuation-resume scan × concurrent marking, no scheduler panic) that
is **not** the dual-STW and **survives** retiring (1).

**Biggest risk:** the terminate participant-set desync window during Phases 2–3 — if the deletions are done
out of order (dropping the minor-empty-join or the incoming-interrupt drain from terminate *before* the last
`caml_try_run_on_all_domains` caller is re-homed), a terminating domain strands a peer's in-flight barrier
forever (the exact sibling of bug#3c). Mitigation: retire the whole family in ONE coordinated Phase-3
change *after* 0–2; keep the interrupt drain during the transition; lean on the proven MMTk deregister
fence; gate the merge on the bug#3c repro + chameneos + full testsuite per plan.

**Gap closed (the 2 dropped workflow agents, re-run 2026-06-25) — verdict UNCHANGED, no blocker, +1 new item:**
- **Process-exit shutdown.** A hypothesized `caml_shutdown`↔MMTk-`harness_end` race **cannot occur: there is no
  MMTk teardown at exit at all** (only `mmtk_ocaml_init`; the heap/worker pool are abandoned at `exit()`; the
  sole atexit hook is the `MMTK_VERBOSE` stats print). The last-domain-alone branch
  (`caml_domain_terminate(true)`, `startup_aux.c:236`) is **already STW-free** (it `break`s at `domain.c:2149`
  before touching the participant set) and is covered by the existing MMTk deregister fence
  (`caml_mmtk_domain_terminate`, `mmtk.c:901-923`).
- **+1 NEW Phase-3 item the synthesis missed:** the **multi-domain** exit path
  `caml_stop_all_domains`→`stw_terminate_domain` (`domain.c:2275-2309`) is a **distinct** `caml_try_run_on_all_domains`
  caller (kill-stragglers) whose cancelled peers **never deregister from MMTk** — harmless *today* only because
  OCaml's STW stops them and the process exits with no further collection. When Phase 3 makes `stop_all_mutators`
  the sole rendezvous, the re-homed "MMTk-stop-then-cancel" **MUST `remove_running` + deregister each cancelled
  peer**, or the next `stop_all_mutators`/`mmtk_ocaml_wait_collection_done` waits forever on a thread that no
  longer exists (`collection.rs:256` `while !running.is_empty()`). New gating test (absent from the suite today):
  an **unjoined-domains-at-exit** repro — spawn N busy-alloc domains, `exit` from domain 0 without joining, under
  each STW plan, assert the process exits (TIMEOUT-bounded).
- **Driver mechanics, per sub-part:** `stw_leader`/`stw_request`/`stw_requests_suspended` + **both** barriers
  (`domains_still_running` entry barrier, `Caml_global_barrier`) + `decrement_stw_domains_still_processing` →
  **delete** (coordinated Phase 3). `all_domains_lock` + `stw_domains` membership → **keep as a plain
  spawn/terminate mutex** (it guards the participant-registry transition, NOT an all-domains barrier; MMTk
  enumerates domains itself via `active_plan::domain_addrs`). The **`young_limit`-poison** (`caml_mmtk_interrupt`,
  `mmtk.c:789`) + `caml_reset_young_limit` → **KEEP** — MMTk's `stop_all_mutators` reuses exactly it to trap
  domains to a safepoint (`collection.rs:244,258`). Backup-thread **STW-answering** role → delete (#20); the
  backup thread survives only to answer OCaml's own rendezvous.

---

## Cross-runtime STW-rendezvous study: ONE GC-owned rendezvous is universal; OCaml-MMTk's dual-STW is the outlier (2026-06-25)

Motivated by the dual-STW deadlock class (#5/#6/bug#3c) and the plan to retire OCaml's own rendezvous
(`caml_try_run_on_all_domains` / `caml_empty_minor_heaps_once`). Comparative research (agent, sourced):

**Every comparable production runtime has exactly ONE GC-owned safepoint rendezvous** — and all three
sibling MMTk bindings **reuse the VM's existing safepoint** rather than running a second stop:

| Runtime | stop mechanism | rendezvous | native/blocking thread during STW |
|---|---|---|---|
| HotSpot / **mmtk-openjdk** | global safepoint via `VM_Operation` (poll page / handshakes) | **ONE** — GC is a `VM_Operation`; mmtk reuses it (`VM_MMTkSTWOperation`) | `_thread_in_native`/`_blocked` already safe, **not woken**; trapped on return (`_thread_in_native_trans`) |
| **mmtk-julia** | Julia's `mprotect` safepoint page (reused) | **ONE** (`stop_all_mutators` waits on Julia's `WORLD_HAS_STOPPED`) | `JL_GC_STATE_SAFE` not awaited; **must pin FFI objects** (moving-GC risk) |
| **mmtk-ruby** | CRuby GIL / Ractor-pause (reused) | **ONE** | released-GVL threads aren't running Ruby; native roots = conservative scan + pinning |
| GHC | `requestSync`/`pending_sync` + heap-check yield | **ONE** (GC = a sync *type*) | safe FFI **releases its capability**; GC doesn't wait; re-acquire on return |
| Go | `stopTheWorld` + `preemptall` (prologue + SIGURG) | **ONE** mechanism (shared w/ scheduler) | `_Gsyscall` not awaited; `exitsyscall` checks `gcwaiting`, parks if stopped |
| CoreCLR | `SuspendEE` + hijack/redirect | **ONE** (`g_TrapReturningThreads`) | preemptive-mode not awaited; blocks at `DisablePreemptiveGC` on return |
| BEAM | per-process GC | **NONE** (contrast — private heaps; n/a to OCaml's shared cross-domain minor refs) |
| **OCaml-MMTk (now)** | OCaml `caml_try_run_on_all_domains` + **backup thread** AND MMTk `stop_all_mutators` + RUNNING set | **TWO** (the deadlock source) | **backup thread per domain** spins to reach OCaml's barrier — the outlier to delete |

**Lessons (evidence-backed, converge with RQ10 pole-A / #18 / #20):**
1. **Make MMTk's `stop_all_mutators` the SOLE rendezvous.** The MMTk `Collection` trait delegates thread
   synchronization to the VM; the binding should drive OCaml's `young_limit`-interrupt safepoint directly
   and enumerate domains as `Mutator`s — one rendezvous, the MMTk one.
2. **Replace the backup thread with a thread-state FLAG.** Universal pattern: a blocked/native thread is
   marked "already safe" (HotSpot `_thread_in_native`, Julia `JL_GC_STATE_SAFE`, Go `_Gsyscall`, GHC
   released-capability, CoreCLR preemptive), the GC scans/skips it WITHOUT waiting, and it re-checks a flag
   on the way back. **The binding's `RUNNING` set is exactly this flag** (a blocked domain is absent →
   not awaited; `mmtk_ocaml_try_mark_running` is the return-edge re-check). Delete the backup thread (#20).
3. **Must-haves at the FFI boundary** (the loudest shared lesson of the Julia + CRuby ISMM'25 reports):
   **pin objects reachable across FFI because MMTk is moving** (GenImmix) — the plan must be pinning-capable
   (Immix/StickyImmix/GenImmix yes); conservative C-stack scanning or precisely-pinned roots for a
   domain blocked in C; the re-entry fence (already `mmtk_ocaml_try_mark_running` + the terminate
   `mmtk_ocaml_wait_collection_done`). Julia's conservative fallback: postpone a *moving* collection while
   any thread is in a GC-safe region.

The GHC "release-the-capability + GC-initiator acquires all capabilities on the way in" model is the direct
analogue of RQ10 pole-A's "the GC initiator acquires the released domain's `domain_lock`." Sources:
mmtk-openjdk (`mmtkUpcalls.cpp`, `mmtkVMCompanionThread.cpp`); ISMM'25 Julia (10.1145/3735950.3735957) +
CRuby (10.1145/3735950.3735960); Marlow GHC ISMM'08; Go `runtime/proc.go`; CoreCLR BOTR; ICFP'20 §4.1.
→ ROADMAP #18/#20, RQ10 pole-A. (Full agent report in this session's transcript.)

---

## #5 ConcurrentImmix deadlock ROOT-CAUSED (lost-wakeup at the pause boundary); LXR P2 plan; testsuite census (2026-06-25)

**Core-dump debugging is the method for the rr-resistant timing deadlocks.** `rr` *masks* both #5 and the
#6 residual (its serialization avoids the race; `--chaos` aborts the bench). Instead: run on real cores,
`kill -ABRT` the hung pid → `gdb <exe> <core>` (fully offline, **no rr, no ptrace** — works under
`ptrace_scope=1`). This captured a 37-thread chameneos deadlock cleanly. Use it for #5 and #6.

**#5 chameneos ConcurrentImmix deadlock — ROOT CAUSE (supersedes the earlier cont_lock + "active-mutator
livelock" hypotheses, both REFUTED by the core).** It is a **lost-wakeup at the Concurrent→FinalMark
work-bucket boundary**, NOT cont_lock (no thread is in cont_lock). Evidence: 27 GC workers parked in
`worker_monitor::park_and_wait` (no work), mutator domains parked in `park_until_resumed` waiting
`gc_active==false`, no thread in `stop_all_mutators` → a collection set `gc_active=true` but
`resume_mutators` never ran. Mechanism (agent, cited): `gc_active` is **per-pause** (set in the binding's
`stop_all_mutators`, cleared in `resume_mutators` called from `on_gc_finished`, `scheduler.rs:666`). The
SATB barrier lets mutators `add()` `ProcessModBufSATB` into `work_buckets[Concurrent]` **lock-free**, but
`notify_one_worker` (`work_bucket.rs:137`) **suppresses the wakeup when the bucket is momentarily
`!enabled`/`!open`** during the Concurrent→FinalMark transition. An orphaned packet then sits in the
Concurrent `Injector` with no worker notified; `concurrent_marking_drained` returns **true via the
`!is_enabled()` short-circuit** (`work_bucket.rs:172`) while the queue is *physically non-empty* — so the
GH#4 FinalMark self-trigger doesn't re-fire, all workers park, `gc_active` stuck. **Fix direction:** make
the Concurrent→FinalMark handoff atomic w.r.t. SATB feeds — clear `concurrent_work_in_progress` *before*
the final barrier flush in `notify_mutators_paused` (`concurrent/immix/global.rs:281-284` flushes while the
flag is still true → targets the about-to-be-disabled bucket), or keep the Concurrent bucket enabled until
FinalMark `Release` has drained it; plus defense-in-depth (broaden the self-trigger to a non-empty
Concurrent bucket; bound `park_until_resumed`'s wait). **Confirm from the core:** `WorkerGoals.current ==
None`, `requests[Gc] == false`, `ConcurrentImmix.concurrent_marking_active == false` with `previous_pause
== FinalMark` yet `gc_active == true`, and the Concurrent bucket `enabled==false` with a non-empty Injector.

**LXR P2 implementation plan (agent, file-cited).** 17 concrete `policy/immix/immixspace.rs` sites + LOS
RC (`largeobjectspace.rs`), gated on `rc_enabled` so existing plans stay byte-identical. The high-risk
core is **P2.3 — RC-travels-with-copy** at `trace_object_with_opportunistic_copy` (immixspace.rs:652-748):
on evacuation, `rc.set(new, rc.count(old))` + straddle re-mark **before** clearing old, else dangling/leak.
Sweep = reclaim a block/line when all `RC_TABLE` entries are 0 (`Block::rc_dead()`). **Side-metadata budget
has room** — `RC_TABLE` (core-global) is disjoint from `GLOBAL_LOG_BIT` (VM-global region); P1 declared the
specs but P2 must register them in each space's `side_metadata_specs()`. Phased P2.0–P2.5 (each
`sanity`-checkable; real-workload validation needs P3's barrier to issue Incs/Decs). Full plan saved.

**Testsuite census (2026-06-25, church/godel, with `ocamltest` built + GNU parallel):** GenImmix bytecode
= **1401 passed / 92 failed / 54 skipped / 1547 considered** (matches the ~97 ROADMAP estimate — the #19
triage worklist). Native variants need `make ocamltest.opt` (the `codegen` tool) or ~50 spuriously fail
(godel saw `catch-try.cmm` exit 127). aligned_alloc/alloc_async are the known #12c gaps.

---

## Three results: LXR P1 OCaml-build-validated; RQ10 pole-B design; #20 backup-thread design (2026-06-25)

**LXR P1 — OCaml-build-validated.** The binding workspace builds **green** against
`origin/0.32-ocaml-lxr` @ `f0319fb5e6` ("P1 — vendor + adapt RC scaffolding, additive, gated"); no
errors (P1 is purely additive — args.rs/rc.rs/FieldBarrier/Pause::RefCount — so the binding's API
surface is unaffected). Resolves the ROADMAP's "NOT yet OCaml-build-validated". Next: P2 (Immix-policy +
LOS RC hooks). Done as a temporary submodule checkout + restore to `ec2f5079f8`.

**RQ10 pole-B (stock minor + MMTk major-only) — key API finding (design agent).** MMTk's *mutator*
allocator API does **not** admit promoting into a *generational* plan's mature space: in GenImmix the
mature Immix space is reachable **only from the GC worker** via `alloc_copy(.., PromoteToMature)`
(`gc/mmtk-core/src/util/copy/mod.rs:75`), and the mutator mapping exposes the nursery only
(`plan/generational/mod.rs:76`). **So pole-B's baseline must use a NON-generational `Immix`/
`ConcurrentImmix` major**, whose mutator Default *is* mature → promotion = `mmtk_ocaml_alloc` (registered
via `post_alloc`), and **needs ZERO mmtk-core trait changes**. A generational-major-under-stock-minor
would need an mmtk-core fork (a mutator-facing mature allocator) — the inverse of RQ7 Bactrian.
Re-introduce: per-domain arena + real `Is_young` + `oldify`→`mmtk_ocaml_alloc` + old→young ref-table
barrier; coordinate by draining all minors inside MMTk's `stop_all_mutators` before the major mark
(stock-faithful). Phased P1–P6; **P3 (oldify→mature) is the moving-GC correctness crux** (validate with
`sanity` at a tiny Immix heap). Mostly runtime-C; ~nil binding; no mmtk-core for the Immix baseline.

**#20 backup-thread retirement — feasibility verdict (design agent): it is DOWNSTREAM of #18/RQ10
pole-A, not standalone.** The backup thread is **not** kept alive by MMTk's STW at all — the binding's
RUNNING set fully subsumes that (a lock-released domain is already STOPPED, so `stop_all_mutators` never
awaits it; the GC *worker pool* drives the pause). It survives **only** to answer OCaml's *own*
`caml_try_run_on_all_domains` rendezvous for a lock-released domain (spawn/terminate/minor-empty/
minor-heap-resize/runtime_events/frame_descriptors — full caller table in the agent report). So deleting
it requires first making MMTk's STW the **sole** rendezvous (#18 "big deletion"). The lock-acquisition
reframe (the GC initiator acquires a released domain's `domain_lock` instead of its backup thread
answering) is sound but has two deadlock hazards: (i) `all_domains_lock`→`domain_lock` order inversion vs
spawn/terminate (which take them in the opposite order), and (ii) **holding `domain_lock` across an MMTk
collection re-creates the exact GH#6 STOPPED↔RUNNING re-entry cycle**. Publishable claim: *under an
always-on tracing GC with its own worker pool, the mutator-rendezvous STW (backup threads + dual
barriers) is entirely eliminable* — which is the RQ10 pole-A question. → ROADMAP #20/#18; RQ10.

---

## GH#6 multidomain d8 deadlock — rr-CONFIRMED: it was the GH#14 scheduler assert all along (FIXED, all plans) (2026-06-25)

**This supersedes finding (1) of the entry below, which was WRONG.** The static-analysis agent's "bug#3c
residual on the RUNNING re-entry edge" / "4-way `stop_all_mutators` cycle" hypothesis was *falsified by
instrumentation, then rr*. Two checks:

1. **Instrument refuted the agent.** A debug print added to `stop_all_mutators`'s `while !running.is_empty()`
   loop (eprintln after 3000 spins) fired **0 times** across 4 deterministic hangs — so the deadlock is
   **not** that barrier. (All ~20 threads were in `futex_wait`, so it was a real deadlock, just not there.)
2. **rr nailed it.** `ptrace_scope=1` blocks `gdb -p`, but `rr` ptraces its own child, so it works — and
   despite rr's thread serialization the deadlock still reproduced (it is structural, not a narrow race).
   The replay backtrace had two panics at the top:
   `scheduler.rs:444 "GC request sent to WorkerMonitor while GC is still in progress."` (the GH#14 assert)
   and `worker_monitor.rs:221` (the poisoned-mutex cascade). The GC workers PANICKED; the domains
   (Thread 15 = a domain being *created* in `domain_create`→`caml_mmtk_domain_init`→`refill_tlab`→
   `Space::acquire`; Threads 13/11/9 = running domains at the alloc poll) are all downstream victims parked
   in `park_until_resumed` waiting for a `gc_active` that never clears (no worker left alive to run
   `resume_mutators`).

**Root cause:** the assert's premise ("in STW GC, mutators cannot request a GC while a GC is in progress")
is **false for OCaml's multi-domain model regardless of plan.** At ≥8 domains, a *second* domain's
allocation poll — or a domain being **created** refilling its initial TLAB — requests the next GC while one
is in progress. The request is harmless (coalesced in `goals.requests[Gc]`, an idempotent bit, serviced by
the next `respond_to_requests` after the current GC). **Fix: remove the assert for ALL plans** (mmtk-core
`ec2f5079f8`). The earlier GH#14 commit `d2e7f3493b` only gated it off for *concurrent* plans, so STW
plans (GenImmix/Immix/StickyImmix) still tripped it at d8 — which is exactly why the "controlled RQ10 d8
sweep" hung across *every* plan. **Validated (turing):** `par_binarytrees` d8 pinned, `MMTK_THREADS=8`:
100% HANG → **5/5 OK**, checksums byte-identical to golden.

**Methodology lesson (again):** the read-only static agent produced a detailed, internally-consistent, and
WRONG root cause + 3 fix options that all targeted the wrong cycle. Instrument-then-rr falsified it in two
cheap steps. *Run before believing* — and instrument the spot the theory names before implementing its fix.
The "RUNNING re-entry edge" fix would have been wasted effort.

---

## Three findings: bug#3c residual root-caused (deterministic d8 repro), chameneos ConcImmix deadlock root-caused, KB workload favours Immix (2026-06-25)

### (1) bug#3c residual — DETERMINISTIC repro + root cause: the RUNNING re-entry edge

The controlled RQ10 re-run (pinned `2·domains` cores, `MMTK_THREADS=domains`) turned the ~1% multidomain
STW deadlock into a **deterministic** one. **Repro (turing):** `par_binarytrees 20` at **8 domains**,
`taskset -c 0-15`, `MMTK_THREADS=8` → hangs every time under **GenImmix** *and* Immix/StickyImmix/
ConcurrentImmix (NOT plan-specific, NOT GH#14). At the hang **all 19 threads are in `futex_wait_queue`**
(total deadlock). `ptrace_scope=1` on turing blocks `gdb -p` attach — use `rr` (it ptraces its own child)
or `/proc/<pid>/task/*/wchan`.

**Root cause (static analysis, agent):** the `7d66a6172f` bug#3c fix brackets `caml_empty_minor_heaps_once`
with `caml_mmtk_enter_blocking`(STOPPED)…`caml_mmtk_become_running`(RUNNING), making the **STOPPED** edge
atomic — but the **RUNNING re-entry** edge (`minor_gc.c:528` → `caml_mmtk_become_running` →
`mmtk_ocaml_try_mark_running`, `collection.rs:197`) is racy. `try_mark_running` re-inserts the domain into
MMTk's `running` set gated **only** on `!gc_active`. The losing interleaving: a domain L re-marks RUNNING,
then enters a *new* all-domains OCaml STW (the terminate path `domain.c:2128-2201` holds `all_domains_lock`
and calls `caml_empty_minor_heaps_once` repeatedly) **while RUNNING** — recreating exactly the bug#3c
cross-STW capture topology on the re-entry edge. The 4-way cycle: MMTk `stop_all_mutators` waits on
`running.is_empty()` → waits for **L**; **L** holds `all_domains_lock` / leads the OCaml STW, waits for the
other domains; those domains were poisoned by `stop_all_mutators` and parked in `park_until_resumed`
(`collection.rs:152`, **unbounded** wait) for `gc_active==false`; `gc_active` clears only when the GC
finishes, which needs `running` empty → needs **L**. Closed.

**Why pinning + 8 domains makes it deterministic:** ~25 runnable threads (8 domains + 8 GC workers + 8
backup + main) on 16 pinned cores → CFS preempts L in the few-instruction window between `try_mark_running`
returning and L's next safepoint with near-certainty; the system is STW-saturated so it is *continuously* in
that transition; high domain count means multiple concurrent terminate-STWs always supply the "L leads a
second STW while RUNNING" condition. Unpinned on 28 cores L usually races through → the ~1%.

**Fix direction (not yet landed):** (a) **primary/structural** — keep the domain STOPPED across the *whole*
terminate critical section (`caml_domain_terminate`, `domain.c:2113`: `enter_blocking` at entry,
`become_running` only at the very end / rely on the deregister fence), not just per `caml_empty_minor_heaps_once`,
so a domain is never RUNNING while it can hold `all_domains_lock`. (b) **surgical** — make
`mmtk_ocaml_try_mark_running` also refuse while an OCaml STW is in flight (`stw_leader != 0`, exposed via a
small C predicate); `become_running` already parks-and-retries on refusal. (c) **defense-in-depth** —
convert `park_until_resumed`'s unbounded wait to a bounded `wait_timeout` re-validating against MMTk's
authoritative GC state, so an orphaned/raced flag self-heals (deadlock → recoverable stall). **The
deterministic repro means any candidate fix is empirically testable** (does d8-pinned stop hanging +
sanity/checksums clean). Confirm the exact cycle with `rr` first (consult `running` set + `stw_leader` at the
hang) per "confirmed beats guessed". → ROADMAP item 1 / #6; RESEARCH_QUESTIONS RQ10.

### (2) chameneos_redux ConcurrentImmix deadlock — separate from GH#14, root-caused

After the GH#14 assert fix, `chameneos_redux` STILL hangs under ConcurrentImmix (d=1, d=4, 64 MB) with **no
assert panic** — a distinct deadlock. **Root cause (agent):** the continuation-resume critical section
`caml_continuation_use_noexc` (`fiber.c:638` `CAMLnoalloc` … `:660` `caml_mmtk_cont_lock` … `:666`
`caml_mmtk_cont_snapshot` — a full fiber-stack SATB walk) is a **no-safepoint window that holds the GC
RUNNING set**. Under concurrent marking a GC worker can win `cont_lock(C)` via `try_lock` (`scanning.rs:337`)
then be descheduled by an InitialMark/FinalMark pause; the resuming mutator spins forever in
`cont_lock::lock` (`cont_lock.rs:57-67`, a `yield_now` spin with **no condvar / no liveness guarantee**),
never reaching a safepoint → `stop_all_mutators` (`collection.rs:256`) never drains RUNNING → the worker
holding `cont_lock` is never scheduled to `unlock`. Circular wait: mutator waits `cont_lock`(worker) ↔ GC
pause waits RUNNING(mutator). This is the **same cont_lock Q3 (`55ab6ce40b`) introduced** to fix a
*data race* under STW marking — concurrent marking re-exposes it as a *liveness* bug (mutators now resume
*while* a worker holds the lock). **Fix direction:** make the resume critical section either safepoint-pollable
or STOPPED-visible while it waits on `cont_lock` (e.g. `enter_blocking` before the blocking spin), or use a
bounded `try_lock`-with-park. **Needs `rr` on Linux to confirm** (is a thread parked in `cont_lock::lock`
spin while the holding worker is parked in the scheduler with `current_pause()==Some(FinalMark)`?). → #5.

### (3) KB (Knuth-Bendix) — the symbolic/Rocq-like workload favours Immix over GenImmix

New panel bench (testsuite `misc-kb`, term-rewriting completion; the panel's first non-numeric bench).
Single-domain, size 50, dynamic heap, turing, checksum `608698882` identical across all plans. Wall vs
vanilla **2.13 s**: **Immix 2.41 s (1.13×)**, GenImmix **2.88 s (1.35×)**, StickyImmix **3.17 s (1.49×)**.
**Immix beats the GenImmix default here** — the opposite of the numeric benches. KB's torrent of short-lived
intermediate terms + a growing (long-lived) rule set makes the copying-nursery per-minor-GC cost a net loss
vs in-place Immix. A clean RQ2 characterization datapoint: plan-fit is workload-dependent, and a symbolic
prover-like profile is NOT automatically a generational win. (MarkSweep/MarkCompact are bytecode-only;
ConcurrentImmix wall pending.)

---

## GH#14 FIXED: gate the STW-only scheduler assert for concurrent plans; #4 self-trigger exonerated (2026-06-25)

**Resolves the panic in the entry below** (mmtk-core `d2e7f3493b` on `0.32-ocaml`, pushed; submodule
bumped in the fork). The assert at `scheduler.rs:444` (`on_last_parked`, `WorkerGoal::Gc` arm) forbade a
pending `Gc` request while a GC is in progress — its own comment said to remove it "when we support
concurrent GC". **Fix:** gate it to non-concurrent plans:

```rust
if worker.mmtk.get_plan().concurrent().is_none() {
    assert!(!goals.debug_is_requested(WorkerGoal::Gc), "GC request sent ... in progress.");
}
```

**Why it's safe (coalescing, not dropping):** `WorkerGoals` keeps `current: Option` and a per-goal
`requests` bitset *independently*. A `Gc` request raised while `current==Some(Gc)` just sets
`requests[Gc]=true`; nothing clears it until `poll_next_goal`, reachable only from `respond_to_requests`
(guarded by `assert!(current.is_none())` at `:512`). So the request survives `on_current_goal_completed`
(which only nulls `current`) and is serviced by the next `respond_to_requests` after the in-progress GC
finishes — exactly once, NOT a forced FinalMark. STW plans are byte-identical (predicate false for them).

**`assert!` is a real release assert** (not `debug_assert!`; `debug_is_requested` is an ordinary `pub
fn`, not `#[cfg(debug_assertions)]`) — so the panic fired in release, matching the report.

**Validated (macOS, native).** Deterministic small-heap repro on spectralnorm / LU_decomposition /
par_spectralnorm × heaps 32/64/128: **before 15/15 HANG → after 15/15 OK**, 0 panic; checksums
byte-identical to golden under both ConcurrentImmix and GenImmix. Build gotcha worth recording: the
native bench links `libasmrun.a`, which **bundles** the mmtk objects (`Makefile.mmtk`
`MMTK_OBJS`/`MMTK_BUNDLE`); `make runtime` rebuilds only the *bytecode* archive — you must `make
runtimeopt` (rebuilds `libasmrun.a`) **and** `cargo clean -p mmtk` once (a stale fingerprint made the
first `cargo build` skip recompiling the edited submodule), or the bench silently links the old
scheduler (kept panicking at the *old* line 444).

**`#4` FinalMark self-trigger EXONERATED — by static trace, no rr needed.** The open question from the
entry below ("does `72ee627050` contribute?") is settled: the self-trigger lives in
`respond_to_requests`, which runs **only when `current()==None`** (asserted at `:512`) — both call sites
(`on_last_parked:435` no-goal branch; `:474` after `on_current_goal_completed`) require it. So it can
never set `requests[Gc]` while `current==Some(Gc)` and **cannot reach the `:444` assert**. The real
trigger is the **mutator allocation-poll** path: the request flag is re-armed at InitialMark
(`notify_mutators_paused`), so once mutators resume into the concurrent-marking window a normal
allocation poll re-requests a GC, setting `requests[Gc]` while a GC is still current.

**STILL-OPEN remnant — `chameneos_redux` hangs under ConcurrentImmix with NO assert panic.** The
effects/continuation workload still deadlocks after this fix (d=1 and d=4, small heap) and its stderr
shows **no** `scheduler.rs` panic — a *separate* deadlock (continuation scan/resume × concurrent
marking), not GH#14's assert. Keep GH#14 open for it; diagnose separately. (The float kernels that
*did* panic — spectralnorm, LU, par_spectralnorm — are fixed.)

---

## CORRECTION: ConcurrentImmix "hang" is a scheduler PANIC, not a livelock — empirical run beats static analysis (2026-06-25)

**Supersedes the static "marker-vs-mutator livelock" hypothesis below.** When the rebuilt quick panel
actually *ran* ConcurrentImmix (vs the earlier static read of the code), the failure is a **panic, not a
livelock**: on `spectralnorm`, `LU_decomposition`, `par_spectralnorm`, and `chameneos_redux` it aborts with

```
GC request sent to WorkerMonitor while GC is still in progress.   (scheduler.rs:444, in on_last_parked)
```

then the poisoned `WorkerMonitor` mutex kills every GC worker → deadlock. The assert is **stop-the-world-era**
and its own comment says so: *"In stop-the-world GC, mutators cannot request for GC while GC is in progress.
When we support concurrent GC, we should remove this assertion."* Under a **concurrent** plan a GC is
legitimately (re-)requested while one is in progress, so the assertion fires. **This is the real root cause;
the "FinalMark is never requested / add a heap-pressure forced trigger" fix I wrote earlier is WRONG** — the
problem is the opposite (a GC request arrives *while a GC is current* and the STW-only assert rejects it), and
a forced-FinalMark trigger would add *more* requests and make it worse.

**Likely fix:** relax/remove that assertion for concurrent plans (coalesce a redundant GC request instead of
asserting) — exactly the code's own TODO. **Open question with a correctness angle:** does our GH#4 FinalMark
self-trigger (`scheduler.rs::respond_to_requests` self-requests `WorkerGoal::Gc`, mmtk-core `72ee627050`)
*contribute* to reaching this assert on the float kernels? `respond_to_requests` only runs when
`current().is_none()`, so it can't directly fire the assert, but it adds a worker-side GC request into the
cycle; #4 fixed the small-heap quiescent deadlock but may leave (or expose) this concurrent-scheduling panic.
Needs a live rr capture on Linux to settle whether #4 is necessary to trigger it. Either way the assert is the
root incompatibility. **Methodology lesson:** the read-only static-analysis agent produced a plausible,
internally-consistent, and *wrong* diagnosis; one real run falsified it. Run before believing. (GH#14 updated;
README perf panel marks ConcurrentImmix **deadlock**, not hang.)

---

## Two findings: ConcurrentImmix hangs on spectralnorm/LU (normal heap); LXR is NOT a clean merge (2026-06-25)

**ConcurrentImmix hang (new, needs investigation).** The fixed quickbench harness (now reaching real
perf sizes, with a `--timeout`) surfaced that **ConcurrentImmix hangs on `spectralnorm` and
`LU_decomposition`** — native, single-domain, **dynamic (normal) heap**, >12s vs ~0.8s for the other
plans. This is **distinct from the GH#4 small-heap sanity deadlock** (fixed by the FinalMark
self-trigger, mmtk-core `72ee627050`, which is in this build): that was ~10 MB + effect/continuation
churn; this is a normal-heap, compute-bound float kernel. Both are boxed-float, high-minor-allocation
benches.

**Root cause (confirmed by static analysis, 2026-06-25).** It is a **marker-vs-mutator livelock**,
distinct from GH#4 (not merely a gap in its fix). Both FinalMark triggers — the worker-side `#4`
self-trigger (`scheduler.rs::concurrent_marking_drained`) and the mutator poll-site
(`ConcurrentImmix::collection_required`, `plan/concurrent/immix/global.rs:80-87`) — gate FinalMark on
`work_buckets[Concurrent].is_drained()`. Under a hot boxed-float kernel the SATB barrier
(`flush_satb` → `WorkBucket::add` → `notify_one_worker`, `plan/concurrent/barrier.rs:69-85`) refills
the `Concurrent` bucket from the mutator on essentially every `caml_modify`, faster than the single
default GC worker (`MMTK_THREADS=1`) drains it. So the bucket is **never empty at the instant all
workers are parked**, `is_drained()` never holds at the decision point, **FinalMark is never
requested**, concurrent marking never finishes, the heap/nursery fills, and the mutator blocks
forever in `block_for_gc` → `park_until_resumed`. GH#4 fixed the *quiescent-mutator* case (mutator
idle, marking drained, nobody requested FinalMark); this is the *active-mutator* case, where the
mutator structurally prevents `is_drained()` from ever being observed. The authors already flagged
the gap (`FIXME` at `global.rs:85`, "Immediately trigger FinalMark when the Concurrent bucket is
drained"). **Fix direction:** add a **heap-pressure forced FinalMark** in `collection_required` —
when `concurrent_marking_in_progress()` and the heap is full (`gc_trigger.is_heap_full()`), return
`true` *regardless of* `is_drained()`; `schedule_collection` already maps marking-in-progress →
`Pause::FinalMark`, whose STW pause halts the SATB feed and drains the bucket to completion. Must be
mutator-driven (the poll path always runs on allocation), since the single-worker default makes the
worker-side `on_last_parked` precondition hard to reach under load. Full file:line evidence + an rr
confirmation recipe are in GH#14. Captured here so the perf panel's ConcurrentImmix omission has a
mechanism, not just a symptom. ConcurrentImmix is therefore omitted from the README
quick-panel table and remains the experimental plan (GH#4 closed for its specific scenario; this is a
new, separate ConcurrentImmix stall — track under the ConcurrentImmix production-completion item).

**LXR is NOT a clean git merge (decisive).** Added `wenyuzhao/mmtk-core` as a remote and tested
`git merge lxr/lxr` into `0.32-ocaml` on a throwaway branch: the lxr branch is **1690 commits past the
shared v0.32.0 root** (ours is +5). Merge result: **3 conflicts** (`policy/space.rs`,
`util/alloc/immix_allocator.rs`, `util/options.rs` — exactly where our `no_zero_alloc` + trigger/nursery
deltas overlap LXR) **plus 125 files** of LXR's divergent core dragged in (the entire research fork —
all of wenyuzhao's plans/refactors/experiments + the binding-breaking VM-trait changes, and 1690 commits
of API evolution that would not build against our 0.32.0 surface). So a merge is out; **one vendored
branch is still the goal but only via a careful ADDITIVE, ADAPTED port** of just the RC pieces (args.rs,
rc.rs, FieldBarrier, RC spec_defs/work-buckets, the lxr plan), gated behind `MMTK_PLAN=LXR`, with the
files re-written against our 0.32.0 API (they can't be copied verbatim — they target the +1690 surface).
This makes the phased plan (P1–P5) the right path and confirms it is real engineering, not a merge.

---

## LXR fork study — API divergence + integration plan (RQ1, 2026-06-25)

Study of **LXR** (Zhao/Blackburn/McKinley PLDI'22; RC on hierarchical Immix + concurrent SATB backup trace,
no read barrier, coalescing field-logging write barrier) in **`wenyuzhao/mmtk-core` branch `lxr`**, for RQ1.
ROADMAP carries the phased plan; this is the grounding detail.

**Same base.** The LXR fork is **mmtk-core 0.32.0** — same as our `0.32-ocaml`. Our fork already shares the
*concurrent-marking half* of LXR (`concurrent/` plan, `Pause`, `SATBBarrier`, `ConcurrentPlan`, the non-RC
parts of `barriers.rs`/`plan_constraints.rs` are byte-identical — strong shared-lineage evidence). We lack the
**RC half**: `src/args.rs` (runtime args: `LAZY_DECREMENTS`, `CYCLE_TRIGGER_THRESHOLD`, `RC_MATURE_EVACUATION`),
`src/util/rc.rs` (`RefCountHelper`; `RC_TABLE` global side-metadata, 2-bit sticky counts at min-object
granularity; `RC_STRADDLE_LINES` for >1-line objects), the `FieldBarrier` (`plan/lxr/barrier.rs`: per-slot
`GLOBAL_FIELD_UNLOG_BIT` CAS-flip → push old target to `decs`, slot to `incs`; deferred `ProcessIncs`/`ProcessDecs`
packets; coalescing = a slot is logged ≤once/cycle), RC `WorkBucketStage`s (`RCProcessIncs`, `RCEvacuateMature`,
`STWRCDecsAndSweep`, `FinishConcurrentWork`), the LXR plan (`plan/lxr/{global,mutator,barrier,mature_evac}.rs` +
`gc_work/{rc,tracing,mature_sweeping}.rs`; constraints `barrier: FieldBarrier, needs_log_bit, needs_field_log_bit,
rc_enabled`, `Pause::RefCount`), and **RC hooks inside `policy/immix/immixspace.rs` (~17 sites:
`trace_object_rc`, `set_as_in_place_promoted`, straddle) + LOS** — the deepest, least-modular part.

**VMBinding incompatibilities vs upstream/our 0.32 (the "API changes" the user flagged), enumerated:**
- `ObjectModel`: NEW required `GLOBAL_FIELD_UNLOG_BIT_SPEC: VMGlobalFieldUnlogBitSpec` (per-slot, `LOG_BYTES_IN_ADDRESS`
  granularity — distinct from the per-object log bit), NEW required `dump_object_s`, `get_class_pointer`;
  `COMPRESSED_PTR_ENABLED` const.
- `Scanning`: **two-arg `visit_slot(slot, out_of_heap)`** (signature-break), NEW required `scan_object_with_klass`,
  `ObjectKind{ValArray,ObjArray(u32),Scalar}` + `get_obj_kind`/`is_obj_array`/`obj_array_data` hooks (RC inc
  special-cases obj-arrays); `RootsWorkFactory::create_process_roots_work(slots, **RootKind**)` (signature-break).
- `Collection`: `stop_all_mutators(tls, **current_gc_should_unload_classes**)` (signature-break) + defaulted
  class-unload hooks.
- `Slot`: NEW `to_address`/`raw_address`/`from_address` (the field barrier needs `slot.to_address()` for the
  unlog-bit CAS).
- `plan/barriers.rs`: `BarrierSelector::FieldBarrier` + `FieldBarrier<S>` + `LOGGED/UNLOGGED_VALUE`.
Most are OpenJDK-shaped (class unloading, klass pointers) and OCaml can **stub** them
(`get_class_pointer → Address::ZERO`, ignore `klass`, pass `false` for out-of-heap, no class unloading).

**What the OCaml binding adds** (barrier plumbing already exists — `runtime/memory.c` calls `caml_mmtk_region_barrier`
+ `caml_mmtk_satb_barrier` pre-store, slot-granular): a `GLOBAL_FIELD_UNLOG_BIT_SPEC` in `object_model.rs`; the
stubbed new `ObjectModel`/`Scanning` methods + `Slot::to_address` on `FieldSlot`; a third C entry
`mmtk_ocaml_field_barrier(mutator, slot)` → `object_reference_write_pre` wired into `caml_modify` (mirror the SATB
path). **RQ1's bet holds strongly:** OCaml is immutable-by-default — `caml_modify` only fires for genuinely-mutable
fields (`ref`/mutable record/array/`Bytes`); the bulk of stores are `caml_initialize` (barrier-free), so most of the
heap is barrier-free — LXR's coalescing barrier's best case. Hard binding risk: **ephemerons/finalisers under RC**
(`load_weak_reference` greys + cycle collection vs OCaml's keep-alive ephe scheme) and **`caml_initialize` zero-init
relaxation vs RC's "newly-allocated RC=0"**.

**Strategy:** MERGE the RC half into `0.32-ocaml`, gated behind `MMTK_PLAN=LXR` (all existing plans byte-identical).
NOT a rebase onto `wenyuzhao/lxr` (would force OpenJDK-shaped trait churn + conflict with our trigger/no-zero/
FinalMark deltas), NOT a cherry-pick (RC not modular). Conflict map + phased plan (P1 scaffolding → P2 immix RC
hooks [highest risk] → P3 LXR plan → P4 OCaml binding+barrier → P5 bring-up; ~5–7 wk; RQ1 measurement reachable
after P4): see ROADMAP "LXR integration".

---

## Fence audit — MMTk preserves OCaml's memory-model fences exactly; its GC barrier is fence-lighter (RQ11, 2026-06-25)

Grounding measurement for RESEARCH_QUESTIONS **RQ11** (memory-model × GC-framework co-design).
Question: does swapping OCaml's GC for MMTk perturb the carefully-placed memory-model fences
(the `stlr`/`dmb`/branch-after-load machinery the ocaml/ocaml `memory-model`-labelled PRs tune —
#14074, #13393, #12715, #14209, …)? Method: disassemble the write-path symbols on **arm64** in the
fork's `ocamlopt.opt` vs a **vanilla 5.5.0** `ocamlopt.opt` (opam switch), census barrier mnemonics
(`objdump -d`, scope per-symbol, count `dmb`/`stlr`/`ldar`/`swpal`/`ldaddal`/…).

| path | vanilla | MMTk fork |
|---|---|---|
| `caml_modify` | `1 dmb + 1 stlr` | **identical** |
| `caml_initialize` | none | **identical** |
| `caml_atomic_exchange_field` | `2 dmb + 1 swpal` | **identical** |
| `caml_atomic_load_field` | `1 dmb + 1 ldar` | **identical** |
| GC-barrier callee | `caml_darken`: **1 `ldaddal`** (atomic, per greyed write) | `mmtk_ocaml_{region,satb}_barrier`: **0 fences** (thread-local buffer append) |

**Findings.** (1) **Mutator memory-model fences are preserved fence-for-fence.** Verified at source too:
the fork leaves `asmcomp/arm64/emit.mlp` (the `emit_stlr` / `dmb ishld` logic, lines ~685-869) untouched —
no commits since the 5.5.0 base — and changes only the *inside* of `write_barrier` (`memory.c:199/206`),
so the "Note [MM]" `acquire-fence + release-store` (`memory.c:222/233`) is stock. The "MMTk double-fences
the mutator" worry is **refuted**. (2) **MMTk's GC write barrier is fence-*lighter* than stock:** vanilla's
SATB greys the old value with an atomic (`ldaddal`) into a shared mark stack on every barrier-active write;
MMTk's SATB enqueues to a thread-local buffer drained at GC time (0 mutator fences). The fence cost moved
**per-write → GC-time.** The instruction-count drop (`caml_modify` 66→16 insns) is just that MMTk's barrier
is an out-of-line *gated call* (`caml_mmtk_generational`/`_concurrent`) vs inlined `caml_darken`. (3)
Whole-runtime `dmb` total **177 (vanilla) vs 1740 (fork)** — the 10× is entirely the bundled mmtk-core
collector + work-stealing scheduler, **off** the mutator path, not added mutator fences.

**Caveat (the load-bearing one).** Deferring SATB to a buffer does NOT remove the *ordering obligation*.
`d0c721a8b7` is the proof: `Atomic.exchange`/`compare_and_set` on pointers stored *before* `write_barrier`,
so the slot-reading SATB call greyed the NEW value and lost the deleted edge — fixed by a dedicated
`caml_mmtk_satb_barrier` *before* the store (`memory.c:355`). A memory-model × GC-barrier ordering bug at the
MMTk seam — the class OCaml's fences exist to prevent.

**Open from this audit (RQ11 sub-2):** the forwarding-bit read is `SeqCst` (`slot.rs:75`) while object/value
reads are `Relaxed` (`slot.rs:108/119`); is `SeqCst` the minimal correct ordering under concurrent marking,
or over-fenced? Needs the concurrent-marking side-metadata ordering audit + ideally a mechanised
barrier↔fence composition proof.

---

## GH#4 FinalMark self-trigger + GH#5 generational weak-clear (soundness half) landed (2026-06-25)

Two GC-correctness fixes from the turing round.

**GH#4 / #30 — ConcurrentImmix small-heap deadlock (mmtk-core `72ee627050`, submodule bumped
`a86ce19c18`).** Root cause (confirmed): the concurrent→FinalMark handoff had no self-driving
trigger — FinalMark was requested only by `ConcurrentImmix::collection_required`, evaluated
solely at the allocation poll, so once the `Concurrent` work bucket drained while every mutator
was quiescent/parked (cont_lock spin, or idle at a small heap), no GC was requested and all
workers parked with goal `None` forever. The intended hook
(`gc_trigger.rs::trigger_internal_collection_request`) was `unimplemented!()`; literal FIXME at
`concurrent/immix/global.rs:84-85`. **Fix:** in `scheduler.rs::respond_to_requests` (reached only
from `on_last_parked` — all workers parked, no Concurrent packet in flight), if no `Gc` is already
requested and concurrent marking is in progress with the `Concurrent` bucket drained, self-request
`WorkerGoal::Gc`; the existing `Gc` arm resolves to `Pause::FinalMark` during concurrent marking,
so FinalMark is scheduled immediately GC-worker-side. Gated to concurrent plans
(`Plan::concurrent()` is `None` otherwise) — the normal STW path is untouched.

**Validation scope (be precise).** On turing with the `sanity` feature on: `cargo check` clean;
a 4-domain effect/continuation + alloc-burst stressor ×5 at `ConcurrentImmix`+sanity+10 MB
completes cleanly (`done`, rc 0) — **no spurious/early FinalMark, no new hang/crash** (the real
risk of this scheduler change); deterministic checksum **identical** across
ConcurrentImmix/Immix/GenImmix (correctness preserved). **The intermittent end-to-end deadlock
itself could NOT be reproduced on demand** despite extensive attempts (single- and multi-domain
heavy compiles, the testsuite effect tests, the 4-domain continuation stressor at multiple small
heaps, sanity on) — consistent with WF2's finding that it was only ever observed during real
sanity-builds and never captured in an rr trace. So this is the **documented FIXME fix, validated
for safety + correctness + no-regression**, but the end-to-end "deadlock gone" confirmation awaits
a live sanity-build/rr capture. Low-risk (gated, only fires when all workers parked + bucket
drained). The verifier additionally `cargo check`-traced every API + safety claim.

**GH#5 — generational weak/ephemeron clear, soundness half (`2a05e10846`).** On a nursery (minor)
GC only `[0,young)` is traced, so a mature referent reachable only through the mature heap is
never visited and its liveness bit is stale; reading it as dead lets the clean pass clear a
still-reachable weak/ephemeron key/data. Mirror stock OCaml (a minor GC clears only dead *young*
referents): snapshot `is_current_gc_nursery()` at the top of `process_weak_refs`, and in
`ephe_is_reachable` treat any non-nursery referent as live during a nursery GC. False on full GCs
and non-generational plans → byte-identical there. Uses the already-merged mmtk-core nursery-query
shim. **Validated:** weak-ephe-final `finaliser`/`weaktest` byte-match reference on GenImmix+Immix;
the pre-existing `ephetest` diff is identical on Immix (where this change is a proven no-op) → no
regression. **DEFERRED (not landed):** the `weaklifetime.ml` residual assert is the *orthogonal*
clear-too-LATE issue (MMTk runs ~0 full GCs at the test heap, so mature-dead weaks never clear);
its fix needs `Gc.major_collections` to count only full GCs **plus** a full-GC-under-mature-
pressure trigger — counting-only-full ALONE would hang `weaklifetime`'s `while major_collections
< 20` loop (the workflow verifier caught this), so it is held until the companion scheduler change
is designed. `finaliser_handover` SIGSEGV is the separate #55 orphan-handover sub-bug.

---

## Parallel-workflow integration: GH#8/#11/#12 + CLBG/Build/MSVC CI fixed; deep bug root-causes (2026-06-25)

Four parallel investigation workflows (external issues, bugs/deadlocks, stock-GC deletion,
CI) ran; each produced adversarially-verified patches/diagnoses. **Landed this round** (all
built + validated on the M4 Pro, pushed to `5.5+mmtk`):

| change | commit | validation |
|---|---|---|
| GH#12 — `is_forwarded` guards unmapped forwarding-bit metadata (LOS/immortal infix) | `43484e0805` | cargo clean; camlinternalFormat no-regression on Immix/StickyImmix/GenImmix |
| GH#11 — defer custom finalizer when a user `Gc.finalise` also targets the block | `5a82393980` | repro11b → `done` rc=0 (was abort 134) on 3 plans |
| CLBG regression — keep zero-fill ON for MarkCompact (no-zero was over-broad) | `7b44c5d417` | MarkCompact binarytrees rc=0 (was CRASH139) |
| stock-GC dead-code deletion (~127 lines, 7 symbols) | `50cc69980e` | finaliser.ml ref-match; Gc.stat compactions=0 |
| CI: Build symbol-check skips bundled MMTk objects; MSVC neutered (POSIX-only GC) | `88ac1f9cf1` | awk member-filter; mmtk-core can't build on Windows |
| GH#8 — benchmarks run.sh NATIVE_PLANS += GenImmix | `benchmarks@85a429f54b` | mechanical |

**GH#11 fix is in `caml_mmtk_run_custom_finalizers` (runtime/mmtk.c), the single drain
point** — so it covers *all* call sites (the earlier finalise.c reorder failed precisely
because custom finalizers drain from multiple sites). Known incompleteness: the
has-user-finaliser check inspects only the current domain's `final_info` (a cross-domain
`Gc.finalise` is not yet deferred), and the exact-base compare misses infix-registered user
finalisers (custom blocks are non-infix, so channels are unaffected).

**CLBG was a real RQ8 regression:** the no-zero gate was made *universal* but RQ8 only
validated Immix-family + ConcurrentImmix. MarkCompact's Lisp-2 VO-bit/forwarding-header
reconstruction reads unzeroed garbage → SIGSEGV. Now gated: no-zero stays for the validated
plans, zeroing ON only for MarkCompact.

### Deep bug root-causes (diagnosed, NOT yet fixed — need Linux/rr or careful work)

- **#4 / #30 — ConcurrentImmix small-heap deadlock (root cause FOUND, needs rr to confirm).**
  The concurrent→FinalMark handoff has **no self-driving trigger**: `collection_required`
  requests FinalMark only when `concurrent_marking_in_progress() && Concurrent bucket
  is_drained()`, and that predicate is evaluated *only* at the allocation poll
  (`gc_trigger.rs poll` ← `space.rs:307`). The intended internal trigger
  (`gc_trigger.rs:184-200 trigger_internal_collection_request`) is `#[allow(unused)] +
  unimplemented!()`, and there's a literal `FIXME` at `concurrent/immix/global.rs:84-85`.
  So when the Concurrent bucket drains while every mutator is quiescent / at a safepoint /
  spinning in `cont_lock` (likely at ~10 MB once the heap is consumed, and during sanity's
  quiescent windows), nothing requests FinalMark → workers park with no goal forever. The
  cont_lock yielding-spin (FAQ-Q3) reinforces it under effect/continuation churn. **Fix
  direction:** implement the GC-worker-side internal FinalMark trigger (fire on Concurrent-
  bucket drain), keep cont_lock as a yielding spin. Verify with a fresh rr trace under
  `MMTK_PLAN=ConcurrentImmix` + sanity at ~10 MB (dump WorkerMonitor parked==worker_count /
  goal==None + plan state). Highest-value GC fix — it makes ConcurrentImmix default-ready.

- **#5 — generational weak/ephemeron cleared too early (macOS-fixable, SOLID, no patch yet).**
  Fix direction (mirror stock): (1) `Gc.major_collections` must count only FULL collections,
  not nursery GCs — the binding needs a separate full-GC counter; (2) the weak/ephemeron
  clear must not treat nursery-GC survivors as fully traced under a generational plan.
  Concrete edits not yet written; this is the next macOS-fixable correctness item (task #39).

- **#55 — finaliser adoption residual (4 edits proposed, verdict NEEDS_MORE — not applied).**
  Direction: route orphan-finaliser adoption to a domain guaranteed to keep polling (the
  main domain, `all_domains[0]`) instead of an arbitrary one that may terminate. The verifier
  flagged gaps, so it needs hardening before landing.

- **#57 (terminate "cannot trace object") and the bug-#3c residual (~1%)** — both agents hit
  the StructuredOutput retry cap; prior diagnosis stands (entry below). Need Linux/rr.

### CI items still open (REVISE — not self-contained)

- **Hygiene — the proposed `.gitattributes` exemption is SELF-DEFEATING.** Editing
  `.gitattributes` flips `full_check_needed=true` in check-typo, re-arming the *whole-tree*
  step (489 errors across configure.ac/Makefile/runtime C + upstream test files), so the job
  stays red. A durable fix must either exempt all three trees (gc/mmtk + patched runtime/ +
  root build files) or keep the whole-tree step gated. Separately, step 5 fails on autoconf
  2.72-vs-2.71 (configure.ac pins `AC_PREREQ([2.71])`; fork regenerates with 2.72) — its own
  fix. (The api.rs per-change failure is the box-drawing-Unicode + >80-col comments.)
- **Testsuite (all GC plans)** — the latest failure was the shared `build` job's
  check-symbol-names gate (now fixed by `88ac1f9cf1`); re-run to confirm the plan matrix
  itself is green vs the known-unsupported set.

---

## External GitHub issue triage + #11/#12 root-cause diagnoses (handoff, 2026-06-25)

Triage of the open external issues on `fplaunchpad/ocaml-mmtk`, with root causes for the two
crashes that were diagnosed this session. **None of #4/#5/#11/#12 are fixed yet** — this entry is
the diagnosis + correct-fix-direction so the next agent doesn't re-derive them.

| # | title | status / where |
|---|---|---|
| #2 | multi-domain burn-pattern hang (bug #3c) | **FIXED** (`7d66a6172f`, 52.5%→~1%) — verify + **close**. Residual ~1% + #57 dominate at threads=8 (need `rr`). |
| #6 | GenImmix copies 13–19M dead-on-arrival cells at fixed heap | **likely improved** by the 64 MiB nursery default (fewer minor GCs ⇒ fewer blind evacuations) — **re-measure** (turing `~/rq8-nozero-results.md` baseline) before closing; ties to RQ2 extreme-alloc probe. |
| #8 | benchmarks `run.sh`: native GenImmix excluded from `NATIVE_PLANS` + stale "native GenImmix is fatal" comment | **mechanical** — add GenImmix (and the other native-capable plans) to `NATIVE_PLANS`, delete the stale comment. The suite lives on the `benchmarks` orphan branch / `ocaml-mmtk-benchmarks` repo (`run.sh`). Native GenImmix has worked since #25. |
| #10 | bare `-lmmtk_ocaml` breaks third-party dune-configurator | c_libraries part **FIXED** (#43); the **rustc-1.92 self-contained-staticlib** part is the open tail (#56). |
| #5 | generational weak/ephemeron cleared too early (weaklifetime.ml, finaliser_handover.ml) | research-grade (generational weak-clear timing tied to stock pacing) = tasks #39/#55; needs care, not macOS-quick. |
| #4 | ConcurrentImmix sanity-build deadlock at small heaps | = #30 GH#4; needs `rr` (Linux). |
| #11 | channel finalizer `try_lock: Invalid argument` abort | **DIAGNOSED, not fixed** — see below. |
| #12 | SIGSEGV scanning large-object infix pointer under moving plans | **root cause known** — see below. |

### #11 — channel finalizer `try_lock: Invalid argument` (diagnosed; reorder fix INSUFFICIENT)

**Mechanism.** A dead `in_channel` custom block has **two** finalizers: (1) the custom-block finalizer
`caml_finalize_channel` (`io.c:548`) which on refcount→0 **destroys the mutex** (`caml_plat_mutex_free`
→ `pthread_mutex_destroy`) **and frees the struct** (`io.c:586,589`); (2) the user `Gc.finalise close_in`
→ `caml_ml_close_channel` (`io.c:725`) which `caml_channel_lock`s the mutex. Under MMTk **both run in the
same GC**, custom-block queue first → `close_in` then `try_lock`s the **destroyed/freed** mutex → `EINVAL`
→ `caml_plat_fatal_error` → abort. `io.c` is **byte-identical to stock 5.5.0**; the bug is MMTk's
finalization *ordering/liveness*: stock spreads the two across **two** cycles (user `close_in` first on the
still-live channel; the struct is reclaimed at a **later** sweep). Deeper cause: in weak-refs mode the root
scan sets `do_final_val=0` (`scanning.rs:224`) so MMTk's custom-finalizer processor sees the channel **dead
this cycle**, while `caml_mmtk_final_update_first` **resurrects** it for the user finaliser in the **same**
cycle → both drain together. Confirmed: `MMTK_WEAK_REFS=0` ⇒ no crash; deterministic abort on
GenImmix@8MiB nursery 1 MiB (repro: `scratchpad/repro11b.ml` — the original issue repro under-allocates and
won't trigger a GC on macOS).

**Attempted fix that DID NOT work (reverted):** reorder `caml_mmtk_run_custom_finalizers()` to *after* the
user-table loop in `caml_final_do_calls_res` (`finalise.c`). **Insufficient** — confirmed still aborts
(rc=134; `finalise.n.o` *was* rebuilt). Reason: `caml_mmtk_run_custom_finalizers` is drained from
**multiple sites** (`finalise.c`, `domain.c:2232` teardown, and a safepoint-poll path — `mmtk.c:801`), so
the in-function reorder does not control when the channel mutex is destroyed relative to `close_in`.

**Correct fix direction (next agent):** mirror stock's **two-cycle separation** — do not let a custom-block
value enter MMTk's ready-to-finalize set in the *same* cycle a user `Gc.finalise` resurrects it. I.e. in the
binding's weak-ref/finalizer processing (`scanning.rs process_weak_refs` ↔ the mmtk-core finalizer queue),
if a dying custom block also has a pending **user** finaliser, defer the custom finalize to a later cycle
(after the user finaliser has run on the live channel). Validate with `repro11b.ml` at a small heap + add it
as a testsuite regression.

### #12 — SIGSEGV on large-object infix pointer under moving plans (root cause known)

**Mechanism (from the issue, verify in `common/`):** scanning an **infix pointer** whose parent closure
lives in the **large-object space** segfaults — `classify` (slot.rs) sees the `Infix_tag` header and calls
`is_forwarded`, which reads **forwarding-bit side-metadata that LOS does not map** → wild read. LOS objects
never move, so an infix pointer into LOS is never forwarded. **Fix:** guard the `is_forwarded` read so it is
skipped for non-moving / LOS objects (e.g. gate on the object being in a space that maps the forwarding-bits
spec, or check `mmtk_ocaml_is_in_los`/space before reading). **Repro gotcha:** the issue's repro needs a
closure with >2048 captured fields to land in LOS; that 2300-var mutually-recursive closure compiles
**pathologically slowly** (`ocamlopt` burned **67 CPU-min** and did not finish on the M4 Pro — a
superlinear closure-conversion/regalloc path). Find a cheaper LOS-infix trigger (smaller N just over the
threshold, or a hand-built infix-into-large-array) before iterating. Best validated under MMTk `sanity` at a
small heap on a moving plan (Immix/StickyImmix/GenImmix).

---

## Default nursery raised 8 MiB → 64 MiB (bounded) — GenImmix single-domain 1.3–3× faster, lower RSS

*2026-06-25*

**Change (`api.rs`):** default nursery `Bounded:2m,8m` → `Bounded:2m,64m` (kept *bounded/absolute*
and commit-on-demand; the max is what changed). Implements SCALABILITY.md §10.4 #1.

**Why.** The 8 MiB default (chosen for bounded RSS, #44) is too small for high-allocation-rate
workloads: it forces hundreds-to-thousands of near-empty minor collections, so GenImmix paid pure
copy-nursery overhead without the benefit. Single-domain, @512 MiB heap, native:

| bench | 8 MiB (old) | 64 MiB (new) |
|---|---|---|
| spectralnorm 3000 | 0.879 s, **723** GCs, 75 MB | 0.692 s, **89** GCs, 131 MB |
| binarytrees 18 | 0.423 s, **110** GCs, 262 MB | 0.142 s, **20** GCs, 185 MB |

At memory parity GenImmix(64 MiB) is competitive-to-best single-domain (fastest *and* leanest on
spectralnorm; binarytrees within 1.5× of Immix at 3× less RSS than Immix's 554 MB). It does **NOT**
fix the multi-domain STW-pause anti-scaling (S(8) stays 0.71 with 8× fewer GCs — the per-collection
STW cost grows with domains regardless of frequency; that is structural, use `MMTK_PLAN=Immix`/
`ConcurrentImmix` for parallel-heavy work). See SCALABILITY.md (branch `night/scalability-findings`)
for the full investigation.

**Validated (M4 Pro, macOS arm64).** Default now 64 MiB (binarytrees 110→20, spectralnorm 723→89);
`MMTK_NURSERY` override still honored (forced 8m → 111 GCs); **tight heaps safe** — `Bounded` adapts
the nursery down to fit, 16/24/32 MiB pinned heaps all rc=0 (nursery never exceeds the heap, which is
why `Bounded` not `Fixed`); CLBG binarytrees byte-matches golden; bytecode path clean; dynamic default
heap clean.

---

## bug #3c FIXED (cross-STW bracket); #52 worker-scaling ABANDONED; the real finding — MMTk-GenImmix SERIALIZES multi-domain execution

*2026-06-24*

**bug #3c — FIXED & landed.** `runtime/minor_gc.c`: bracket the body of `caml_empty_minor_heaps_once`
with `caml_mmtk_enter_blocking((uintnat)Caml_state)` … `caml_mmtk_become_running(...)`, so a domain
leading/joining OCaml's all-domains minor STW is marked STOPPED in MMTk's view for that window —
`stop_all_mutators` no longer awaits it, so the two STW protocols cannot capture each other's domains.
The domain stays a registered mutator (roots still scanned); both primitives are idempotent + no-ops
without an MMTk mutator. Validated on church (Immix/512 MB/threads=8, 20 s kill): cross-STW hang
**52.5% → ~1%** (≈78×), `sanity` 0 invalid refs, regression checksum stable. Residual ~1% is a rarer
interleaving (concurrent multi-domain terminate; needs `rr`). bug #57 (`active_plan.rs:59`) is now the
dominant threads=8 failure — separate, pre-existing. Topic branch `fix/bug3c-cross-stw` (`7d66a6172f`),
merged to `5.5+mmtk`.

**#52 dynamic GC-worker scaling — ABANDONED (premise disproved).** Hypothesis: par_binarytrees
anti-scales because the default 1 GC worker serialises the parallel major GC. **Disproved by
measurement** (M4 Pro, par_binarytrees, GenImmix dynamic heap, wall s; vanilla = non-flambda 5.5.0):

depth 18: vanilla d1→d8 = 0.30→**0.084 (3.6× speedup)**; fork T=1 = 0.37→0.57 (anti); fork T=8 = 0.40→0.75 (anti).
depth 21 (with CPU%):

| domains | vanilla | fork T=1 | fork T=8 |
|---|---|---|---|
| 1 | 3.05s (99%) | 3.91s (100%) | 3.84s (240%) |
| 2 | 1.53s (186%) | 4.57s (114%) | 4.09s (319%) |
| 4 | 1.08s (266%) | 5.69s (120%) | 4.76s (393%) |
| 8 | **0.85s (419%)** | 8.40s (126%) | 5.90s (649%) |

The benchmark **divides** a fixed `niter` across domains (it is *supposed* to scale — vanilla does, 3.6×).
The fork **anti-scales at BOTH T=1 and T=8** (wall *rises* with domains). Worker count is a third-order
knob: T=8 vs T=1 trades CPU for a little pause latency (d8 5.9 vs 8.4 s) but does not change the
direction. Decisive signal = **mutator-core utilisation**: at d8 vanilla runs at ~419% CPU (≈4.2 cores
in parallel); the fork at T=1 sits at **126%** (~1.26 cores) — *the domains never run concurrently*. At
T=8 the high CPU% is GC **workers** churning during STW, not mutators: it burns **38 CPU-s** to vanilla's
3.5 (11× waste) and still anti-scales. fork d1 = 1.28× vanilla d1 = the known ~1.27× sequential
per-collection cost.

**Root cause = STW-everything, not the thread pool.** Every minor collection stops *all* domains, and
the benchmark's ~64 `Domain.spawn`/`join` each force a full MMTk GC on terminate; the domains spend
nearly all their time stopped/coordinating → ~1 core of real progress + super-linearly growing total
work (fork total CPU at d8 grows to 10.7 s at T=1). `MMTK_THREADS=N` already serves anyone wanting more
workers. #52's deferred-spawn cut also deadlocked with ≥2 active workers (one livelocks in
`poll_schedulable_work` — it preallocates `max` slots for pool sizing but activates only a prefix,
leaving phantom stealers in the poll/steal path). Parked on `feat/dynamic-worker-scaling`
(super `265b1fc784`, submodule `b73e7fa472`) with the full diagnosis; not worth fixing.

**THE research question this surfaces:** MMTk-GenImmix *eliminates OCaml's multi-domain parallel
scalability* — it turns a 3.6× vanilla speedup into a ~2× slowdown — because of the GC's STW design
(all-domains STW minor GC + full-GC-per-terminate), independent of GC worker count. The levers worth
chasing: per-domain-local / concurrent minor collection (avoid the all-domains rendezvous) and a cheap
domain terminate (no full GC). Plus the single-thread per-collection cost (the 1.27× residual, #G1).
---

## Stock-GC dead-code audit — M9 excision is structurally complete; the inert residue is mostly load-bearing

*2026-06-24*

Audited `runtime/` for stock-GC code that could still be deleted (ROADMAP #18). Verdict: the heavy
bodies are gone; what remains is a **small** deletable residue plus a large amount of
inert-looking-but-**load-bearing** scaffolding. **Recording the classification so a future cleanup
pass doesn't delete code whose absence breaks the link, deadlocks teardown, or spins the mutator.**

**Deletable now (no dependencies):**
- `caml_final_update_first` / `caml_final_update_last` (`finalise.c:118-142`) + their
  `EV_FINALISE_UPDATE_*` spans + `finalise.h:72-73` decls — **zero in-tree callers** (the live
  finaliser path is `caml_final_update_last_minor`, `finalise.c:413` ← `minor_gc.c:419`). Truly
  dead; the backlog had not catalogued these.
- ~8 phantom `runtime_events` spans wrapping no-ops — perf-backlog #R3 (`EV_MINOR`/`EV_MAJOR`/
  `EV_C_MAJOR_*`/opportunistic-mark). Removing them is what stops olly reporting fictional pauses.
- `caml_compactions_count` (`major_gc.c:108`) — written nowhere; collapse the two `Gc.stat` reads
  (`gc_ctrl.c:77`) to literal `0`, then drop the symbol.

**Inert but LOAD-BEARING — do NOT delete:**
- No-op `caml_darken`/`caml_darken_cont`/`caml_finish_marking`/`caml_finish_sweeping`/
  `caml_finish_major_cycle`/`caml_major_collection_slice`/`caml_opportunistic_major_collection_slice`/
  `caml_mark_roots_stw`/`caml_orphan_ephemerons` — link symbols the weak/ephemeron/finaliser/
  continuation/teardown paths call. Two carry load-bearing side effects:
  `caml_major_collection_slice` records `major_slice_epoch` (else bytecode spins in
  `caml_poll_gc_work` — `major_gc.c:1064-1069`); `caml_finish_marking`/`_sweeping` set the
  `*_done` flags domain teardown waits on (`domain.c:2127,2136`).
- `caml_gc_phase` frozen at `Phase_sweep_main` (set once `gc_ctrl.c:392`) gates the stock
  weak/ephemeron paths into their no-op branch — deleting the init store would flip the guards.
- `young_start/end/ptr/limit/trigger` — NOT dead: alias the MMTk TLAB/Immix-nursery block
  (`domain.c:460-462`, `mmtk.c:332-336`); back the native bump + minor-words odometer.
- `caml_do_roots` link anchor (`mmtk.c:49`) forces `roots.o` into the link.
- `caml_adjust_gc_speed`/`caml_adjust_minor_gc_speed`/`caml_alloc_dependent_memory`/
  `caml_free_dependent_memory` — dead *for GC* but exported `CAMLextern` ABI (`memory.h:40-43`)
  + called from `custom.c`. Internally dead, ABI-undeletable.

**The big deletion is gated on bug #3c, not independent.** The entire OCaml minor-STW rendezvous
(`caml_empty_minor_heaps_once`/`caml_try_empty_minor_heap_on_all_domains`/neutered
`caml_empty_minor_heap_promote`/minor barriers/`caml_minor_cycles_started`) is inert as collection
but is the live `Domain.spawn`/terminate STW rendezvous (MMTk drives it via `mmtk.c`). Retiring it
= making MMTk's STW the sole rendezvous = the #3c fix; do that once and a swath of `minor_gc.c` +
barrier machinery becomes deletable as a side effect.

---

## bug #3c — CORRECTED: a cross-STW rendezvous deadlock (OCaml minor STW × MMTk STW), not an orphaned flag

*2026-06-24*

The earlier "orphaned `gc_active` shadow flag" diagnosis (entry below) was an **idle end-state snapshot**,
NOT the cause. A church before/after **disproved** the parker self-heal: baseline **23/40 hangs (57.5%)**,
with-fix **34/60 (57%)** — unchanged; the self-heal never fires (at every hang `gc_in_progress_relaxed()`
correctly returns *true* — MMTk is genuinely mid-collection, `gc_status == GcPrepare`).

**Real cause (verified via thread-stack dumps at the hang):** a **cross-STW rendezvous deadlock** between
OCaml's minor-heap STW and MMTk's STW. A terminating domain calls `caml_empty_minor_heaps_once`
(`domain.c:2129`) while **still in MMTk's RUNNING set**, leads OCaml's minor STW, and spins on
`all_domains_lock` waiting for the other domains — which MMTk has poisoned and parked in
`park_until_resumed`. A GC worker is meanwhile blocked in `stop_all_mutators` at the `running.is_empty()`
barrier. Each STW has captured domains the other waits on. **Not rare:** a 2-domain × 1500-round spawn loop
fails ~2/10 even at `MMTK_THREADS=1`.

**Correct fix (OPEN):** serialise OCaml's minor STW against MMTk's STW so neither captures the other's
domains — mark a domain STOPPED in MMTk's view while it leads/joins OCaml's minor STW, OR have MMTk defer a
collection while an OCaml minor STW is in flight. The shadow-flag self-heal (branch `fix/bug3c-parker` +
mmtk-core `fix/bug3c-gc-in-progress-relaxed`, **NOT merged**) is at most defensive infra; it does not fix
this. church report: `church:~/bug3c-fix.md`.

**Two side issues surfaced (orthogonal, pre-existing):** (1) **GH#10 build break on rustc 1.92** — its
`staticlib` isn't self-contained (~539 std + 4 `__rdl_*` symbols undefined), so the runtime-archive object
bundling fails to link (workaround: `--whole-archive` + libstd dylib). (2) a separate **"MMTk cannot trace
object"** crash (`active_plan.rs:59`) during domain terminate under light spawn — a likely dangling-root
trace, distinct from #3c.

---

## Space-overhead heap trigger — replaces MemBalancer (binarytrees 3.5× → 1.27×)

*2026-06-24*

The dynamic default heap was MemBalancer (`DynamicHeapSize`), the OOPSLA'22 optimal-heap sizer:
`heap = live + sqrt(live × alloc_rate/gc_rate / 0.2)`. Its headroom term is **sqrt(live)** — *sublinear*,
so for a large live set the headroom is a small fraction of live → the heap settles ~1.1–1.3× live →
major-GC thrash re-tracing the live set. binarytrees-20 was **3.5× slower than stock** under it (290 GCs,
mostly major, 4.3 s of GC).

**Fix (LANDED `70a709e179`; submodule `0fe660bb9c`).** New `GCTriggerSelector::SpaceOverheadSize(min,max,
overhead_pct)` + `SpaceOverheadTrigger` in the `gc/mmtk-core` fork: after each **full** GC, `heap_limit =
live × (1 + overhead/100)`, clamped `[min,max]`. Headroom is **linear** in live (stock OCaml's
`Gc.space_overhead`), so it's always proportional to what's alive. Binding default (when `MMTK_HEAP_SIZE_MB`
unset): `SpaceOverheadSize:16MiB,RAM,120` + a **bounded 2–8 MiB nursery** (the major heap is now sized
separately, so the nursery must NOT be heap-proportional — a proportional nursery blew RSS to 362 MB–1.1 GB).
(The nursery max was later raised 8 MiB → **64 MiB** — too small for high-alloc workloads; see the
2026-06-25 entry at the top.)

**Two implementation traps, both fixed:** (1) recompute must be **gated on full-heap GC**
(`gen.last_collection_full_heap()`) — a nursery GC's `get_reserved_pages()` is transiently inflated
(un-released nursery + Immix fragmentation), and resizing on it overshoots (worse with smaller nurseries →
more nursery GCs → RSS climbed 362→732→1153 MB). (2) the nursery must be **bounded absolute**, not
proportional to the (now larger) heap.

**Result (binarytrees-20, 1 GC thread, M4 Pro):** **1.27× slower than stock** (was 3.5×); 640 GCs / 1353 ms
GC; panel goldens byte-identical; CLI tools stay ~26 MB RSS. The residual 1.27× is the per-collection copy
cost (#G1 territory), heap-policy-independent. RSS ≈ 4× live (197 MB) is **Immix mature-space fragmentation**
× the 1.2 overhead (lowering overhead to 60% barely helped RSS but cost throughput — 1.73×), so 120% is the
right default; the RSS floor is a separate Immix-defrag lever. Overridable via `MMTK_GC_TRIGGER`/`MMTK_NURSERY`.

---

## finaliser_handover UAF — FIXED (root orphaned finalisers every GC); + a separate adoption-routing residual

*2026-06-24*

FIXED the orphaned-finaliser use-after-free (rr diagnosis below). `caml_mmtk_scan_orphaned_finalisers`
(`runtime/major_gc.c`) walks `orph_structs.final_info` under `orphaned_lock` and roots it — first/last
`.fun` always, `.val` only `if (do_val)`; every `todo` run-queue `.fun`/`.val` unconditionally — mirroring
`caml_final_do_roots` via `Call_action` (slot-passing, so the moving GC fixes the `caml_stat_alloc`'d table
slots). The binding's `scan_vm_specific_roots` calls it with `do_final_val = weak_refs ? 0 : 1` (matching the
live-domain `scan_roots_in_mutator_thread` path). `finaliser_handover.ml`: **3/3 SIGSEGV → 0/62** at
`GenImmix MMTK_HEAP_SIZE_MB=64`. Landed **`a1971a465d`** (+84/-4, 3 files). `weak-ephe-final` dir now 9/14
(finaliser_handover passes; the 5 fails are the pre-existing GH#5 weak-clear, single-domain, untouched).

**Separate residual the crash was masking (NOT introduced by the fix):** `assert(finalise_count ==
work_count)` in finaliser_handover fails **~2/30 at 64 MB** — a **multi-domain orphan-adoption-routing**
issue (0/20 single-domain, 0/20 default heap; only under small-heap multidomain GC pressure). `diff = 502 =
251×2` = one terminated domain's *entire* finaliser set: adopted finalisers get queued onto
`domain_addrs().first()`, which then never reaches a safepoint to drain them, so not all run. This is an
under-execution/liveness bug (finalisers adopted but not all run), **not** a crash. Follow-up.

---

## bug #3c (rare multidomain spawn hang) — rr-diagnosed: orphaned `gc_active` STW shadow flag

*2026-06-24*

Reverse-debugged the rare bug #3c hang on **church** (rr). **It needs REAL parallelism:** 0/40 hangs
serialized (`rr record --num-cores=1`), but hung on the FIRST run at `--num-cores=8` (which records cleanly,
no MMTk meta-mmap abort — the right way to capture it). Cheap repro:
`MMTK_PLAN=Immix MMTK_HEAP_SIZE_MB=512 ./runtime/ocamlrun benchmarks/clbg/build/fannkuchredux.byte 7`
(the CLBG `Domain.spawn` bench; hangs ~60–75% under Immix bytecode — far cheaper than the burn driver).
**rr trace: `church:/tmp/rr-bug3c/t1` (packed ~95M); report `church:~/bug3c-rr.md`.**

**Root cause — the binding's `gc_active` / `GC_ACTIVE` STW shadow flag gets orphaned `true`.** At the
deadlock (the program's first `Domain.spawn`): the main domain is parked in `caml_domain_spawn →
caml_mmtk_leave_blocking → become_running → cooperative_park → park_until_resumed`, blocked on `STW_COND`
waiting for `gc_active == false`; all GC workers idle in `poll_slow`. Authoritative reads: MMTk's
`WorkerMonitor` shows `worker_count = parked = 8, goal = None` — **MMTk is provably idle with no GC
scheduled** — yet `gc_active` is stuck `true`. The event timeline shows **3 `stop_all_mutators` vs 2
`resume_mutators`**: GC #3's stop set `gc_active=true` and passed its barrier (reached the mutator-visit
loop), but its collection never called `resume_mutators` (the last `GC_ACTIVE` write is 0→1). `gc_active`
is a *shadow* of MMTk's real GC-in-progress state, kept in sync only by "stop sets it / resume clears it";
the spawn handshake rapidly bounces the parent STOPPED↔RUNNING while allocation trips back-to-back
collections, opening a window that breaks the pairing → flag orphaned-true. The parker keys ENTIRELY off
`gc_active`, so it waits forever. StickyImmix hangs identically (binding machinery, not a plan defect).

**Fix directions (not yet implemented):** (a) make the parker consult MMTk's *authoritative* GC state
(`GlobalState.gc_status`) instead of the shadow flag — needs a small `pub(crate)` accessor; or (b) give
`park_until_resumed` a bounded, re-validating wait so an orphaned flag self-heals. **Not fully pinned:** the
exact `on_last_parked` branch by which GC #3's physical collection reached idle without `resume_mutators`.

---

## `finaliser_handover.ml` multidomain SIGSEGV — rr-diagnosed: use-after-free of a finaliser value

*2026-06-24*

Reverse-debugged the residual `weak-ephe-final/finaliser_handover.ml` SIGSEGV (bug #3b /
"multi-domain orphan-finaliser handover sub-bug") on **godel** (rr 5.7.0). Fresh clone
`~/ocaml-mmtk-finh` @ `8777a2208`, bytecode `world` + `ocamlrund`. **Deterministic repro** under
`MMTK_PLAN=GenImmix MMTK_HEAP_SIZE_MB=64` (crashed on the FIRST run; small heap → frequent
nursery GCs). **rr trace saved: `godel:~/rr-finh-saved` (packed, 59M)** + live `godel:/tmp/rr-finh`;
full writeup `godel:~/finh-rr.md`.

**Crash mechanism.** A GC worker recurses ~87,000 frames in the Immix copy allocator
(`overflow_alloc → acquire_clean_block → alloc_slow_* → overflow_alloc …`) trying to satisfy ONE
absurd request of **17,213,390,904 bytes (~17 GB)**, exhausts the C stack → SIGSEGV. The size is
`get_current_size(from)` (`common/object_model.rs`) reading a **garbage header**
(`0x00000200ffc01857`, wosize ~2.15e9) for `from = 0x4000000f5f8`.

**Root cause — stale finaliser value (use-after-free), not a missed root or mis-forward.**
`0x4000000f5f8` is a `roots:true` slot on the **main domain's `current_stack`**
(`caml_scan_stack`, `fflags=0`), recurring across many bytecode frames = the argument `v` of the
running finaliser body `fun v -> ignore @@ check v`. But it points into the **interior of a
heap-allocated bytecode fiber stack** in the nursery (`0x4000000e900..f700` — code ptrs, the ASCII
string "index out of bounds", etc.), **not** a real `Node`. The main mutator is STW-parked in
`pthread_cond_wait`, stopped mid-finaliser with the corrupt `v` live; the worker scans its stack
and dies copying `v`. This GC's `process_weak_refs` **does** run
`caml_mmtk_adopt_orphaned_finalisers` — `orph_structs.final_info` non-NULL, hundreds of orphaned
entries from the terminated `Domain.spawn(work)` domains, all nursery-resident (valid headers AT
adoption: val `0x800` Node, fun `0x10f7` Closure). The lifetime hole: finaliser values orphaned by
a terminated domain are nursery objects that are **not roots and not forwarded by the nursery GCs
running between orphaning → adoption → the finaliser actually running**; the TLAB/copy-nursery is
recycled (a fiber stack bump-allocated over it), so a queued finaliser's `val` ends up aliasing
unrelated nursery bytes.

**Where (code).** `caml_orphan_finalisers` (major_gc.c ~512) splices only pointers into
`orph_structs`; `caml_mmtk_adopt_orphaned_finalisers` (~576) merges first/last via
`caml_final_merge_finalisable` (memcpy, no retain/forward of table values) and re-examines them
only *within the adopting GC*. Nothing keeps orphaned (or queued-but-not-yet-run) finaliser values
live + address-correct across the intervening GCs.

**Proposed fix (to validate).** Root the orphan structs **every** GC: enumerate
`orph_structs.final_info` first/last tables + run-queues in a VM root pass (orphaned_lock-guarded)
and feed `fun`/`val` to `collect_root_slot`, so each nursery GC forwards-and-updates them in place
— mirroring how a live domain's `final_info` is rooted via `caml_final_do_roots`. Also verify the
weak-refs `do_final_val=0` path keeps a live domain's `todo_head` run-queue vals alive+forwarded
until `caml_final_do_calls` runs them. Then re-verify in a GenImmix/64MB loop + mmtk `sanity` at a
small heap.

---

## GC worker pool: default to 1 (was nproc) — the dominant minor-GC fix

*2026-06-24*

The turing perf profile (`~/perfgc-minor.md`) showed the "~1.2 ms minor-GC floor" was two costs:
(1) a **worker-handshake tax linear in `MMTK_THREADS`** — at the nproc default, every worker parks/wakes on
EVERY collection contending on one `WorkerMonitor` mutex+condvar; perf attributes **82% of GC-worker CPU to
`park_and_wait`** doing zero work. Per-GC fixed cost: 0.055 ms (1 worker) → 0.73–0.87 ms (28). (2) a
survivor-scaling copy+scan cost (~90% on real workloads; ~16% of it side-metadata atomics). **The 1-worker
floor (~0.055 ms) is 5–7× *cheaper* than stock's 0.3–0.4 ms** — MMTk's minor machinery is lean; the slowdown
was the oversized pool + atomic metadata, not the algorithm. (Great Bactrian signal.)

**Fix (landed):** default the GC worker count to **1** when `MMTK_THREADS` is unset (`api.rs`), gated so the
env still overrides. Validated on M4 Pro (hyperfine, 8 runs): binarytrees-20 (2 MiB nursery) **2.86 s ±0.02 (nproc=12) →
2.09 s ±0.02 (1.37× faster)**, default == `MMTK_THREADS=1`, golden byte-identical. The smoking gun is the
*system* time — nproc=12 burns **3.75 s in the kernel** (futex park/wake) vs the default's 0.07 s. The intended policy is **workers = number of running
domains** (KC) — match GC parallelism to mutator parallelism — but **mmtk-core fixes the pool at init**
(`WorkerGroup::new` once + `spawn_gc_threads`; only stop-all-for-fork / respawn-all, no runtime resize), so
the dynamic scaling is a **gc/mmtk-core-fork follow-up** (resize-on-domain-spawn, or over-provision + wake-a-
subset per GC). Interim: parallel/multi-domain workloads set `MMTK_THREADS`.

**Related, from the same investigation:**
- **#G1 (next to implement):** the binding does *major*-GC root scanning on every *minor* GC —
  `scan_roots…` calls `caml_do_roots(…, 0 /*scan everything*/)` (`scanning.rs:215`) + `caml_scan_global_roots`
  (all globals incl. old, `:244`), with no minor-vs-major distinction. The narrow machinery
  (`caml_scan_global_young_roots`, `SCANNING_ONLY_YOUNG_VALUES|RECENT_FRAMES`) still ships in the C runtime,
  unused. Small for binarytrees (~1.5%) but `O(all module state)`; correctness-sensitive (recent-frames
  watermark). → implement gated on a nursery-GC query + sanity + testsuite.
- **#E1 DROPPED:** non-atomic forwarding is unsafe with >1 GC worker — the SeqCst forwarding CAS is what stops
  two work-stealing workers double-forwarding the same object. Load-bearing; not removable.
- **`caml_alloc2` = misattribution:** native small-alloc is correctly inline (`fun_fast` defaults true,
  `linearize.ml:343`; disasm shows `subq $24,%r15; cmpq young_limit; jb caml_call_gc` — no `caml_alloc2`
  call). Byte-for-byte stock, as claimed.

---

## Dynamic heap default (footprint fix) + the real finding: MMTk's minor GC is expensive

*2026-06-24*

Investigating the nursery size surfaced that the fork shipped naive MMTk defaults. Measured on the M4 Pro
(binarytrees, native, non-flambda, vs stock OCaml 5.4.1; RSS via `/usr/bin/time -l`; stock GC counts via
`OCAMLRUNPARAM=v=0x400`; MMTk GC stats via `MMTK_VERBOSE=1`):

- **Fixed 1 GB heap → FIXED (dynamic now default).** `runtime/mmtk.c` hard-coded `heap_mb=1024` and `api.rs`
  set `FixedHeapSize`. The 1 GB was virtual (a tiny tool was 26 MB RSS, not 1 GB, so it still ran), but the
  heap-full trigger let alloc-heavy programs balloon: **binarytrees-18 = 589 MB vs stock 39 MB (~15×)**. Fix:
  `runtime/mmtk.c` passes `heap_bytes=0` when `MMTK_HEAP_SIZE_MB` is unset; `api.rs` turns 0 into
  `DynamicHeapSize:16 MiB,<physical RAM>` (MemBalancer; 64 GiB fallback if RAM unknown). Gated on
  `MMTK_GC_TRIGGER`; a non-zero `MMTK_HEAP_SIZE_MB` still pins a fixed heap. Added `physical_memory_bytes()`
  (sysconf/sysctl). Result: **binarytrees-18 RSS 589 → 105 MB**, correct, no thrash (the 2026-06-20
  `32M,cap` gcbench >190 s thrash did NOT reproduce — binarytrees-22 = 458 GCs / 8.6 s, completes).
  **Supersedes the older "Dynamic heap sizing — reverted to FixedHeapSize" note below.**

- **Nursery left on mmtk's proportional default** (0.25..1.0 × dynamic heap; floor 2 MiB, ceiling 1 TiB/64-bit).
  A fixed 8 MiB cap was tried and **rejected** — it is the wrong model: stock affords a tiny minor because its
  minor GC is cheap, MMTk's is not (next point), so a small nursery forces MMTk's worst regime. The right model
  (per KC) is a nursery sized to a **target survival rate (~10%)** with a floor that amortizes MMTk's per-
  collection cost — an **adaptive controller** (future work). mmtk already measures promotion
  (`gc_trigger.rs:451`) for its MemBalancer *heap* controller; reuse that signal for an analogous *nursery*
  controller. Isolate as an opt-in mode (NOT a new plan — nursery sizing is a trigger/policy concern shared by
  all generational plans), keeping a frozen baseline config.

- **The real finding: MMTk's per-minor-GC cost is high.** At stock's exact 2 MiB nursery, MMTk-GenImmix did
  **1896 GCs in 2.86 s** vs stock's **1806 GCs in 1.21 s** — same count, ~2.4× slower; per-GC floor ≈1.2 ms
  (MMTk) vs ≈0.3–0.4 ms (stock). MMTk routes every nursery collection through its full STW-handshake + GC-
  worker-thread + work-packet machinery; stock's minor GC is an inline Cheney copy on the mutator. The big-
  nursery "win" is fake — it only wins by ballooning RSS. **Methodology: compare at memory parity, report RSS
  with wall time.** NEXT: Linux `perf` profile of one GenImmix minor GC to attribute the ~1.2 ms floor (worker
  wakeup vs root scan vs copy vs scheduling) — sets the nursery amortization floor and the path to a viable
  OCaml minor GC under MMTk.

---

## GH#10 FIXED — bundle the MMTk staticlib into the runtime archives (drop the bare `-lmmtk_ocaml` from c_libraries)

*2026-06-24*

External report (@udesou, found building the macro-benches): the macOS-native-link relocatability fix had put a
**bare `-lmmtk_ocaml` (no `-L`)** into `ocamlc -config`'s `{bytecomp,native}_c_libraries`. Third-party
`dune-configurator` feature-probes link a test program via bare `cc` (without OCaml's `-L<stdlib>`), so `ld`
couldn't resolve `-lmmtk_ocaml` → the probe failed → the lib mis-detected the feature (lwt "requires pthreads",
ctypes "'bool' cannot be defined", owl "cblas not found"). Blocked building real libraries + the macro-benches
campaign.

**Fix** (branch `fix/mmtk-clibs-probe`, merged `35bf263b3a`): **bundle `libmmtk_ocaml.a`'s objects directly into
the runtime static archives** (`lib{asm,caml}run*.a`) the compiler always links — so the `mmtk_ocaml_*` glue
resolves in every native + `-custom` program **without** a bare `-lmmtk_ocaml` — and **drop the bare
`-lmmtk_ocaml` from `mmtk_c_libraries`** (keep the per-OS system libs). Per-object bundling (a staging dir
`runtime/mmtk_objs/`, not `ld -r`) so the linker still dead-strips → no per-binary bloat. `MMTK_LINK` drops
`$(MMTK_LIB)` (objects now in the archives → no duplicate-symbol). This **supersedes** the `-lmmtk_ocaml`
relocatability mechanism the macOS-native entry below describes. Tradeoff: each runtime archive grows to
~135 MB (~1.1 GB build tree); the separate 131 MB `libmmtk_ocaml.a` install is dropped.

**Verified both platforms:** Linux (turing) — config has 0 `-lmmtk_ocaml`; the dune-configurator probe + lwt's
pthread probe PASS (control with the old config FAILS); native + `-custom` link+run. macOS (arm64, fresh shallow
clone, `world.opt` 111 s) — the `ar`/`ranlib` bundle step works, config clean, native links+runs, and the
bare-cc probe with the Darwin `-framework` tokens PASSES (control FAILS). Residual (pre-existing, orthogonal):
the in-tree `-custom` header-path gap; the shared-runtime `.dylib`/`-dynamiclib` undefined-symbol case.

---

## macOS (arm64) native compile + link + run: VERIFIED — root cause was a stale configured tree, not a source gap

*2026-06-24*

**Symptom (reported).** Native *compile* worked but native *link of user programs* failed on macOS:
`./ocamlopt.opt -I stdlib /tmp/nat.ml -o /tmp/nat` →
`Undefined symbols for architecture arm64: _mmtk_ocaml_alloc, … (referenced from libasmrun.a(mmtk.n.o))`,
`ld: symbol(s) not found`. `libasmrun.a(mmtk.n.o)` references `mmtk_ocaml_*` but the MMTk staticlib was not
on the native user-program link line.

**Root cause — stale configured tree, NOT a Darwin source bug.** The relocatable link mechanism was already
committed and is already Darwin-aware:
- `configure.ac` computes `mmtk_c_libraries="-lmmtk_ocaml $mmtk_native_libs"` with a `$host_os` `*darwin*`
  branch (`-lobjc -framework IOKit -framework CoreFoundation -liconv`) — commit `e07de24a75` (Jun 20),
  made relocatable in `3206a3b7fc` (Jun 22).
- `utils/config.generated.ml.in` prepends `@mmtk_c_libraries@` to both `bytecomp_c_libraries` and
  `native_c_libraries`, so `ocamlc`/`ocamlopt` place `-lmmtk_ocaml` *after* `libasmrun.a` on the C link line.
- `Makefile` symlinks `stdlib/libmmtk_ocaml.a → ../gc/mmtk/target/release/libmmtk_ocaml.a` (a `runtime`
  prereq), and the compiler auto-adds `-L<stdlib>` (Ccomp prefixes every Load_path dir with `-L`), so
  `-lmmtk_ocaml` resolves both in-tree and from an installed/relocated prefix.

This Mac's tree was last configured at **5.5.0~rc1 (config.status dated Jun 19)** — *before* both commits — so
its (gitignored) `utils/config.generated.ml` still read `-lpthread` only, and `stdlib/libmmtk_ocaml.a` had
never been created. The fix on a stale tree is just to **reconfigure + rebuild**; no source change was needed
for the link mechanism.

**Fix applied.** `./configure` (no args, matching the original) → regenerates `config.status` +
`config.generated.ml` (now `-lmmtk_ocaml -lobjc -framework IOKit -framework CoreFoundation -liconv … -lpthread`);
`make stdlib/libmmtk_ocaml.a` creates the symlink; `make world.opt` recompiles the compiler so the new
`config.ml` (`native_c_libraries`) is embedded in `ocamlopt.opt`. Hardcoded Darwin `mmtk_native_libs` confirmed
sufficient: `cargo … --print native-static-libs` reports `-lobjc -framework IOKit -framework CoreFoundation
-liconv -lSystem -lc -lm` (the `-lSystem -lc -lm` tail is supplied by the default toolchain).

**Verification (arm64 Darwin, this Mac).**
- Native compile+link+RUN: `ocamlopt.opt -I stdlib nat.ml -o nat` → exit 0, no undefined symbols;
  `nat` prints correct output; verbose link line ends `… stdlib/libasmrun.a -lmmtk_ocaml -lobjc -framework
  IOKit -framework CoreFoundation -liconv -lpthread` (correct archive order), zero linker warnings.
- Moving GC end-to-end through the linked exe: a 5M-iteration alloc-churn program @64 MB GenImmix →
  **55 GCs, 31068 objects copied**, correct output, exit 0. Immix/StickyImmix/GenImmix all run.
- No startup mmap/ASLR flake observed (macOS has no `setarch`; none was needed here).
- Bytecode unaffected: `ocamlc.opt` compile + `ocamlrun` run clean.

**Residual / caveats (distinct from the link fix).**
- `-custom` bytecode from the *in-tree* build fails at the C-compile of the prim stub with
  `'caml/mlvalues.h' file not found` — the headers live in `runtime/caml/`, not `stdlib/caml/`. This is a
  pre-existing in-tree path quirk (the link step, where `bytecomp_c_libraries` matters, is never reached);
  `-custom` from an installed prefix finds headers in `$LIBDIR/caml/`. Not a regression from this work.
- `mmtk_native_libs` is still hardcoded per-OS in two places (`configure.ac` + `Makefile.mmtk`); the
  `--print native-static-libs` derivation TODO (M4/packaging) is unchanged.

**Merge-readiness.** The link mechanism needs no source change, so a future fresh configure on macOS Just
Works. The only committed deltas are doc updates (README/ROADMAP/this NOTES) reflecting the now-verified state.
Merged to `5.5+mmtk` (2026-06-24) as part of the doc consolidation.

---

## RQ8 no-zero allocation: SAFE, ~15–22% mutator recovery, GC unchanged — LANDED on mainline (`338cce723`), no-zero universal incl. ConcurrentImmix

*2026-06-24*

**LANDED (2026-06-24, mainline tip `338cce723`).** No-zero is now ON for **all** plans — **including
ConcurrentImmix** — via a *runtime* plan-gate: an `alloc_zeroed` flag forwarded to the two zeroing sites, set
0 by `runtime/mmtk.c:158` (`mmtk_ocaml_set_alloc_zeroed(0)`), so one binary is correct across every
`MMTK_PLAN`. The `gc/mmtk-core` fork (`0.32-ocaml`) is now the **mainline** mmtk dependency (submodule), not a
side branch. ConcurrentImmix was **verified allocate-black** — the concurrent marker eager-marks acquired
lines and never field-scans a newly-allocated object, so it never reads the half-initialized window (RQ9) —
so no-zero is safe and **enabled** there too; the gate is therefore *no-zero-universal*, superseding the
earlier "gate OFF for ConcurrentImmix" plan below. The rest of this entry records the original
measurement/correctness work that justified landing.

Implemented + measured the no-zero allocation mode (mmtk-core fork `0.32-ocaml` `no_zero_alloc` feature gating
the two alloc-time zeroing sites; binding forwards it; branch `rq8-nozero`). Build-glue: `Makefile.mmtk` gained
an on-demand `git submodule update --init gc/mmtk-core` (commit `812ee0eb6`, rq8-nozero only).

**Provably took** (disasm of `libmmtk_ocaml.a`): `acquire_recyclable_lines` 1→0 and
`get_new_pages_and_initialize` 7→0 `memory::zero` calls; GC-time bzero untouched.

**Correctness — SAFE (STW plans only):** sanity (full-heap re-trace) + no_zero @48 MB on
binarytrees / alloc-churn / closure-stress × GenImmix/Immix/StickyImmix → **0 Invalid-ref, 9/9**; the
load-bearing **closure self-compile** (`ocamlc parser.ml`) under sanity+no_zero → 0 Invalid-ref; CLBG
byte-identical ON vs OFF vs golden **15/15**; quick-panel self-check 18/18. (ConcurrentImmix was not exercised
in *this* STW-focused round; it was later verified allocate-black and enabled too — see the LANDED note above /
RQ9.) GC count/time/objects-copied identical OFF vs ON — **the win is pure mutator time.**

**Recovery (the quick panel was the decision vehicle):** spectralnorm **+21.9%** (CLBG; memset hotspot
~19%→~1% of cycles); panel ON-vs-OFF per plan — alloc +2.4–6.1%, mutate +2.8–10.2%, binarytrees +2.1–4.9%,
**nbody +0.0%** (compute control neutral — red-flag check passes); parallel scaling preserved (par_* ON-faster
at every domain count). RQ8's hypothesis (eager-zero redundant for OCaml; ~15–20% on alloc-bound) **confirmed.**

**Landing — done (see the LANDED note at the top).** The original plan here assumed a compile-time `no_zero`
cargo feature would have to be **gated OFF for ConcurrentImmix** (the then-RED-FLAG) and proposed a runtime
plan-gate that retained zeroing for ConcurrentImmix. What actually landed is a runtime `alloc_zeroed` flag
(threaded to the two sites) that is **no-zero-universal**: ConcurrentImmix was verified allocate-black (RQ9), so
zeroing is dropped for it as well. One binary, correct across all plans switched via `MMTK_PLAN`.

**Independent finding (follow-up, NOT no-zero):** the panel's large alloc sizes (40–64 M) are pathologically
slow under MMTk — `alloc 40000000` = 2m43s; GenImmix copies ~13–19 M cells once the fixed heap fills
(reproduces OFF). Worth a separate look at copy-nursery behaviour under extreme allocation + fixed heap.
Writeup: `~/rq8-nozero-results.md` on turing.

---

## GenImmix-default validated: throughput win over Immix, but a confirmed generational-minor weak-clear regression (GH#5)

*2026-06-24*

Validated the default flip (Immix → GenImmix, commit `49588c70ae`) on a fresh build at `99a7a123b`.

**Default confirmed:** `MMTK_PLAN` unset → GenImmix (`mmtk.c:128`). Bootstrap smoke (`ocamlopt.opt -c typecore.ml`
under default) PASS — 1 GC, 775 K copied (copy-nursery genuinely active). Broader regression slice **74/74**
(basic / effects / lib-array / hashtbl / gc-roots / list) — common path clean.

**Throughput — GenImmix is a near-strict improvement over Immix** (native, interleaved A/B vs released vanilla 5.5.0):

| bench | heap | GenImmix/van | Immix/van | vs Immix |
|---|---|---|---|---|
| nbody | 256 | 1.006 | 1.005 | tie (0 GC) |
| fft | 128 | 1.03–1.08 | 1.115 | **beats** |
| fft | default | 1.052 | 1.079 | **beats** |
| spectralnorm | 256 | 1.603 | 1.738 | **beats** (still loses to vanilla) |
| fannkuchredux | 256 | 1.11–1.13 | 0.985 | **LOSES** (copy-nursery jitter, short alloc-light bench; fork min 0.575 = vanilla) |
| binarytrees | 512 | 0.662 | 0.661 | tie (fork wins 1.5× vs vanilla) |

GenImmix fires more GCs than Immix (spectralnorm 45 vs 23) but copies few objects (low nursery survival) —
the generational hypothesis holds for OCaml. The lone throughput regression vs Immix is fannkuchredux (jitter
on a short bench, not a real loss). Net: parity-or-fork-win vs vanilla everywhere except spectralnorm (1.6×,
still better than Immix's 1.74×).

**CORRECTNESS CAVEAT — the real blocker (GH#5).** `weak-ephe-final`: GenImmix **8/14** vs Immix **10/14**. Two
regressions are **flip-introduced** (pass Immix, fail GenImmix; reconfirmed both ways):
- **`weaklifetime.ml`** (native + bytecode): a weak reports CLEARED while its block is still reachable
  (assert line 53) — **weak cleared too early under the generational minor (copy-nursery) collection.**
- **`finaliser_handover.ml`** (bytecode only): multi-domain `Gc.finalise` handover timing in the interpreter.

(Four other `weak-ephe-final` failures are **pre-existing** — fail under Immix too.) **`MMTK_WEAK_REFS=0` does
NOT help — it makes it worse (6→9)**: never-clear breaks the positive-clear assertions. **Root cause (scoped,
GH#5 comment) — refined:** `process_weak_refs` (scanning.rs:313) *does* run on nursery GCs; the bug is the
liveness query. The clear-vs-keep decision is `ObjectReference::is_reachable()` (`ephe_is_reachable`,
scanning.rs:131), but `ImmixSpace` doesn't override `SFT::is_reachable` → it falls through to
`is_live → is_marked()` against a `mark_state` **advanced only on full GCs**. So steady-state mature objects
(marked at the last full GC) report live correctly, but an object **freshly promoted to mature during this very
minor GC** has no current mark bit → `is_reachable()==false` → its still-held weak is cleared (weaklifetime.ml:
young block stored strongly into a mature slot, promoted on the next nursery GC, weak then mis-cleared). A
pre-existing generational bug now on the default path, not new code. **Fix:** make the predicate
generational-aware — on a nursery GC, treat any **non-nursery-resident** referent as live, clearing only **dead
nursery** objects (stock OCaml's minor rule + mmtk-core's intended `is_reachable` contract). The needed
`is_current_gc_nursery()`/`is_object_in_nursery()` are module-sealed in mmtk-core → add a small **public shim to
the `gc/mmtk-core` fork (`0.32-ocaml`)**, alongside the RQ8 no-zero work. Full GCs unchanged (gate false → Immix
byte-identical → preserves 10/14). `finaliser_handover.ml` is partly fixed by this; its residual is a
multi-domain orphan-finaliser handover sub-bug, tracked separately. Design: `~/gh5-weakclear-design.md`.

**REFRAMED (2026-06-24, by direct instrumentation — the hypothesis above is WRONG).** The generational-aware
liveness shim was implemented + built + sanity-clean (Immix byte-identical, 10/14 preserved) — but it **did not
move the gate** (weak-ephe-final failed/14 unchanged pre→post: Immix 4, StickyImmix 5, GenImmix 6, GenCopy 6).
Instrumentation shows the hypothesized **clear-too-early** (line 52, freshly-promoted referent) **does not
reproduce, even on pre-fix code**. The actual failure is **clear-too-LATE** (`weaklifetime.ml:53`): at the test
heap the generational plans run **zero full GCs** (instrumented 4/4 nursery), so mature-*dead* weaks are never
cleared; and **`Gc.major_collections` counts every GC including nursery**, so the test's `n+2`-major window is
unsatisfiable. The same binary **passes at a 16 MB heap** (mature fills → a full GC runs). So **GH#5 is a
GC-scheduling + `Gc.major_collections`-accounting problem, not a weak-liveness-query bug.** The
generational-aware shim is kept as a **correct defensive change** (no regression; sound under a real
nursery/mature liveness split) but is **not** what the test needs. **Real fix (separate, perf-sensitive):**
(1) schedule a **full GC under mature-space pressure** on generational plans (so mature-dead weaks/objects are
reclaimed without waiting for OOM); (2) fix **`Gc.major_collections`** to count only full collections, not
nursery GCs (a Gc.stat accounting bug in the MMTk reimpl). `finaliser_handover.ml` SIGSEGV under GenImmix is
**pre-existing** (reproduces pre-fix) — the separate multi-domain orphan-finaliser handover sub-bug. CI
corroborates the *pattern*: weaklifetime fails on exactly the plans that don't run a full GC every cycle
(StickyImmix/GenImmix/NoGC/GenCopy/ConcurrentImmix), passes Immix+SemiSpace. Code (not yet on mainline): branch
`rq8-runtime-gate-gh5-weakclear` (`2a48fb963`), fork `0.32-ocaml` (`6f3c4afc5b`). Writeup: `~/fork-impl-results.md`.

**Verdict:** flip is throughput-safe and the common path is clean, but it carries a confirmed weak/finaliser
soundness regression on the default (affects `Weak`/`Ephemeron`/`Gc.finalise` users). **Decision pending:**
keep GenImmix default + fix (GH#5), vs revert to Immix until fixed. Environment caveat: turing was not fully
quiet during Task 2 (competing rq8 build + spot benches); the main sweep ran in a verified-clean window, reruns
skipped to avoid mutual pollution. Writeup: `~/genimmix-default-validation.md` on turing.

---

## Native ConcurrentImmix (RQ1) — SATB barrier is ~free, concurrent marking cuts max pause 3–4×, ~0% throughput tax

*2026-06-24*

First native perf characterization of the flagship concurrent plan (`MMTK_PLAN=ConcurrentImmix`, **no extra
gate**; `runtime/mmtk.c:138` arms `caml_mmtk_concurrent` → SATB deletion barrier + per-continuation scan lock;
mmtk-core schedules InitialMark(STW) → concurrent mark → FinalMark(STW)). All 5 native CLBG benches run clean
(rc=0, byte-identical to vanilla, no SIGSEGV) at production heaps.

**Throughput (CI vs Immix):** neutral on 5/6 — nbody 1.000, fft@128 0.996, fft@default 0.925, spectralnorm
0.972, fannkuchredux 1.000; only binarytrees **+10.4%**. (CI vs vanilla tracks Immix — binarytrees 0.599, i.e.
CI is also 1.5× *faster* than vanilla there.)

**Latency — the RQ1 metric** (channel: bracketed stop→resume `gc_time` + an instrumented per-pause histogram):
binarytrees **max pause 76→20 ms (3–4×), total STW 760→205 ms**; spectralnorm max 6.4→3–4 ms (halved), total
STW barely moves (tiny live set → residual is nearly all root scan). Root scan is **STW in both** (InitialMark
scans roots; FinalMark `new_no_scan_roots`) — the irreducible floor (~6 ms/pause); the ~44 ms/pause CI sheds on
binarytrees is heap trace moved concurrent (≈88% of the old Immix pause was reducible marking).

**SATB barrier cost (bears on #30):** empirically **~free** — `SATBBarrierSemantics::memory_region_copy_slow`
0.01% self, `stw_api_barrier` 0.05%; `caml_modify`/`caml_mmtk_satb_barrier` don't even appear. The +10%
binarytrees tax is **GC-worker/metadata contention** (`scan_object` 5.8%, `side_metadata_access` ~7.8%,
`lock_contended` 1.9%) from 4 GC workers sharing the mutator's cores — a scheduling problem (likely recovered
with dedicated GC cores), **not** the write barrier. **So task #30's UNLOG-bit barrier-gate perf concern is not
borne out** for these workloads.

**RQ1 takeaway:** strongly favorable — ~0% throughput tax on 5/6, +10% on one (contention, not barrier), for a
3–4× max-pause cut on the trace-heavy bench. The feared barrier cost is a non-event; this is the first strong
*native* evidence for the immutability→read-barrier-free-low-latency hypothesis. Next frontiers: concurrent/lazy
**root scanning** (the residual pause floor), a dedicated-GC-core sweep on the binarytrees tax, and the
multidomain **`Domain.spawn` init-time deadlock** (bug #3c / GH#2 — a new repro was added; CI was *more* robust
than Immix, 0/40 vs Immix 1/20) must be fixed before any production low-latency claim. Writeup:
`~/concurrent-immix-native-perf.md` on turing.

---

## spectralnorm's 1.74× = MMTk's eager zero-fill double-write + nursery-locality loss — NOT heap-fixable; generational recovers only ~8%

*2026-06-24*

Drilled into the one structural MMTk-vs-vanilla loss (spectralnorm, native Immix 1.74× @256MB). The verdict is
**(c) intrinsic allocation-path cost, with a partial (a) plan component, and explicitly NOT (b) heap-bound** —
and it points at a concrete, publishable lever (see RQ8).

1. **Allocation profile.** 720 M minor words ≈ **5.76 GB, 100% dead-on-arrival** (`promoted=0`, `major_words=0`).
   The driver is **boxed floats**, not the float arrays (~0.5 MB total): the build is **non-flambda**, so
   `eval_A` isn't inlined and returns a 2-word boxed float — 360 M boxes × 2 words = 720 M words (matches
   `Gc.stat`). The 23 GCs are **allocation-volume-driven** (5.76 GB ÷ 256 MB), NOT a poll storm: `caml_call_gc`
   = 0.29% of cycles, total GC time ~106 ms (<3% of wall). **Not a confound to "fix" with flambda:** the
   boxed-float allocation here is just normal non-flambda codegen — and non-flambda *is* the real-world config
   we compare against (vanilla 5.5.0 is non-flambda; upstream flambda1 is mostly useless and flambda2 hasn't
   landed). The comparison is non-flambda-vs-non-flambda, so spectralnorm's allocation volume is the workload
   as built, not an artifact to measure away; the structural finding below holds for any allocation-heavy
   OCaml regardless.

2. **Plan sweep @256MB** (ratio vs vanilla 2.165 s): GenImmix **1.60×** = GenCopy 1.60× < SemiSpace 1.65× <
   ConcurrentImmix 1.68× < StickyImmix 1.71× < Immix 1.74×. **Generational helps but does not close it** —
   it removes the whole-heap line sweep (1.74→1.60) but the residual 1.60× is shared mutator-/alloc-side cost
   no plan removes. (Confirms the GenImmix-default decision is right, but worth only ~8% here.)

3. **Heap-multiple curve — ANTI-frequency regime.** Immix 1.74×(24 GC)@256 → 1.87×(12)@512 → 2.08×(6)@1G →
   2.52×(3)@2G. Bigger heap ⇒ fewer GCs but **worse** ratio; Immix GC time stays ~flat (~106–120 ms) as GC
   count drops 8×, so per-GC sweep scales with heap. **Not heap-fixable — run at the smallest heap that fits
   the live set.**

4. **Sweep-cost anatomy (`perf record`).** vanilla: 99.6% mutator, **zero memset**. Immix: mutator 51%, **libc
   memset 20.3%**, bzero_metadata 4.6% + SweepChunk 2.2% + Line::is_marked 1.0% (~8% sweep), page-faults ~5%.
   **The 20% memset is eager line-zeroing** (`immix_allocator.rs:253` → `util::memory::zero` → `write_bytes`):
   mmtk-core zero-fills every recyclable line before the mutator fills it, so **every word is written twice**
   (MMTk zeroes, then OCaml initializes) vs once in vanilla. It is **unconditional in mmtk-core 0.32** — no VM
   flag to skip — and present in **every** plan incl. GenImmix. IPC collapses 3.65→2.32 with 115× cache-misses
   / 124× dTLB-misses: vanilla's 256 KB minor heap stays cache-hot; MMTk bump-allocates across a multi-MB heap.

**The lever (→ RQ8).** OCaml fully initializes every object before any safepoint, and **vanilla OCaml already
runs on an *unzeroed* minor heap** — an existence proof that OCaml's allocation discipline tolerates non-zeroed
young memory. So MMTk's eager zero-fill is **redundant for OCaml**, and a **no-zero allocation mode** in
mmtk-core (gated on the binding asserting full-init-before-GC-observable) should recover ~20% on
allocation-heavy code, with **zero pause-time impact**. The correctness crux — can a GC observe a
partially-initialized object on the unzeroed path? — is exactly what vanilla's design already answers (no GC
between alloc and field-fill). Full writeup: `~/spectralnorm-investigation.md` on turing.

---

## perf: the obvious-removal micro-levers buy ~1.5% (one load-bearing: C1-sftbound) — the MMTk-vs-vanilla gap is structural

*2026-06-24*

The overnight optimization workflow implemented each **obvious-removal** lever from the PERFORMANCE Appendix A
backlog in isolation on turing, ran a 4-gate correctness check (build / mmtk `sanity` small-heap 0-invalid-ref /
CLBG byte-identical / native compile-repro), and benchmarked native-Immix. Of **seven** attempted, **six passed
and were pushed** as `perf-lever-*` branches; **one was rejected**:

| lever | what | verdict |
|---|---|---|
| **#C1-sftbound** | cached `[heap_start,heap_end)` bounds pre-check before the per-edge `is_in_mmtk_spaces` SFT lookup in `FieldSlot::classify` | **the only load-bearing lever (~+1%, outside noise); halves the SFT-lookup self% cluster. On the current base the win sits on binarytrees (the most trace-heavy bench).** ✅ `perf-c1-sftbound` @`b953a50e4` |
| #C2-header | read the block header once in `scan_object` (was read twice) | neutral (L1 hit, ≤ noise) ✅ |
| #C4-debugbranch | hoist per-root `debug_check_enabled()` out of the STW root-scan inner loop | neutral (≤ sampling floor) ✅ |
| #B2-islong | `Is_long(new_val)` short-circuit on the **region** (new-value) barrier; SATB deletion barrier left unguarded (depends on OLD value) | neutral ✅ |
| #B3-emptyslice | drop the throwaway empty-src slice in the barrier shims (also fixes a latent debug-assert) | neutral (barrier gated off under Immix) ✅ |
| #A3-allocdefault | `mmtk_ocaml_alloc_default` skipping the 5-arm `match semantics` (bytecode alloc path only) | neutral (native never reaches it — TLAB fast path) ✅ |
| **#C1-doubleload** | cache the classified slot word in `FieldSlot`, drop the re-read in `load()` | **REJECTED — the second load is semantically required.** ❌ |

**#C1-doubleload is NOT an obvious removal — correction to the backlog.** Caching the slot word breaks the
moving-GC sanity checker: `cache_roots_for_sanity_gc` *clones* root `FieldSlot`s and the SanityGC re-calls
`load()` on the clones **after** the real GC has stored forwarded refs into the live slot words; a cached value
then returns stale pre-GC pointers into moved/freed regions → dangling edge (`Invalid reference` / SIGSEGV in
`scan_ocaml_object`). Production happens never to re-load a stored slot (so CLBG passed) but it is not
guaranteed and it breaks our primary correctness tool. So `Slot::load()` must reflect *current* slot memory;
only the **SFT-bounds pre-check** (C1-sftbound) — purely additive, never approves a trace — is a safe
classify-path win. (My session-spawned consolidation agent independently tested only C2/C4/B2/B3 and called
them all neutral; correct as far as it went, but it **missed C1-sftbound**, the one real win.)

**Vanilla-vs-MMTk-Immix baseline — DEFINITIVE, post-fft-fix** (fork @ `c2560f1596` vs released vanilla 5.5.0,
native Immix `MMTK_THREADS=4`, interleaved A/B, hyperfine 2 warmup + 10 timed; outputs byte-identical):

| bench | heap | GCs | fork/vanilla (post-fix) | (pre-fix) | status |
|---|---|---|---|---|---|
| nbody 5M | 256 MB | 0 | **1.005×** | 1.00× | parity |
| fft | 128 MB | 1 | **1.115×** | 1.69× | **fft fix CLOSED the poll storm** |
| fft | default | 0 | 1.079× | 1.02× | small genuine mutator residual |
| spectralnorm 3000 | 256 MB | 23 | **1.738×** | 1.75× | **the one structural gap** |
| fannkuchredux 11 | 256 MB | 9 | **0.985×** | 1.65× | **fix CLOSED it — fork now wins** |
| binarytrees 20 | 512 MB | 15 | **0.661×** | 0.51× | **fork wins 1.5×** |

**The fft fix (`46cb3253f2`) closed TWO of the three big gaps** — fft@128 (1.69×→1.11×) *and* fannkuchredux
(1.65×→0.985×, fork now slightly faster). Both were the **post-GC poll storm**: a bench that fires ≥1 GC then
runs an allocation-light hot loop trapped into `caml_garbage_collection` on every poll. fannkuchredux's
`perf record` is the smoking gun — PRE: `caml_call_gc` 16.4% + `caml_garbage_collection` 9.5% +
`caml_find_frame_descr` 9.2% ≈ **37% of cycles in the poll storm**, mutator 49.6%; POST: those two ≈ **0%**,
mutator 79.4%. nbody / fft@default never trap (0 GCs). **spectralnorm (1.74×) is the lone remaining structural
gap** — it allocates continuously (every poll-trap promptly refills, so the storm never builds), and its
overhead is real Immix **sweep/metadata**: `bzero_metadata` 4.9% + `side_metadata_access` 2.7% +
`SweepChunk::do_work` 2.2% + `Line::is_marked` 1.0% + ~16% libc memset; `caml_call_gc` absent (IPC 3.65→2.33,
memory-stall bound). **Net: 5 of 6 configs are now parity-or-better; spectralnorm's Immix sweep cost is the one
real loss and the genuine M8 research target.** C1-sftbound on the current base is a consistent ~+1% (its win
migrated from fannkuchredux — now trace-light post-fix — to binarytrees, the most edge-classify-heavy bench).

**What the profiles confirm.** (1) The surviving serial overhead is the **STW root scan** —
`caml_call_gc` + `caml_garbage_collection` + `caml_find_frame_descr` ≈ 21% of fft@128, ≈ 32% of fannkuchredux;
no micro-lever touches it. (2) The **write barrier is hot in ZERO profiles** — these workloads do initializing
or unboxed-float stores, never old-pointer mutation — which is *why* #B2/#B3 are neutral. (3) Overhead is
**GC-frequency-driven, not codegen** (fft 1.69×@128 but 1.02×@default; nbody identical instruction counts). (4)
On **parallel alloc-heavy** work MMTk already **wins ~2×** — vanilla's cross-domain STW minor GC
(`caml_try_run_on_all_domains_with_spin_work` 13%, `oldify_one`, `pool_sweep`) loses to MMTk's parallel workers.

**Research implication.** Micro-levers buy ~1.5% (and only C1-sftbound is real); the publishable overhead
question is **structural** — STW root-scan cost (and its growth with domain count), Immix sweep/metadata
maintenance, and young-object throughput vs vanilla's minor collector — plus the standing **#A1** (bytecode has
no TLAB). M8 effort goes there, not into more micro-tuning.

**Branches.** The one lever worth landing is now isolated: **`perf-c1-sftbound`** @`b953a50e4` — C1-sftbound
cherry-picked clean onto current mainline `c2560f1596`, correctness-gated (sanity 0-invalid-ref across
Immix/StickyImmix incl. typecore 2.47 M copied; CLBG byte-identical), ~+1% on binarytrees. The two earlier
lever-integration branches are superseded and can be deleted: `perf-basic-overheads` @`19a07ea8` (current base
but missing C1-sftbound) and `perf-basic-overheads-integrated` @`cac434f7b` (all five but stale base
`8122989c4`); the five neutral cleanups (C2/C4/B2/B3/A3) remain on their `perf-lever-*` branches if ever wanted.
Nothing merged to `5.5+mmtk` — landing `perf-c1-sftbound` is a maintainer call. Fuller logs on turing:
`~/postfix-baseline-findings.md`, `~/perf_opt_findings.md`, `~/optbase_results/`.

---

## bug #31 / GH#3 FIXED — `Domain.join` use-after-free on the un-promoted domain result (all moving plans, not "native-generational")

*2026-06-23*

The intermittent SIGSEGV (rc=139, originally ~2/6, up to ~100% under an 8-domain stressor) was **mis-titled
"native-generational"** — it is a general **moving-plan `Domain.join` use-after-free**, on Immix / StickyImmix
/ GenImmix, **multi-domain only** (a single-domain control with the identical alloc/continuation pattern never
crashes — the discriminator that ruled out the gc_regs / generational-remembered-set hypotheses). The
continuations were only GC pressure, not the corrupted root.

**Root cause.** `sync_and_terminate` → `make_finished` allocates the domain's `Finished(Ok v)` result on the
**terminating domain's young TLAB**, then `sync_result` publishes it into `term_sync->state` and wakes the
joiner. Stock OCaml's terminate minor-GC (`caml_empty_minor_heap_promote`) oldified young survivors; under
always-on MMTk that routine is **neutered to a bare `young_ptr = young_start` discard — it does not promote**.
So the result stays *young* while published to the joiner; the domain then deregisters/tears down; a GC on a
third domain relocates/reclaims that young block, and the joiner dereferences a corrupted `Finished` chain →
SIGSEGV in `Domain.join`. Confirmed by core dump: `#0 Domain.join … movzbq -8(%rax)`, `%rax = 0x29 =
Val_int(20)` (a pointer field overwritten with a stray int).

**Fix** (`runtime/domain.c`, +28/-1, commit `1d2504ab4f`): root the result with `CAMLlocal1` and call
`caml_mmtk_collect()` after `make_finished` and **before** `sync_result` publishes it — tracing it into stable
space (promote for generational plans; mark its block live for Immix) while the domain is still a registered,
running STW participant. Self-gated (no-op for NoGC).

**Validated** (16 MB worst band, 8-domain `cont_stress3` stressor): StickyImmix control 14/15 crash → fix
**0/15 and 0/40**; GenImmix **0/12**; Immix **0/12**; correct checksum; loose heap 5/5. Regression: 7 effects
tests + `parallel/domain_dls` + `parallel/join` clean. No functional regression.

**rr was unusable** here — default `rr record` serializes and hides the cooperative race (9+ min, no crash);
`rr record --chaos` aborts (chaos randomizes layout, trips MMTk's meta-memory mmap). The race reproduces on a
**single physical core** under normal OS preemption, so it's a *logical* scheduling race — diagnosed via a
**core dump** (`~/i31_traces/core.domain_join_crash`) instead of reverse-debugging.

**Perf follow-up (flagged):** the fix forces a full MMTk collection per domain-terminate — correct and
acceptable (terminate is rare; stock OCaml also did real GC work there), but a lighter mechanism (promote just
the result, or retain the terminating domain's last block until traced) is worthwhile for join-heavy code.

---

## Native ConcurrentImmix — VALIDATED; both expected gaps were already closed; one real atomics bug fixed

*2026-06-23*

Native ConcurrentImmix is now sound + landed. Diagnosis-first showed the two anticipated native "gaps"
**did not need the expected fix**:

1. **Native SATB barrier — already covered.** This tree's native codegen has **no inlined write barrier** —
   every `Caml_modify`-kind pointer overwrite is an out-of-line `Cextcall("caml_modify", …)`
   (`asmcomp/cmm_helpers.ml` setfield/array-set; `amd64.S` has no `caml_modify` symbol), and `Array.fill` is
   the C primitive `caml_array_fill` → `caml_uniform_array_fill`. All these C helpers already fire the SATB
   barrier pre-store, so native reaches it. (The earlier "native inlines `caml_modify` and skips it" claim was
   wrong for this tree — vindicating the PERFORMANCE #B1 read.) `caml_initialize` correctly takes no SATB.
2. **TLAB allocate-black — automatic.** During a concurrent cycle, mmtk-core's `ImmixAllocator`
   eager-marks (allocate-blacks) the lines it acquires; OCaml's native TLAB is acquired through
   `ImmixAllocator::alloc`, so the gapless bump fill inherits black-ness. No binding change needed.

**The one real defect (fixed, `d0c721a8b7`):** the SATB barrier on **pointer-valued atomics** —
`caml_atomic_exchange_field` / `caml_atomic_cas_field` (`runtime/memory.c`) greyed the slot **after** the
store, so the SATB barrier re-read the *new* value and lost the deleted referent. A latent soundness hole for
`Atomic.exchange` / `compare_and_set` on pointers, **shared by bytecode and native** (both go through these C
helpers). Fixed by greying the old referent before the store, mirroring `caml_modify`.

**Validated:** macOS arm64 — cargo + `make world` (bytecode) clean; hello/lazy/atomics stressors byte-identical
under Immix and ConcurrentImmix (the macOS *bytecode* build gate, never previously checked, is now closed).
Linux native (turing) — `world.opt` builds; native ConcurrentImmix runs the 4-domain continuation stressor +
lazy + atomics stressors, all clean; MMTk `sanity`-clean (66 re-traces, 0 Invalid); 21 native ConcurrentImmix
runs, 0 crashes; non-concurrent regression clean. Native plan set is now **7** (adds ConcurrentImmix).
**Open (perf, not correctness):** an UNLOG-bit barrier fast-path gate; the sanity-build-only ~10 MB deadlock
(`rr`, issue #4). (native-on-macOS linking: since verified working — see the 2026-06-24 macOS entry above.)

**rsync stale-binary lesson:** the macOS build cost two restarts — Mach-O `runtime/sak` + `yacc/ocamlyacc`
slipped past `*.o` excludes (truncating generated `prims.c`), and an unanchored `--exclude='ocamlc'` deleted
`boot/ocamlc`. After any rsync, verify the *generated* artifacts + boot binaries, not just `.o`.

---

## M8 macro-benches campaign — partial results (7 of 8 benches, SALVAGED)

*2026-06-23*

The full-campaign agent died twice (a 529, then a watchdog stall on the slow maxRSS pass) but **captured 61
hyperfine cells across 7 benches** — salvaged, not re-run. Only **menhir** is missing entirely; **sedlex** has
only one cell. Subjects: vanilla-5.5.0 vs the fork's Immix/StickyImmix/GenImmix/GenCopy; heap1× = ⌈vanilla
maxRSS⌉ (iso-memory), heap2× = 2×; socket-1 pinned; **non-flambda** switches (fine for these parser/compiler/
data benches). Raw JSONs on turing `~/campaign_results/`.

| bench | vanilla | best @ iso (h1) | best @ 2× (h2) | notes |
|---|---|---|---|---|
| **yojson** | 7.94 s | **GenCopy 7.74 (0.97×, faster)** | GenImmix 7.92 | short-lived, 0-copy — MMTk wins |
| **zarith** | 10.84 s | **StickyImmix 10.33 (0.95×, faster)** | ~10.9 | bignum — MMTk wins |
| **decompress** | 12.92 s | Immix 15.17 (1.17×) | Immix 15.23 | modest |
| **cpdf** | 12.86 s | GenImmix 16.47 (1.28×) | **Immix 13.33 (1.04×)** | near-parity at 2× |
| **ocamlformat** | 6.96 s | GenImmix 13.29 (1.91×) | GenImmix 9.68 (1.39×) | iso cells OOM (others) |
| **merlin** | 10.05 s | StickyImmix 21.94 (2.18×) | **GenImmix 12.46 (1.24×)** | multicore typer; iso thrash |
| **sedlex** | 10.12 s | (iso OOM) | StickyImmix 27.90 | mostly OOM at iso |

**Findings:**
- **MMTk beats vanilla at iso-memory on short-lived / bignum workloads** (yojson 0.97×, zarith 0.95×).
- **Several iso (h1) cells OOM** — MMTk can't fit in vanilla's RSS (the §3.4 memory-premium finding); GenImmix
  is the most heap-tolerant (it often survives iso where Immix/StickyImmix/GenCopy OOM).
- **The gap is heap-pressure, not fundamental:** at 2× heap it largely closes — cpdf Immix **1.04×**, merlin
  GenImmix **1.24×** (vs 2.18× at iso), ocamlformat 1.39× (vs 1.91×). This is exactly the time-vs-heap story
  PERFORMANCE.md §2 predicts — the single iso point understates MMTk.
- **Champion is workload-dependent (RQ2):** GenCopy / StickyImmix / GenImmix / Immix each win somewhere; plain
  **Immix is rarely best**.

**Gaps to fill (do NOT re-run the 7 done):** menhir (entirely), sedlex's iso cells, the maxRSS pass (the agent
stalled mid-pass), and the per-bench fingerprints. **Add for the full native set:** SemiSpace + **native
ConcurrentImmix** (now working). The robust re-run should run cells as background jobs (the one-long-blocking-
job structure tripped the 600 s watchdog).

---

## fft → parity (1.66× → 1.05×): the cause was a post-GC POLL STORM, not per-GC root scan — fixed by refill-at-resume

*2026-06-23* — **this supersedes an earlier wrong diagnosis in this same entry** (kept as a cautionary record).

fft was 1.66× slower under MMTk-Immix; it is now **1.05× (parity)** via a 1-line fix
(`46cb3253f2`, branch `fft-refill-after-gc`). My earlier reading here — "a full STW + root scan every
collection; gen plans don't help because they still root-scan per nursery GC" — **was wrong**, and the
correction is the lesson.

**What fft actually does.** It barely allocates (`minor_words: 1013`) and does **NOT box floats** (they stay
unboxed/in-register even *without* flambda); its memory is a few **large float arrays** (~33.5 MB each,
~67 MB live). Under `MMTK_VERBOSE`: **1 GC at heap ≤128 MB (0 ms STW, 0 objects copied), 0 GCs at ≥130 MB.**
The 128 MB iso heap sits exactly on a cliff (peak-live + the next array pair overflow → 1 GC); at ≥130 MB fft
was already ~1.05×. So there is essentially **no collection** — the root-scan-per-GC story cannot apply.

**The real cause — a post-GC poll storm.** After that one GC, `caml_mmtk_uninterrupt` collapsed the domain's
young region to zero (`young_start == young_end == young_ptr`) so the *next* alloc would refill. But a
collapsed region leaves `young_ptr == young_limit`, so the inlined native fast path **traps into
`caml_call_gc` at every poll/alloc safepoint until a refill happens**. fft's post-GC hot loop seldom
allocates → never refills promptly → **35.9M spurious `caml_garbage_collection` entries** (all `nallocs==0`
polls), each a full `caml_find_frame_descr` stack-walk + pending-action check. So the `caml_call_gc` 10% /
`caml_garbage_collection` 5.6% / `caml_find_frame_descr` 3.5% in the profile were **the poll storm, not
collection** — I misread those symbols as per-GC root scanning. (124 MB: 35.9M entries; 256 MB: 1.
`MMTK_THREADS` irrelevant — mutator-thread cost, not marking.)

**The fix (1 line, `runtime/mmtk.c:caml_mmtk_uninterrupt`).** After the collapse, immediately
`caml_mmtk_refill_tlab(d, …)` — hand the domain a fresh young region (all mutators are stopped in the
GC-worker resume; the same call `caml_mmtk_domain_init` already makes). `young_ptr` is then above
`young_limit`, so the fast path runs straight through — no poll storm. On true OOM the refill returns 0 →
falls back to collapse-then-trap → `Out_of_memory` still raises. Minor-words accounting unchanged. **This is
a general win** — *any* post-GC low-allocation phase paid the storm, not just fft.

**Verified:** fft Immix @128 MB 3.91→2.51 s (1.63×→**1.05×**; vanilla 2.40); all gen plans ~1.05×; the cliff
is gone (124–1024 MB all ~2.5 s). Checksum correct; MMTk `sanity` clean across Immix/Sticky/GenImmix/GenCopy
(+ a heavy typecore compile @64 MB); **no regression on GC-heavy binary_trees d19** (96 real GCs — refill
runs every collection — StickyImmix 8.24→8.10, Immix 20.5→19.7, both slightly *faster*); bug #3c crash-rate
unchanged (orthogonal).

**Corrections to the prior reading.** (1) **#C1 / per-GC root-scan was a red herring *for fft*** (1 GC, 0 ms
STW) — it stays a real lever for genuinely GC-heavy workloads (binary_trees), just not this one. (2) fft does
**not** over-box floats, so a flambda build changes little here (the cost was the large-array GC cliff + poll
storm, not float churn). The residual ~5% is general per-edge/safepoint overhead (#C1/#A1 territory),
orthogonal to this fix. **Lesson:** a profile symbol (`caml_garbage_collection`) can be dominated by *spurious
safepoint polls*, not real collections — confirm GC *count* (`MMTK_VERBOSE`) before attributing cost to GC.

---

## M8 first baseline (PRELIMINARY) — MMTk vs vanilla OCaml 5.5.0

*2026-06-23*

First directional numbers (NOT the campaign — see PERFORMANCE.md for the real protocol). Method: vanilla
5.5.0 vs the fork's `Immix`/`StickyImmix`/`GenImmix`; per-bench heap = `ceil(vanilla maxRSS)` (the maintainer's
iso-memory rule). Detail + commands in `~/baseline_findings.md` on turing (vanilla built at
`~/vanilla-5.5.0-prefix`, benches in `~/bench_work/`).

Results (wall = hyperfine median; maxRSS = `/usr/bin/time -v`). **All three benchmarks are NATIVE binaries.**

| bench (all native) | heap | vanilla | Immix | StickyImmix | GenImmix |
|---|---|---|---|---|---|
| binary_trees d19 — alloc/GC-stress (`ocamlopt`) | 128 MB | 8.32 s / 105 MB | 11.30 s (1.36×) / 153 MB | **7.29 s (0.88×)** / 195 MB | 8.31 s (1.00×) / 186 MB |
| fft — numeric (`ocamlopt`) | 128 MB | 2.21 s / 68 MB | 3.68 s (1.66×) / 139 MB | 3.64 s (1.65×) / 139 MB | 3.67 s (1.66×) / 139 MB |
| `ocamlc.opt` compiling ~400k lines → bytecode | iso 1088 MB | 12.0 s / 1031 MB | **OOM** | **thrash** | 41.9 s / 1.5 GB (overran heap) |
| ″ (working heap) | 4096 MB | 11.98 s / 1031 MB | 25.88 s (2.16×) / 4.1 GB | 23.06 s (1.93×) / 5.2 GB | **20.87 s (1.74×)** / 3.2 GB |

The third row is the macro-benches `ocamlc-self-compile` workload — native **`ocamlc.opt`** (the bytecode
compiler, built native) compiling a generated unit of the **20 JSOO classic benchmarks × 30 replicas** to
bytecode `.cmo`. "self-compile" is a misnomer (it does NOT compile the compiler's own source), and the output
being bytecode does NOT make it a bytecode-*run* program.

**Findings (corrected):**
- **All three benches are NATIVE → they all exercise the native TLAB alloc path. #A1 (bytecode alloc / no-TLAB)
  is UNTESTED here.** The first pass wrongly cited `ocamlc` as confirming #A1; `ocamlc.opt` is native, so its
  cost is general GC overhead, not the bytecode path. Testing #A1 needs a genuinely bytecode-executed workload
  (run under `ocamlrun`), which this baseline lacks.
- **MMTk can beat stock on TIME for GC-stress** — StickyImmix binary_trees **0.88× (12% faster)** — but at
  **+86% RSS** (195 vs 105 MB). Default **Immix is worst** on binary_trees and ocamlc.
- **The native compiler workload is the weak spot:** ~1.7–2.2× slower AND **3–5× the memory**; OOMs at
  iso-memory. Alloc-heavy real app (AST/typing, ephemerons, Hashtbl, Marshal) — GC overhead, not #A1.
- **Memory premium across the board:** MMTk reserves its heap, so RSS ≈ heap + overhead ≈ **1.5–5× vanilla** →
  never truly iso-memory; the time-vs-memory *curve* (PERFORMANCE.md §2) is the real comparison.

**Caveats:** single heap point (+ a 4× point for ocamlc); 3-bench **native-only** subset; this first pass ran
under the **powersave** governor (relative factors valid, absolute soft).

**Blockers now CLEARED:** opam updated to **2.5.1** with sandboxing disabled (the bwrap/userns failure is gone)
and the governor set to **performance** — so the full `macro-benches` campaign (proper protocol: heap-multiple
sweeps, workload fingerprints, ~8–12 benches, two-stage bake-off, incl. a real bytecode-run workload for #A1)
is now running.

---

## Consolidation onto OCaml 5.5.0 — ConcurrentImmix (bytecode) landed; native plan set finalized

*2026-06-23*

Three workstreams consolidated onto a verified branch and landed on `5.5+mmtk` (fast-forward; `4fbe8c1755`
is an ancestor of the 5.5.0 merge):

- **Base advanced rc1 → OCaml 5.5.0 final.** Merged the upstream `5.5.0` tag (6 commits, pure release
  plumbing: VERSION/Changes/Makefile.cross/ocaml_version.m4/regenerated boot+configure/ocaml-variants.opam;
  *zero* runtime/gc/asmcomp changes; only `configure` overlapped, regenerated with autoconf 2.72). Zero
  conflicts; `world`/`bootstrap`/`world.opt` green; compilers report 5.5.0. The vanilla perf baseline is
  now **released 5.5.0** (see PERFORMANCE.md §3).
- **`ConcurrentImmix` + SATB barrier landed (bytecode).** ~82 lines: `mmtk_ocaml_satb_barrier` →
  mmtk-core's slot-granularity `memory_region_copy_pre` (re-using OCaml's existing `(start,count)` barrier
  shape — *not* the object-granularity path, which needs a src object `caml_modify` lacks); `caml_modify` /
  `Array.fill` fire it pre-store, gated on `caml_mmtk_concurrent`, inert off the concurrent plan.
  Availability confirmed (real `PlanSelector` in 0.32; `needs_prepare_mutator` = zero binding work). `lazy`
  proven clean (force-vs-mark + force-vs-relocate; FAQ Q2 / RESEARCH_QUESTIONS RQ1). **Open:** FAQ Q3
  (continuation fiber stacks scanned concurrently vs a resume — fix = vanilla's per-continuation lock, in
  progress); native SATB fast-path + an UNLOG-bit barrier gate.
- **Native plan set finalized at 6** (Immix/StickyImmix/GenImmix/GenCopy/**SemiSpace**/**NoGC**) — SemiSpace
  blessed `sanity`-clean (0 Invalid, 3M+ copied); NoGC native is moot (never reclaims). **`MarkCompact`
  native is INFEASIBLE via TLAB aliasing** (confirmed by two independent agents): it needs a per-object
  reserved Lisp-2 header word + a VO bit that the inlined *gapless* TLAB bump can't produce — first
  compaction panics *"does not have a forwarding pointer"*. `mmtk.c`'s native-abort message updated to name
  the real 6 supported plans + why MarkSweep/MarkCompact are out. (Supersedes the earlier ROADMAP note that
  guessed MarkCompact native was "a small refill-match extension" — it is not.)

All commits authored `KC Sivaramakrishnan <kc@kcsrk.info>` (the main ID; both that and `kc@tarides.com` map
to GH `kayceesrk`, but kcsrk.info is canonical). FAQ.md added — mechanism-level Q&A for these hazards.

---

## Performance work — method of record (`PERFORMANCE.md`) + fast-path audit findings

*2026-06-23*

A multi-agent perf-planning pass (5 parallel read-only audits + synthesis) produced
**`PERFORMANCE.md`** (the measurement method of record — heap-size-multiple sweeps not single
numbers, workload fingerprint first, median+dispersion, GC-vs-mutator split; grounded in
MemBalancer / Distilling-the-Real-Cost / Myths-and-Realities) and a ranked optimization backlog
(`PERFORMANCE.md` Appendix A; summarized in ROADMAP #17). Standing order from the maintainer:
**obvious fast-path removals first, then measure, then deeper levers.**

**Headline static findings (two spot-verified against the tree):**
- **Native small-alloc is byte-for-byte stock** (`asmcomp/amd64/emit.mlp:607-636`, `runtime/amd64.S`)
  — same `sub/cmp/jb` poison-safepoint, no extra branch / dead check / zeroing. **Do not touch it;**
  all native MMTk cost is in slow paths.
- **Bytecode has NO TLAB (the dominant lever, #A1).** `Alloc_small_with_reserved` is `#undef`'d and
  redefined as a per-object `caml_mmtk_alloc_small()` C-call wrapped in the `Setup_for_gc`/`Restore`
  root-publish dance (`runtime/caml/memory.h:263-277`) — where stock/native do a 3-instruction inline
  bump. *Verified.*
- **Double slot-load on every traced edge (#C1).** `FieldSlot::from_address` loads the slot word
  (`slot.rs:92` → `classify`), then `load()` re-reads it via `raw_value()` (`slot.rs:179`/`:103`);
  plus a per-slot `is_in_mmtk_spaces` SFT lookup. *Verified.* Caching the word is a clean quick win
  (care under moving plans where the word can change between classify and trace).
- **Native write barrier is a no-op *call* under the default Immix plan (#B1).** Every pointer store is
  an unconditional out-of-line `caml_modify` (`cmm_helpers.ml:2290`) → `caml_mmtk_region_barrier`,
  which returns immediately when `caml_mmtk_generational==0`. Stock inlines the test.

**GOTCHA — `runtime_events` is BROKEN under MMTk (blocks olly).** The real STW window
(`gc/mmtk/binding/src/collection.rs:224-287`, pause-start `:230`, elapsed `:287`) emits **zero**
`caml_ev_*` events — nothing in `gc/mmtk/` or `mmtk.c` writes the ring. Meanwhile the *surviving*
stock spans wrap neutered no-ops: `EV_MAJOR` brackets the inert `caml_major_collection_slice`
(`domain.c:1951`, `major_gc.c:1012`); `EV_MINOR`/`EV_EMPTY_MINOR` wrap the dead minor; the words
counters read stock fields that are 0 under MMTk (`EV_C_MINOR_ALLOCATED_WORDS` =
`young_end-young_ptr` = 0 in bytecode; `EV_C_MINOR_PROMOTED_WORDS` structurally always 0). **So olly
reports fictional tiny pauses + zero/wrong words.** Until the fix (backlog #R1–#R4: emit a real
GC-STW span around the MMTk pause from a domain with a ring slot, source words from the MMTk
odometer, stop the phantom spans), **get pause times from `bpftrace` uprobes, not olly.** The true
numbers live in `MMTK_VERBOSE=1` (`GCs / GC time / objects copied`) and `caml_mmtk_gc_stats`.

**Prereqs that don't exist yet:** a lifetime-dispersion (Gini) profiler + a per-GC survival/mutation
meter — RQ2's per-benchmark workload fingerprint needs both (backlog #P1/#P2). Host `turing`:
governor is `powersave` (set `performance` before timing); olly not installed; perf/turbo already OK.

---

## ConcurrentImmix / SATB — the `lazy` hazard (open research question; implement-and-test-breakage)

*2026-06-23*

Recording the design hazard before the work starts (RQ1 flagship; ROADMAP open work #8). Two parallel
agents are live: a **native-batch** agent (MarkCompact native + bless SemiSpace/NoGC native + fix the
`mmtk.c` native-abort message) and a **ConcurrentImmix** agent (diagnose availability in mmtk-core 0.32,
then wire the SATB barrier + the lazy coverage + *characterise the breakage*). This note is the analysis;
the agents' findings land in `~/native_batch_findings.md` / `~/cimmix_findings.md` on turing.

**The SATB-is-OCaml-native point (de-risks the barrier).** OCaml's *stock* mostly-concurrent major GC is
*itself* an SATB marker: `caml_modify` greys the **old** referent on overwrite (Yuasa 1990 deletion
barrier). bug-#3's work rewired `caml_modify` → `caml_mmtk_modify` to MMTk's **generational**
(slot-remembering) region barrier, gated on `caml_mmtk_generational`; M9 deleted the stock concurrent
major. So **no SATB path is wired today**, but the *shape* is native to the runtime/codegen —
ConcurrentImmix re-introduces a known mechanism (gate SATB greying on the concurrent plan; feed
mmtk-core's marker), it does not invent one. Reference for the binding-side barrier surface:
`_references/mmtk-openjdk` (the JIT emits the inline fast-path; the Rust binding exposes the
`object_reference_write_pre/post` slow-paths; mmtk-core's plan constraints pick SATB vs object-remembering).

**The `lazy` corner (the sharp open question).** OCaml is immutable-by-default, but **forcing a `lazy`
mutates the suspension in place** — it overwrites the thunk + its captured environment with the result, or
installs a `Forward_tag`. Under concurrent marking that is the canonical hazard, two distinct failure modes:
1. **Missed deletion barrier.** The thunk's captured env may be reachable *only* through the suspension;
   if forcing doesn't grey the old suspension, the concurrent marker loses it → collected-while-referenced
   → dangling. So the **lazy-forcing path itself** must route through the SATB barrier — verify whether it
   goes through `caml_modify`/the lazy update primitives or a raw store (if raw, add the barrier there).
2. **Force-vs-mark race.** The tag transitions `Lazy`/`Forcing` → `Forward`/result *while the marker scans
   the block*; multi-domain forcing adds OCaml's `Forcing`/`Undefined` protocol. The binding's
   `scan_object` must not mis-scan a half-updated lazy.

**Why it's research, not just engineering.** It's a falsifiable probe of RQ1's thesis: if OCaml's
immutability is what makes concurrent GC cheap, `lazy` is the one place the SATB obligation concentrates.
Whether OCaml's *own* lazy/SATB protocol composes cleanly with a *third-party* concurrent marker (vs
OCaml's bespoke one) is genuinely open — a clean compose strengthens RQ1; a fundamental conflict is itself
a publishable language-runtime/GC-framework impedance finding (extends RQ4 into the concurrent regime).
**Method:** implement, then deliberately break it — heavy multi-domain forcing under concurrent marking at
a small heap with `sanity` on; classify each break fixable (missing barrier) vs open (protocol gap).
**Gate:** re-enable the disabled `lazy/…force` testsuite test. **Coverage must also include** `Obj.set_field`/
`set_tag`/`Obj.truncate` (the other edge-deleting in-place mutations).

---

## Native GenImmix + GenCopy (copy-nursery TLAB aliasing) — the stock-faithful native default

*2026-06-23*

Native code now runs **GenImmix** (and **GenCopy**), not just Immix/StickyImmix. GenImmix is the
stock-faithful model for OCaml (copying nursery + mark mature ≈ OCaml's own copying-minor +
mark-major; generational fits the high-rate, mostly-short-lived allocation profile) → the candidate
native default.

**Why it was a 2-file change (the key finding).** The binding has **no minor-vs-major root path**:
*every* MMTk collection — a GenImmix nursery (`CopySpace`) evacuation or a full GC — runs the same
`stop_all_mutators → scan_roots_in_mutator_thread → caml_do_roots → scan_stack_frames` (fiber.c),
reporting each native `gc_regs`/stack root as an **updatable `FieldSlot`** (`create_process_roots_work`).
That is the identical machinery Immix opportunistic defrag already uses to move *mature* objects and
fix native roots, so a GenImmix minor evacuation — which moves the native young objects — reuses it
verbatim. **No new moving-root machinery was needed.**

**The only gap: TLAB allocator selection.** `mmtk_ocaml_refill_tlab` (api.rs) hard-required
`AllocatorSelector::Immix`. GenImmix/GenCopy's Default allocator is a `BumpAllocator` over the nursery
`CopySpace` (`BumpPointer(_)`). Generalized the refill to match `Immix(_)` → `ImmixAllocator` (in-place)
**and** `BumpPointer(_)` → `BumpAllocator` (copy-nursery), via a macro stamping the same proven
probe / eject-cursor / retry loop for each (both expose `pub bump_pointer: BumpPointer{cursor,limit}`).
The in-place Immix path is unchanged; `mmtk.c` only got comment / fatal-error / `MMTK_VERBOSE` wording
("copy-nursery" vs "Immix"). 2 files, native-only. Native MarkSweep (free-list) / PageProtect (no
bump) still abort native by design; native `SemiSpace`/`MarkCompact` deferred.

**Verified:** native GenImmix + GenCopy boot; `ocamlopt.opt -c typing/typecore.ml` compiles; a proper
**old→young A/B** (young POINTERS held in a mature array across 4000 nursery-evacuation rounds at a
tight 64 MB heap) is correct for GenImmix + GenCopy (+ Immix/StickyImmix regression — all four clean);
MMTk `sanity` under native GenImmix (typecore, 300 MB) — **3,054,450 objects copied, 0 Invalid
reference**; byte-identical exit-0 across all plans; clean `world.opt`. (A rare 4-domain/tiny-heap
sanity crash is the pre-existing bug #3c — StickyImmix crashes identically — not a GenImmix defect.)

---

## Workstreams archive (migrated from ROADMAP, 2026-06-23)

*2026-06-23*

When ROADMAP.md was slimmed (718→~214 lines) its `## Workstreams (A–I)` section and the
three overlapping status views were removed: almost all of that content was already
covered by the dated entries in this file (bugs #1–#4, #15, the M9 stages, opam
relocatability, native TLAB, parallel collection, pinning, weak/ephemeron/finaliser,
plan matrix, etc. — see the entries below). This entry preserves the **one piece that
was ROADMAP-unique and not already here**: the M8 benchmark baseline + optimisation
levers (ROADMAP Workstream H). Everything else from Workstreams A–I is unchanged in
substance and lives in the dated entries below; ROADMAP now points here.

**M8 first benchmark baseline (2026-06-20, `gcbench` native, single-domain, large
persistent live set ~192 MB + heavy churn — a GC-heavy worst-ish case):**

| Config | wall | RSS |
|---|---|---|
| stock GC | **3.85 s** | 440 MB |
| MMTk Immix 512 MB | 14.1 s | 524 MB |
| MMTk Immix 1024 MB | 7.0 s | 1.0 GB |
| MMTk Immix 2048 MB | 5.9 s | 2.1 GB |
| MMTk StickyImmix 1024 MB | **5.4 s** | 1.25 GB |

So today MMTk is **~1.4–1.8× slower and uses more memory** here. Two structural reasons
(not bugs): (1) **fixed heap** — MMTk reserves the whole `MMTK_HEAP_SIZE_MB` (RSS ≈ heap;
tight heaps thrash: 512 MB → 14 s) where stock auto-sizes; (2) stock is **generational**,
so its frequent collections don't re-trace the old set, whereas non-gen **Immix re-traces
the whole 192 MB live set every GC**. A *generational* MMTk plan (**StickyImmix**) already
closes much of the gap, and more heap headroom helps.

**Optimisation levers + first-round results (2026-06-20):**

1. **Dynamic heap sizing — TRIED, REGRESSED, reverted.** *(SUPERSEDED 2026-06-24 — dynamic
   heap `DynamicHeapSize:16 MiB,RAM` IS now the default; the gcbench thrash below did not
   reproduce in re-test. See the top entry.)* Switched `gc_trigger` to
   `DynamicHeapSize:32M,cap`. On `gcbench` it *thrashed* — one run took >190 s (vs 5.9 s
   fixed) because it starts at 32 MB against a ~192 MB live set and mmtk 0.32's grow
   heuristic ramps too slowly. A small-min dynamic heap is *worse* for large-live-set
   programs. Reverted to `FixedHeapSize`. Future: a much larger/auto min, or investigate
   mmtk's MemBalancer trigger.
2. **Generational plan (StickyImmix) — faster** (`gcbench` 5.4 s vs Immix 7.0 s, ≈1.4×
   stock; TLAB-compatible). The StickyImmix bootstrap SEGV and CI bug #2 that once blocked
   making it the default are both fixed (see the `slot.rs` Infix_tag and bug #2 entries
   below); Immix remains the default for now.
3. **Inline the bytecode allocation fast path** — bytecode all-MMTk calls
   `mmtk_ocaml_alloc` per object (vs stock's inlined bump); inline a bump fast path.
4. **GC-thread count** — default is `nproc` (e.g. 28) *per process* (a big chunk of the
   slow self-hosting bootstrap); a smaller default helps short programs but a long
   GC-heavy run wants parallel marking — needs a balanced default.
5. **Immix defrag/policy tuning** — reduce TLAB-refill overhead; revisit the LOS
   threshold.

(All of these are M8 / ROADMAP open-work #17, and tie to `RESEARCH_QUESTIONS.md`.)

---

## bug #3b: multidomain spawn/STW deadlock — FIXED via an MMTk-native per-mutator STW

*2026-06-23*

**Status: FIXED.** bug #3b was the residual after bug #3 (the blocking-section counter underflow):
a ~20–35% **hang** in `parallel/domain_*_spawn_burn*` + `domain_dls` (and the CI debug-matrix exit
-9 timeouts).

**Root cause — the binding's hand-rolled global stop-counter STW was the wrong shape.**
`stop_all_mutators` (binding `collection.rs`) waited on a single global `stopped` counter to reach
`number_of_mutators()`. bug #3 fixed the *balance* of the `+1`/`-1` across blocking sections, but the
counter design itself can't represent all the states a mutator passes through. The deadlock: a parent
domain wedged in `caml_domain_spawn`'s handshake wait is a **registered mutator that is neither at a
safepoint nor safe-stopped** — busy in OCaml's own spawn/STW handshake, not polling MMTk's stop flag,
not counted as stopped — so MMTk's barrier never reached `number_of_mutators()`. OCaml's STW and
MMTk's STW deadlocked (rr on a captured hang: wedged in the OCaml spawn handshake, no `caml_mmtk_*`
frame on the GC path).

**Fix — MMTk-native per-mutator stop state.** Replaced the global `stopped` counter with a per-mutator
**RUNNING set** (a `HashSet` of `caml_domain_state` addresses currently executing OCaml, in the
binding's lock-guarded `StwState`). `stop_all_mutators` now waits for `running.is_empty()`. A domain
is born **STOPPED** at `bind_mutator` and is RUNNING only between "(re)entered OCaml" and "left OCaml
/ parked / blocking / terminating." Idempotent set semantics make the bug #3 underflow **structurally
impossible**; a transitioning domain (booting/terminating/blocked) is simply **absent** from the
awaited set (bug #3b). Four coordinated pieces:
1. **Running-set accounting** (`collection.rs` / `active_plan.rs`) — the barrier predicate.
2. **Cooperative RUNNING transition** (`caml_mmtk_become_running` → `mmtk_ocaml_try_mark_running`): a
   STOPPED→RUNNING edge marks RUNNING iff no GC is active, else the domain **parks cooperatively**
   (releasing its domain lock so the backup thread answers OCaml's own STW) and retries — which also
   fixed a **second deadlock** (MMTk-STW vs OCaml's minor-heap STW `caml_empty_minor_heaps_once`: a
   domain never spins on GC-active while holding its domain lock). `park` waits on `gc_active` (written
   under the STW lock), not an epoch → no lost wakeup.
3. **Terminate fence:** deregister (remove from registry + running set) **then** wait out any in-flight
   collection before teardown — replaces `caml_mmtk_park_terminating` and fixed a **bug #3 `cannot
   trace` corruption** surfaced when the terminate park was first removed.
4. **Spawn handshake:** the parent's idle wait is bracketed STOPPED; the child becomes RUNNING only
   when it starts executing OCaml.
**Removed:** the global `stopped` counter, the `>= number_of_mutators()` barrier,
`caml_mmtk_park_terminating`, and the underflow class. Touches
`gc/mmtk/binding/src/{collection,active_plan}.rs`, `gc/mmtk/include/mmtk_ocaml.h`,
`runtime/{mmtk.c,domain.c,caml/mmtk.h}`.

**Verified:** native `domain_dls` 14/30 hang → **30/30** (agent), 25/25 (independent forced-clean
re-run), 0/12 (integrated mainline); burn 26/30 → 30/30; bytecode dls/stress 30/30; MMTk `sanity`
(48 MB, multidomain, Immix+StickyImmix) no panic; **0 crashes** across all sweeps; checksums identical
(1919992825); gc-roots 4/4; clean `world.opt`.

**Residual → bug #3c (separate, pre-existing).** A rare hang (~2/30 *bytecode* burn; ~0–1/20 native
burn; dls/stress 30/30) persists **only** in the `burn` pattern (3 driver domains hammering
`Gc.minor`/`Gc.major` + 25-way spawn bursts) — a different race: a GC during the tight `Gc.minor`
OCaml-minor-STW loop and/or during `caml_mmtk_refill_tlab` at domain init (child holds
`all_domains_lock`, no backup thread). Fix sketch: route `Gc.minor` to MMTk (don't run OCaml's own
minor STW) and/or suppress collection (`is_collection_enabled`) around the init-time TLAB refill. The
earlier wild-pointer `cannot trace` residual was not observed in any post-fix sweep (0 crashes).

---

## #15: GC-plan wiring status — generic plan dispatch, and why Compressor / ConcurrentImmix are deferred

*2026-06-23*

Status of wiring mmtk-core 0.32's 11 plans. **Nine are wired and validated** (Immix, StickyImmix,
GenImmix on bytecode + Immix/StickyImmix native; MarkSweep, NoGC, SemiSpace, GenCopy, MarkCompact,
PageProtect on bytecode). The remaining **two are deferred** (Compressor, ConcurrentImmix); this note
records the generic plan dispatch, the one per-plan subtlety, and why those two are genuinely deferred.

**The binding is generic over the plan — wiring a bump-pointer plan was mostly validation.**
`mmtk_ocaml_init` (binding `api.rs`) passes `MMTK_PLAN` straight to mmtk-core
(`memory_manager::process(&mut builder, "plan", plan_str)`); there is no hardcoded plan allowlist.
Moving-vs-non-moving is handled generically: the forwarding-bits side-metadata spec is registered
**iff** the plan `moves_objects` **and not** `needs_forward_after_liveness`. This is the **bug #1
fix** generalised twice: (a) a non-moving plan (NoGC, MarkSweep) never maps the forwarding-bits
metadata, so registering the spec for it made `is_forwarded()` read unmapped side metadata and SEGV
on the first `Infix_tag` header; (b) the **forward-after-liveness movers** (`MarkCompact`,
`Compressor`) also don't map that spec — they forward via their own offset-vector metadata after a
liveness pass — so they need the same exclusion (MarkCompact SEGV'd in `slot::is_forwarded` until
gated). `moves_objects && !needs_forward_after_liveness` is the precise discriminator. Because the
spec, object model, and scanning are otherwise plan-independent, `SemiSpace`, `GenCopy`, `MarkCompact`,
and `PageProtect` came up bytecode-wired behind that one-line gate — CLBG byte-identical; the work was
bring-up + cross-plan validation, not trait code. (`GenCopy` also needs `caml_mmtk_generational` set — the C glue already includes it in
the generational set.) These are the next plans to validate, **bytecode-first** (none has a native
Immix nursery allocator; native bump-pointer support is M8, ROADMAP #16).

**Two deferrals, with reasons:**

1. **`Compressor` — deferred: needs a unified object-reference model.** mmtk-core's Compressor is a
   bitmap mark-compact that assumes a single object reference equal to the object start. OCaml's
   value layout is incompatible: the value reference points at field 0 with the header one word
   *before* it (`OBJECT_REF_OFFSET = WORD_SIZE`), so there is no single "object reference ==
   object start" identity for Compressor's bitmap addressing to use. Supporting it means redesigning
   the object model around a unified reference, not just flipping a plan flag. (The all-plans CI
   gate even greps for Compressor's `requires a unified object reference` abort so the deliberately-
   red matrix classifies it correctly.)

2. **`ConcurrentImmix` — deferred: needs an SATB write barrier.** This is the only high-value
   unwired plan (the low-latency / concurrent line; see `RESEARCH_QUESTIONS.md`). Our generational
   write barrier is a **slot-remembering region barrier** (`memory_region_copy_post`, matching
   OCaml's slot-based remembered set) — it is *not* snapshot-at-the-beginning. ConcurrentImmix's
   concurrent marking needs an SATB barrier (grey the old referent on overwrite, à la Yuasa) so the
   mutator can't hide a live object from the concurrent marker. Implementing SATB is real work
   (high effort), but it is the highest-payoff unwired plan — interesting precisely because OCaml's
   own collector is SATB and the language is immutable-by-default (most writes are barrier-free
   initialising writes), so the cost model may differ sharply from the imperative-language
   measurements in the literature.

---

## M9 #8: Is_young address-space reservation retired + header-colour audit — M9 cleanup complete

*2026-06-23*

The last M9 cleanup item: **retired the `Is_young` address-space reservation** (the counterpart #6
deliberately KEPT). Under TLAB nursery-aliasing nothing is ever allocated in
`[caml_minor_heaps_start, caml_minor_heaps_end)` — young objects live in MMTk Immix blocks outside
it — so `Is_young(v)` is **always false** at all ~8 call sites (`weak.c`, `finalise.c`, `memprof.c`,
`globroots.c`, `obj.c`, `intern.c`, `fiber.c`, `minor_gc.c`, `array.c`). Audited each; the
always-false branch is the MMTk-correct behaviour. Folded `Is_young(val)` →
`(CAMLassert(Is_block(val)), 0)` and `Is_block_and_young` → `(Is_block(val) && 0)` — a **constant-fold
of an already-false macro, byte-identical to prior runtime behaviour** (the safest way to remove the
dead machinery, esp. for the #11-entangled weak.c/finalise.c, whose semantics are unchanged). Removed
`caml_minor_heaps_start/_end`, the `minor_heap_reservation_{start,end}` per-domain fields,
`reserve_/unreserve_/domain_resize_minor_heaps_reservation_from_stw_single` (incl. the reservation
`caml_mem_map`/`unmap`), and simplified `stw_resize_minor_heaps_reservation` to bump the scalar cap
`caml_minor_heap_max_wsz` under the global barrier. −190/+54 lines; **no binding change**.

**Synergy with #21:** deleting `unreserve_minor_heaps_reservation_from_stw_single` removes the
`domain.c:605` debug assert (`young_start/end == NULL` for a running domain — a stock-arena invariant
invalid under MMTk TLAB) that was reddening the CI debug-matrix Build.

**Header colour/mark audit (task a) — no code change.** No *live* runtime-C path reads the stock
header colour for liveness under MMTk: `caml_gc_phase` never advances past `Phase_sweep_main`, the
`caml_darken`/major-slice drivers are inert, and the M6 weak/finaliser path queries MMTk reachability,
not colour bits. Colour *construction* (`Make_header(…, NOT_MARKABLE)`, `caml_allocation_status`) is
correct and kept. **Flagged (out of scope):** `memprof.c:1558` reads the stock colour for liveness —
latent if memprof is ever wired to MMTk (memprof is currently unsupported).

Verified (agent + an independent main-agent re-check on a forced-fresh build): forced `world.opt`
clean; MMTk `sanity` (full-heap re-trace) at small heaps (24–64 MB), bytecode + native, Immix +
StickyImmix, heapstress + gc-roots — no panic; multidomain stress 13/13 identical checksums across
Immix/StickyImmix (independent re-run); gc-roots 4/4; weak-ephe-final 14/14 (Immix). One pre-existing
failure (NOT a regression, A/B-proven by reverting): `weak-ephe-final/weaklifetime.ml` asserts under
StickyImmix (line 53) — weak-clear timing tied to stock generational promotion pacing MMTk doesn't
reproduce (#11). **M9's stock-GC excision is now complete bar #11 (weak semantics) + the memprof
colour flag.**

---

## bug #4: gc_regs bucket leak on OOM-raise inside caml_call_gc — FIXED

*2026-06-23*

A **deterministic native SIGSEGV** compiling a large module under a tight **Immix** heap:
```
MMTK_PLAN=Immix MMTK_HEAP_SIZE_MB=64 setarch x86_64 -R \
  ./ocamlopt.opt <std native flags> -c typing/typecore.ml -o /tmp/tc.cmx
```
4/4 at 64 MB; **0/4 at 96 MB+**; **Immix only** (StickyImmix at 64 MB is clean). `rip =
caml_call_gc+8`, faulting `movq %r11, 0x58(%r15)` (SAVE_ALL_REGS) with
`%r15 == Caml_state->gc_regs_buckets == 0`. (The testsuite triage's top "native ocamlopt
SIGSEGV"; the `parser.ml` *native* crash no longer reproduces — bug #3's STW fix resolved that.)

**Root cause (rr-confirmed): a gc_regs-bucket free-list leak on an exception raised from inside
`caml_call_gc`.** `caml_call_gc` (amd64.S) runs SAVE_ALL_REGS, which **pops** a bucket off the
free-list (`gc_regs_buckets` → NULL, `gc_regs` → the bucket) and relies on RESTORE_ALL_REGS
pushing it back on return. Under MMTk's TLAB nursery the alloc slow path
(`caml_alloc_small_dispatch`, minor_gc.c) fails to refill at a tight heap and calls
**`caml_raise_out_of_memory()` from inside that window** — the raise (`caml_raise` →
`caml_raise_exception`) longjmps straight to the OCaml handler, **never returning to
`caml_call_gc`, so RESTORE_ALL_REGS never runs** and the popped bucket is never pushed back. The
free-list holds exactly one bucket (steady state for a single-domain native program — confirmed
by a forward watchpoint showing `gc_regs_buckets` only oscillating between one address and NULL),
so it is left stuck NULL. The compiler's `Misc.try_finally`/`Fun.protect` backtracking
**catches** the `Out_of_memory`, execution resumes, and the next allocation's `caml_call_gc`
SAVE reads NULL into `%r15` and faults. **Immix + tight-heap only** because only there does the
refill genuinely fail mid-compile while the type-checker survives the caught OOM.

**Fix** (3 files): `caml_mmtk_recycle_gc_regs_bucket()` (fiber.c) pushes the in-use `gc_regs`
bucket back onto the free-list (idempotent; guarded on `gc_regs_buckets==NULL && gc_regs!=NULL`),
called right before the `caml_raise_out_of_memory()` at the TLAB-refill-failure site (minor_gc.c)
— exactly what RESTORE_ALL_REGS would have done. The saved register values are discarded, correct
since the exception abandons that computation. **Verified:** repro **25/25 SIGSEGV → 0/25** at
64 MB (independently re-run **20/20** clean on the integrated tree); `make world.opt` clean;
multidomain spawn-burn 8/8 under Immix + StickyImmix; tighter heaps now raise a clean
`Out_of_memory` instead of crashing.

**Correction (process note — keep this).** An earlier same-day instrumentation pass *wrongly
refuted* this exact OOM-raise hypothesis: it reported `oom_raises=0`, `depth_max=0`, and "keeping
a spare bucket doesn't help," and concluded "memory corruption." rr proved
`caml_raise_out_of_memory` **is** reached and the fix at that site eliminates the crash. The
instrumented runs were almost certainly **stale binaries**: sources were `rsync -a`'d (preserves
mtimes), so `make` saw the `.o` as newer than the source and **skipped recompiling the changed
file**. Lesson: after rsyncing sources to the build host, **force the recompile** (`rsync
--no-times`, or `touch` the changed files, or verify the `.o` mtime advanced) before trusting an
A/B result — and for a moving-GC corruption-vs-leak question, prefer **rr** over `fprintf`
instrumentation (which a crash can also drop). Confirmed mechanism beats guessed mechanism.

---

## opam relocatability: `libmmtk_ocaml.a` linked relocatably + DWARF build-root stripped

*2026-06-22*

The CI `opam installation` job's `test-in-prefix` (`testsuite/tools/testRelocation.ml`) failed
— two distinct build-dir leaks:

1. **Absolute archive path in config.** `configure.ac` (~3019) substituted the absolute build
   path `$ac_pwd/gc/mmtk/target/release/libmmtk_ocaml.a` into `{bytecomp,native}_c_libraries`,
   which is baked into `config.cmx` / `ocamlcommon.cma` / the compiler binaries (and the archive
   was never installed into the prefix). Fix: reference it relocatably as **`-lmmtk_ocaml`**,
   exactly like the stock C libs — `ocamlc`/`ocamlopt` already pass `-L<standard-library>`
   (`Ccomp.call_linker` prefixes every `Load_path` dir with `-L`). The archive is **symlinked into
   `stdlib/`** during the build (`Makefile`: `stdlib/libmmtk_ocaml.$(A)`, a `runtime` prereq,
   mirroring `stdlib/libcamlrun.a`) so `-lmmtk_ocaml` resolves in-tree, and **installed into
   `$(LIBDIR)`** via `common-install` (using the `$(ROOTDIR)`-relative `MMTK_LIB_REL` — opam/clone/
   list install modes record sources relative to `$(ROOTDIR)`; an absolute path → broken
   `.install`).
2. **Build root in the archive's DWARF.** `gc/mmtk/Cargo.toml` sets `[profile.release] debug =
   true`, so Cargo embeds the absolute build root in `libmmtk_ocaml.a`'s DWARF, propagated by the
   linker into every native binary AND the installed archive. Fix: `Makefile.mmtk` passes
   `RUSTFLAGS=--remap-path-prefix=$(abspath $(ROOTDIR))=.` to the cargo build — the Rust analogue
   of the C toolchain's `-fdebug-prefix-map` (`cc_has_debug_prefix_map`). (Cargo registry paths
   under `~/.cargo` aren't the build root, so the check ignores them; only `$(ROOTDIR)` needs
   remapping.)

`configure` regenerated with autoconf 2.72 (reproducible — re-running `tools/autogen` is
byte-identical). Verified on turing: fresh `distclean`+`cargo clean` → configure → `make world.opt`
→ install all OK; `test-in-prefix` **exit 0** ("relocatable and reproducible", 0 build-dir
occurrences in the installed archive); and `-custom` bytecode + native programs compile/link/run
from the installed prefix **with the build tree moved away**.

**Residual (local-iteration footgun, NOT a CI issue):** the native `.opt` binaries link the archive
via the runtime `Config.*_c_libraries` flag, not a Makefile prerequisite edge (same as
`libasmrun.a`), so an *incremental* rebuild that changes only `libmmtk_ocaml.a` won't auto-relink
them. CI always builds fresh, so it's correct there. Locally, after rebuilding the binding `rm` the
affected `.opt` binaries (or `make clean world.opt`) before re-checking relocatability.

---

## Bug #3: MMTk STW stop barrier was a no-op (blocking-section counter underflow)

*2026-06-22*

**The multidomain moving-GC crash** (`parallel/domain_*_spawn_burn*`: MMTk panic
`cannot trace object 0x1 / 0x11 … does not belong to any MMTk space`, where `0x1`/`0x11`
are `Val_int(0)`/`Val_int(8)`) was the GC **scanning a domain that had not actually
stopped** — tracing its live, mutating fiber stack / `gc_regs`, where a slot held a tagged
immediate at trace time. Deterministic under Immix at a small heap (5/5); pre-existing
(pristine HEAD crashes too — not introduced by the shared-heap / linux-O0 work).

**Root cause.** `stop_all_mutators` (binding `collection.rs`) waits for a `stopped` counter
to reach `number_of_mutators()`. That counter is `+1` by `caml_mmtk_enter_blocking` and `-1`
by `caml_mmtk_leave_blocking` (called from `caml_enter/leave_blocking_section`, signals.c).
But the blocking-section hooks release/re-acquire `domain_lock` **asymmetrically around those
calls**: `enter` runs AFTER `caml_enter_blocking_section_hook` → `caml_release_domain_lock`
set `caml_state = NULL`, so the old guard (`if Caml_state[_opt] != NULL`) skipped the `+1`;
`leave` runs AFTER the hook re-acquired the lock (`Caml_state` valid) → still did the `-1`.
Each blocking round therefore netted `stopped` **down by one** → as a `usize` it
**underflowed to ~UINTPTR_MAX** → `stopped >= n` was always true → the stop barrier became a
**no-op**: the GC never waited for running domains and scanned their live roots. (Light
multidomain with no blocking sections mostly escaped; heavy concurrent spawn+alloc+GC
reliably tripped it — which is why earlier lighter multidomain checks passed.)

**Fix** (`runtime/signals.c`, `runtime/mmtk.c`, `runtime/caml/mmtk.h`): capture the domain's
`caml_domain_state*` in `caml_enter/leave_blocking_section` **while `Caml_state` is still
bound** and pass it to `caml_mmtk_enter_blocking(dom)` / `caml_mmtk_leave_blocking(dom)`,
which test the passed `dom` (not the now-NULL `Caml_state`). enter/leave are balanced, the
count is accurate, the barrier waits. 3 C files; no binding/Rust change. (Supersedes the
linux-O0 `Caml_state_opt` guard in those two functions.) Verified on turing: 0
immediate-as-root crashes across ~90 runs (was 5/5); multidomain + canonical `parser.ml`
regress clean.

**Still open — separate, pre-existing issues, NOT this bug and NOT caused by the fix:**
- A **~20–35% hang** in the spawn-burn tests — a deadlock in OCaml's own domain spawn/STW
  machinery (`caml_try_run_on_all_domains` / `all_domains_lock` / backup thread), outside
  MMTk's collection path (rr on a captured hang hit no `caml_mmtk_*`). CLAUDE.md already
  lists these GC-burn tests as known hangs under MMTk.
- A **deterministic SIGSEGV on native `ocamlopt` compiles** (e.g. `parser.ml` → `.cmx`);
  reproduces 3/3 on pristine HEAD — a separate native-code GC crash.
- A **rare residual** (~1/30) `cannot trace` panic with a **wild garbage value** (not the
  `0x1`/`0x11` immediate) — a different, rarer stale-root-slot race (freed fiber stack /
  reused `gc_regs` bucket / a terminating domain's torn-down finaliser/ephemeron structures;
  one instrumented hit had root source 0 = globals/finalisers). An attempted stronger
  identity-set stop barrier did not reduce it and added deadlock surface (reverted). Needs a
  fresh rr capture targeting a wild-pointer (not immediate) crash. Tracked as **bug #3b**.

---

## Minor-heap arena removed (+ a shared_heap.h-include fallout)

*2026-06-22*

Removed the stock per-domain minor-heap **arena** (the committed minor heap). Under TLAB
nursery-aliasing the domain's `young_*` are bootstrapped by `caml_mmtk_refill_tlab` (called
from `caml_mmtk_domain_init` at domain create) pointing at an MMTk Immix block — so
`allocate_minor_heap_arena`'s `young_*` setup was always immediately overwritten and the
arena mmap unused. Deleted `allocate/free/reallocate_minor_heap_arena`; domain create now
just sets `minor_heap_wsz` to the nominal size (for `Gc.stat`/`Gc.get` + minor-table sizing)
and leaves `young_*` NULL until the refill — verified nothing allocates an OCaml value in the
create window before `caml_mmtk_domain_init` (it's all `caml_stat`/mmap). domain terminate,
`caml_set_minor_heap_size`, and `stw_resize_minor_heaps_reservation` no longer touch an arena.
At the time, **KEPT the address-space reservation** (`caml_minor_heaps_start/end`) because
`Is_young(v)` (address_class.h) is `v ∈ [start,end)` — retiring it was entangled with
young-object classification. **(Later RETIRED by #8, 2026-06-23: the consumers were audited,
`Is_young` was confirmed always-false everywhere and folded to a constant, and the reservation +
its STW machinery removed — see the #8 entry above.)** Verified on
turing: multidomain spawn/terminate (Immix+StickyImmix), gc-roots, native old→young
(StickyImmix), simple programs.

**Include fallout from the earlier `shared_heap.h` deletion:** deleting `caml/shared_heap.h`
broke two *testsuite* C files that `#include`d it — `gc-roots/globrootsprim.c` (needs
`NOT_MARKABLE`, now in `caml/major_gc.h`) and `cxx-api/all-includes.h` (dropped). The deletion
grepped `runtime/` but not `testsuite/`. Lesson: when deleting a `caml/` header, grep the
WHOLE repo — testsuite C stubs include `caml/` headers too. (These were surfacing as
gc-roots *compile* failures, easily mistaken for runtime bugs.)

---

## linux-O0 `tests/parallel`: `check_minor_heap` asserts + a real domain-terminate race

*2026-06-22*

The `-O0` job's 28 `tests/parallel` failures were two distinct things (not the memprof
assert first guessed):

1. **`check_minor_heap` (domain.c) stale stock-arena asserts — 21 tests, DEBUG-only.**
   Its `young_ptr == young_end` and "`young_{start,end}` within
   `minor_heap_reservation_{start,end}`" asserts are stock per-domain-arena invariants.
   Under native TLAB nursery-aliasing `caml_mmtk_refill_tlab` repoints `young_*` at an
   MMTk Immix block (unrelated to the stock reservation) and resets `young_ptr` to
   `young_start` after a collection, so neither holds. Reached from
   `free/allocate_minor_heap_arena` on every domain teardown → every native
   domain-spawning test tripped it. Dropped both (kept the log). Same class as
   `minor_gc.c:439`.

2. **A real domain-terminate lock-drop race — RELEASE-affecting, fixed at the source.**
   `caml_domain_terminate → caml_mmtk_domain_terminate` parked for an in-progress MMTk
   collection via the regular park, which RELEASES `domain_lock` (handing OCaml-STW duty
   to the backup thread) to avoid an MMTk-vs-OCaml-STW barrier deadlock. But
   `caml_domain_terminate` relies on holding `domain_lock` continuously across teardown
   to stop a fresh domain from REUSING the slot's `caml_domain_state` mid-teardown
   (`domain_create` blocks on the same `d->domain_lock`). The lock-drop broke that: a
   reusing domain observed half-torn-down state → debug: `memprof == NULL` assert
   (domain.c:895); **release: `mmtk_ocaml_bind_mutator: domain … already registered`
   panic** + double memprof handling. Fix: `caml_mmtk_park_terminating()` (mmtk.c) parks
   (`mmtk_ocaml_stw_park`: stopped++/wait/stopped--, satisfying MMTk's barrier) WITHOUT
   releasing `domain_lock`. Safe because by terminate the domain has left the OCaml STW
   participant set (`stop_active_domain`), so `caml_try_run_on_all_domains` no longer
   waits for it and the deadlock the lock-handoff prevents cannot arise. `domain_dls`
   0/8 → 15/15 (debug), no release regression.

Also disabled `major_gc_wait_backup.ml` (asserts stock major-slice pacing forces a
collection + exercises the GC backup thread MMTk lacks — genuinely incompatible).

**Still failing (pre-existing, NOT these fixes — confirmed against the pristine runtime):**
the GC-burn tests (`domain_*_spawn_burn*`, and `domain_dls` in release) SIGSEGV with an
MMTk tracing panic `cannot trace object 0x11 / 0x1 …` — a stale/bad root during heavy
parallel spawn + `Gc.minor`/`major`. Reproduces under **Immix too** (not just
StickyImmix) — this is the bug #3 class (needs the `sanity`/`rr` workflow). `tak`/`churn`
native timeouts are core contention from the crashing burn tests under the parallel
harness, not real hangs.

---

## Stock shared heap (`shared_heap.c`) deleted

*2026-06-22*

Under always-on MMTk the stock shared major heap is never allocated into (MMTk owns the
heap), so `shared_heap.c` (1476 lines) + `caml/shared_heap.h` are deleted — pool allocator,
sweep, compaction, large-object, adoption, verification, lifecycle all dead. NOT everything
in the header was dead: the **mark-status colour machinery** (`caml_global_heap_state`, the
`status`/`Has_status_*`/`is_marked`/`is_unmarked`/`is_garbage`/`caml_allocation_status`
helpers), **`caml_atom`** + its 256-entry atoms table (zero-length blocks), and
**`caml_compactions_count`** are still live (weak/ephemeron/finaliser processing; every
allocator) → relocated to `major_gc.{c,h}`, not removed.

Heap-size/stats consumers rewired to MMTk: `caml_heap_size`/`caml_top_heap_words` (custom.c,
major_gc.c, sys.c) → new `caml_mmtk_heap_size_bytes()` (wraps `mmtk_ocaml_total_bytes`); the
dead `gc_ctrl.h` `caml_stat_heap_*` macros dropped; `gc_stats.c` stops sampling the empty
stock heap; the shared-heap lifecycle calls removed from domain.c/startup_aux.c. The
`caml_domain_state.shared_heap` field is **kept (set NULL)** to avoid shifting struct offsets
the native code generator bakes in (remove it later in an ABI-aware pass).

**Link gotcha (non-obvious, will recur):** deleting `shared_heap.c` removed the last *C*
reference to `caml_do_roots` — under MMTk it's now called only by the Rust binding
(`scanning.rs`). The link line lists `libcamlrun`/`libasmrun` *before* the staticlib, so
`roots.o` stopped being pulled → `undefined reference to caml_do_roots`. Fix: a link anchor
in `mmtk.c` (always linked, since the C runtime calls `caml_mmtk_*`) that references
`caml_do_roots`. Any future "the Rust binding calls a C function no remaining C code
references" needs the same anchor. Verified on turing: clean `world.opt` (incl. ocamldoc),
`Gc.stat` reports the MMTk heap (heap_words=8388608 for a 64 MB heap, major=1), weak-ephe-final
+ gc-roots run clean, native StickyImmix old→young = 1000000.

---

## Native write barrier wired (`caml_modify` / `caml_initialize`)

*2026-06-22*

Native `caml_modify`/`caml_initialize` were `#ifdef NATIVE_CODE` no-ops — native code under
a generational plan never recorded old→young refs. Correct for non-generational Immix,
silently wrong for StickyImmix: an old (mature) object mutated to point at a young object
wasn't remembered, so the young object was reclaimed at the next nursery collection →
dangling. Now unconditional (both runtimes); self-gates on `caml_mmtk_generational` so
non-gen plans pay one predictable branch. A/B repro (`old_young.ml`: a mature array
reachable only via a global ref — never a live stack local — so its young element tuples
are found only via the remembered set; compiled native, then churn to force nursery GCs):
without the barrier native StickyImmix returns 511500, with it 1000000; native Immix 1000000
either way; the testsuite `gc-roots` dir passes both (its roots are scanned regardless, so
it does NOT exercise the pattern — the bespoke test is required). Unblocks native GenImmix
copy-nursery TLAB aliasing (M8).

---

## linux-O0: debug-runtime stock-GC asserts under MMTk

*2026-06-22*

The `-O0` CI job runs the testsuite with `USE_RUNTIME=d` (the debug runtime, where
`CAMLassert` is live). Several asserts encode stock-GC invariants MMTk doesn't maintain,
and each fix revealed the next — verify the full set on turing (`USE_RUNTIME=d` over
`parallel callback gc-roots weak-ephe-final`, the CI's dirs) rather than one CI cycle at a
time. Removed (all DEBUG-only; release/all-plans unaffected): `minor_gc.c:439`
`young_ptr == young_end` + its `Debug_free_minor` poison in the STW empty-minor path
(under TLAB the "minor heap" is an MMTk Immix block; `young_ptr` stays mid-block after
clear), and `major_gc.c:391` `caml_gc_phase != Phase_sweep_main` in `caml_orphan_ephemerons`
(MMTk never drives `caml_gc_phase`; it stays at its initial `Phase_sweep_main`, and the
ephemeron lists are empty so the body is a no-op early return).

**The subtle one — `Caml_state` vs `Caml_state_opt`.** `caml_mmtk_enter/leave_blocking`
guarded `if (Caml_state != NULL ...)`, which works in release but aborts 57× in the debug
runtime: `#define Caml_state (CAMLassert(Caml_state_opt != NULL), Caml_state_opt)`, so
reading the `Caml_state` macro to compare it against NULL trips its own assert in exactly
the early-startup NULL case (`caml_open_descriptor_in` before the domain is created) the
guard exists to handle. Fix: test the raw `Caml_state_opt`. **Any "might be NULL" guard in
the runtime must use `Caml_state_opt`, never the `Caml_state` macro.**

Residual linux-O0 fails (separate triage, NOT stock-GC asserts): `domain.c:895`
`domain_state->memprof == NULL` on domain-slot reuse (`domain_dls.ml`); the known MMTk
hangs (`signal 9` timeouts); and tests asserting stock-GC behaviour MMTk lacks
(`major_gc_wait_backup` — GC backup thread; `signals_alloc` — GC-stat output) which should
be disabled like the other stock-GC-specific tests.

---

## M9 cleanup: `caml_mmtk_enabled` removed — MMTk is unconditional

*2026-06-22*

MMTk is the only GC, so the per-site `caml_mmtk_enabled` dual-path branch is gone (~35 sites
across memory.{c,h}, mmtk.{c,h}, array.c, intern.c, interp.c, gc_ctrl.c; 153 net lines
deleted). Allocation (`Alloc_small` macro, `caml_alloc_shr`), the write barriers
(`caml_modify`/`caml_initialize`/array fill), `Gc.stat`/major/compact, and the unmarshaller
now go unconditionally through MMTk; the dead stock minor-bump and `caml_shared_try_alloc`
fallbacks are deleted.

**The one real pre-init subtlety** (the rest was vestigial): `caml_mmtk_enabled` doubled as the
"MMTk ready?" guard for the brief early-startup window. That collapses safely almost
everywhere — no OCaml *value* allocation happens pre-init (`domain_create`'s allocations are
C/`caml_stat`/mmap; the global-data intern runs after the mutator is bound), and the region
barrier self-gates on `caml_mmtk_generational` (0 pre-init) before any `Caml_state` deref. The
exception: `caml_mmtk_enter/leave_blocking` is reached via `caml_open_descriptor_in` during
startup while `Caml_state` is still NULL, so those now guard
`if (Caml_state != NULL && Caml_state->mmtk_mutator != NULL)`. The build+run caught this as a
NULL-deref SIGSEGV at the first compile — reading alone would have missed it.

Also fixed in passing: `alloc_shr`'s `noexc` path now routes to the non-raising
`caml_mmtk_try_alloc_shr` (the old MMTk branch ignored `noexc` and always raised — a latent
contract bug).

Verified on turing: clean `make world.opt` (bytecode + native self-host), bug-#2 ocamldoc
manpage repro clean (0 segfaults), native programs correct under Immix + StickyImmix.

Next transitional flag of the same shape: **`MMTK_WEAK_REFS`** — retire it after the weak-ref
`pr5233` resurrection-ordering fix (the default `process_weak_refs` still has that bug, so
`=0` stays as the safety fallback for now). See ROADMAP.

---

## CI moving-GC bug (#2) FIXED — native unmarshalling allocated OFF-HEAP

*2026-06-22 (resolves the "STILL OPEN" entry below)*

**Root-caused and fixed.** The native (`ocamldoc.opt`) crash was the unmarshaller allocating
unmarshalled objects **outside MMTk spaces**. In `intern.c`, the MMTk-aware allocation — both
the "skip the bulk `Alloc_small` String_tag pre-allocation" guard (`intern_alloc_storage`) and
the per-object `caml_mmtk_try_alloc_shr` path (`intern_alloc_obj`) — was wrapped in
`#ifndef NATIVE_CODE`, so it applied to **bytecode only**. In native, `intern_alloc_obj` fell
through to the stock `caml_shared_try_alloc(d->shared_heap, …)`. Under M9 (stock heap excised)
that allocates in a non-MMTk region (a `caml_stat`/malloc area, observed ~`0x7913…`); MMTk's
root-scan / `scan_object` pointer filter (`is_in_mmtk_spaces`) drops those objects, so their
fields are never traced and anything reachable only through the unmarshalled graph (the loaded
ocamldoc module/info records and their sub-objects) is collected → dangling pointer → SIGSEGV
when ocamldoc later walks the doc tree (`odoc_man.ml`).

This is exactly why the bytecode `parser.ml` proxy + the earlier `is_collection_enabled` intern
fix passed while native ocamldoc kept crashing: **both that fix and this allocation path are
`#ifndef NATIVE_CODE`** (bytecode-only).

**Fix (`intern.c`):** remove the two `#ifndef NATIVE_CODE` guards so the MMTk allocation path
applies in native too — native unmarshalling now allocates each object via
`caml_mmtk_try_alloc_shr` (MMTk heap, traceable). Added `CAMLassert(!caml_mmtk_enabled)` on the
now-dead stock `caml_shared_try_alloc` branch (reachable only in the pre-init window, where no
unmarshalling occurs) to catch any regression.

**Verified:** from-scratch `make clean && make -j world.opt` (the CI build, including the
`ocamldoc Stdlib.3o` manpage step) succeeds, and the manpage repro runs **0/12 crashes** under
default Immix (was **12/12**). Diagnosis via the saved rr trace: `is_in_mmtk_spaces` of the
crashing record = 0 (off-heap) vs its referent = 1 (heap, reclaimed-and-zeroed); plan-sensitivity
Immix 5/5 vs GenImmix 0/5 (only a full-heap trace reclaims the un-rooted object).

**M9 cleanup (follow-up):** since MMTk is the only GC, `caml_mmtk_enabled` is always true
post-init and the stock `caml_shared_try_alloc` / bulk paths are dead code — removing the
`caml_mmtk_enabled` branch entirely is tracked M9 cleanup (ROADMAP).

---

## CI moving-GC bug (#2) — was STILL OPEN, now RESOLVED (see entry above)

*2026-06-22 (correction to the entry below — superseded by the fix above)*

**The "GC-mid-`intern_rec`" fix below is real and good, but it does NOT fix the CI/ocamldoc
crash.** After pushing it to `5.5+mmtk`, the **Build CI still SIGSEGVs** at
`ocamldoc build/man/Stdlib.3o` (Error 139) on x86-64 **and** linux-arm64 — the exact original
symptom. Not a CI/cache artifact: reproduced **12/12 on turing** with the exact pushed commit
(`0e707cbe51`), clean-built (binding recompiled, `is_collection_enabled` present; CI also builds
the binding fresh), under the **default Immix** plan.

Why the earlier validation misled me: I validated against **bytecode** `parser.ml` (the proxy used
below), which the intern fix genuinely fixed (~45%→0). But the CI crash is **native**
`ocamldoc.opt` — a different code path. `caml_mmtk_enabled` is 1 in the native runtime too, so the
intern suppression *is* active; the crash simply isn't a mid-intern GC.

**Real crash (gdb, deterministic under default Immix):**
```
#0  camlOdoc_man.fun_3850 () at ocamldoc/odoc_man.ml:307   | Odoc_info.Raw s -> bs b (self#escape s)
#1  camlStdlib__List.iter_373 () at list.ml:114
#2  camlOdoc_man.fun_3839 () at ocamldoc/odoc_man.ml:295
 ...
#9  camlOdoc.entry () at ocamldoc/odoc.ml:117
#10 caml_program ()
```
A **mutator** dereference while walking the doc tree — *not* the GC scan, *not* intern. Signature
of a moving-GC correctness bug: a relocated object whose reference was never updated → the mutator
follows a stale pointer → SIGSEGV. Under default Immix (full-heap moving, traces everything) the
prime suspect is a **missed root** — and since this is native code, **native-stack root scanning**
(which the bytecode `parser.ml` proxy never exercises — explaining why the proxy passed).

**Deterministic repro (default Immix):**
```
cd api_docgen/ocamldoc && rm -rf build/man && setarch x86_64 -R make V=1 build/man/Stdlib.3o
```
(turing can't fetch from the `mmtk` remote — ship code via `git diff` patch / `git bundle` + scp.)

Bug #2 is **REOPENED**. The intern-GC fix below and the `slot.rs` Infix_tag fix both stand — they
fixed other real manifestations. Investigation continues via rr on this repro (trace recorded
`/tmp/rr-odoc` on turing).

---

## GC triggered mid-`intern_rec` — ROOT-CAUSED + FIXED (a bytecode crash, NOT the CI/ocamldoc one)

*2026-06-22*

**A real moving-GC crash on the bytecode `parser.ml` repro is fixed** (this is *not* the CI/ocamldoc
crash — see the correction entry above). Root cause: the
**unmarshaller triggers a GC in the middle of `intern_rec`** under MMTk, which vanilla
OCaml never does.

- Vanilla `intern_alloc_storage` reserves the whole result block up front via
  `caml_shared_try_alloc` (which never collects — returns NULL→OOM on failure). So no GC
  runs while `intern_rec` fills the structure through **raw, un-rooted C pointers** (the
  `dest` recursion cursor and the off-heap `intern_obj_table` back-reference array).
- The MMTk port (`intern_alloc_obj`, intern.c) instead allocates **each object** via
  `caml_mmtk_alloc_shr` → `mmtk_ocaml_alloc` → `memory_manager::alloc`, whose slow path
  **polls and runs a collection** on heap pressure. So a GC fires mid-unmarshal,
  relocating/collecting the half-built structure; `intern_rec`'s raw pointers then dangle
  and it writes/links stale references. The damage surfaces later when a StickyImmix GC
  scans the structure: it follows a stale/dangling reference, reads a field as a header
  (huge wosize / object-start-used-as-value), and SIGSEGVs in `scan_ocaml_object`.

This explains the entire matrix: **StickyImmix ~25-45%** (frequent in-place GCs, so a
mid-intern collection is likely and relocates), **Immix rare** (far fewer GCs), **GenImmix
clean** (`caml_mmtk_alloc_shr` objects are mature; a nursery GC never moves them), **MarkSweep
/ ALWAYS_DEFRAG clean** (full-heap, non-generational). It is *not* the array.c barrier
(a separate real bug, fixed earlier) and *not* weak refs (`MMTK_WEAK_REFS=0` A/B still crashed).

**rr path to it:** the crash is a GC worker faulting in `scan_ocaml_object`; the object's
"header" was a heap pointer (huge wosize). Reverse-watching that header word
(`watch` + `reverse-continue`, with `set language c` so gdb knows `unsigned long`) led to
`intern_rec`'s `*dest = v` (intern.c) — i.e. the unmarshaller wrote it. (Pinning the intern
objects did NOT help — pinning blocks *relocation*, not the *collection* of the unrooted
in-progress objects, and doesn't stop the GC from running at all.)

**Fix (restores the invariant using MMTk's own hook):** suppress collection for the duration
of the unmarshal. MMTk's `gc_trigger.rs:110` consults `VMCollection::is_collection_enabled()`;
the binding now implements it (`collection.rs`) to read a runtime counter
`caml_mmtk_gc_disabled` (mmtk.c: `caml_mmtk_disable_collection`/`enable_collection`/
`collection_enabled`, atomic, nestable, cross-domain). `intern_rec` bumps it on entry (flag
`gc_was_disabled` in the intern state) and `intern_cleanup` drops it on every exit (success +
error/longjmp). On genuine heap exhaustion mid-unmarshal the new non-raising
`caml_mmtk_try_alloc_shr` returns NULL, so `intern_alloc_obj` runs `intern_cleanup` (freeing
state, re-enabling GC) before raising `Out_of_memory` — matching the vanilla
`caml_shared_try_alloc` path (and fixing a pre-existing cleanup-skip-on-OOM leak there).

**Validation:** `parsing/parser.ml` under **StickyImmix 64 MB ×96: 0 crashes** (was ~45% =
34/76); **default Immix ×30: 0 crashes** (no regression). All runs reach the expected
warning-as-error (`exit 2`) — i.e. unmarshalling succeeds and the compile runs through.

Caveats: `is_collection_enabled` is a global VM hook, so this briefly suppresses GC
process-wide during any unmarshal (fine — unmarshals are short); a huge unmarshal at a tight
heap can now OOM where a mid-unmarshal GC might have freed space (correct, same as vanilla's
up-front reservation failing). The `array.c` `Is_young`-barrier fix from earlier stands; the
remaining `Is_young`-gated elisions (`weak.c`/`finalise.c`) are still worth hardening but are
not this crash.

## CI bug: session summary — `Is_young` barrier-elision class (one FIXED), crash still in partial-defrag

*2026-06-22*

Long rr + detector + code-review session. Net outcomes:

**FIXED (real latent bug, commit-worthy): missing generational write barrier in
`caml_uniform_array_make` (`runtime/array.c`).** The large-array branch did
`Field(res,i) = init` directly, guarded by `CAMLassert(!(Is_block(init) &&
Is_young(init)))`. **Under always-on MMTk `Is_young` is ALWAYS false** (the stock minor
heap is gone), so that assert is vacuous and the barrier was wrongly elided — a
mature/LOS `res` ← (possibly nursery) `init` edge was not recorded in MMTk's remembered
set. Fixed to use `caml_initialize` (which calls `caml_mmtk_region_barrier`), like every
other large-alloc fill path. **General insight (credit: code-review agent): every
`Is_young`-gated barrier-elision in the runtime is suspect under MMTk** — `Is_young` is
identically false, so "skip the barrier because it isn't young" branches are unsafe for
generational plans. Other dead-under-MMTk `Is_young` barriers exist (`weak.c`
`ephe_write_barrier` → `add_to_ephe_ref_table` on the dead stock `ephe_ref`; `finalise.c`)
— latent, to harden, but NOT this crash (below).

**The array.c fix did NOT fix the StickyImmix crash** (validation: 41/96 runs still
crashed, 139/132, ~43% — unchanged; the fixed `array.b.o` was confirmed linked into the
running `ocamlrun`). So the array site is real but separate; the CI crash is elsewhere.

**Refuted this session (so the crash is NONE of these):**
- *Weak/ephemeron/finaliser forwarding* — `MMTK_WEAK_REFS=0` A/B: bug PERSISTS, more
  frequent (23/59). With weak-refs off, ephemerons/finalisers are strongly rooted, so the
  dead `weak.c`/`finalise.c` `Is_young` barriers are covered there → not the crash.
- *The generational region barrier being a no-op* — over-conservative but SAFE for
  StickyImmix: `OCamlMemorySlice::object()` is `None`, so MMTk's `memory_region_copy_slow`
  uses `is_address_in_nursery(slot)`, and StickyImmix's returns **`false`** unconditionally
  (no separate nursery address range) ⇒ every region-barrier slice is enqueued (no edges
  lost).
- *`FieldSlot::store` writing an off-by-8* — `MMTK_DEBUG_OFFBY8` detector saw 121 off-by-8
  SCAN hits but ZERO STORE hits on a crash run (store-check has false negatives: only fires
  when the target's `val-8` reads wosize-0).

**Narrowed conclusion: the crash is in the Immix *partial / opportunistic in-place
evacuation* path** (binding copy/forward/scan under partial defrag). Evidence: MarkSweep
clean (never moves); `MMTK_IMMIX_ALWAYS_DEFRAG` clean (full moving → it's specifically
*partial*); GenImmix clean (separate copying nursery); plain **Immix crashes rarely** and
**StickyImmix ~25-45%** — Immix is **non-generational (no remembered set)**, so its crashes
cannot be a barrier/remembered-set bug; they scale with GC/defrag frequency (StickyImmix
does far more GCs). Symptom: a reference one header-word too low (object *start* used as
*value*) / dangling, created **without** `FieldSlot::store`.

**Concrete next step (fresh session):** rr-record StickyImmix `--num-cores=1`, breakpoint in
`common::object_model::copy_object` / the Immix evacuation, and watch a slot to an object
evacuated in a GC where *neighbouring* objects are NOT — catch a partial-defrag GC leaving a
reference at an un-relocated/old start. Or build a **post-GC all-heap validity sweep** (walk
live objects; flag any pointer whose target's `target-8` is itself a plausible header ⇒
pointer one word low) to pin the slot + introducing GC independently of `FieldSlot::load`.
Already audited correct (do not re-chase): `copy_object`, `get_reference_when_copied_to`,
`slot.rs` load/store + infix offset, `object_model` offsets, closure `start_env`
(matches mlvalues.h:322), the region barrier.

## CI bug: detector confirms MANY off-by-8 refs, none via FieldSlot::store; weak refs exonerated

*2026-06-22*

Built a gated **off-by-8 detector** in `common/src/slot.rs` (`MMTK_DEBUG_OFFBY8=1`):
`is_offby8(v)` = `v` in the heap range and the word at `v-8` (its purported header)
has **wosize 0** (impossible for a real heap object). Checks fire in `FieldSlot::load`
(SCAN — a scanned slot holds an off-by-8 value) and `FieldSlot::store` (STORE — a GC
update writes one, with a verdict: "object already off" vs "info/offset caused"). Cheap
range check (no `is_in_mmtk_spaces`) — heap reservation is fixed at
`0x200ffc00000..0x20103c00000` under `setarch -R` + 64 MB. Ran the parser.ml repro in
parallel workers (StickyImmix 64 MB) until a bug-run.

**Results (a bug-run, exit 139):** **121 `OFFBY8-SCAN` hits — 74 *unique* off-by-8 values
across 105 slots — and ZERO `OFFBY8-STORE`.** Clean/OOM runs fire **nothing** (no false
positives). So:
- The off-by-8 is **real and widespread** (not one value; 74 distinct), and
- **No GC `FieldSlot::store` ever writes an off-by-8.** Since the bytecode mutator does no
  pointer arithmetic (it only *copies* references), a *new* off-by-8 value can only be born
  in a GC forwarding path — but **not** the `FieldSlot::store` edge-update path.

**Weak/ephemeron/finalizer forwarding EXONERATED.** `ephe_forward`/`ephe_retain`
(`binding/src/scanning.rs`) rewrite weak refs via a C callback path that bypasses
`FieldSlot::store` — a prime "no-STORE" suspect. A/B test with **`MMTK_WEAK_REFS=0`** (routes
weak refs through the conservative `FieldSlot` rooting instead): the bug **persists and is
MORE frequent — 23/59 runs crashed (21×139 + 2×132), 36 OOM**. So `process_weak_refs` is not
the cause.

**So the off-by-8 is born in a moving-GC path that is neither `FieldSlot::store` nor weak-ref
forwarding, and only under *partial* in-place Immix defrag (MarkSweep / GenImmix /
ALWAYS_DEFRAG clean).** Remaining non-`store` reference-bearing paths to scrutinise:
`common::copy_object` memcpy interaction with scanning, `scan_ocaml_object` field-slot
address computation (closure/infix), the order of slot-load vs object-forward during the
trace, and anything in the runtime root/relocation glue (`runtime/mmtk.c`,
`caml_scan_stack`, `Setup_for_gc`/`Restore_after_gc`). A concurrent static code review is
running. Detector caveat: it only fires when an off-by-8 slot is *scanned during a GC*;
weak-refs-off runs usually crash before the next GC, so the detector stays silent there —
use a long-surviving (weak-refs-on) run to capture slots.

## CI bug: O is mutator-ALLOCATED at 0x…850 (not GC-moved) → stale-pointer hypothesis

*2026-06-22*

Continued the propagation walk and ran the move-vs-realloc experiment (forward, the
reliable direction).

**Propagation walk (reverse-watch, `/tmp/rr-a`):** the off-by-8 `accu` at MAKEBLOCK1
(`interp.c:803`) was **already** off-by-8 at MAKEBLOCK1 *entry* (`interp.c:800`) — so the
`Alloc_small` GC there did NOT corrupt it. One hop further: `accu` was set by **PUSHACC6**
(`interp.c:431 accu = sp[6]`) — i.e. the bad value is being **copied** stack→accu→stack→heap,
not computed. It is injected once upstream and then sprayed around (into ≥2 MAKEBLOCK
blocks' field0 and passed as APPLY2 `arg2`).

**Move-vs-realloc (forward HW watch on header word `0x20100c9e850`):** the location is a
**hot bump-allocation region, reused constantly by the mutator** — across the run the word
cycles through headers `0x800`(wo2), `0x400`(wo1), `0xffffffffffffffff` (a
`caml_uniform_array_make` fill, `array.c:234`), etc. O's own header `0x1400` (wosize 5, tag 0)
is written by **`mmtk_ocaml_alloc(wosize=5,tag=0)` ← `caml_mmtk_alloc_small` ← MAKEBLOCK
(`interp.c:785`)** — a **mutator allocation, NOT `copy_object`**. So O was *allocated* at
`0x…850`, not GC-moved there.

**Implication — leaning to a stale/dangling pointer (missed update under moving GC), not an
off-by-8 forwarding store:** because (a) O is never GC-moved, a *correct* ref to O
(`0x…858`) could not be turned into `O-8` by forwarding; and (b) `0x…850` is a hot reused
young address. The consistent mechanism: a slot held an old value `0x…850` that referenced a
now-dead/moved object whose start was `0x…848` (value `0x…850`); that slot was **not updated
when its referent was relocated** (a missed root/field update — but roots check clean, so a
**heap field** or a scan-coverage gap), the address was reused, and the stale pointer now
lands on O's header. This fits: MarkSweep clean (never moves ⇒ no dangling), moving plans
crash. (Off-by-8 forwarding is not fully excluded; the generic copy/forward/store path is
audited correct, which also argues against a blanket forwarding error.)

**Tooling notes (this session):** interactive `rr` via tmux works, but `tmux send-keys "end"`
sends the **End key** (not the literal) — closing a gdb `commands` block needs `send-keys -l
"end"`. A `source`d gdb file's `continue` fails ("program is not being run") in `rr` batch;
do `break`+first `continue` as top-level `-ex`, then `source` the rest. Forward HW
watchpoints fire on rr's **mmap/zeroing syscalls** at odd rips (gdb-Python type lookups like
`long` then throw) — break once past init to map the heap, set the watch after, and use `x`
or `*(int*)` (not `*(long*)`) when inspecting.

**Next:** find the **ref-creation event** (the missed-update / the GC after which the slot
went stale). Either (a) continue the reverse stack-walk on `/tmp/rr-a` (PUSHACC6 `sp[6]` →
who pushed it → … → a GETFIELD from a heap field, then reverse-watch that field for the GC
that failed to update it), or (b) build the pre-approved **all-heap + roots validity-sweep
detector** (after each GC, flag any pointer whose target header is malformed — e.g. wosize 0,
or target-8 is itself a header so the pointer is one word low) — robust, independent of
`FieldSlot::load`, and pinpoints the slot + the introducing GC.

## CI bug: tracing the off-by-8 value back — propagation chain (rr interactive)

*2026-06-22*

Drove `/tmp/rr-a` interactively (tmux-held `rr replay`, so the ~3-min replay-to-trap
happens once; then iterate). Recovered the register map (debug build): `pc=rbp`,
`accu=r14`, `sp=r15`, `sizes=rbx`. Confirmed `sp=r15` via `sp[1]==accu`. Reverse
watchpoints on **recent** writes work reliably on this `--num-cores=1` trace; on
**far-back** writes they trip the known "runs to trace start" rr/gdb bug (see below).

**The off-by-8 value `0x20100c9e850` lives in the heap.** `find /g` over the real
heap mapping (`info proc mappings` → **`0x200ffc00000`–`0x20103c00000`**, the 64 MB
MMTk reservation — NB heap addresses are 11 hex digits; an earlier `find` used a
12-digit `0x201000000000` base, entirely above the heap, hence false "not found"):
- `0x20100c9e850` (off-by-8 ref to O) is stored at **two** heap fields:
  `0x20103b01c68` and `0x20103b04758`.
- `0x20100c9e858` (the *correct* value of O) is stored at one heap field,
  `0x20100c9e8a0` (a sibling record's field) — so O is referenced both correctly and
  off-by-8.

**Propagation chain (reverse-watch, all reliable/recent):**
1. Failing SWITCH: `accu = sp[1] = 0x20100c9e850`, written to `sp[1]` by **APPLY2**
   (`interp.c:563 sp[1]=arg2`) — F is a 2-arg function; `arg2` is the bad value.
2. The same bad value was stored into heap field `0x20103b04758` by **MAKEBLOCK1**
   (`interp.c:803 Field(block,0)=accu`) — i.e. `accu` was *already* off-by-8 and got
   written into a freshly-allocated 1-field block O2. **Propagation, not origin.**
3. MAKEBLOCK1's `Alloc_small(...,Enter_gc)` (`interp.c:802`) can GC. Tracking `accu`'s
   origin via `watch $r14` is unreliable: once execution leaves
   `caml_bytecode_interpreter` into the Rust allocator/GC, `r14` is just a scratch
   register (the watch stopped on incidental churn at `api.rs:152`, the alloc return).

**Reverse-watch of O's *header* word `0x20100c9e850` (to learn whether O was
GC-*moved* there or mutator-*allocated* there) ran to the trace start** — the far-back
limitation. So the move-vs-realloc question (and thus: off-by-8 *forwarding store* vs
*stale/dangling* pointer to a freed object reused under O's header) is **still open**
and is the fix-critical crux.

**Next (forward, reliable):** restart `rr replay`, break once at `caml_mmtk_alloc_small`
(heap now mapped), set a HW watch on header `0x20100c9e850`, delete the breakpoint, and
`continue` *forward* logging every write — the sequence of objects that occupy that
word tells move-vs-realloc directly. If only O ever lives there (one `0x1400` write by
`copy_object`) → off-by-8 forwarding store; if a prior object X lived at value
`0x...850` then was freed/reused → stale-pointer (missed update). Then forward-watch a
field (`0x20103b01c68`) for the first write of `0x...850` to catch the creating store.
The generic moving path is audited correct, so suspicion remains on partial-defrag /
absent-VO-bit object-boundary handling.

## CI bug: SMOKING GUN — the bad value is a pointer ONE WORD (8 bytes) TOO LOW

*2026-06-22*

**The desync is not abstract "control flow corruption" — it is a concrete
off-by-`HEADER_SIZE` pointer.** Drove the `/tmp/rr-a` ocamlrund assert trace
(StickyImmix 64 MB, `rr record --num-cores=1`; aborts `interp.c:942`). Recovered the
live register map for the *debug* build (DWARF marks `sp`/`env`/`accu` "optimized
out" even at the SWITCH — read them from registers):

- **`pc` = `rbp`**, **`accu` = `r14`**, **`sizes` = `rbx`**, **`sp` = `r15`**
  (found by disassembling the SWITCH at `interp.c:942` and the frame-build block,
  which writes the return record to `-0x8(%r15)`/`-0x10(%r15)`). Verified
  `sp[1] == accu` (`ACC1` ran just before the SWITCH).

At the failing SWITCH: `accu = 0x20100c9e850`, `index = Tag_val(accu) = 5`,
`sizes = 0x50000` (5 block-cases, tags 0-4). **`sp` is NOT drifted**: the return
frame `[retpc, env, extra_args]` sits exactly at `sp[2..4]`, i.e. F is a 2-arg
function (APPLY2 layout `[arg1, arg2, retpc, env, extra]`) reading its own in-frame
local `arg2 = sp[1]`. So the value is wrong, not the stack pointer.

**Decoding the heap around `accu` is decisive.** The neighbourhood is a contiguous
run of tag-0 wosize-5 records (header `0x1400`, stride `0x30`), headers at
`…820/…850/…880/…8b0`. The array's own internal pointers use the correct
header+8 (`value`) convention (e.g. a field holds `…858`, `…828`). But
`accu = …850` points **at a header word**, not at the value `…858`:

- `Hd_val(accu)` reads `accu-8 = …848`, which is actually the *previous record's
  last field* (`0x5` = `Val_int 2`) → spurious "tag 5".
- The correct value is `accu+8 = …858` (header `0x1400` at `…850` ⇒ tag 0,
  wosize 5) → SWITCH tag 0, in range, no crash.

So **`accu` is exactly one word (8 bytes) too low — it points to an object's
header instead of its first field (the OCaml `value`).** `arg1` (`sp[0]`) is a
valid tag-0 wosize-2 block; only `arg2`/`accu` is off-by-8 → a *single localized*
bad pointer carried through the APPLY2 cascade, not a systematic forwarding error
(which reconciles with `MMTK_IMMIX_ALWAYS_DEFRAG` being clean — if every forwarded
ref were off-by-8 the whole heap would break instantly).

**Audited correct (so the bug is NOT in the generic moving path):** `copy_object`
and `get_reference_when_copied_to` return `to_start + OBJECT_REF_OFFSET` (value);
`slot.rs` `load`/`store` apply the infix offset symmetrically; `object_model.rs`
`ref_to_object_start`/`ref_to_header` subtract one word consistently;
`OBJECT_REF_OFFSET = WORD_SIZE = 8`. All header↔value conversions are self-consistent.

**So the off-by-8 enters somewhere partial-defrag-specific** — the prime suspect
remains object-boundary identification during Immix *in-place* evacuation with **no
VO bit** (a reference resolved to an object *start* instead of its `value`, or a
metadata-granularity mismatch). Next, decisive: find the GC that first writes an
off-by-8 pointer and the field it lands in. Two routes (both reuse `/tmp/rr-a`, no
rebuild): (a) forward conditional breakpoint in the copy path when the destination
start `== 0x20100c9e850` → see who is copied + which slot is then mis-updated;
(b) an **all-heap validity sweep** detector in the binding (after each GC, walk live
objects; flag any pointer field whose target's header is malformed — e.g. wosize 0,
or target-8 is itself a valid header so the target is one word low). The validity
sweep is robust (independent of `FieldSlot::load`, which `sanity` and the root
mis-forward detector both effectively trust) and pinpoints the introducing GC.

## CI bug: driving the ocamlrund assert trace — at the first bad SWITCH

*2026-06-22*

**Reproduction (clean, deterministic).** `MMTK_PLAN=StickyImmix MMTK_HEAP_SIZE_MB=64
rr record --num-cores=1 -o /tmp/rr-a ./runtime/ocamlrund ./boot/ocamlc <boot flags>
-c parsing/parser.ml` → exit 132, aborts at `interp.c:942 CAMLassert((uintnat)index
< (sizes >> 16))`. ~1 in N runs (others compile clean / SIGSEGV / OOM — all the same
timing-sensitive bug). **Must record+replay back-to-back with NO rebuild between** —
rr flags "metadata changed: replay divergence" if `runtime/ocamlrund` is rebuilt,
giving garbage state. (KC: ocamlrund doesn't use more memory than ocamlrun; the 64
MB "Out of memory" runs are the bug's timing, not a heap-size issue. So stay at 64
MB — 96 MB suppresses it.)

**State at the assert** (read via C locals; the debug build is unoptimized so
`accu`/`pc` are NOT in the same registers as ocamlrun — use `p accu`, not `$r13`):
`accu = 0x20100c9e850`, header `0x5` → **tag 5, wosize 0**; `index = 5`;
`sizes = 0x50000` → **5 block-cases (tags 0-4), 0 int-cases**. So `accu`'s tag (5) is
exactly one past the switch table → the desync. This is the *first* out-of-range
SWITCH (ocamlrund aborts on the first), so it is at/near the cascade origin.

**Reverse-trace (debug build uses a C `switch(curr_instr)` at interp.c:393/396, with
a verbose per-op header 376-396 — bcodcount, trace checks, sp CAMLasserts).** The
opcode immediately before the SWITCH is **ACC1** (`interp.c:404 accu = sp[1]`):
`accu` transitions `0x20100e3f7e8` → `0x20100c9e850` (tag 5) there. So **`sp[1]` =
the tag-5 block**, and this function's SWITCH on it expects tags 0-4.

`sp[1]` is a *value-stack slot* — and the mis-forward detector (which snapshots
exactly these roots) was **clean** — so `sp[1]` was forwarded *correctly*; it
genuinely holds a tag-5 block (not a mis-forward to a wrong object). So the desync
is upstream of this frame: **either a wrong-typed value was pushed to `sp[1]`
earlier (a prior wrong call/arg/jump — a non-SWITCH desync ocamlrund didn't assert
on), or `sp` is drifted so ACC1 reads the wrong slot.** Next: (1) dump the stack at
the SWITCH and look for the return frame at the expected offset → tells drift vs
wrong-value; (2) reverse to where `sp[1]` was pushed (the value's entry to the
stack) and to this function's entry (APPLY/GRAB) to see if it was called correctly.
Reverse via *breakpoints* + reverse-stepi (reverse-continue needs 2× to clear the
SIGILL; reverse SW-watchpoints run to the trace start — unreliable).

## CI bug: ocamlrund aborts AT the desync (SWITCH assert) — clean repro point

*2026-06-22*

`ocamlrund` (the *debug* runtime, CAMLasserts on) reproduces the bug and **aborts
at the desync itself**: `runtime/interp.c:942 ### Assertion failed:
(uintnat) index < (sizes >> 16)` — the SWITCH bounds assert, i.e. a block/tag-index
beyond the switch's block-case table (exactly the block-where-int-expected found by
rr). This is a far cleaner stop than ocamlrun's far-downstream SIGSEGV, and it
fires at the *first* out-of-range SWITCH → likely at/near the cascade origin.
Caveats: the debug runtime needs a **bigger heap** (`MMTK_HEAP_SIZE_MB>=128`; at 64
MB most runs hit `Out of memory` before the bug), and it's slow. Tip (from KC):
`ocamlrund -t` (repeatable `-t -t …`) traces the interpreter for more detail.

Plan from here:
1. `rr record --num-cores=1` **ocamlrund** (heap ≥128 MB) → it aborts at interp.c:942;
   replay → at the assert, examine the SWITCH input + reverse to where that
   wrong-but-valid value was loaded (the first desync's source).
2. Extend the mis-forward detector to **heap fields** (snapshot field values in
   `scan_object`) — catches a heap-field mis-forward, the leading remaining cause
   (the root-only detector is clean). Storage-heavy but feasible for a debug run.
3. sanity is expected NOT to catch it (mis-forward/desync is to a *valid* object;
   sanity only flags dangling/invalid edges) — re-confirm if cheap.

## CI bug: mis-forward detector is CLEAN — it's a pure control-flow desync

*2026-06-22*

Built + ran a **mis-forward detector** (`MMTK_DEBUG_STACK_CHECK`): snapshot each
root (FieldSlot + its pre-GC object) in `collect_root_slot`, then after the closure
verify `root.load() == get_forwarded_object(old)`. A root resolving to a *different
valid* object would be a mis-forward. **Result across 16 StickyImmix runs (incl. 2
crashes): ZERO mis-forwards.** So roots/values are forwarded **perfectly**.

Combined with everything else this means the bug is a **pure control-flow desync**:
the interpreter runs one function's bytecode against another's stack/args, and
*correct* values are consumed where a different type/value was expected. Verified
*not* the cause: stale roots (clean), mis-forwarded roots (clean), object copy,
classify/load/store, the write barrier, `Alloc_small` Setup/Restore, stack
relocation (`check_stacks` reloads `sp`; no realloc in the window), and `sp` is
consistent across the checked APPLY2→GRAB window. Yet control flow diverges — so
*something* feeding control flow is wrong despite all values/roots/`sp` checking
out. The two remaining candidates:
1. a **heap-field mis-forward** (an object field updated to a valid-but-wrong
   object) — the detector covers roots only, not the millions of heap fields, and
   `sanity` can't catch a mis-forward-to-a-valid-object (it follows the edge);
2. an `sp` drift in an **ancestor** frame (the detector doesn't check `sp`).

The cascade is deep (each frame got a wrong-but-valid closure/arg from its caller),
so the origin is many levels up. Pinning it needs either all-heap-field snapshot
mis-forward detection (storage-heavy) or a long reverse walk to the first wrong
control transfer. Trace preserved at `~/rr-sticky-fresh` (`rr replay`); record fresh
with `rr record --num-cores=1`.

## CI bug: cascade traced — root is a *control-flow* desync (likely a mis-forward)

*2026-06-22*

Drove the cascade back another level. The crashing function was reached by
**falling through** `CHECK_SIGNALS` (`interp.c:1050`, `0x772b76042e28`) into a
`GRAB` (`0x772b76042e2c`) — these sit in *different* functions in the bytecode
(one function's poll, the next's entry), so fall-through is wild. And the function
the caller's APPLY2 actually entered has `Code_val = 0x772b760457f4`, far from the
GRAB — so **function@`…57f4` itself made a wild backward jump** into another
function's code; that function (Y) then runs `ACC0`/`SWITCH` against `…57f4`'s
stack, and the block-vs-int SWITCH (above) sends it wild again → GETFIELD3 crash.

**Key reframing: every value involved is VALID** — `accu` closures, `arg1` blocks,
return frames all check out; the generalized root check is clean; MarkSweep clean.
So nothing in the heap or roots is *stale*. What's wrong is the **control flow**:
the interpreter runs one function's bytecode against another's stack. A *valid*
value (e.g. a tag-0 block) is then consumed where a different type was expected,
because the SWITCH/branch it reaches belongs to the wrong function.

**So the root is a control-flow divergence, and the most consistent mechanism is a
MIS-FORWARD**: Immix *in-place partial* defrag updating some reference to a *valid
but wrong* object (not a stale one). That explains everything observed:
- forwarded-stale checks are clean (the wrong object isn't forwarded — it's a live
  object, just the wrong one);
- MarkSweep/GenImmix clean (no in-place defrag of the relevant objects);
- the trigger is *partial* moving (some objects relocate, some don't — a mis-pair);
- a wrong-but-valid value reaching a branch desyncs control flow → cascade.
A prime suspect for mis-pairing under in-place defrag is the absent VO bit (object
boundaries during block evacuation), though precise scanning shouldn't need it —
needs checking against mmtk-core's Immix defrag.

**Next, decisive check:** snapshot each root's value *before* a GC and verify after
that `new == get_forwarded_object(old)` (a *mis-forward* detector, unlike the
current *forwarded-stale* detector). If a root's new value isn't the forward of its
old value → caught the mis-forward + the slot. Implement as a before/after pass in
the binding (snapshot in `scan_roots`, compare in `process_weak_refs`).

## CI bug: driving the fresh trace — desync is a SWITCH cascade from upstream

*2026-06-22*

Drove `~/rr-sticky-fresh` interactively. The crash (GETFIELD3 on `accu=0x3`) is the
tail of a **control-flow desync cascade**, mechanism now pinned:
- The crashing function entered via **GRAB** (arity OK) → **ACC0** (`accu=sp[0]`) →
  **SWITCH** (`interp.c:938`). At the SWITCH, `accu = sp[0] = 0x20100e3f7e8` is a
  **tag-0 block** (hdr `0x800`), but `sizes = 0x112a` ⇒ `sizes>>16 = 0` block-cases
  (only int-cases). So this SWITCH expects an **int**; given a block it takes the
  block branch and indexes `pc[(sizes&0xFFFF)+0] = pc[0x112a]` — *past* the int
  jump-table → a garbage offset → wild `pc` → (a few wild opcodes later) the
  GETFIELD3 crash.
- So `sp[0]` (the function's first arg) has the **wrong type** — a valid block
  where an int is required. It was already a block at the GRAB. Reversing to the
  caller's **APPLY2** (`interp.c:505`): it passed `arg1 = 0x20103b07980`, which a GC
  during the call's `check_stacks` then forwarded to `0x20100e3f7e8` — i.e. `arg1`
  was a **block before and after** that GC (not type-flipped by it; the slot was
  correctly updated). So the caller genuinely passed a block to an int-expecting
  function: a **type error → the caller is itself desynced**.

So the crash is the *downstream* end of a cascade of desyncs (each frame runs one
function's code against another's stack/args). No missed/stale root is involved
(roots clean, values forward correctly, MarkSweep clean) — it's pure control-flow
divergence that began upstream, after some GC, and propagated through calls. The
remaining work is to find the **first** desync (walk back through the cascade /
bisect on GC count to the GC after which control flow first diverges). Tooling: the
preserved trace + `--num-cores=1` recording; reverse to control-transfer opcodes
(APPLY/RETURN/GRAB/do_return) is reliable, reverse-watchpoints are not.

## CI bug: fresh StickyImmix rr trace + it's an `sp` (stack-pointer) misalignment

*2026-06-21*

**Recording breakthrough: `rr record --num-cores=1` records StickyImmix.** Plain
`rr record` of StickyImmix aborts in MMTk init (`mmap meta memory: File exists`);
the collision is with rr's multi-core simulation. `--num-cores=1` avoids it and
records cleanly — and caught the crash (exit 139). So we now have a **fresh,
deterministic, current-tree** StickyImmix trace, **preserved on turing at
`~/rr-sticky-fresh`** (`rr replay ~/rr-sticky-fresh`). Re-record with
`MMTK_PLAN=StickyImmix MMTK_HEAP_SIZE_MB=64 rr record --num-cores=1 -o <dir>
./runtime/ocamlrun ./boot/ocamlc <boot flags> -c parsing/parser.ml`. MarkSweep,
Immix and GenImmix already recorded fine (no `--num-cores=1` needed).

**Generalised stale-root check came back CLEAN.** `MMTK_DEBUG_STACK_CHECK=1` now
re-walks every enumerated root (value stack raw, plus `caml_do_roots` and
`caml_scan_global_roots`) after the closure, flagging any slot whose referent is
forwarded-but-not-updated. Across 16 StickyImmix runs (incl. crashes): **no
forwarded-stale root** (the only hits were the dropped is_reachable/Infix false
positives). So no enumerated root is mis-relocated.

**The crash is an `sp` (value-stack pointer) misalignment.** On the fresh trace the
SIGSEGV is at `interp.c:856` GETFIELD3 `accu = Field(accu, 3)` with `accu = 0x3`
(an *immediate*, `Val_int 1`, not a block). `accu` was just loaded by ACC5
(`interp.c:412` `accu = sp[5]`) from `sp[5] = 0x3`. But the stack at `sp` shows a
**return frame only 2 slots up**: `sp[2]` = a code pointer (in `[prog,prog+size)`),
`sp[3]` = an env block, `sp[4]` = `Val_long(0)` = `[retpc, env, extra_args]`. So
ACC5 reads *past* the current frame's return record into caller data — `sp` is
~3 slots too high. Same pattern as the old `strcrash` trace (do_return read an
argument as the return PC). So the bug is **stack-accounting drift** (some opcode's
push/pop count is off by ~3 under StickyImmix's partial in-place defrag), not a
stale heap reference — consistent with: roots clean, copy/classify/store correct,
GenImmix/MarkSweep clean.

Suspects for the ~3-slot drift: a `Setup_for_gc`/`Restore_after_gc` (±3) mismatch
around an allocation-triggered GC, or a `GRAB`/`RESTART` `num_args =
Wosize_val(env) - 3` miscount if `env`'s closure is briefly wrong.

**Audit complete — every component on the relocation/alloc path is correct, so the
root is upstream and subtle:**
- Roots: generalized stale-root check (all of `caml_do_roots` + `caml_scan_global_roots`)
  CLEAN across crashes. Not a mis-relocated root.
- Object copy (`common::copy_object`): correct — `get_current_size = (wosize+1)*WORD`,
  bulk `copy_nonoverlapping` of header + all fields (preserves code/closinfo/infix).
- `FieldSlot::classify`/`load`/`store`: correct (infix offset applied both sides).
- Write barrier: shared with GenImmix (which is clean) → not the barrier.
- Fiber/value stacks: `mmap`/`caml_stat_alloc`'d (fiber.c `alloc_for_stack`), NOT in
  the MMTk heap → MMTk never relocates them → `sp`/`current_stack` are stable.
- `Alloc_small` MMTk path (memory.h): balanced — `Setup_for_gc` (−3) /
  `Restore_after_gc` (+3) with a temp so `accu`/`env` aren't clobbered.

So the bug is neither a stale heap reference nor a missed/mis-relocated root nor an
unbalanced GC publish — it is a **subtle Immix in-place-defrag interaction** (the
only thing unique to the crashing plans: StickyImmix collects young *in Immix* with
opportunistic in-place defrag; GenImmix copies young to a separate space and is
clean; MarkSweep never moves and is clean; `ALWAYS_DEFRAG` moves everything and is
clean → the trigger is *partial* in-place moving). It surfaces far downstream as
`sp`-accounting drift / control-flow corruption (the drift propagates through
calls, so its origin is upstream of the crashing frame).

**Refinement — it's a `pc`/`sp` desync, and the stack is well-formed.** On the fresh
trace the value stack around the crash is *intact*: valid return frames at `sp[2]`
(`[code 0x772b75f2cbb8, closure (hdr 0x10f7, Closure_tag wo4), Val_long 0]`) and at
`sp[14]` (`[code 0x772b75f2dab0, closure, Val_long]`). So `sp[5]=0x3` is a
*legitimate* value in the caller's locals — nothing is corrupted in the heap or
stack. The fault is that the **running code does not match the frame `sp` points
at**: the function whose frame is at `sp[2]` has only 2 locals, yet the executing
opcode is ACC5+GETFIELD3 (needs ≥6). I.e. `pc` and `sp` are out of sync — a control
transfer (RETURN/APPLY) used a wrong `sp` (or `pc`), so afterwards the interpreter
runs one function's bytecode against another's stack frame. This is the downstream
face of the same upstream event; the heap/roots being clean is expected for a
desync (no value is wrong — the *pairing* of pc and sp is).

**Handoff for the focused next session.** Record a fresh trace with
`rr record --num-cores=1` (StickyImmix, 64 MB) and drive it *interactively*: from
the crash, reverse to the function's entry (the APPLY that pushed the return frame
visible at `sp[2]`) and forward-step tracking `sp` (r14) to find the first opcode
or GC where `sp` diverges by ~3 from the frame structure; then determine why
(unmatched push/pop, or a value used for the push/pop count that a partial defrag
left wrong). The gated `MMTK_DEBUG_STACK_CHECK` tooling + the `--num-cores=1`
recording method are the enablers.

## Fix: `is_forwarded` broke non-moving plans; and the CI bug is RELOCATION, not a missed root

*2026-06-21*

Two results from a MarkSweep (non-moving) cross-check of the parser.cmo crash.

**Bug fixed — the bug-#1 `is_forwarded` check crashed every non-moving plan.**
`FieldSlot::classify` (slot.rs) calls `is_forwarded(addr)` on the `Infix_tag`
branch (the bug-#1 forwarding-pointer/Infix disambiguation), which reads the
forwarding-bits **side metadata**. The binding registered that spec
unconditionally at init (`api.rs`), but **non-moving plans (MarkSweep, NoGC) never
map it** — so the *first* infix-tagged field scanned read unmapped memory →
deterministic SIGSEGV in `is_forwarded`. MarkSweep was 12/12 crash. Fix: register
the spec **only when `plan.constraints().moves_objects`** is true; non-moving plans
leave it unset, so `is_forwarded` returns `false` — correct there (nothing is ever
forwarded, so every `Infix_tag` header is genuine). After the fix: MarkSweep
**8/8 clean** on parser.cmo, Immix/StickyImmix unchanged (bug-#1 fix intact). Run
via `runtime/ocamlrun` (the `boot/ocamlrun` bootstrap binary is stale — rebuild
relinks `runtime/`, not `boot/`). **Validated broadly: the full bytecode testsuite
under MarkSweep now passes 1367 (140 skipped, 44 failed — the failures are the
known-unsupported set: statmemprof/runtime-events/Gc.stat + by-design, same class
as Immix). Before the fix MarkSweep crashed on essentially every test.** No
regression on the default plan either: the full suite under **Immix** passes 1366
(140 skipped, 45 failed — same failure profile; the +1 vs MarkSweep is consistent
with the rare moving-GC bug #2). So this session's changes (finaliser-handover fix,
`is_forwarded` gating, gated debug tooling) leave Immix unregressed.

**The CI/ocamldoc bug is a RELOCATION bug, not a missed root.** With the above fix,
MarkSweep runs parser.cmo **clean** (8/8) while StickyImmix/Immix still crash. A
*missed root* would crash under MarkSweep too (the object would be collected); it
doesn't, so the object stays **live** — it just **moves**, and a reference to it is
**not updated**. Combined with the earlier findings (value-stack slots forwarded
correctly; `caml_global_data`/globals scanned every GC), the un-updated reference
is neither on the value stack nor in the globals.

**Plan matrix narrows it to the Immix-defrag forwarding path (NOT the generational
barrier).** parser.cmo @ 64 MB, fresh runtime:
- MarkSweep (never moves): **clean** (after the is_forwarded fix).
- GenImmix (generational, *copying* nursery — always moves young): **clean** (6/6).
- Immix + `MMTK_IMMIX_ALWAYS_DEFRAG=1` (full-heap defrag every GC): **clean** (6/6).
- Immix (default, opportunistic defrag): **rare** crash (the CI ocamldoc case).
- StickyImmix (sticky nursery collected *in Immix* + opportunistic defrag): **~25%**.

GenImmix shares the generational write barrier and is clean → the bug is **not** the
barrier. It is specific to **Immix-style in-place forwarding** (forwarding pointer
in the header + side bits) exercised by StickyImmix's frequent young-in-Immix
collection (and rarely by plain Immix). Notably `ALWAYS_DEFRAG` (move *everything*)
is clean while *opportunistic* defrag crashes — so the trigger is **partial**
moving: some objects relocate, some don't, in the same GC, and a reference assumes
the wrong one. This is bug #1's exact area (the in-header forwarding ptr / Infix
handling); bug #2 is a residual there. Verified *correct* so far: `FieldSlot::
classify` / `load` / `store` (infix offset applied on both sides), and the write
barrier (shared with clean GenImmix). Remaining suspects: the object **copy**
(`copy_object`) under Immix defrag, closure **field-range scanning**
(`scan_ocaml_object` TAG_CLOSURE/TAG_INFIX — note the stale "implement infix
redirect in copy_object" TODO at common/scanning.rs:62), or a slot read **before**
its referent is forwarded vs **after** (order-dependence, like bug #1).

Next: this needs StickyImmix under rr, but StickyImmix's metadata won't record
under rr (Immix/GenImmix/MarkSweep do). Options: (a) chase it on the existing
`strcrash` StickyImmix trace looking for a slot that reads an *un-forwarded* object
mid-defrag; (b) try recording plain Immix in a loop until the rare crash; (c) make
the binding scan-time check (`MMTK_DEBUG_STACK_CHECK`) general — flag *any* traced
slot whose load resolves to a forwarded object that the trace then fails to update.

## Bug #2 / ocamldoc-CI: latent moving-GC crash — confirmed live + characterised

*2026-06-21*

The latent moving-GC bug is the **same crash failing CI**: the linux-arm64 `Build`
job SEGVs at `make -C ocamldoc man` (api_docgen) — the "rare ocamldoc
`Lexing.engine` crash" long tied here to the moving GC. CI builds the default
(Immix), so it manifests on **Immix** too (opportunistic moving → rare), while
StickyImmix (always-relocating) triggers it deterministically. So it is not a
tight-heap-only edge — it blocks CI. The root cause is in architecture-independent
root/stack handling, so the x86-64 StickyImmix repro fixes the Arm64 crash too.

**Repro (current tree, `~/ocaml-mmtk-del`).** The bootstrap compile of parser.ml
under the *boot* compiler:
`MMTK_PLAN=StickyImmix MMTK_HEAP_SIZE_MB=64 setarch x86_64 -R ./boot/ocamlrun
./boot/ocamlc <boot flags: -nostdlib -I ./boot -use-prims runtime/primitives -g
-strict-sequence … -c parsing/parser.ml>` (script `~/repro-parser-segv.sh`).
Exit 139 on ≈25% of runs at 64 MB (intermittent — layout/timing sensitive; use
`rr record --chaos` to raise the rate). Immix: clean (exit 2 = the standalone
warning-as-error, *not* a crash). NB: a `-I stdlib` *standalone* invocation
short-circuits with a `camlinternalMenhirLib.cmi` naming error before the heavy
allocation — must use the boot flags (`-I ./boot`) for a full compile that crashes.

**Crash mechanism** (from the saved rr trace `~/.local/share/rr/strcrash`; replay
is deterministic). SIGSEGV at `interp.c:623` `Next` — the bytecode dispatch
`jmp *jumptable[*pc]` — faulting on `movslq (%rax),%rax` because **pc is garbage**
(0x1). Line 623 is the tail of `do_return` "return to callee":
`pc=(code_t)sp[0]; env=sp[1]; extra_args=Long_val(sp[2])`. The consumed RETURN
frame on the bytecode value stack is corrupt:
- `sp[0]` saved return-PC = `0x1`              (want a bytecode addr ~`0x748401……`)
- `sp[1]` saved env       = `0x2010349e6b8`, hdr `0x1403` → **tag 3, wosize 320**
                            (want the caller's `Closure_tag` closure)
- `sp[2]` extra_args      = `0xffff…ec41`
So a whole live RETURN frame went stale across a StickyImmix move: the saved-env
slot points at a live but *wrong* block (tag-3 — memory the moved closure's old
address was reused for), and the adjacent return-PC slot is garbage. `sanity` does
**not** flag it (heap is consistent), so the unforwarded slot is a **root the GC
scan missed**, not a heap field — matches the prior bug-#2 signature.

**Where it is NOT.** The normal alloc path publishes roots correctly:
`Setup_for_gc` (interp.c:76) pushes accu/env/pc and sets `current_stack->sp = sp`
before every `Alloc_small(…,Enter_gc)`, and the MMTk shr/alloc hooks
(`CAML_MMTK_SETUP_ROOTS`) mirror it — so accu/env around a normal allocation are
covered. The missed slot is a *deeper* live RETURN frame, so the suspect is the
value-stack scan running with a stale/short `sp` at some GC, or a StickyImmix
generational (nursery / remembered-set) path not forwarding a stack-held young
closure. TBD — needs reverse-execution.

**rr recording friction (open).** Fresh `rr record` of the MMTk runtime aborts in
MMTk init — `space.rs:724 failed to mmap meta memory` ("Inappropriate ioctl" with
the syscall buffer on; "File exists" with `-n`): rr's memory layout collides with
MMTk's fixed metadata mmap. The Jun-20 `strcrash` trace still replays fine (use
it). Fresh traces against the current tree need this solved (try
`--disable-avx-512` / `--num-cores=1`, or shrink the metadata footprint).

**Reverse-execution findings (strcrash replay).** Confirmed via `caml_do_roots`
breakpoints (the binding's root scan: `f=mmtk_ocaml::scanning::collect_root_slot`)
+ software watchpoints:
- The crashing `do_return` reads its frame at `sp=0x619797ff1038`:
  `pc=sp[0]=0x1`, `env=sp[1]=0x2010349e6b8`, `extra_args=sp[2]=0xffff…ec41`.
- **No GC between the frame's construction and the crash** — the last `caml_do_roots`
  before the fault is rr event 1795, with `current_stack->sp=0x619797ff1010`; the bad
  slot `0x619797ff1040` is 6 words *above* that sp (i.e. inside the scanned range),
  but at that GC it still held a *different, valid* value (`0x201034981e8`). So the
  bad frame is built **after** the last GC.
- At event 1795, address `0x2010349e6b8` was **free** (header = a free-list link,
  `0x2010349e6c0`), and that address churns through many objects over time
  (`0xe1` header earlier, etc.). So `0x2010349e6b8` is a **stale pointer reused as a
  tag-3/wosize-320 block** — the closure that lived there moved/died in an *earlier*
  GC and a slot kept pointing at it; the staleness was then carried forward (through
  `accu`/the stack via `PUSHACC1` @421 and `APPLY1` @494) into this frame.
- The frame the dying callee returns through does **not** line up with a clean
  APPLY1 frame (`[arg1, pc, env, extra_args]`) at the expected offset — hinting at
  either an `sp` imbalance (~2 slots) or an unforwarded slot; **unconfirmed**.

**Methodology caveat.** Batch (`-batch` over ssh) software-watchpoint *reverse*
gave self-contradictory Old/New readings here (a slot's "last writer" reported a
value that disagrees with the crash-time contents) — SW-watchpoint reverse is
unreliable in this mode. The final pin needs **interactive** rr: from event 1795,
single-step *forward* to the crash watching the stack build the bad frame (forward
HW watchpoints are fine; only *reverse* + long runs trip the rr async bug), or
walk the GCs forward tracking the specific closure reference. Solving the rr
recording friction (above) to get a fresh, `sanity`+`MMTK_VERBOSE` trace would
also help.

**Instrumentation result — the value stack is NOT the missed root.** Added a gated
post-GC stack check (`MMTK_DEBUG_STACK_CHECK=1`): in `process_weak_refs` (after the
root scan has forwarded everything it found), raw-walk each domain's bytecode value
stack `[sp, Stack_high)` (via new `caml_mmtk_debug_stack_range`) and flag any slot
pointing to a **forwarded** object (a root the scan failed to update). Across 24
StickyImmix/64 MB runs (4 crashes): **zero forwarded-stale slots, on crash runs
too** — so caml_scan_stack *does* correctly forward every value-stack heap pointer.
A second variant flagging slots pointing to **unreachable** objects fired heavily
(36–106/run) **on clean runs as well** — all false positives: their headers end in
`0xf9` = `Infix_tag` (interior pointers into closures, where `is_reachable` is
meaningless — the VO bit is at object starts, not infix interiors).

Conclusion: the corrupted RETURN frame's stale `env` is **not** an unforwarded
value-stack slot. The check + `caml_mmtk_debug_stack_range` are kept (gated off)
as reusable tooling.

**It is an `sp` imbalance, not a stale pointer.** Scanning the stack around the
crash `sp` (`0x619797ff1038`) shows the *real* return frames plainly — valid
`[code_ptr, closure, Val_long]` triples at `0x1080`
(`[0x748401261f5c, 0x201033516a8, 0x1]`), `0x10b8`, `0x1110`, `0x1130` (code
pointers all inside `[prog, prog+prog_size)` = `[0x748401101010, +3137604)`). But
`do_return` read its frame at `0x1038`, *below* all of them, where the words are
the function's working data (`[0x1, 0x2010349e6b8, 0xffff…ec41, …]`), not a frame —
`sp[0]=0x1` is not a code pointer. So `do_return` ran with `sp` left too low (extra
slots on the value stack) and misread operands as `[pc, env, extra_args]`. The
"stale env" (`0x2010349e6b8`, a *live* tag-3 block freshly allocated after the last
GC) is just whichever live pointer happened to sit in the misread slot.

This reframes the bug: **a stack-pointer accounting error**, GC-triggered (Sticky­
Immix's per-minor-GC moving makes it deterministic; Immix opportunistic → rare),
consistent with the clean forwarded-check (an imbalance is not an unforwarded
slot).

**Root chain traced (forward watch from event 1795 — reliable).** The last write
to the crash's `sp[0]` slot (`0x619797ff1038`) is `interp.c:509` `sp[0] = arg1`
inside **APPLY2**, with `arg1 = 0x1` (the integer `0` — the *first argument*). So
`do_return` later reads that argument slot as the return PC (`pc=0x1`) → SIGSEGV.
At that APPLY2, `accu` (the applied closure) = `0x2010336c9d0`: a **valid
`Closure_tag` block** (hdr `0x8f7`, wosize 2) with a **valid code pointer**
`0x7484011199d0` (in `[prog,prog+size)`) — but `closinfo = 0x5`, i.e. **arity 0**.
Applying an arity-0 closure via APPLY2 (2 args) is nonsensical: `accu` holds the
**wrong closure** (valid memory, wrong function). The arity/`extra_args` dance then
miscounts and leaves `sp` misaligned, so `do_return` reads an argument as the
return frame.

**So the primary corruption is a wrong/stale closure in the `accu` *register*** —
not the value stack (hence the clean forwarded-check; `accu` lives in a C register,
`r13`). Likely a closure that moved in a GC where `accu` was not published as a
root, its old address then reused by the arity-0 closure now in `accu`. **Prime
suspect: `Setup_for_c_call` (interp.c:105) publishes `env` + `pc+1` but NOT `accu`**
— and the last GC before the crash (event 1795) looked like a C-call GC (published
`sp[0]=env=heap`, `sp[1]=0x748401102704=pc+1`). If a bytecode C primitive can leave
a live closure in `accu` across its allocation-triggered GC, `accu` goes stale.
(Stock OCaml's C primitives root their own args, so this may be an interpreter-side
gap specific to how MMTk collects mid-primitive.)

**`accu` was loaded from `caml_global_data` (GETGLOBALFIELD), which is NOT stale.**
Reverse-stepping from the APPLY2: `accu` was set by GETGLOBALFIELD (interp.c:752-754),
`accu = Field(Field(caml_global_data, idx1), idx2)` = the arity-0 closure. But
`caml_global_data` is a **generational global root** (`caml_register_generational_global_root`,
interp.c:328) and `caml_scan_global_roots` — which the binding calls every GC via
`scan_vm_specific_roots` — iterates **all three** lists (`caml_global_roots` +
`_young` + `_old`, globroots.c:256-258), so `&caml_global_data` is visited and
updated on every collection. With `sanity` clean (heap + global-data fields
consistent), the closure read from the global is the *current* value. So the
APPLY2 is applying the value the bytecode told it to — meaning the divergence is
**upstream**: a `pc`/control-flow error led to this GETGLOBALFIELD+APPLY2 (wrong
global index / wrong opcode), i.e. the crash is a *far-downstream* manifestation
(as long suspected). The chain pc→GETGLOBALFIELD→accu→APPLY2→sp-misalign→do_return
is fully mapped; the remaining unknown is the *first* `pc` divergence.

**Correction — `accu` is probably a *valid* closure, not "wrong/arity-0".** The
`closinfo = 0x5` is exactly `Make_closinfo(0, 2)` — what **GRAB** (interp.c:646) and
a 0-capture **CLOSURE** legitimately produce. Bytecode encodes a function's arity
via the GRAB/RESTART dance, *not* the closinfo arity byte (that's the native-code
convention; `Arity_closinfo` `>>56` is meaningless for bytecode closures). So the
wosize-2 `[code, closinfo]` closure in `accu` is a normal no-capture closure, and
applying it via APPLY2 is ordinary currying. The earlier "wrong closure" reading
was a misread of bytecode closure layout — disregard it.

**Lead (1) ruled out.** `caml_global_data` registers as UNTRACKED at first
(`= Val_unit`), then `caml_modify_generational_global_root` files it in
`caml_global_roots_old`; `caml_scan_global_roots` scans young+old+non-gen every GC
(the binding always calls the *full* scan, even for nursery GCs), so it is updated
every collection. The dead young↔old reclassification under MMTk (no minor GC to
promote) is harmless because all lists are scanned. Not the miss.

**Honest status.** The full downstream chain is mapped —
`do_return` reads APPLY2's argument slot as the return PC ← `sp` misaligned ←
APPLY2 of a (valid) closure from `caml_global_data` — but **no missed/stale root was
found** on the value stack (instrumented clean) or in the global roots (scanned
every GC). So the `sp` misalignment's primary cause is *upstream* and GC-triggered
but not a simple unforwarded root: a control-flow/`pc` or curry-dance state
corruption whose effect surfaces far downstream. Pinning the *first* divergence
needs **interactive** rr (forward-step from well before event 1795 / a `pc`-range
conditional breakpoint), which batch-over-ssh can't do reliably (SW-watchpoint
reverse is flaky; forward single-step over the whole window is too slow here).
Remaining cheap lead: audit **`accu` liveness across `Setup_for_c_call`**
(publishes `env`+`pc`, not `accu`) for C_CALL opcodes that allocate, and the
GRAB/RESTART `extra_args` accounting across a STW. The gated `MMTK_DEBUG_STACK_CHECK`
tool + `caml_mmtk_debug_stack_range` remain for reuse.

## M6 fix: adopt orphaned finalisers under MMTk (cross-domain handover)

*2026-06-21*

Found while re-enabling the tabled weak/ephemeron/finaliser/lazy testsuite dirs.
`weak-ephe-final/finaliser_handover` failed: finalisers registered on a spawned
domain that then terminates never ran (0/N). Root cause: at domain termination
`caml_orphan_finalisers` hands the domain's `caml_final_info` to the global
`orph_structs.final_info`; the stock collector drained that back into a live
domain inside the major cycle via `adopt_orphaned_work`, which the M9 stage-3
deletion removed (it was reachable only from the slice). Nothing else adopted
them, and `Scanning::process_weak_refs` only iterates *live* `domain_addrs()`, so
the orphaned tables were never processed — their values became unreachable with
no table holding them.

**Fix.** New `caml_mmtk_adopt_orphaned_finalisers(domain, retain, ctx)`
(`major_gc.c`): under `orphaned_lock`, drains `orph_structs.final_info` into a
live domain's `final_info` — `caml_final_merge_finalisable` for the first/last
tables (mark each orphaned value "old" first, since there is no minor/major split
under MMTk), and splices the already-queued run-queue, `retain`-ing each entry's
`fun`/`val` (they are not roots of this GC). The binding calls it once at the top
of the `process_weak_refs` `with_tracer` closure, into `domain_addrs()[0]`, before
the ephemeron/finaliser passes; draining the list to NULL makes the fixpoint's
later rounds (and GCs with no orphans) no-ops. The merged entries are then handled
by the normal `caml_mmtk_final_update_first` / `_cleanup` path.

Not done: **orphaned ephemerons** (`orph_structs.ephe_list_live`) have the same
gap, but no enabled test exercises it and `caml_orphan_ephemerons` leans on the
now-inert stock phase/mark machinery, so adopting them needs more care — tracked
TODO, not attempted here.

**Re-enabled testsuite triage** (Immix default, `setarch -R`): `weak-ephe-final`
14/0, `lazy` 10/0, `lib-lazy` 2/0 all pass; `ephe-c-api` is `skip;` upstream
(C-API never ported to multicore — not an MMTk issue). Two tests re-tabled as
MMTk-incompatible-by-design (no stock minor heap): `weak-ephe-final/finaliser2`
(its `test1` asserts a `finalise_last` fires synchronously at a `Gc.minor()`
boundary; `test2`/`test3` are fine and handover is covered by
`finaliser_handover`) and `lazy/minor_major_force` (asserts minor-vs-major
residency / remembered-set state). Each carries an in-file comment explaining the
reason and when to re-enable.

## M9 stage 3: delete the dead stock major-GC machinery (branch `m9-stage3-delete`)

*2026-06-21*

Follow-up to the inert step (below). With the stock major collector inert under
always-on MMTk, its mark/sweep/slice/cycle bodies were unreachable. Removed them,
compiler-guided (the build uses `-Wall` without `-Werror`, so unused `static`
functions are warnings — delete → rebuild → read warnings → repeat; `Caml_inline`
helpers don't warn, so those were traced by hand for zero call sites).

**`major_gc.c` 2540→1002 lines.** The seven entry points are now thin stubs that
preserve the inert behaviour:
- `caml_darken` / `caml_darken_cont`: pure no-op.
- `caml_major_collection_slice`: record `major_slice_epoch` then return
  (load-bearing — without it the bytecode mutator spins in `caml_poll_gc_work`).
- `caml_opportunistic_major_collection_slice` / `caml_finish_major_cycle`: no-op.
- `caml_finish_marking` / `caml_finish_sweeping`: set `marking_done` /
  `sweeping_done` (satisfies `caml_domain_terminate`).
- `caml_mark_roots_stw`: no-op — still *called* from `minor_gc.c`, but only when
  `caml_gc_mark_phase_requested` is set, which never happens under MMTk (its only
  setter, `request_mark_phase`, lived in the now-deleted slice path).

Deleted internal machinery: the marking core (`mark`, `do_some_marking`,
`mark_slice_darken`), all mark-stack helpers (push/range/prune/realloc/shrink,
the prefetch buffer, `add_addr`, `ptr_to_chunk*`), the cycle/phase STW machinery
(`cycle_major_heap_from_stw_single`, `stw_cycle_all_domains`,
`stw_finish_major_cycle`, `stw_try_complete_gc_phase`, `is_complete_phase_*`),
`request_mark_phase`, and the ephemeron/orphan helpers reachable only from the
slice (`adopt_orphaned_work`, `ephe_next_cycle`, `prepare_for_ephe_marking`,
`record_ephe_marking_done`, `no_orphaned_work`).

**`shared_heap.c` 1677→1476 lines.** Deleted `caml_sweep` and its now-orphaned
callees `large_alloc_sweep` / `verify_swept` (+ the `verify_pool`/`verify_large`/
`mem_stats` heap-accounting block they used), `caml_redarken_pool` (zero callers),
and `caml_cycle_heap` / `caml_cycle_heap_from_stw_single` (callers were in the
deleted `stw_cycle_all_domains`); removed their decls from `caml/shared_heap.h`.

**Kept (still referenced — "when in doubt, keep"):**
- The pool **allocator** (`caml_shared_try_alloc`, `pool_sweep`, `pool_find`,
  `pool_global_adopt`, `large_allocate`, …). Note `pool_sweep` is *not* dead — the
  allocator calls it from `pool_find`/`pool_global_adopt` to reclaim space; only
  `caml_sweep` (the whole-heap sweep driver) was dead.
- `caml_orphan_ephemerons` / `caml_orphan_finalisers` (called from `domain.c`) and
  the ephemeron machinery they still use: `ephe_mark`, `ephe_sweep`,
  `ephe_todo_list_emptied`, `prepare_for_ephe_sweeping`, `ephe_list_tail`,
  `ephe_cycle_info`, `ephe_lock`.
- `caml_init_major_gc` / `caml_teardown_major_gc` (allocate/free the per-domain
  `struct mark_stack`, which is now never populated but still managed), the pacing
  functions, and the `Gc`-stat/phase helpers (`caml_gc_phase`, `caml_gc_phase_char`,
  `update_major_slice_work`, …).
- `caml_finalise_heap` / `pool_finalise` / `large_alloc_finalise` (shutdown), and
  `caml_verify_heap_from_stw` / `caml_compact_heap` (verification/compaction —
  exported, now callerless but out of the sweep scope; left in place).

**Validated:** `world` + `world.opt` build clean (no unused-function warnings for
the removed set); 25×4 `Domain.join` GC battery passes (bytecode + native, Immix +
StickyImmix); testsuite spot-check `gc-roots` 4/0, `effects` 24/0, `basic` 40/0,
`callback` (incl. `nested_fiber` ✓) — the only non-pass across the spot-check are
the documented baseline flakes (`callback/signals_alloc` bytecode signal-ordering;
`parallel` `domain_dls` `register_mutator … called twice` binding panic /
`domain_parallel_spawn_burn_gc_set` SIGSEGV = bug #3), none of which touch the
deleted code (the three commits only touched `major_gc.c`/`shared_heap.c`/
`shared_heap.h`).

---

## M9 stage 3: stock major GC now INERT on m9-mmtk-only; the "blocker" was a separate bug

*2026-06-21*

Resolves the stage-3 blocker entry below. The `callback/nested_fiber` SIGSEGV was
**not** caused by the inert step — gdb (under rr) at the pre-inert commit showed it
crashes there too, under non-moving MarkSweep, and reverting the guards doesn't fix
it. **Root cause: a pre-existing missing GC root** — the binding's `scan_ocaml_object`
treated `Cont_tag` (245) as an ordinary block, so MMTk never scanned the suspended
fiber `stack_info` a continuation holds in field 0 (`Val_ptr(stack)`, reads as an
immediate). That stack (+ its `Stack_parent` chain) is reachable only through the
continuation block, so a GC taken while a C callback / captured continuation had
detached the parent fiber chain (`alloc_and_clear_stack_parent`) reclaimed live
stack objects → crash on resume. **Latent** until the pr5233 exhaustive-`full_major`
fix made `Gc.full_major` actually collect those mature objects (so it was invisible
in earlier baselines, where `nested_fiber` "passed").

**Fix (commit c5760e7134, on m9-mmtk-only):** add a `Cont_tag` case to the binding's
`scan_object` — recover the stack via `common::scanning::continuation_stack`
(`Ptr_val` of field 0) and scan it with `caml_scan_stack`, feeding each fiber-stack
slot to the slot visitor (the analogue of stock `caml_darken_cont`). Validated:
`nested_fiber` passes, **effects dir 23/0**, clean bootstrap.

**With that fixed, the inert step merged** (commits 8a32daae47 + 07a9917ee5): the
stock major GC is now inert under MMTk — `caml_darken`, the slice drivers, and
`caml_finish_*` are no-ops (finish_* still set `marking_done`/`sweeping_done` so
`caml_domain_terminate` exits). Validated: clean Immix+StickyImmix bootstrap, 25×4
Domain.join battery, nested_fiber + effects. (One earlier hang fixed: the inert slice
must still record `major_slice_epoch` or the bytecode mutator spins in
`caml_poll_gc_work`.) **The stock major GC's mark/sweep/slice bodies are now dead
code** — next: delete them from `major_gc.c` + the `shared_heap.c` sweep.

---

## M9 stage 3 (inert stock major GC): implemented on a branch, BLOCKED by an effects/GC regression

*2026-06-21*

Attempted the stage-3 removal as an *inert* step first (make the stock major GC
never run, then delete the dead bodies). On branch `m9-stage3-inert` (NOT merged):
guard `caml_darken`, `caml_major_collection_slice`/`caml_opportunistic_*`,
`caml_finish_major_cycle`/`marking`/`sweeping` to no-op under MMTk (the finish_*
ones still set `marking_done`/`sweeping_done=1` so `caml_domain_terminate`'s
`marking_and_sweeping_done()` loop still exits). This renders `major_gc.c`'s
mark/sweep/slice bodies unreachable under MMTk.

**Bug found + fixed during validation (GC-pacing hang).** First cut made
`caml_major_collection_slice` a bare no-op, which skipped its tail bookkeeping
`Caml_state->major_slice_epoch = major_slice_epoch`. On the bytecode path
`caml_poll_gc_work` advances the global `caml_major_slice_epoch`, so
`caml_reset_young_limit` then saw `domain->major_slice_epoch < caml_major_slice_epoch`
forever and re-armed the interrupt every safepoint → the mutator spun (bootstrap hung
at `LINKC ocamlobjinfo`). Fixed by recording the epoch in the inert path. After the
fix: clean full bootstrap (Immix + StickyImmix) + a 25×4-domain spawn/join battery.

**BLOCKER (open): `callback/nested_fiber` SIGSEGVs.** It passed in the flag-off
baseline AND the pre-inert M6 run, so the inert step regressed it. The test runs
Effects (`match_with`) with a C callback (`caml_to_c`) that does `Gc.full_major` +
allocation inside a nested fiber. It prints `g() check 2047` / `g() returned: 1` /
`f() check: 15` then **crashes during the outer effect-handler's return path** (before
`f() returned: 2`). So a `Gc.full_major` taken inside a nested fiber, with the stock
major GC inert, corrupts something that manifests on fiber return. Not yet
root-caused — none of the guarded entry points is obviously on the fiber/GC path
(`Gc.full_major` routes to `caml_mmtk_collect`, not the stock cycle), so suspect a
subtle interaction (continuation-stack handling, or the exhaustive-GC + inert combo).
Needs gdb (like the epoch hang). **Do NOT merge `m9-stage3-inert` until this is fixed.**

**Status:** `m9-mmtk-only` stays at the validated M6 milestone (M6 default-on). The
stock major GC is *not yet* removed — the inert step is correct for bootstrap +
multidomain but breaks effects+GC; the stock collector retains a subtle load-bearing
role for the nested-fiber/`Gc.full_major` path that must be understood before it can
be disabled. This is the precise stage-3 blocker.

---

## Default-on M6 validated by full bootstrap; bug #5: fuzzer OOM hang

*2026-06-21*

`MMTK_WEAK_REFS` is now ON by default. Validated under the heavy stress test — a full
`make clean && make world` bootstrap, where the **compiler's own internal Weak
hashtables now actively clear** (vs the old keep-alive). Results: clean bytecode
bootstrap on **Immix and StickyImmix**, clean native `world.opt`, and a clean
core/lib testsuite spot-check (`basic` 76/76, all `lib-*`). So weak-clearing under
the real compiler workload is correct. This is the green light for the M9 stage-3
removal (next).

**bug #5 (separate, not weak-ref): `lib-marshal/fuzzy` hangs under MMTk.** The fuzzer
flips random bytes in a marshalled buffer then `Marshal.from_bytes`, expecting a
`Failure`/`Invalid_argument`/`Out_of_memory` it catches. At `-n ≥ 100` the `ocamlrun`
child pins 100% CPU at a flat ~16 MB RSS and never completes (not OOM-killed, not
SIGSEGV; `-n 50` completes fine). Likely a corrupted length field makes the
unmarshaler request an absurd allocation that, under MMTk, **spins in the
allocator/GC instead of raising `Out_of_memory`** (which the test would catch). An
OOM-surfacing behavioural difference (MMTk allocation-failure → OCaml exception path),
not a GC-correctness bug. Deferred.

---

## M6 is solid: pr5233 fixed (full_major must be exhaustive); "1/8" was a harness artifact

*2026-06-21*

Two findings resolve the M6 picture — it is in good shape, and the major-GC removal
is unblocked.

**(1) `regression/pr5233` root-caused + fixed.** Symptom: a weakly-reachable value
was over-retained (weak slot never cleared) — but ONLY under StickyImmix, and only
for **large-object-space** values. Isolated with a minimal test (`Weak.set` a 1 MB
Bytes, `Gc.full_major` ×3, check): StickyImmix → RETAINED, Immix → CLEARED; small
values clear on both. Root cause: `Gc.major`/`full_major`/`compact` routed to
`memory_manager::handle_user_collection_request(mmtk, tls)`, which calls
`handle_user_collection_request(tls, false, false)` — **exhaustive=false**. Under a
generational plan (StickyImmix/GenImmix) a non-exhaustive user GC is a *nursery*
collection, so mature/LOS objects are never re-traced and weakly-reachable ones
never get reclaimed. Immix is non-generational (every GC is full) so it was masked
there. Fix: the binding now calls `mmtk().handle_user_collection_request(tls, true,
true)` (force + exhaustive) so `Gc.full_major` is a true full-heap collection on
every plan. Verified: rd.ml LOS-weak CLEARs, distilled + **real pr5233 now print the
reference output** under StickyImmix. (This is a general StickyImmix correctness fix,
not weak-specific — `Gc.full_major` now reclaims mature garbage as promised.)

**(2) "M6 fixes only 1/8 targeted tests" was a measurement artifact.** The full
`make parallel` runs reported most weak/finaliser tests failing, but re-running each
**in isolation** shows they PASS deterministically — flag-on AND flag-off. The
parallel-harness failures are `sh: 1: : Permission denied` from ocamltest's
output-comparison subprocess under load (the tree built with `WITH_OCAMLTEST=` empty),
not GC/flag effects. So M6 introduces **no regressions** and the real weak/finaliser
behaviour is correct. Genuine remaining testsuite failures are **non-M6**:
`statmemprof/*` (Gc.Memprof unsupported), `lib-runtime-events/*` (stock EV_* not
emitted), `misc/gcwords`+`Gc.stat` accounting (M9 stage-4), `c-api/alloc_async`
(separate hang), and the flaky multidomain spawn-burn crashes (bug #3, pre-existing,
crash flag-off too).

**Status:** M6 (weak arrays, ephemerons, `Gc.finalise`/`finalise_last`, custom-block
finalizers) works under both Immix and StickyImmix with `MMTK_WEAK_REFS=1`. Next:
flip the default on, then proceed with the M9 stage-3 removal cascade.

---

## M6 custom-block finalize landed (pr3612 passes); pr5233 over-retention still open

*2026-06-21*

Closing most of the gap from the "INCOMPLETE" entry below. Implemented custom-block
finalization (`Custom_operations.finalize`) via MMTk's finalizer queue
(`memory_manager::add_finalizer`/`get_finalized_object`, FinalizableType =
ObjectReference): register every finalizable custom block at creation, drain the
dead ones at a safepoint and run their finalize op. Still gated on `MMTK_WEAK_REFS`.

- Register sites: `caml_alloc_custom` (`custom.c`) **and** the unmarshal path
  (`intern.c` ~806) — the latter was the catch: `Marshal.from_string` builds custom
  blocks directly, bypassing `caml_alloc_custom`, so pr3612's ~1M deserialised blocks
  weren't registered (only 1 of 1M finalized). Same lesson as the old M2 intern bug:
  *every* object-creation path must be hooked. Bigarray sub-arrays go through
  `caml_alloc_custom_mem`, so they're covered.
- Drain: `caml_mmtk_run_custom_finalizers` (`mmtk.c`) called from
  `caml_final_do_calls`; `caml_mmtk_uninterrupt` sets the domain's action-pending
  post-GC so the drain runs at the next safepoint.
- **`regression/pr3612` now PASSES** (flag on; flag-off still 1000001 ≠ −1). Smoke
  tests still green. **Implication: `shared_heap.c` custom-finalize-on-sweep is now
  replaced** for stage 3 (still need the rest of shared_heap audited).

Also fixed an ephemeron/finaliser ordering bug: the mark pass was unlinking dead
ephemerons from `ephe_info` *before* finalise-first could resurrect them, orphaning
a resurrected weak array (PR#5233). Now the mark pass keeps dead ephemerons linked;
only the clean pass (after resurrection) unlinks the still-dead ones.

**STILL OPEN — `regression/pr5233`.** A weak array resurrected by its finaliser:
after the referent dies, the weak slot should read "no value", but we print
"value found / testing... ok" — the referent is **over-retained** (kept alive +
intact), not dangling (so not the original safety bug, but still wrong).
- **LOS ruled out** (2026-06-21): a minimal weak-clear test with a 1 MB (LOS) value
  *and* a small value both clear correctly under StickyImmix + Immix. So it is not a
  large-object-space weak-clear bug.
- **Narrowed to the finaliser-resurrection-under-moving-GC path.** When `process_weak_refs`
  resurrects the dead weak array via `trace_object` (finalise-first), a moving plan
  may **copy** it; the finaliser queue / `smuggle` get the new copy, but the
  `ephe_info` list still holds the *pre-copy* address. Whether the next clean pass
  re-forwards that link or unlinks it (orphaning the live copy so its slot never
  clears) hinges on `is_reachable(old_addr)` for a forwarded-from reference. Suspect
  the callbacks should treat `get_forwarded_object().is_some()` as reachable — BUT
  the "retained while live" smoke test (a live, likely-copied referent) passes, which
  argues `is_reachable` already follows forwarding. **Unresolved — needs instrumented
  callbacks on a distilled resurrection repro (turing).** Deferred until the full-suite
  re-measurement triages whether pr5233 is the only remaining M6 gap or one of several
  (finaliser-timing: signals_alloc, lib-threads/tls, lib-sys/opaque).

---

## M6 full-suite validation: INCOMPLETE — custom-block finalize is the missing mechanism

*2026-06-21*

Correcting the optimistic "M6 implemented + validated" entry below: the smoke tests
passed, but the **full bytecode suite with `MMTK_WEAK_REFS=1` fixes only 1 of the 8
targeted weak/finaliser failures** (vs the flag-off baseline). No flag-attributable
regressions (the lone PASS→FAIL, `parallel/domain_parallel_spawn_burn`, is a
pre-existing flaky moving-GC crash — reproduced with the flag *off* too; same class
as bug #3), and the smoke binaries still pass — so what's implemented is *correct for
simple cases* but *covers far less than expected*. M6 is **partial, not done**, and
the major-GC removal is **not** unblocked yet.

Gap analysis:
- **Custom-block finalizers are entirely unimplemented under MMTk — the big one.**
  These are `Custom_operations.finalize` (Bigarray, `Int64`/`Nativeint`, channels,
  marshalled custom blocks…), a mechanism *separate* from `Gc.finalise`. Stock OCaml
  calls them from `shared_heap.c` **sweep** (lines 574/687/716/770/1409); under MMTk
  the stock sweep never runs, so they never fire. This fails `regression/pr3612`
  (deserialised custom blocks never freed) and likely `c-api/alloc_async` (hangs
  waiting on one) and the Gc-stat/bigarray cases. My M6 work only did the OCaml
  `Gc.finalise` table, not custom blocks. Fixing it needs MMTk-side dead-object
  notification — register finalizable custom blocks (those with a non-NULL
  `finalize`) via mmtk's finalizer queue at `caml_alloc_custom`, and run their
  `finalize` op on the dead ones from a `process_weak_refs`/finalizer pass — *or* a
  scan that detects dead custom blocks. Non-trivial; this is the gating piece. NB
  this also means **`shared_heap.c` cannot just be deleted in stage 3** — its sweep
  is load-bearing for custom finalize until this lands.
- **Weak + finaliser-resurrection ordering** (`regression/pr5233`): a weak slot must
  be cleared based on reachability from the *strong + ephemeron* closure, **before**
  finaliser resurrection — a value resurrected only to run its finaliser must still
  read as cleared through a weak pointer. My pass lets finalise-first retention keep
  such a value visible → the weak slot wrongly stays full ("value found" vs expected
  "no value"). Need to match OCaml's phase order (decide weak/ephemeron clearing
  before/independently of finaliser resurrection).
- Probable false failures: `tool-ocaml/t340-weak` + `t350-heapcheck` fail with
  `Not_found` from the `lib.cmo` toplevel harness — an infra issue, maybe not M6.
- Confirmed fixed by the flag: `backtrace/callstack`.

**Status:** keep `MMTK_WEAK_REFS` default-OFF; do NOT flip the default or start the
major-GC removal until custom-block finalize + the pr5233 ordering are done and the
suite re-validates. See the (now-qualified) M6 entries below.

---

## Testsuite baseline under StickyImmix (M9 stage-2 tree) + a new multidomain repro

*2026-06-21*

Ran the bytecode testsuite on the committed stage-2 tree (`26c86d681a`) under
`MMTK_PLAN=StickyImmix MMTK_HEAP_SIZE_MB=512` (default flag-off scheme). Result:
**1524 tests → 1476 PASS, 48 non-PASS.** All core dirs pass (basic*, lib-* data
structures, float, effects, exceptions, callback, parallel bar one). Harness notes
for next time: build with `make world` (not `make all` — a fresh tree has no
`boot/ocamlrun`); set `native_compiler=false`/`native_dynlink=false` in
`ocamltest/ocamltest_config.ml` or every test's native variant errors and masks the
bytecode result; run per-dir with a ~120s/test cap (serial `make all` hangs on the
finaliser tests). Full log on turing `/tmp/testsuite-stickyimmix.log`.

**Real regression (1) — new repro, bug #3:**
`tests/parallel/domain_parallel_spawn_burn_gc_set.ml` **SIGSEGVs deterministically
under StickyImmix** (every run) but prints `ok`/exits 0 **under Immix** (every run).
Multi-domain test that hammers `Gc.set` + `Gc.minor`/`Gc.major` + `Domain.spawn`.
StickyImmix-specific (generational/moving) + multi-domain — the same class as the
tight-heap root-coverage gap (bug #2). Default plan is Immix (passes), so it does
not block the default, but it is a genuine moving+multidomain bug to root-cause
(likely a missed/!forwarded root on the spawn/STW path under a generational plan).

**The other 47 are known-unsupported, none are crashes**, and they split into
buckets — importantly, two of them are exactly what **M6 (`MMTK_WEAK_REFS=1`)
fixes**, so they double as M6's broader acceptance test:
- *finalisers don't fire* (6): `c-api/alloc_async`, `backtrace/callstack`,
  `lib-threads/tls`, `lib-sys/opaque`, `regression/pr3612`, `callback/signals_alloc`
  — **M6 should fix.**
- *weak refs don't clear* (2): `regression/pr5233`, `tool-ocaml/t340-weak` —
  **M6 should fix.**
- *Gc.stat/counters differ from stock* (6: `misc/gcwords`, `lib-obj/with_tag`,
  `regression/pr7798`, `lib-bigarray/subarraystub`, `parallel/major_gc_wait_backup`,
  `tool-ocaml/t350-heapcheck`) — M9 stage-4 (`Gc.stat` on MMTk numbers), not M6.
- *statmemprof* (18) — `Gc.Memprof` sampling unsupported by MMTk.
- *lib-runtime-events* (8) — MMTk doesn't emit stock `EV_MINOR/EV_MAJOR`.
- build-infra quirks (`output-complete-obj` custom relink misses `libmmtk_ocaml.a`;
  2 slow-not-hung timeouts) — not GC bugs.

---

## M6 implemented + validated (gated by `MMTK_WEAK_REFS`)

*2026-06-21*

The M6 design below is now **implemented and validated**, merged to `m9-mmtk-only`
behind `MMTK_WEAK_REFS=1` (default off — the conservative `caml_mmtk_scan_ephe_roots`
keep-all-alive scheme is still the default, so behaviour is unchanged unless the flag
is set). What landed:

- `process_weak_refs` (scanning.rs) runs a two-phase pass: an **ephemeron mark
  fixpoint** (retain data only when all keys are reachable) interleaved with
  **finalise-first** retention (ordered *after* ephemeron marking converges each
  round, so a value reachable via a live ephemeron's data is never finalised
  early), then a **cleanup** phase (clear dead ephemeron keys/data, queue dead
  `finalise_last` values as unit, forward all survivors).
- C glue: `caml_mmtk_ephe_mark_pass`/`_clean_pass` (runtime/mmtk.c) and
  `caml_mmtk_final_update_first`/`_cleanup` (runtime/finalise.c), driven by
  is_reachable/forward/retain callbacks that the binding closes over the GC
  worker's tracer. Under the flag the root scan passes `do_final_val=0` (keep
  finaliser *functions* + the run-queue alive, let table *values* die).
- Finalisers actually **run** now (under the flag) — `Gc.finalise` and
  `Gc.finalise_last` both fire.

**Validation (turing, StickyImmix + Immix, 32 MB heap):** weak-clear/ephemeron
smoke `ALL PASS` ×11, finaliser smoke `PASS` ×5; flag-off baseline reproduces the
expected "never clears" behaviour; **no crash, no sanity panic, and no live
referent/key ever wrongly cleared** (the memory-safety invariant). Smoke tests are
narrow but cover the core machinery; broader validation = re-enable the tabled
weak/ephemeron/finaliser testsuite dirs and run them under the flag (pending).

**Next (M9 stage 3 unblock):** broader testsuite validation → flip the default →
remove the conservative scheme + the stock major GC (the removal cascade in the
stage-3 audit entry below).

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
SEGV from the entry below) is **root-caused and fixed**. Fix: `gc/mmtk/common/src/slot.rs`
(`FieldSlot::classify`).

> **Correction (2026-06-22):** the speculation here that this *also* fixed the CI ocamldoc
> crash was wrong. That crash — now pinned to a mutator deref in `odoc_man.ml:307` (not
> `Lexing.engine`) — is **still open**; see the top-of-file entry. The `slot.rs` fix itself stands.

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
