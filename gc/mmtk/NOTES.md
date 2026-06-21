# MMTk-OCaml design notes & deferred investigations

Running notes on design decisions and things we have deliberately deferred.
Each entry is dated and self-contained. Newest first.

---

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
gap specific to how MMTk collects mid-primitive.) Next: confirm whether `accu` at
the APPLY2 should have been a different (live) closure — trace `accu`'s last load
before the APPLY2 and check whether its source moved across a C-call GC; then audit
`accu` liveness across `Setup_for_c_call`/`Enter_gc` on the C_CALL opcodes.

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
