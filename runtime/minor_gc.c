/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*              Damien Doligez, projet Para, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 1996 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "caml/config.h"
#include "caml/custom.h"
#include "caml/domain.h"
#include "caml/runtime_events.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/finalise.h"
#include "caml/gc.h"
#include "caml/gc_ctrl.h"
#include "caml/gc_stats.h"
#include "caml/globroots.h"
#include "caml/major_gc.h"
#include "caml/memory.h"
#include "caml/memprof.h"
#include "caml/minor_gc.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/platform.h"
#include "caml/roots.h"
#include "caml/mmtk.h"
#include "caml/signals.h"
#include "caml/startup_aux.h"
#include "caml/weak.h"

struct generic_table CAML_TABLE_STRUCT(char);

CAMLexport atomic_uintnat caml_minor_collections_count;
CAMLexport atomic_uintnat caml_major_slice_epoch;

/* [sz] and [rsv] are numbers of entries */
static void alloc_generic_table (struct generic_table *tbl, asize_t sz,
                                 asize_t rsv, asize_t element_size)
{
  void *new_table;

  tbl->size = sz;
  tbl->reserve = rsv;
  new_table = (void *) caml_stat_alloc_noexc((tbl->size + tbl->reserve) *
                                             element_size);
  if (new_table == NULL) caml_fatal_error ("not enough memory");
  if (tbl->base != NULL) caml_stat_free (tbl->base);
  tbl->base = new_table;
  tbl->ptr = tbl->base;
  tbl->threshold = tbl->base + tbl->size * element_size;
  tbl->limit = tbl->threshold;
  tbl->end = tbl->base + (tbl->size + tbl->reserve) * element_size;
}

static void reset_table (struct generic_table *tbl)
{
  tbl->size = 0;
  tbl->reserve = 0;
  if (tbl->base != NULL) caml_stat_free (tbl->base);
  tbl->base = tbl->ptr = tbl->threshold = tbl->limit = tbl->end = NULL;
}

static void clear_table (struct generic_table *tbl)
{
    tbl->ptr = tbl->base;
    tbl->limit = tbl->threshold;
}

struct caml_minor_tables* caml_alloc_minor_tables(void)
{
  struct caml_minor_tables *r =
      caml_stat_alloc_noexc(sizeof(struct caml_minor_tables));
  if(r != NULL)
    memset(r, 0, sizeof(*r));
  return r;
}

static void reset_minor_tables(struct caml_minor_tables* r)
{
  reset_table((struct generic_table *)&r->ephe_ref);
  reset_table((struct generic_table *)&r->custom);
}

void caml_free_minor_tables(struct caml_minor_tables* r)
{
  reset_minor_tables(r);
  caml_stat_free(r);
}

#ifdef DEBUG
extern int caml_debug_is_minor(value val) {
  return Is_young(val);
}

extern int caml_debug_is_major(value val) {
  return Is_block(val) && !Is_young(val);
}
#endif

void caml_set_minor_heap_size (asize_t wsize)
{
  caml_domain_state* domain_state = Caml_state;
  struct caml_minor_tables *r = domain_state->minor_tables;

  /* Under always-on MMTk the minor heap is an MMTk TLAB block (Immix nursery), not
     a resizable stock arena: Gc.set minor_heap_size cannot actually resize it. We
     just record the nominal size (reported by Gc.stat/Gc.get and used to size the
     minor tables) and re-size the tables. No stock-arena reallocation, and no
     minor collection / young_ptr==young_end assert (which never holds under TLAB —
     young_ptr sits mid-block). */
  domain_state->minor_heap_wsz = caml_norm_minor_heap_size(wsize);
  reset_minor_tables(r);
}

/*****************************************************************************/

/* in progress updates are zeros except for the lowest color bit set to 1
   that is, reserved == wosize == tag == 0, color == 1 */
#define In_progress_update_val Make_header(0, 0, 1 << HEADER_COLOR_SHIFT)
#define Is_update_in_progress(hd) ((hd) == In_progress_update_val)

static void spin_on_header(value v) {
  SPIN_WAIT {
    if (atomic_load(Hp_atomic_val(v)) == 0)
      return;
  }
}

CAMLno_tsan_for_perf
Caml_inline header_t get_header_val(value v) {
  header_t hd = atomic_load_acquire(Hp_atomic_val(v));
  if (!Is_update_in_progress(hd))
    return hd;

  spin_on_header(v);
  return 0;
}

header_t caml_get_header_val(value v) {
  return get_header_val(v);
}




void caml_empty_minor_heap_domain_clear(caml_domain_state* domain)
{
  struct caml_minor_tables *minor_tables = domain->minor_tables;

  caml_final_empty_young(domain);

  clear_table ((struct generic_table *)&minor_tables->ephe_ref);
  clear_table ((struct generic_table *)&minor_tables->custom);

  domain->extra_heap_resources_minor = 0.0;
}

/* Increment the counter non-atomically, when it is already known that this
   thread is alone in trying to increment it. */
static void nonatomic_increment_counter(atomic_uintnat* counter) {
  atomic_store_relaxed(counter, 1 + atomic_load_relaxed(counter));
}

/* Per-domain young-region reset — the sole load-bearing residue of the old
   all-domains minor-empty STW (caml_empty_minor_heap_promote, excise Phase 3a).
   Runs on the TRIGGERING domain at its own safepoint (caml_poll_gc_work,
   bytecode) and on the terminate flush — never on native (the caml_mmtk_tlab
   early-return in caml_poll_gc_work fires first, and the TLAB refill /
   caml_mmtk_uninterrupt already reset the young region there) and never on a GC
   worker. It only touches `domain`'s own state, so it needs no cross-domain
   rendezvous. This is the bytecode (`else`) branch of the old promote verbatim:
   discard the stock minor arena (young_ptr = young_end), re-arm the half-heap
   major-slice trigger, re-set the memprof trigger and the safepoint poison. The
   per-domain stat_minor_words update is preserved; stat_promoted_words is dropped
   (the old promote did `+= allocated_words - prev_alloc_words`, already 0 once
   oldification was removed — nothing here mutates allocated_words). */
void caml_minor_gc_reset_young_region(caml_domain_state* domain)
{
  uintnat minor_allocated_bytes =
    (uintnat)domain->young_end - (uintnat)domain->young_ptr;

  domain->young_ptr = domain->young_end;
  /* Trigger a GC poll when half of the minor heap is filled. At that point, a
   * major slice is scheduled. */
  domain->young_trigger = domain->young_start
    + (domain->young_end - domain->young_start) / 2;
  caml_memprof_set_trigger(domain);
  caml_reset_young_limit(domain);

  domain->stat_minor_words += Wsize_bsize (minor_allocated_bytes);
}

/* Domain-LOCAL minor-cycle bookkeeping, re-homed off the all-domains minor STW
   (excise Phase 1). Runs on the TRIGGERING domain at its own safepoint
   (caml_poll_gc_work, bytecode) and on the terminate flush path — NOT on a GC
   worker (that would be caml_mmtk_uninterrupt, which holds the worker-monitor
   lock; see the resume_mutators self-deadlock fix cd62bd47f9). Each piece touches
   only `domain`'s own state, so it needs no cross-domain rendezvous; this is what
   lets Phase 3 delete the minor STW. Order matches the old STW body
   (stats -> memprof -> finalisers -> table-clear). `bump_count` is 1 only on the
   bytecode minor path (native keeps caml_minor_collections_count at 0; terminate
   passes 0). */
void caml_minor_gc_domain_bookkeeping(caml_domain_state* domain, int bump_count)
{
  caml_collect_gc_stats_sample_stw(domain);   /* writes this domain's own sample slot */
  caml_memprof_after_minor_gc(domain);
  caml_final_update_last_minor(domain);
  caml_empty_minor_heap_domain_clear(domain); /* incl. caml_final_empty_young */
  if (bump_count)
    nonatomic_increment_counter(&caml_minor_collections_count);
}

/* Called by minor allocations when [Caml_state->young_ptr] reaches
   [Caml_state->young_limit]. We may have to either call memprof or
   the gc. */
void caml_alloc_small_dispatch (caml_domain_state * dom_st,
                                intnat wosize, int flags,
                                int nallocs, unsigned char* encoded_alloc_lens)
{
  intnat whsize = Whsize_wosize(wosize);

  /* First, we un-do the allocation performed in [Alloc_small] */
  dom_st->young_ptr += whsize;

  while(1) {
    /* We might be here because of an async callback / urgent GC
       request. Take the opportunity to do what has been requested. */
    if (flags & CAML_FROM_CAML)
      /* In the case of allocations performed from OCaml, execute
         asynchronous callbacks. */
      caml_get_value_or_raise(caml_do_pending_actions_res());
    else {
      /* In the case of allocations performed from C, only perform
         non-delayable actions. */
      caml_handle_gc_interrupt();
    }

    /* Now, there might be enough room in the minor heap to do our
       allocation. */
    if (dom_st->young_ptr - whsize >= dom_st->young_start)
      break;

    /* If not, then make room and check again for async callbacks. In TLAB mode
       (MMTk owns the nursery) we refill a fresh MMTk block instead of running a
       minor GC; otherwise we empty the minor heap. */
    CAML_EV_COUNTER(EV_C_FORCE_MINOR_ALLOC_SMALL, 1);
    if (caml_mmtk_tlab) {
      if (!caml_mmtk_refill_tlab(dom_st, whsize)) {
        /* MMTk bug #4: this raise happens from inside caml_call_gc's saved-regs
           window (caml_garbage_collection -> here). Recycle the popped gc_regs
           bucket back to the free-list first, so the raise (which bypasses
           caml_call_gc's RESTORE_ALL_REGS) does not leave gc_regs_buckets NULL and
           crash the next caml_call_gc. See caml_mmtk_recycle_gc_regs_bucket. */
        caml_mmtk_recycle_gc_regs_bucket();
        caml_raise_out_of_memory();
      }
    } else {
      caml_poll_gc_work();
    }
  }

  /* Re-do the allocation: we now have enough space in the minor heap. */
  dom_st->young_ptr -= whsize;

  /* Check if the allocated block has been sampled by memprof. */
  if (dom_st->young_ptr < dom_st->memprof_young_trigger) {
    if(flags & CAML_DO_TRACK) {
      caml_memprof_sample_young(wosize, flags & CAML_FROM_CAML,
                                nallocs, encoded_alloc_lens);
      /* Until the allocation actually takes place, the heap is in an
         invalid state (see comments in [caml_memprof_sample_young]).
         Hence, very few heap operations are allowed between this point
         and the actual allocation.

         Specifically, [dom_st->young_ptr] must not now be modified
         before the allocation, because it has been used to predict
         addresses of sampled block(s).
      */
    } else { /* CAML DONT TRACK */
      caml_memprof_set_trigger(dom_st);
      caml_reset_young_limit(dom_st);
    }
  }
}

/* Request a minor collection and enter as if it were an interrupt.
*/
CAMLexport void caml_minor_collection (void)
{
  caml_request_minor_gc();
  caml_handle_gc_interrupt();
}

CAMLexport value caml_check_urgent_gc (value extra_root)
{
  if (Caml_check_gc_interrupt(Caml_state)) {
    CAMLparam1(extra_root);
    caml_handle_gc_interrupt();
    CAMLdrop;
  }
  return extra_root;
}

static void realloc_generic_table
(struct generic_table *tbl, asize_t element_size,
 ev_runtime_counter ev_counter_name,
 const char *msg_threshold, const char *msg_growing, const char *msg_error)
{
  CAMLassert (tbl->ptr == tbl->limit);
  CAMLassert (tbl->limit <= tbl->end);
  CAMLassert (tbl->limit >= tbl->threshold);

  if (tbl->base == NULL){
    alloc_generic_table (tbl, Caml_state->minor_heap_wsz / 8, 256,
                         element_size);
  }else if (tbl->limit == tbl->threshold){
    CAML_EV_COUNTER (ev_counter_name, 1);
    CAML_GC_MESSAGE(STACKSIZE, msg_threshold, 0);
    tbl->limit = tbl->end;
    caml_request_minor_gc ();
  }else{
    asize_t sz;
    asize_t cur_ptr = tbl->ptr - tbl->base;

    tbl->size *= 2;
    sz = (tbl->size + tbl->reserve) * element_size;
    CAML_GC_MESSAGE(STACKSIZE, msg_growing, (intnat) sz/1024);
    tbl->base = caml_stat_resize_noexc (tbl->base, sz);
    if (tbl->base == NULL){
      caml_fatal_error ("%s", msg_error);
    }
    tbl->end = tbl->base + (tbl->size + tbl->reserve) * element_size;
    tbl->threshold = tbl->base + tbl->size * element_size;
    tbl->ptr = tbl->base + cur_ptr;
    tbl->limit = tbl->end;
  }
}

void caml_realloc_ephe_ref_table (struct caml_ephe_ref_table *tbl)
{
  realloc_generic_table
    ((struct generic_table *) tbl, sizeof (struct caml_ephe_ref_elt),
     EV_C_REQUEST_MINOR_REALLOC_EPHE_REF_TABLE,
     "ephe_ref_table threshold crossed\n",
     "Growing ephe_ref_table to %" CAML_PRIdNAT "k bytes\n",
     "ephe_ref_table overflow");
}

void caml_realloc_custom_table (struct caml_custom_table *tbl)
{
  realloc_generic_table
    ((struct generic_table *) tbl, sizeof (struct caml_custom_elt),
     EV_C_REQUEST_MINOR_REALLOC_CUSTOM_TABLE,
     "custom_table threshold crossed\n",
     "Growing custom_table to %" CAML_PRIdNAT "k bytes\n",
     "custom_table overflow");
}
