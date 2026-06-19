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

/* ── GC control ─────────────────────────────────────────────────────── */

void mmtk_ocaml_handle_user_collection_request(uintptr_t domain_state_addr);

/* ── Stop-the-world (multi-domain) ──────────────────────────────────── */

/** True while a collection is in progress (queried at domain safepoints). */
bool mmtk_ocaml_stw_active(void);

/** Park the calling domain at a safepoint until the collection finishes. */
void mmtk_ocaml_stw_park(void);

/** A domain is entering a C blocking section (counts as safe-stopped). */
void mmtk_ocaml_enter_blocking(void);

/** A domain is leaving a blocking section; waits if a collection is active. */
void mmtk_ocaml_leave_blocking(void);

/** Deregister a terminating domain (by caml_domain_state address). */
void mmtk_ocaml_deregister_domain(uintptr_t domain_state_addr);

/* ── Object queries ─────────────────────────────────────────────────── */

bool mmtk_ocaml_is_in_mmtk_spaces(const void* addr);

#ifdef __cplusplus
}
#endif

#endif /* MMTK_OCAML_H */
