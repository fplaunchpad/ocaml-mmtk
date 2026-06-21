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

/* Set to 1 once MMTk is initialised and the current domain's mutator is bound.
 * The allocation macros consult this to decide MMTk vs. the stock minor heap;
 * it stays 0 during early runtime bootstrap (before MMTk is ready). */
extern int caml_mmtk_enabled;

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

/* Stop-the-world support. caml_mmtk_stw_poll is called from
 * caml_handle_gc_interrupt: if a collection is in progress it parks this domain
 * at the safepoint. caml_mmtk_interrupt / _uninterrupt poison / reset a domain's
 * young_limit; they are called by the GC worker (via the binding). */
extern void caml_mmtk_stw_poll(void);
extern void caml_mmtk_park(void);

/* Report a domain's weak arrays / ephemerons (domain->ephe_info lists) as strong
 * roots, so MMTk keeps them alive and updated instead of letting them dangle.
 * Conservative interim until proper weak-reference processing exists. */
extern void caml_mmtk_scan_ephe_roots(scanning_action f, void *fdata,
                                      caml_domain_state *domain);

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

/* Generational write barrier: record that `count` value-sized slots at `start`
 * may now point into the nursery. Self-gated (no-op unless a generational plan
 * is active). Called from write_barrier, caml_initialize, and array blits. */
extern void caml_mmtk_region_barrier(volatile value *start, mlsize_t count);
extern void caml_mmtk_interrupt(uintnat domain_state_addr);
extern void caml_mmtk_uninterrupt(uintnat domain_state_addr);

/* Blocking-section participation: a domain in a C blocking section is safe for
 * GC (not mutating; sp published). caml_mmtk_enter/leave_blocking are called
 * from caml_enter/leave_blocking_section. caml_mmtk_domain_terminate
 * deregisters a terminating domain. */
extern void caml_mmtk_enter_blocking(void);
extern void caml_mmtk_leave_blocking(void);
extern void caml_mmtk_domain_terminate(caml_domain_state *dom);

#endif /* CAML_INTERNALS */

#endif /* CAML_MMTK_H */
