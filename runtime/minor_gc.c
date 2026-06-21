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
#include "caml/globroots.h"
#include "caml/major_gc.h"
#include "caml/memory.h"
#include "caml/memprof.h"
#include "caml/minor_gc.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/platform.h"
#include "caml/roots.h"
#include "caml/shared_heap.h"
#include "caml/mmtk.h"
#include "caml/signals.h"
#include "caml/startup_aux.h"
#include "caml/weak.h"

struct generic_table CAML_TABLE_STRUCT(char);

CAMLexport atomic_uintnat caml_minor_collections_count;
CAMLexport atomic_uintnat caml_major_slice_epoch;

static caml_plat_barrier minor_gc_end_barrier = CAML_PLAT_BARRIER_INITIALIZER;

static atomic_uintnat caml_minor_cycles_started = 0;

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

void caml_alloc_table (struct caml_ref_table *tbl, asize_t sz, asize_t rsv)
{
  alloc_generic_table ((struct generic_table *) tbl, sz, rsv, sizeof (value *));
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
  reset_table((struct generic_table *)&r->major_ref);
  reset_table((struct generic_table *)&r->ephe_ref);
  reset_table((struct generic_table *)&r->custom);
}

void caml_free_minor_tables(struct caml_minor_tables* r)
{
  CAMLassert(r->major_ref.ptr == r->major_ref.base);

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

  if (domain_state->young_ptr != domain_state->young_end) {
    CAML_EV_COUNTER (EV_C_FORCE_MINOR_SET_MINOR_HEAP_SIZE, 1);
    caml_minor_collection();
  }
  CAMLassert (domain_state->young_ptr == domain_state->young_end);

  if(caml_reallocate_minor_heap_arena(wsize) < 0) {
    caml_fatal_error("Fatal error: No memory for minor heap");
  }

  reset_minor_tables(r);
}

/*****************************************************************************/

struct oldify_state {
  value todo_list;
  uintnat live_bytes;
  caml_domain_state* domain;
};


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




typedef struct {
  bool locked_ephemerons;
} promote_result;


void caml_empty_minor_heap_domain_clear(caml_domain_state* domain)
{
  struct caml_minor_tables *minor_tables = domain->minor_tables;

  caml_final_empty_young(domain);

  clear_table ((struct generic_table *)&minor_tables->major_ref);
  clear_table ((struct generic_table *)&minor_tables->ephe_ref);
  clear_table ((struct generic_table *)&minor_tables->custom);

  domain->extra_heap_resources_minor = 0.0;
}

/* Try to do a major slice, returns nonzero if there was any work available,
   used as useful spin work while waiting for synchronisation. The return type
   is [int] and not [bool] since it is passed as a parameter to
   [caml_try_run_on_all_domains_with_spin_work]. */
int caml_do_opportunistic_major_slice
  (caml_domain_state* domain_unused, void* unused);
static void minor_gc_leave_barrier
  (caml_domain_state* domain, int participating_count);

static promote_result
caml_empty_minor_heap_promote(caml_domain_state* domain,
                              int participating_count,
                              caml_domain_state** participating)
{
  value* young_ptr = domain->young_ptr;
  value* young_end = domain->young_end;
  uintnat minor_allocated_bytes = (uintnat)young_end - (uintnat)young_ptr;
  uintnat prev_alloc_words;
  struct oldify_state st = {0};


  prev_alloc_words = domain->allocated_words;

  caml_gc_log ("Minor collection of domain %d starting", domain->id);
  CAML_EV_BEGIN(EV_MINOR);
  call_timing_hook(&caml_minor_gc_begin_hook);

  CAMLassert(domain == Caml_state);

  /* MMTk (always-on) owns the heap, so there is nothing to promote — every live
     object is already an MMTk object reachable from roots (native: the TLAB
     nursery; bytecode: allocated straight into MMTk, leaving the stock minor heap
     permanently empty). The oldify/promotion that stock OCaml ran here has been
     removed. This routine survives only as the all-domains minor-empty STW
     rendezvous (it synchronizes domain spawn/terminate — skipping it livelocks
     multi-domain termination) and to reset the young region below. */
  promote_result result = { .locked_ephemerons = false };

  if (caml_mmtk_tlab) {
    /* Discard the current MMTk block: setting young_ptr = young_start forces the
       next allocation to trap and refill a fresh block (deferred outside this STW,
       so no nested MMTk GC). The old block's live objects stay reachable via roots.
       No half-heap major-slice trigger and no memprof sampling in this mode. */
    domain->young_ptr = domain->young_start;
    domain->young_trigger = domain->young_start;
    domain->memprof_young_trigger = domain->young_start;
    caml_reset_young_limit(domain);
  } else {
    domain->young_ptr = domain->young_end;
    /* Trigger a GC poll when half of the minor heap is filled. At that point, a
     * major slice is scheduled. */
    domain->young_trigger = domain->young_start
      + (domain->young_end - domain->young_start) / 2;
    caml_memprof_set_trigger(domain);
    caml_reset_young_limit(domain);
  }

  domain->stat_minor_words += Wsize_bsize (minor_allocated_bytes);
  domain->stat_promoted_words += domain->allocated_words - prev_alloc_words;

  /* Must be called during the STW section -- before any mutators
     start running, so before arriving at the barrier. */
  caml_collect_gc_stats_sample_stw(domain);

  /* The code above is synchronised with other domains by the barrier below,
     which is split into two steps, "arriving" and "leaving". When the final
     domain arrives at the barrier, all other domains are free to leave, after
     which they finish running the STW callback and may, depending on the
     specific STW section, begin executing mutator code.

     Leaving the barrier synchronises (only) with the arrivals of other domains,
     so that all writes performed by a domain before arrival "happen-before" any
     domain leaves the barrier. However, any code after arrival, including the
     code between the two steps, can potentially race with mutator code.
  */

  /* arrive at the barrier */
  if( participating_count > 1 ) {
    if (caml_plat_barrier_arrive(&minor_gc_end_barrier)
        == participating_count) {
      caml_plat_barrier_release(&minor_gc_end_barrier);
    }
  }
  /* other domains may be executing mutator code from this point, but
     not before */

  call_timing_hook(&caml_minor_gc_end_hook);
  CAML_EV_COUNTER(EV_C_MINOR_PROMOTED,
                  Bsize_wsize(domain->allocated_words - prev_alloc_words));
  CAML_EV_COUNTER(EV_C_MINOR_PROMOTED_WORDS,
                  domain->allocated_words - prev_alloc_words);

  CAML_EV_COUNTER(EV_C_MINOR_ALLOCATED, minor_allocated_bytes);
  CAML_EV_COUNTER(EV_C_MINOR_ALLOCATED_WORDS,
                  Whsize_wosize(minor_allocated_bytes));

  CAML_EV_END(EV_MINOR);
  if (minor_allocated_bytes == 0)
    caml_gc_log ("Minor collection of domain %d completed:"
                 " no minor bytes allocated",
                 domain->id);
  else
    caml_gc_log ("Minor collection of domain %d completed:"
                 " %2.0f%% of %u KB live",
                 domain->id,
                 100.0 * (double)st.live_bytes / (double)minor_allocated_bytes,
                 (unsigned)(minor_allocated_bytes + 512)/1024);

  /* leave the barrier */
  if( participating_count > 1 ) {
    CAML_EV_BEGIN(EV_MINOR_LEAVE_BARRIER);
    minor_gc_leave_barrier(domain, participating_count);
    CAML_EV_END(EV_MINOR_LEAVE_BARRIER);
  }
  return result;
}

/* Increment the counter non-atomically, when it is already known that this
   thread is alone in trying to increment it. */
static void nonatomic_increment_counter(atomic_uintnat* counter) {
  atomic_store_relaxed(counter, 1 + atomic_load_relaxed(counter));
}

static void minor_gc_leave_barrier
  (caml_domain_state* domain, int participating_count)
{
  /* Spin while we have major work available */
  SPIN_WAIT_BOUNDED {
    if (caml_plat_barrier_is_released(&minor_gc_end_barrier)) {
      return;
    }

    if (!caml_do_opportunistic_major_slice(domain, 0)) {
      break;
    }
  }

  /* Spin a bit longer, which is far less fruitful if we're waiting on
     more than one thread */
  unsigned spins =
    participating_count == 2 ? Max_spins_long : Max_spins_medium;
  SPIN_WAIT_NTIMES(spins) {
    if (caml_plat_barrier_is_released(&minor_gc_end_barrier)) {
      return;
    }
  }

  /* If there's nothing to do, block */
  caml_plat_barrier_wait(&minor_gc_end_barrier);
}

int caml_do_opportunistic_major_slice
  (caml_domain_state* domain_state, void* unused)
{
  int work_available = caml_opportunistic_major_work_available(domain_state);
  if (work_available) {
    /* NB: need to put guard around the ev logs to prevent spam when we poll */
    uintnat log_events =
        atomic_load_relaxed(&caml_verb_gc) & CAML_GC_MSG_SLICESIZE;
    if (log_events) CAML_EV_BEGIN(EV_MAJOR_MARK_OPPORTUNISTIC);
    caml_opportunistic_major_collection_slice(Major_slice_work_min);
    if (log_events) CAML_EV_END(EV_MAJOR_MARK_OPPORTUNISTIC);
  }
  return work_available;
}

/* Make sure the minor heap is empty by performing a minor collection
   if needed.

   This function also samples [caml_gc_mark_phase_requested] to see whether
   [caml_mark_roots_stw] should be called. To guarantee that all domains
   agree on whether the roots should be marked, this variable is sampled
   only once, instead of having domains check it individually.
*/
void caml_empty_minor_heap_setup(caml_domain_state* domain_unused,
                                 void *mark_requested_p) {
  /* Check whether the mark phase has been requested */
  *(uintnat*)mark_requested_p =
    atomic_load_relaxed(&caml_gc_mark_phase_requested)
    ? atomic_exchange(&caml_gc_mark_phase_requested, 0)
    : 0;
  /* Increment the total number of minor collections done in the program */
  nonatomic_increment_counter (&caml_minor_collections_count);
  caml_plat_barrier_reset(&minor_gc_end_barrier);
}

/* must be called within a STW section */
static void
caml_stw_empty_minor_heap_no_major_slice(caml_domain_state* domain,
                                         void* mark_requested_p,
                                         int participating_count,
                                         caml_domain_state** participating)
{
#ifdef DEBUG
  uintnat* initial_young_ptr = (uintnat*)domain->young_ptr;
  CAMLassert(caml_domain_is_in_stw());
#endif

  /* mark_requested_p must be read before minor GC barrier */
  uintnat mark_requested = *(uintnat*)mark_requested_p;

  if( participating[0] == domain ) {
    nonatomic_increment_counter(&caml_minor_cycles_started);
  }

  caml_gc_log("running stw empty_minor_heap_promote");
  /* Under always-on MMTk, promote no longer oldifies, so it never locks
     ephemerons (locked_ephemerons is always false); the stock minor ephemeron
     clean is dead. */
  caml_empty_minor_heap_promote(domain, participating_count, participating);

  CAML_EV_BEGIN(EV_MINOR_MEMPROF_CLEAN);
  caml_gc_log("updating memprof");
  caml_memprof_after_minor_gc(domain);
  CAML_EV_END(EV_MINOR_MEMPROF_CLEAN);

  /* while the minor heap is empty, allow the major GC to mark roots */
  if (mark_requested)
    caml_mark_roots_stw(participating_count, participating);

  /* Stock minor custom-block finalization is gone: under always-on MMTk the stock
     minor heap is empty, so it only ever skipped MMTk objects (Is_young false).
     Custom finalization is MMTk's responsibility (currently parked). */

  CAML_EV_BEGIN(EV_MINOR_FINALIZERS_ADMIN);
  caml_gc_log("running finalizer data structure book-keeping");
  caml_final_update_last_minor(domain);
  CAML_EV_END(EV_MINOR_FINALIZERS_ADMIN);

  CAML_EV_BEGIN(EV_MINOR_CLEAR);
  caml_gc_log("running stw empty_minor_heap_domain_clear");
  caml_empty_minor_heap_domain_clear(domain);

#ifdef DEBUG
  {
    for (uintnat *p = initial_young_ptr; p < (uintnat*)domain->young_end; ++p)
      *p = Debug_free_minor;
  }
#endif

  CAML_EV_END(EV_MINOR_CLEAR);
  caml_gc_log("finished stw empty_minor_heap");
  CAMLassert(domain->young_ptr == domain->young_end);
}

static void caml_stw_empty_minor_heap (caml_domain_state* domain,
                                       void* mark_requested_p,
                                       int participating_count,
                                       caml_domain_state** participating)
{
  caml_stw_empty_minor_heap_no_major_slice(domain, mark_requested_p,
                                           participating_count, participating);
}

/* must be called within a STW section  */
void caml_empty_minor_heap_no_major_slice_from_stw(
  caml_domain_state* domain,
  void* unused,
  int participating_count,
  caml_domain_state** participating)
{

  static uintnat mark_requested; /* Written by only one domain */
  Caml_global_barrier_if_final(participating_count) {
    caml_empty_minor_heap_setup(domain, &mark_requested);
  }

  /* if we are entering from within a major GC STW section then
     we do not schedule another major collection slice */
  caml_stw_empty_minor_heap_no_major_slice(domain, &mark_requested,
                                           participating_count, participating);
}

/* must be called outside a STW section */
int caml_try_empty_minor_heap_on_all_domains (void)
{
  #ifdef DEBUG
  CAMLassert(!caml_domain_is_in_stw());
  #endif

  caml_gc_log("requesting stw empty_minor_heap");
  uintnat mark_requested = 0;
  return caml_try_run_on_all_domains_with_spin_work(
    1, /* synchronous */
    &caml_stw_empty_minor_heap, /* stw handler */
    &mark_requested,
    &caml_empty_minor_heap_setup, /* leader setup */
    &caml_do_opportunistic_major_slice, 0 /* enter spin work */);
    /* leaves when done by default*/
}

/* must be called outside a STW section, will retry until we have emptied our
   minor heap */
void caml_empty_minor_heaps_once (void)
{
  /* TLAB mode note: MMTk owns the nursery, so there is no promotion to do — but we
     STILL run the all-domains minor-empty STW below. That STW is the rendezvous
     that synchronizes domain spawn/terminate; skipping it livelocks multi-domain
     termination. The per-domain work is neutered in caml_empty_minor_heap_promote
     (it just resets the young region; no promotion, no GC inside the STW). */

  uintnat saved_minor_cycle = atomic_load_relaxed(&caml_minor_cycles_started);

  #ifdef DEBUG
  CAMLassert(!caml_domain_is_in_stw());
  #endif

  CAML_EV_BEGIN(EV_EMPTY_MINOR);

  /* To handle the case where multiple domains try to execute a minor gc
     STW section */
  do {
    caml_try_empty_minor_heap_on_all_domains();
  } while (saved_minor_cycle ==
           atomic_load_relaxed(&caml_minor_cycles_started));

  CAML_EV_END(EV_EMPTY_MINOR);
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
      if (!caml_mmtk_refill_tlab(dom_st, whsize))
        caml_raise_out_of_memory();
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

void caml_realloc_ref_table (struct caml_ref_table *tbl)
{
  realloc_generic_table
    ((struct generic_table *) tbl, sizeof (value *),
     EV_C_REQUEST_MINOR_REALLOC_REF_TABLE,
     "ref_table threshold crossed\n",
     "Growing ref_table to %" CAML_PRIdNAT "k bytes\n",
     "ref_table overflow");
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
