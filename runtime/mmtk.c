/**************************************************************************/
/*                                                                        */
/*                MMTk garbage collector glue (bytecode)                  */
/*                                                                        */
/**************************************************************************/

/* Implementation of the bytecode<->MMTk glue. Compiled only into the
 * bytecode runtime (runtime_BYTECODE_ONLY_C_SOURCES). All of it is behind
 * #ifndef NATIVE_CODE so a stray native build is a no-op. */

#define CAML_INTERNALS

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "caml/config.h"

#ifndef NATIVE_CODE

#include "caml/mlvalues.h"
#include "caml/domain_state.h"
#include "caml/domain.h"
#include "caml/misc.h"
#include "caml/mmtk.h"

/* The in-tree MMTk binding's C ABI (gc/mmtk/include/mmtk_ocaml.h). */
#include "../gc/mmtk/include/mmtk_ocaml.h"

int caml_mmtk_enabled = 0;

static int caml_mmtk_initialised = 0;
/* Whether the active plan collects (anything but NoGC). NoGC must NOT start
   collection: forcing a GC it cannot perform would spin/fail. */
static int caml_mmtk_collects = 0;
static int caml_mmtk_collection_started = 0;

/* Objects this size (bytes) or larger are routed to MMTk's large object
 * space. Conservative: smaller than the smallest line/block in collecting
 * plans, so it is also correct for Immix later. */
#define CAML_MMTK_LOS_THRESHOLD (16 * 1024)

/* AllocationSemantics codes shared with the Rust ABI (see api.rs). */
#define CAML_MMTK_SEM_DEFAULT 0
#define CAML_MMTK_SEM_LOS     2

void caml_mmtk_init(void)
{
  if (caml_mmtk_initialised) return;

  const char *plan = getenv("MMTK_PLAN");
  if (plan == NULL || plan[0] == '\0') plan = "NoGC";

  size_t heap_mb = 1024;
  const char *heap_env = getenv("MMTK_HEAP_SIZE_MB");
  if (heap_env != NULL && heap_env[0] != '\0') {
    long v = strtol(heap_env, NULL, 10);
    if (v > 0) heap_mb = (size_t)v;
  }

  mmtk_ocaml_init(heap_mb * 1024 * 1024, plan);
  caml_mmtk_initialised = 1;
  caml_mmtk_collects = (strcmp(plan, "NoGC") != 0);

  if (getenv("MMTK_VERBOSE") != NULL)
    fprintf(stderr, "[mmtk] initialised: plan=%s heap=%zuMiB\n", plan, heap_mb);
}

/* MMTk is opt-in during bring-up: it manages the heap only when MMTK_ENABLED is
   set to something other than "0"/empty. Default off, so a normal build and the
   self-hosting compiler bootstrap run on OCaml's stock GC. NoGC in particular
   cannot sustain the compiler build (it never reclaims), so always-on would
   break `make`. Enable explicitly to exercise MMTk:
     MMTK_ENABLED=1 [MMTK_PLAN=NoGC] [MMTK_HEAP_SIZE_MB=1024] ./runtime/ocamlrun prog.byte */
static int caml_mmtk_wanted(void)
{
  const char *e = getenv("MMTK_ENABLED");
  return e != NULL && e[0] != '\0' && strcmp(e, "0") != 0;
}

void caml_mmtk_domain_init(caml_domain_state *dom)
{
  if (!caml_mmtk_wanted()) return;  /* stock GC unless explicitly enabled */
  caml_mmtk_init();
  dom->mmtk_mutator = mmtk_ocaml_bind_mutator((uintptr_t)dom);
  /* For collecting plans, spawn the GC worker threads once (must happen before
     an allocation can trigger a collection). */
  if (caml_mmtk_collects && !caml_mmtk_collection_started) {
    mmtk_ocaml_initialize_collection((uintptr_t)dom);
    caml_mmtk_collection_started = 1;
  }
  caml_mmtk_enabled = 1;
}

Caml_inline int caml_mmtk_semantics(mlsize_t wosize)
{
  size_t bytes = (size_t)(Whsize_wosize(wosize)) * sizeof(value);
  return bytes >= CAML_MMTK_LOS_THRESHOLD ? CAML_MMTK_SEM_LOS
                                          : CAML_MMTK_SEM_DEFAULT;
}

/* Note on the header: the binding writes (wosize << 10) | tag, which is the
   correct OCaml header for all non-mixed blocks (the wosize shift is fixed at
   HEADER_TAG_BITS + HEADER_COLOR_BITS = 10, independent of any reserved bits,
   which sit above wosize and stay 0). Mixed blocks (which use reserved bits)
   are not handled yet; NoGC never scans, so this is correct for M1.
   TODO(M2/mixed-blocks): thread `reserved` through the binding's alloc ABI. */

value caml_mmtk_alloc_small(mlsize_t wosize, tag_t tag, reserved_t reserved)
{
  (void)reserved;
  return (value)mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag,
                                 CAML_MMTK_SEM_DEFAULT);
}

value caml_mmtk_alloc_shr(mlsize_t wosize, tag_t tag, reserved_t reserved)
{
  (void)reserved;
  return (value)mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag,
                                 caml_mmtk_semantics(wosize));
}

/* ── Stop-the-world ──────────────────────────────────────────────────── */

/* Park this domain for an MMTk collection, cooperating with OCaml's own
   stop-the-world.

   OCaml has its OWN multi-domain STW (caml_try_run_on_all_domains), used at
   domain spawn/terminate. If a domain froze in MMTk's park while OCaml tried to
   run a STW, the two barriers would deadlock: OCaml waits for this domain to
   join its barrier while MMTk waits for every domain to park. We avoid that by
   handing this domain's OCaml-STW participation to its backup thread for the
   duration of the park — exactly what a C blocking section does
   (caml_enter/leave_blocking_section_default). The backup thread answers
   caml_try_run_on_all_domains on our behalf while we wait. */
void caml_mmtk_park(void)
{
  caml_bt_exit_ocaml();
  caml_release_domain_lock();
  mmtk_ocaml_stw_park();        /* stopped++, wait for the resume epoch, stopped-- */
  caml_bt_enter_ocaml();
  caml_acquire_domain_lock();
}

/* Called from caml_handle_gc_interrupt at every safepoint. If MMTk has a
   collection in progress, park this domain (roots are already published by the
   safepoint, e.g. Setup_for_event) until the collection finishes. */
void caml_mmtk_stw_poll(void)
{
  if (caml_mmtk_enabled && mmtk_ocaml_stw_active()) {
    caml_mmtk_park();
  }
}

/* Poison a domain's young_limit so its next safepoint check
   (Caml_check_gc_interrupt) traps into caml_handle_gc_interrupt. */
void caml_mmtk_interrupt(uintnat domain_state_addr)
{
  caml_domain_state *d = (caml_domain_state *) domain_state_addr;
  atomic_store_release(&d->young_limit, (uintnat) CAML_UINTNAT_MAX);
}

/* Reset a domain's young_limit (un-poison) after the collection. */
void caml_mmtk_uninterrupt(uintnat domain_state_addr)
{
  caml_reset_young_limit((caml_domain_state *) domain_state_addr);
}

/* A domain is entering / leaving a C blocking section. While blocking it is
   safe for GC; on leaving it must wait out any in-progress collection. */
void caml_mmtk_enter_blocking(void)
{
  if (caml_mmtk_enabled) mmtk_ocaml_enter_blocking();
}

void caml_mmtk_leave_blocking(void)
{
  if (caml_mmtk_enabled) mmtk_ocaml_leave_blocking();
}

/* Called when a domain terminates: park if a collection is in progress (so it
   participates), then deregister so future collections don't wait for it. */
void caml_mmtk_domain_terminate(caml_domain_state *dom)
{
  if (!caml_mmtk_enabled || dom->mmtk_mutator == NULL) return;
  caml_mmtk_stw_poll();
  mmtk_ocaml_deregister_domain((uintptr_t) dom);
  dom->mmtk_mutator = NULL;
}

#endif /* NATIVE_CODE */
