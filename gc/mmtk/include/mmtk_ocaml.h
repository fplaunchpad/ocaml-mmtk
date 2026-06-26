#ifndef MMTK_OCAML_H
#define MMTK_OCAML_H

/* C ABI for the in-tree MMTk binding (gc/mmtk/binding).
 * Consumed by the patched OCaml runtime (runtime/). Single symbol prefix:
 * mmtk_ocaml_*. */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque mutator handle; stored per-domain (e.g. caml_domain_state->mmtk_mutator). */
typedef void* MMTk_Mutator;

/* ── Initialisation ─────────────────────────────────────────────────── */

/**
 * Initialise MMTk.  Call once (from caml_main / startup) before any domain
 * is bound.
 *
 * @param heap_size  Total heap size in bytes (FixedHeapSize trigger).
 * @param plan       GC plan name: "NoGC", "MarkSweep", "Immix", "StickyImmix", …
 */
void mmtk_ocaml_init(size_t heap_size, const char* plan);

/** Start MMTk GC worker threads.  Call once after mmtk_ocaml_init. */
void mmtk_ocaml_initialize_collection(uintptr_t tls);

/* ── Domain (mutator) lifecycle ─────────────────────────────────────── */

/**
 * Register an OCaml 5.x domain as an MMTk mutator.
 *
 * @param domain_state_addr  Address of the domain's caml_domain_state struct.
 * @return  Opaque mutator handle.
 */
MMTk_Mutator mmtk_ocaml_bind_mutator(uintptr_t domain_state_addr);

/** Destroy the mutator for a terminating domain. */
void mmtk_ocaml_destroy_mutator(MMTk_Mutator mutator);

/* ── Allocation ─────────────────────────────────────────────────────── */

/**
 * Allocate an OCaml heap block.  Writes the header; returns a pointer to field 0.
 *
 * @param mutator    The domain's mutator handle.
 * @param wosize     Number of word-sized fields.
 * @param tag        OCaml block tag (0–255).
 * @param semantics  0 Default, 1 Immortal, 2 Los, 6 NonMoving.
 */
void* mmtk_ocaml_alloc(MMTk_Mutator mutator, size_t wosize, size_t tag, int semantics);

/**
 * Native TLAB refill (nursery aliasing). Hands the runtime a contiguous region
 * [*out_start, *out_end) to bump-fill as its young nursery — the region is an
 * MMTk Immix block. Call when the inlined native fast-path exhausts the young
 * region, in place of a minor GC.
 *
 * @param mutator    The domain's mutator handle.
 * @param min_bytes  Minimum region size required (the triggering allocation,
 *                   header included). The region returned is >= this.
 * @param out_start  Receives the low bound (block start; OCaml's young_limit).
 * @param out_end    Receives the high bound (block end; OCaml's young_end).
 * @return  true on success; false on heap exhaustion (raise Out_of_memory) or
 *          if the plan's Default allocator is not Immix (caller falls back).
 */
bool mmtk_ocaml_refill_tlab(MMTk_Mutator mutator, size_t min_bytes,
                            uintptr_t* out_start, uintptr_t* out_end);

/* ── GC control ─────────────────────────────────────────────────────── */

void mmtk_ocaml_handle_user_collection_request(uintptr_t domain_state_addr);

/* ── Stop-the-world (multi-domain) ──────────────────────────────────── */

/** True while a collection is in progress (queried at domain safepoints). */
bool mmtk_ocaml_stw_active(void);

/** Park the calling domain (by caml_domain_state address) at a safepoint until
    the collection finishes: mark it STOPPED, wait for the resume epoch, then mark
    it RUNNING again. */
void mmtk_ocaml_stw_park(uintptr_t domain_state_addr);

/** A domain (by address) is entering a C blocking section (counts as safe-stopped). */
void mmtk_ocaml_enter_blocking(uintptr_t domain_state_addr);

/** Atomically try to mark a domain (by address) RUNNING. Returns 0 iff a
    collection is active (caller must park cooperatively and retry), else inserts
    it into the RUNNING set and returns 1. */
int mmtk_ocaml_try_mark_running(uintptr_t domain_state_addr);

/** Deregister a terminating domain (by caml_domain_state address). */
void mmtk_ocaml_deregister_domain(uintptr_t domain_state_addr);

/** Wait until no collection is in progress (does not touch the RUNNING set).
    Used by the terminate path after deregistration so the domain's roots stay
    valid until any collection that snapshotted the registry has finished. */
void mmtk_ocaml_wait_collection_done(void);

/** Ragged safepoint (excise Phase 2, step 1). Snapshot the addresses of domains
    currently RUNNING OCaml into buf[0..len); returns the count written
    (truncated to len). mmtk_ocaml_is_running reports whether one domain is still
    RUNNING. Used by caml_mmtk_quiesce_running_domains to wait for in-flight
    lock-free readers to drain. DORMANT: no callers yet. */
size_t mmtk_ocaml_snapshot_running(uintptr_t* buf, size_t len);
int mmtk_ocaml_is_running(uintptr_t addr);

/* ── Write barrier (generational plans) ─────────────────────────────── */

/**
 * Generational region write barrier: record that `count` value-sized slots at
 * `start` may now point into the nursery. Used for scalar field writes
 * (count == 1) and array blits. No-op for non-generational plans.
 */
void mmtk_ocaml_region_barrier(MMTk_Mutator mutator, uintptr_t start, size_t count);

/**
 * SATB (snapshot-at-the-beginning) deletion write barrier for the concurrent
 * plan (ConcurrentImmix). Greys the OLD referents currently held in `count`
 * value-sized slots at `start`. MUST be called BEFORE the store, while the slots
 * still hold the old values. No effect outside concurrent marking. The C side
 * gates this on the concurrent plan (caml_mmtk_concurrent).
 */
void mmtk_ocaml_satb_barrier(MMTk_Mutator mutator, uintptr_t start, size_t count);

/**
 * Per-continuation scan lock for the concurrent plan (ConcurrentImmix). The GC
 * worker holds this while scanning a continuation's suspended fiber stack; a
 * resuming domain must acquire it (blocking) before switching onto that stack, so
 * a resume never races an in-progress concurrent stack scan. lock() blocks until
 * free; unlock() releases. `cont_addr` is the continuation block's address.
 */
void mmtk_ocaml_cont_lock(uintptr_t cont_addr);
void mmtk_ocaml_cont_unlock(uintptr_t cont_addr);

/** True iff the concurrent plan is currently in its concurrent marking phase. */
bool mmtk_ocaml_concurrent_marking_active(void);

/**
 * RQ8: set whether MMTk zeros allocation memory before handing it to the mutator.
 * Process global; default true (zero). Call once at init, BEFORE any allocation,
 * with false for stop-the-world Immix-family plans (OCaml initializes every block
 * before the next GC-observable safepoint) and true for ConcurrentImmix (a
 * concurrent marker may observe the header-written/fields-unwritten window).
 */
void mmtk_ocaml_set_alloc_zeroed(bool zeroed);

/* ── Object queries ─────────────────────────────────────────────────── */

bool mmtk_ocaml_is_in_mmtk_spaces(const void* addr);

/** Pin a block so a moving collection won't relocate it (interim weak/ephemeron
 * support). Returns false for non-MMTk addresses / inert under non-moving plans. */
bool mmtk_ocaml_pin_object(const void* addr);

/** Objects relocated by copying collection so far (Immix defrag, etc.). */
size_t mmtk_ocaml_objects_copied(void);

/** Number of collections performed so far. */
size_t mmtk_ocaml_gc_count(void);

/** Total stop-the-world GC time so far, in milliseconds. */
uint64_t mmtk_ocaml_gc_time_ms(void);

/** Register a custom block (with a finalize op) on MMTk's finalizer queue. Kept
 *  alive + forwarded until unreachable, then returned by mmtk_ocaml_poll_finalizable. */
void mmtk_ocaml_add_finalizer(const void* addr);

/** Pop one ready-to-finalize object (unreachable since the last GC), or 0 if none.
 *  The returned block is resurrected/valid for its finalize call. */
uintptr_t mmtk_ocaml_poll_finalizable(void);

/** Heap stats for Gc.stat (page-granular bytes). total = heap size;
 *  used = live+retained (proxy for live); free = free bytes. */
size_t mmtk_ocaml_total_bytes(void);
size_t mmtk_ocaml_used_bytes(void);
size_t mmtk_ocaml_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* MMTK_OCAML_H */
