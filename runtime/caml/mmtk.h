/**************************************************************************/
/*                                                                        */
/*                MMTk garbage collector glue (bytecode)                  */
/*                                                                        */
/**************************************************************************/

/* C glue between the OCaml bytecode runtime and the in-tree MMTk binding
 * (gc/mmtk). This header is only meaningful for the bytecode runtime; the
 * allocation redirection in memory.h/memory.c is guarded by #ifndef
 * NATIVE_CODE, so none of this is referenced by the native runtime. */

#ifndef CAML_MMTK_H
#define CAML_MMTK_H

#ifdef CAML_INTERNALS

#include "config.h"
#include "mlvalues.h"
#include "roots.h"

/* TLAB / nursery-aliasing mode (MMTK_TLAB=1): MMTk owns the nursery; the native
 * fast-path bumps an MMTk Immix block and the runtime refills a new block
 * instead of running a minor GC. No OCaml minor GC, no promotion. Read on the
 * allocation slow path. */
extern int caml_mmtk_tlab;

/* Refill the domain's young region with a fresh MMTk block (TLAB mode), in place
 * of a minor GC. `whsize` is the words (header included) the triggering
 * allocation needs. Returns 1 on success, 0 on exhaustion / unsupported plan. */
extern int caml_mmtk_refill_tlab(caml_domain_state *dom, mlsize_t whsize);

/* Service an explicit Gc.major/full_major/compact request by triggering a real
 * MMTk collection (rather than the stock major-GC machinery, which is bypassed
 * under MMTk). No-op for NoGC and when MMTk is disabled. */
extern void caml_mmtk_collect(void);

/* True (1) iff value `v` is a heap block currently in the generational nursery.
 * 0 for immediates, mature blocks, non-generational plans, and NoGC. Used by the
 * domain-termination result handoff to confirm promotion (issue #31). */
extern int caml_mmtk_is_young(value v);

/* Initialise MMTk once for the process. Reads the plan from the MMTK_PLAN
 * environment variable (default "NoGC") and the heap size from
 * MMTK_HEAP_SIZE_MB (default 1024 MiB). Idempotent. */
extern void caml_mmtk_init(void);

/* Bind the given domain as an MMTk mutator and store the handle in
 * dom->mmtk_mutator. Calls caml_mmtk_init() first if needed, then enables the
 * MMTk allocation path. */
extern void caml_mmtk_domain_init(caml_domain_state *dom);

/* Allocate a small block through MMTk and write its header. Returns the OCaml
 * value (pointer to field 0). Mirrors the result of Alloc_small. */
extern value caml_mmtk_alloc_small(mlsize_t wosize, tag_t tag,
                                   reserved_t reserved);

/* Allocate a (possibly large) block through MMTk; replacement for the body of
 * alloc_shr. Returns the OCaml value. */
extern value caml_mmtk_alloc_shr(mlsize_t wosize, tag_t tag,
                                 reserved_t reserved);

/* Non-raising variant: returns (value)0 on heap exhaustion instead of raising
 * Out_of_memory, so the unmarshaller can clean up first. See runtime/intern.c. */
extern value caml_mmtk_try_alloc_shr(mlsize_t wosize, tag_t tag);

/* Stop-the-world support. caml_mmtk_stw_poll is called from
 * caml_handle_gc_interrupt: if a collection is in progress it parks this domain
 * at the safepoint. caml_mmtk_interrupt / _uninterrupt poison / reset a domain's
 * young_limit; they are called by the GC worker (via the binding). */
extern void caml_mmtk_stw_poll(void);
extern void caml_mmtk_park(uintnat domain_state_addr);
/* Transition a domain to the RUNNING (must-stop) state, parking cooperatively if
 * a collection is active (so the backup thread services OCaml STW meanwhile).
 * Used on every STOPPED->RUNNING edge. */
extern void caml_mmtk_become_running(uintnat domain_state_addr);

/* Report a domain's weak arrays / ephemerons (domain->ephe_info lists) as strong
 * roots, so MMTk keeps them alive and updated instead of letting them dangle.
 * Conservative interim until proper weak-reference processing exists. */
extern void caml_mmtk_scan_ephe_roots(scanning_action f, void *fdata,
                                      caml_domain_state *domain);

/* DEBUG (moving-GC bug hunt): a domain's current bytecode value-stack live range
 * [*lo, *hi) (sp .. Stack_high). Used by the binding's post-GC stale-root check. */
extern void caml_mmtk_debug_stack_range(caml_domain_state *domain,
                                        value **lo, value **hi);

/* M6 (experimental, MMTK_WEAK_REFS=1): MMTk-native weak/ephemeron processing,
 * driven by the binding's Scanning::process_weak_refs in place of the conservative
 * caml_mmtk_scan_ephe_roots scheme above. caml_mmtk_weak_refs is the gate flag.
 * The pass functions walk a domain's ephemeron lists using MMTk reachability:
 * is_reachable(v) — reached by the strong closure; forward(v) — v's current
 * address under a moving plan; retain(ctx,v) — trace v (keep alive) + current
 * address. mark_pass retains data of fully-reachable-key ephemerons (returns 1 if
 * any newly retained — re-run to a fixpoint); clean_pass then clears dead keys/data
 * and forwards survivors. See gc/mmtk/NOTES.md (M6 design). */
extern int caml_mmtk_weak_refs;
typedef int   (*caml_mmtk_ephe_reachable_fn)(value v);
typedef value (*caml_mmtk_ephe_forward_fn)(value v);
typedef value (*caml_mmtk_ephe_retain_fn)(void *ctx, value v);
extern int  caml_mmtk_ephe_mark_pass(uintptr_t domain_addr,
                                     caml_mmtk_ephe_reachable_fn is_reachable,
                                     caml_mmtk_ephe_forward_fn forward,
                                     caml_mmtk_ephe_retain_fn retain, void *ctx);
extern void caml_mmtk_ephe_clean_pass(uintptr_t domain_addr,
                                      caml_mmtk_ephe_reachable_fn is_reachable,
                                      caml_mmtk_ephe_forward_fn forward);

/* M6 finalisers (Gc.finalise / finalise_last), same gate + callbacks as above.
 * update_first retains + queues dead first-set values (returns 1 if any retained,
 * for the mark fixpoint); cleanup queues dead last-set values (as unit) and
 * forwards surviving table values. Defined in runtime/finalise.c. */
extern int  caml_mmtk_final_update_first(uintptr_t domain_addr,
                                         caml_mmtk_ephe_reachable_fn is_reachable,
                                         caml_mmtk_ephe_retain_fn retain, void *ctx);
extern void caml_mmtk_final_cleanup(uintptr_t domain_addr,
                                    caml_mmtk_ephe_reachable_fn is_reachable,
                                    caml_mmtk_ephe_forward_fn forward,
                                    caml_mmtk_ephe_retain_fn retain, void *ctx);

/* Adopt finalisers orphaned by terminated domains (orph_structs, in major_gc.c)
 * into the live domain [domain_addr], so the passes above then process them.
 * Drains the orphan list, so it is a no-op after the first call within one GC's
 * mark fixpoint. [retain] forwards the already-queued run-queue entries (not
 * roots of this GC). Defined in runtime/major_gc.c. */
extern void caml_mmtk_adopt_orphaned_finalisers(uintptr_t domain_addr,
                                                caml_mmtk_ephe_retain_fn retain,
                                                void *ctx);

/* Scan finalisers orphaned by terminated domains (orph_structs, in major_gc.c)
 * as roots: mirrors caml_final_do_roots over every orphaned struct so the binding
 * can report their fun/val slots in scan_vm_specific_roots. Without this the
 * orphaned nursery values are neither rooted nor forwarded between orphaning and
 * adoption, and a queued finaliser runs against recycled nursery bytes
 * (finaliser_handover use-after-free). [do_val] gates first/last *values* (0 with
 * MMTK_WEAK_REFS, matching caml_do_roots, so finalisers can fire); run-queue (todo)
 * values are always rooted. Defined in runtime/major_gc.c. */
extern void caml_mmtk_scan_orphaned_finalisers(scanning_action act,
                                               scanning_action_flags fflags,
                                               void *fdata, int do_val);

/* Custom-block finalizers (Custom_operations.finalize), same MMTK_WEAK_REFS gate.
 * register: enqueue a finalizable custom block on MMTk's finalizer queue (called
 * from caml_alloc_custom). run_custom_finalizers: drain the ready queue + run each
 * finalize op (called at a safepoint from caml_final_do_calls). */
extern void caml_mmtk_register_finalizable(value v);
extern void caml_mmtk_run_custom_finalizers(void);

/* Fill Gc.stat heap-size fields (in words) from MMTk's page accounting; the stock
 * shared-heap counters are ~0 under MMTk. See runtime/mmtk.c. */
extern void caml_mmtk_gc_stats(uintnat *heap_words, uintnat *live_words,
                               uintnat *free_words, uintnat *collections);

/* Total bytes reserved by MMTk for the heap. Replaces the deleted
 * caml_heap_size(shared_heap). See runtime/mmtk.c. */
extern uintnat caml_mmtk_heap_size_bytes(void);

/* Generational write barrier: record that `count` value-sized slots at `start`
 * may now point into the nursery. Self-gated (no-op unless a generational plan
 * is active). Called from write_barrier, caml_initialize, and array blits. */
extern void caml_mmtk_region_barrier(volatile value *start, mlsize_t count);

/* SATB (snapshot-at-the-beginning) deletion write barrier for the concurrent
 * plan (ConcurrentImmix). Greys the OLD referents in `count` value-sized slots at
 * `start`. MUST be called BEFORE the store (while the slots still hold the old
 * values). Self-gated (no-op unless the concurrent plan is active). Called from
 * write_barrier (caml_modify) and, pre-store, from the array-fill paths. */
extern void caml_mmtk_satb_barrier(volatile value *start, mlsize_t count);

/* Per-continuation scan lock (concurrent plan). caml_mmtk_cont_lock is called from
 * the continuation resume path (caml_continuation_use_noexc) BEFORE the fiber stack
 * is taken/switched-onto, so a resume cannot race a GC worker concurrently scanning
 * that continuation's stack. Self-gated: a no-op unless the concurrent plan is
 * active. Pair lock/unlock. `cont` is the continuation block. */
extern void caml_mmtk_cont_lock(value cont);
extern void caml_mmtk_cont_unlock(value cont);

/* SATB snapshot of a continuation's fiber stack, called on the resume path (under
 * the concurrent plan + active marking) BEFORE the cont->stack edge is deleted, so
 * the stack's snapshot roots are greyed into the SATB buffer and survive the cycle.
 * Self-gated; a no-op off the concurrent marking window. */
extern void caml_mmtk_cont_snapshot(value cont);
extern void caml_mmtk_interrupt(uintnat domain_state_addr);
extern void caml_mmtk_uninterrupt(uintnat domain_state_addr);

/* Ragged safepoint (excise Phase 2, step 1). caml_mmtk_quiesce_ack records, at a
 * safepoint, that this domain has passed one (a plain atomic store of the global
 * quiesce epoch into the domain's mmtk_seen_quiesce_epoch; no lock — called from
 * caml_poll_gc_work). caml_mmtk_quiesce_running_domains blocks the caller until
 * every domain RUNNING OCaml at call time has either acked a safepoint or left
 * the RUNNING set, WITHOUT a global STW barrier and WITHOUT a GC — so a writer
 * that just published new state can drain all in-flight lock-free readers of the
 * OLD state before freeing it. DORMANT: no callers yet. See runtime/mmtk.c. */
extern void caml_mmtk_quiesce_ack(caml_domain_state *d);
extern void caml_mmtk_quiesce_running_domains(void);

/* Blocking-section participation: a domain in a C blocking section is safe for
 * GC (not mutating; sp published). caml_mmtk_enter/leave_blocking are called
 * from caml_enter/leave_blocking_section with the domain's caml_domain_state
 * address, captured by the caller while Caml_state is still bound — these must
 * NOT read Caml_state themselves, as the blocking-section hooks release/re-acquire
 * the domain lock asymmetrically around the calls (enter sees Caml_state NULL,
 * leave sees it valid), which would unbalance MMTk's safe-stopped accounting.
 * caml_mmtk_domain_terminate deregisters a terminating domain. */
extern void caml_mmtk_enter_blocking(uintnat dom);
extern void caml_mmtk_leave_blocking(uintnat dom);
extern void caml_mmtk_domain_terminate(caml_domain_state *dom);
/* Deregister a force-cancelled peer (excise Phase 3b, caml_stop_all_domains) from
 * MMTk's registry + RUNNING set, WITHOUT waiting for an in-flight collection (the
 * peer's roots are not torn down, so there is nothing to protect). */
extern void caml_mmtk_deregister_domain(caml_domain_state *dom);
/* Block until any in-flight collection finishes (returns at once if none is
 * active). Used to RCU-retire memory a GC worker may have snapshotted as a root:
 * remove the root, wait the grace period, then free (see free_domain_ml_values,
 * GH#15 Bug B). */
extern void caml_mmtk_wait_collection_done(void);

/* Collection-suppression counter. While the count is non-zero MMTk does not
 * trigger a collection (the binding's VMCollection::is_collection_enabled reads
 * caml_mmtk_collection_enabled via gc_trigger). Restores vanilla's "no GC during
 * intern_rec" invariant (runtime/intern.c). Nestable; cross-domain safe. */
extern void caml_mmtk_disable_collection(void);
extern void caml_mmtk_enable_collection(void);
extern int  caml_mmtk_collection_enabled(void);

#endif /* CAML_INTERNALS */

#endif /* CAML_MMTK_H */
