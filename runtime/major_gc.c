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

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include "caml/addrmap.h"
#include "caml/camlatomic.h"
#include "caml/config.h"
#include "caml/codefrag.h"
#include "caml/domain.h"
#include "caml/runtime_events.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/finalise.h"
#include "caml/globroots.h"
#include "caml/gc_stats.h"
#include "caml/memory.h"
#include "caml/memprof.h"
#include "caml/mlvalues.h"
#include "caml/mmtk.h"
#include "caml/platform.h"
#include "caml/roots.h"
#include "caml/signals.h"
#include "caml/shared_heap.h"
#include "caml/startup_aux.h"
#include "caml/weak.h"

/* Default speed setting for the major GC. */
_Atomic uintnat caml_percent_free = Percent_free_def;
_Atomic uintnat caml_small_heap_limit = Small_heap_limit_def;

/* The mark stack will be pruned if it grows bigger than
   1/caml_mark_stack_prune_factor of the domain's major heap size */
atomic_uintnat caml_mark_stack_prune_factor = 32;

/* This variable is only written with the world stopped, so it need not be
   atomic */
uintnat caml_major_cycles_completed = 0;

/* [num_domains_to_mark] records the number of domains to mark in the current
   major cycle. The number is set to the [num_domains_in_stw] at the start of
   the cycle. The value of [num_domains_to_mark] may decrease or increase.

   [num_domains_to_mark] may grow larger than the value of [num_domains_in_stw]
   at the start of the cycle. This is because [caml_modify] may push a block
   into a potentially empty mark stack of the newly spawned domain.

   Terminating domains empty their mark stack before terminating. */
static atomic_uintnat num_domains_to_mark;

/* [num_domains_to_ephe_sweep] is set to the [participating_count] at the start
   of the [Phase_sweep_ephe] and strictly decreases. */
static atomic_uintnat num_domains_to_ephe_sweep;

/* [num_domains_to_final_update_first] and [num_domains_to_final_update_last]
   are initialised to [num_domains_in_stw] at the start of the cycle. Whenever
   a domain finishes processing its first or last finalisers, it decrements the
   appropriate counter.

   Newly created domains increment both counters. A terminating domain
   orphans its finalisers and then decrements the counters. The counters
   also increase when some orphaned finalisers are adopted by a terminating
   domain that had orphaned its finalisers. See [caml_final_domain_terminate]
   and [adopt_orphaned_work]. */
static atomic_uintnat num_domains_to_final_update_first;
static atomic_uintnat num_domains_to_final_update_last;

/* When domains terminate, they will orphan their finalisers. As mentioned in
   the comment attached to [num_domains_to_final_update_*] counters, a domain
   will decrement the counters when the corresponding finalisers are processed
   for that domain. We would like to preserve this invariant when adopting
   orphaned finalisers. To this end, we orphan and adopt finalisers only in
   [Phase_sweep_main] or [Phase_sweep_and_mark_main] when
   [num_domains_to_final_update_*] counters have not been decremented
   for the domain yet.

   [num_domains_orphaning_finalisers] keeps a count of the number of domains
   currently orphaning finalisers. This counter is only used in the
   [Phase_sweep_and_mark_main] to determine whether to proceed to
   [Phase_mark_final]. If domains are currently orphaning finalisers, we remain
   in [Phase_sweep_and_mark_main] so that the orphaned finalisers can be
   adopted before moving onto [Phase_mark_final] where the [GC.finalise]
   (finalise first) finalisers are processed. */
static atomic_uintnat num_domains_orphaning_finalisers = 0;

gc_phase_t caml_gc_phase;

/* The caml_gc_phase global is only ever updated at the end of the STW
   section, by the last domain leaving a barrier. This means that no
   synchronization is required on most accesses.

   We know of two situations in the runtime that could run in parallel
   with a phase update, and cannot safely access the gc phase:

   - The caml_domain_terminate logic runs after the thread has un-registered
     itself as a STW participant, so it may race with a STW section.

   - Opportunistic collections may happen while a domain is waiting on
     a STW barrier, so it might race with the code running inside
     another in-STW barrier. (It is possible that a deeper analysis of
     the current runtime code would in fact rule out such a race, but
     it is simpler to avoid phase accesses during opportunistic
     collections.)
 */

Caml_inline char caml_gc_phase_char(int may_access_gc_phase) {
  if (!may_access_gc_phase)
    return 'U';
  switch (caml_gc_phase) {
    case Phase_sweep_main:
      return 'S';
    case Phase_sweep_and_mark_main:
      return 'M';
    case Phase_mark_final:
      return 'F';
    case Phase_sweep_ephe:
      return 'E';
    default:
      return 'U';
  }
}

/*******************************************************************************
 * Handling work counters
 ******************************************************************************/

static uintnat mark_work_done_between_slices(void)
{
  uintnat work = Caml_state->mark_work_done_between_slices;
  Caml_state->mark_work_done_between_slices = 0;
  return work;
}

static uintnat sweep_work_done_between_slices(void)
{
  uintnat work = Caml_state->sweep_work_done_between_slices;
  Caml_state->sweep_work_done_between_slices = 0;
  return work;
}

/*******************************************************************************
 * Ephemerons
 ******************************************************************************/

extern value caml_ephe_none; /* See weak.c */

static struct ephe_cycle_info_t {
  atomic_uintnat num_domains_todo;
  /* Number of domains that need to scan their ephemerons in the current major
   * GC cycle. This field is decremented when ephe_info->todo list at a domain
   * becomes empty.  */
  atomic_uintnat ephe_cycle;
  /* Ephemeron cycle count */
  atomic_uintnat num_domains_done;
  /* Number of domains that have marked their ephemerons in the current
   * ephemeron cycle. */
} ephe_cycle_info;
  /* In the first major cycle, there is no ephemeron marking to be done. */

/* ephe_cycle_info is always updated with the critical section protected by
 * ephe_lock or in the global barrier. However, the fields may be read without
 * the lock. */
static caml_plat_mutex ephe_lock = CAML_PLAT_MUTEX_INITIALIZER;

static void ephe_todo_list_emptied (void)
{
  /* If we haven't started marking, the todo list can grow (during ephemeron
     allocation), so we should not yet announce that it has emptied */
  CAMLassert (caml_marking_started());
  caml_plat_lock_blocking(&ephe_lock);

  /* Force next ephemeron marking cycle in order to avoid reasoning about
   * whether the domain has already incremented
   * [ephe_cycle_info.num_domains_done] counter. */
  caml_atomic_counter_init(&ephe_cycle_info.num_domains_done, 0);
  (void)caml_atomic_counter_incr(&ephe_cycle_info.ephe_cycle);

  /* Since the todo list is empty, this domain does not need to participate in
   * further ephemeron cycles. */
  (void)caml_atomic_counter_decr(&ephe_cycle_info.num_domains_todo);
  CAMLassert(caml_atomic_counter_value(&ephe_cycle_info.num_domains_done) <=
             caml_atomic_counter_value(&ephe_cycle_info.num_domains_todo));

  caml_plat_unlock(&ephe_lock);
}

/* Prepare to mark ephemerons by making all 'live' ephes become 'todo' */
/* Record that ephemeron marking was done for the given ephemeron cycle. */
Caml_inline value ephe_list_tail(value e)
{
  value last = 0;
  while (e != 0) {
    CAMLassert (Tag_val(e) == Abstract_tag);
    last = e;
    e = Ephe_link(e);
  }
  return last;
}

/* Prepare to mark ephemerons by moving the ephemerons on the live
   list to the todo list. This is needed since the live list may
   contain ephemerons with unmarked keys, which need to be
   cleaned. This code is executed exactly once per major cycle per
   domain, using the [ephe_info->must_sweep_ephe] state as
   a reminder. */
static void prepare_for_ephe_sweeping(caml_domain_state *domain_state)
{
  value e = ephe_list_tail (domain_state->ephe_info->todo);
  if (e == (value)NULL) {
    domain_state->ephe_info->todo = domain_state->ephe_info->live;
  } else {
    CAMLassert(Ephe_link(e) == (value)NULL);
    Ephe_link(e) = domain_state->ephe_info->live;
  }
  domain_state->ephe_info->live = (value)NULL;
  /* If the todo list is empty, then the domain has no ephemeron
     sweeping work to do. */
  if (domain_state->ephe_info->todo == (value)NULL) {
      (void)caml_atomic_counter_decr(&num_domains_to_ephe_sweep);
  }
}

#define EPHE_MARK_DEFAULT 0
#define EPHE_MARK_FORCE_ALIVE 1

static intnat ephe_mark (intnat budget, uintnat for_cycle,
                         /* Forces ephemerons and their data to be alive */
                         int force_alive)
{
  value v, data, key, f, todo;
  value* prev_linkp;
  header_t hd;
  mlsize_t size, i;
  caml_domain_state* domain_state = Caml_state;
  int alive_data;
  intnat marked = 0, trivial_data = 0, made_live = 0;

  CAMLassert(caml_marking_started());
  if (domain_state->ephe_info->cursor.cycle == for_cycle &&
      !force_alive) {
    prev_linkp = domain_state->ephe_info->cursor.todop;
    todo = *prev_linkp;
  } else {
    todo = domain_state->ephe_info->todo;
    prev_linkp = &domain_state->ephe_info->todo;
  }
  while (todo != 0 && budget > 0) {
    v = todo;
    todo = Ephe_link(v);
    CAMLassert (Tag_val(v) == Abstract_tag);
    hd = Hd_val(v);
    data = Ephe_data(v);
    alive_data = 1;

    if (force_alive)
      caml_darken (domain_state, v, 0);

    /* If ephemeron is unmarked, data is dead */
    if (is_unmarked(v)) alive_data = 0;

    size = Wosize_hd(hd);
    for (i = CAML_EPHE_FIRST_KEY; alive_data && i < size; i++) {
      key = Ephe_key(v, i);
    ephemeron_again:
      if (key != caml_ephe_none && Is_block(key)) {
        if (Tag_val(key) == Forward_tag) {
          f = Forward_val(key);
          if (Is_block(f)) {
            if (Tag_val(f) == Forward_tag || Tag_val(f) == Lazy_tag ||
                Tag_val(f) == Forcing_tag || Tag_val(f) == Double_tag) {
              /* Do not short-circuit the pointer */
            } else {
              Field(v, i) = key = f;
              goto ephemeron_again;
            }
          }
        }
        else {
          if (Tag_val (key) == Infix_tag) key -= Infix_offset_val (key);
          if (is_unmarked (key))
            alive_data = 0;
        }
      }
    }

    bool keep;
    if (data == caml_ephe_none || Is_long(data)) {
      /* Not yet known whether this ephemeron's keys/block will be marked,
         but since the data is trivial nothing will happen if they are,
         so remove it from the todo list */
      trivial_data++;
      keep = false;
    } else if (force_alive || alive_data) {
      /* This ephemeron's keys & block are marked, so mark the data,
         and remove it from the todo list */
      caml_darken (domain_state, data, 0);
      made_live++;
      keep = false;
    } else {
      /* Leave this ephemeron on the todo list */
      keep = true;
    }

    if (keep) {
      prev_linkp = &Ephe_link(v);
    } else {
      Ephe_link(v) = domain_state->ephe_info->live;
      domain_state->ephe_info->live = v;
      *prev_linkp = todo;
    }
    marked++;
    budget -= mark_work_done_between_slices();
  }

  caml_gc_log ("Mark Ephemeron: %s. Ephemeron cycle=%" CAML_PRIdNAT " "
               "examined=%" CAML_PRIdNAT " trivial_data=%" CAML_PRIdNAT " "
               "marked=%" CAML_PRIdNAT,
               domain_state->ephe_info->cursor.cycle == for_cycle ?
                 "Continued from cursor" : "Discarded cursor",
               for_cycle, marked, trivial_data, made_live);

  domain_state->ephe_info->cursor.cycle = for_cycle;
  domain_state->ephe_info->cursor.todop = prev_linkp;

  return budget;
}

static intnat ephe_sweep (caml_domain_state* domain_state, intnat budget)
{
  value v;
  CAMLassert (caml_gc_phase == Phase_sweep_ephe);

  while (domain_state->ephe_info->todo != 0 && budget > 0) {
    v = domain_state->ephe_info->todo;
    domain_state->ephe_info->todo = Ephe_link(v);
    CAMLassert (Tag_val(v) == Abstract_tag);

    if (is_unmarked(v)) {
      /* The whole array is dead, drop this ephemeron */
    } else {
      caml_ephe_clean(v);
      Ephe_link(v) = domain_state->ephe_info->live;
      domain_state->ephe_info->live = v;
      budget -= Whsize_val(v);
    }
  }
  return budget;
}

/*******************************************************************************
 * Orphaning and adoption
 ******************************************************************************/

/* These are biased data structures left over from terminating domains.

   Synchronization:
   - operations that mutate the structure
     (adding new orphaned values or adopting orphans)
     are protected from each other using [orphaned_lock];
     this is simpler than using atomic lists, and not performance-sensitive
   - the read-only function [no_orphaned_work()] uses atomic accesses
     to avoid taking a lock (it is called more often)
 */
static struct {
  value _Atomic ephe_list_live;
  struct caml_final_info * _Atomic final_info;
} orph_structs = {0, NULL};

static caml_plat_mutex orphaned_lock = CAML_PLAT_MUTEX_INITIALIZER;


void caml_orphan_ephemerons (caml_domain_state* domain_state)
{
  CAMLassert (caml_gc_phase != Phase_sweep_main);

  struct caml_ephe_info* ephe_info = domain_state->ephe_info;
  if (ephe_info->todo == 0 &&
      ephe_info->live == 0 &&
      ephe_info->must_sweep_ephe == 0)
    return;

  /* Mark all ephemerons on live list.
     (ephe_mark() may move unmarked ephemerons to the live list if
      their data is none or immediate.)

     See Note [invariants on orphaned ephemerons]. */
  for (value v = ephe_info->live; v != 0; v = Ephe_link(v)) {
    if (is_unmarked(v)) {
      /* This can only happen when the data is trivial. */
      CAMLassert(Ephe_data(v) == caml_ephe_none || Is_long(Ephe_data(v)));
      caml_darken(domain_state, v, 0);
    }
  }

  if (caml_gc_phase == Phase_sweep_and_mark_main
      || caml_gc_phase == Phase_mark_final)
  {
    /* Force all ephemerons and their data on todo list to be alive */
    if (ephe_info->todo) {
      while (ephe_info->todo) {
        ephe_mark (100000, 0, EPHE_MARK_FORCE_ALIVE);
      }
      ephe_todo_list_emptied ();
    }
    /* No need to sweep ephemerons: they will be adopted before
       the cycle completes. */
  } else {
    /* Ensure that the ephemerons of this domain are swept/cleaned, in
       case they stay orphaned until the next GC cycle. This mirrors
       the logic in [major_collection_slice] for [Phase_sweep_ephe]. */
    if (ephe_info->must_sweep_ephe) {
      ephe_info->must_sweep_ephe = 0;
      prepare_for_ephe_sweeping(domain_state);
    }
    if (ephe_info->todo) {
      while (ephe_info->todo) {
        ephe_sweep(domain_state, 100000);
      }
      (void)caml_atomic_counter_decr(&num_domains_to_ephe_sweep);
    }
  }
  CAMLassert(ephe_info->todo == 0);

  if (ephe_info->live) {
    value live_tail = ephe_list_tail(ephe_info->live);
    CAMLassert(Ephe_link(live_tail) == 0);

    caml_plat_lock_blocking(&orphaned_lock);
    Ephe_link(live_tail) = orph_structs.ephe_list_live;
    orph_structs.ephe_list_live = ephe_info->live;
    ephe_info->live = 0;
    caml_plat_unlock(&orphaned_lock);
  }

  CAMLassert (ephe_info->must_sweep_ephe == 0);
  CAMLassert (ephe_info->live == 0);
  CAMLassert (ephe_info->todo == 0);
}

void caml_orphan_finalisers (caml_domain_state* domain_state)
{
  struct caml_final_info* f = domain_state->final_info;

  if (f->todo_head != NULL || f->first.size != 0 || f->last.size != 0) {
    /* have some final structures */
    (void)caml_atomic_counter_incr(&num_domains_orphaning_finalisers);
    /* At present, we only call this function (during
     * domain_terminate) after caml_finish_marking(), so should be in
     * Phase_sweep_and_mark_main. However, we shouldn't rely on that;
     * if we're not in a mark/sweep phase then we should force the
     * current cycle into one. */
    if (caml_gc_phase != Phase_sweep_main &&
        caml_gc_phase != Phase_sweep_and_mark_main) {
      /* Force a major GC cycle to simplify constraints for orphaning
         finalisers. See note attached to the declaration of
         [num_domains_orphaning_finalisers] variable in major_gc.c */
      caml_finish_major_cycle(0);
    }
    CAMLassert(caml_gc_phase == Phase_sweep_main ||
               caml_gc_phase == Phase_sweep_and_mark_main);
    CAMLassert (!f->updated_first);
    CAMLassert (!f->updated_last);

    /* Add the finalisers to [orph_structs] */
    caml_plat_lock_blocking(&orphaned_lock);
    f->next = orph_structs.final_info;
    orph_structs.final_info = f;
    caml_plat_unlock(&orphaned_lock);

    /* Create a dummy final info */
    f = domain_state->final_info = caml_alloc_final_info();
    (void)caml_atomic_counter_decr(&num_domains_orphaning_finalisers);
  }

  /* [caml_orphan_finalisers] is called in a while loop in
     [caml_domain_terminate].
     We take care to decrement the [num_domains_to_final_update*] counters only
     if we have not already decremented them for the current cycle. */
  if(!f->updated_first) {
    (void)caml_atomic_counter_decr(&num_domains_to_final_update_first);
    f->updated_first = 1;
  }
  if(!f->updated_last) {
    (void)caml_atomic_counter_decr(&num_domains_to_final_update_last);
    f->updated_last = 1;
  }
}

/* ── M6: adopt orphaned finalisers into a live domain (MMTK_WEAK_REFS) ─────────
   A terminating domain hands its [final_info] to [orph_structs] (above). The
   stock GC drained that inside the major cycle (the deleted [adopt_orphaned_work]);
   under MMTk nothing did, so finalisers registered on a domain that then
   terminates never ran — their values become unreachable but no live domain's
   table holds them. Re-attach the orphaned structures to [domain_addr] (a live
   domain), so the binding's process_weak_refs then processes them through the
   normal MMTk finaliser pass (caml_mmtk_final_update_first / _cleanup).

   Called from process_weak_refs, once per GC, on the GC worker with mutators
   stopped; the lock only guards against concurrent orphaning (impossible during
   STW, but cheap). [retain] traces an object (keeping it live) and returns its
   forwarded address — used for the already-queued run-queue entries, which are
   not roots of this GC. Draining [orph_structs.final_info] to NULL makes repeated
   calls within one GC's mark fixpoint no-ops. */
void caml_mmtk_adopt_orphaned_finalisers(uintptr_t domain_addr,
                                         caml_mmtk_ephe_retain_fn retain,
                                         void *ctx)
{
  caml_domain_state *domain_state = (caml_domain_state *) domain_addr;
  struct caml_final_info *target = domain_state->final_info;
  if (target == NULL) return;

  caml_plat_lock_blocking(&orphaned_lock);
  struct caml_final_info *f = orph_structs.final_info;
  orph_structs.final_info = NULL;
  caml_plat_unlock(&orphaned_lock);

  while (f != NULL) {
    struct caml_final_info *next = f->next;

    /* No minor/major split under MMTk: mark every orphaned value "old" so the
       merge prepends it into the target's finalisable (old) region. The merged
       entries are re-examined by caml_mmtk_final_update_first / _cleanup. */
    if (f->first.young > 0) {
      f->first.old = f->first.young;
      caml_final_merge_finalisable(&f->first, &target->first);
    }
    if (f->last.young > 0) {
      f->last.old = f->last.young;
      caml_final_merge_finalisable(&f->last, &target->last);
    }

    /* Splice the orphaned run-queue (finalisers already deemed runnable) onto
       the target's. Their fun/val were live when queued but are not roots of
       this GC, so retain them to keep them alive and pick up forwarding. */
    for (struct final_todo *td = f->todo_head; td != NULL; td = td->next) {
      for (int i = 0; i < td->size; i++) {
        td->item[i].fun = retain(ctx, td->item[i].fun);
        if (Is_block(td->item[i].val))
          td->item[i].val = retain(ctx, td->item[i].val);
      }
    }
    if (f->todo_head != NULL) {
      if (target->todo_tail == NULL)
        target->todo_head = f->todo_head;
      else
        target->todo_tail->next = f->todo_head;
      target->todo_tail = f->todo_tail;
    }

    /* [first.table]/[last.table] were copied by the merge; free them and the
       now-empty orphaned struct. The run-queue blocks were spliced (not copied)
       and are owned by the target now. */
    if (f->first.table != NULL) caml_stat_free(f->first.table);
    if (f->last.table != NULL) caml_stat_free(f->last.table);
    caml_stat_free(f);
    f = next;
  }
}

/*******************************************************************************
 * Pacing
 ******************************************************************************/

/* These two counters keep track of how much work the GC is supposed to
   do in order to keep up with allocation. Both are in GC work units.
   `alloc_counter` increases when we allocate: the number of words allocated
   is converted to GC work units and added to this counter.
   `work_counter` increases when the GC has done some work.
   The difference between the two is how much the GC is lagging behind
   (or in advance of) allocations.
   These counters can wrap around (see function `diffmod`) as long as they
   don't get too far apart, which is guaranteed by the limited size of
   memory.
*/
static atomic_uintnat alloc_counter;
static atomic_uintnat work_counter;

/* Value of work_counter at the latest color rotation (start of sweep)
   and number of allocations done during the latest sweep phase.
   Not atomic because these are only accessed in stw. */
static uintnat latest_sweep_allocs;

/* Small-memory mode: at the end of sweeping, we will not switch to
   Phase_mark_and_sweep_main (and thus will stay in idle mode) until
   work_counter has reached this value. */
static atomic_uintnat work_counter_min_before_mark;

static inline intnat max2 (intnat a, intnat b)
{
  if (a > b){
    return a;
  }else{
    return b;
  }
}

static inline intnat max3(intnat a, intnat b, intnat c)
{
  if (a > b){
    return max2 (a, c);
  }else{
    return max2 (b, c);
  }
}

/* Take two natural numbers n1 and n2 and let N = 2^{64}.
   Assume that n1 and n2 are not too far apart (less than N/2).
   Given unsigned numbers x1 = n1 modulo N and x2 = n2 modulo N, return
   the (signed) difference between n1 and n2.
*/
static inline intnat diffmod (uintnat x1, uintnat x2)
{
  return (intnat) (x1 - x2);
}

/* Initialize the counters for GC pacing.
   This is for use in caml_init_gc, when everything is still single-threaded.
   caml_small_heap_limit must be initialized before calling this function.
*/
void caml_init_major_pacing (void)
{
  alloc_counter = 0;
  work_counter = 0;
  caml_gc_log ("work_counter: initialize to 0");
  work_counter_min_before_mark = caml_small_heap_limit;
}

/* Reset the work and alloc counters to be equal to each other, by
 * setting them both equal to the "larger" (in the wrapping-around
 * sense we are using here for work_counter and alloc_counter).
 *
 * For use at times when we have disturbed the major GC from its usual
 * pacing and tempo, for example, after any synchronous major
 * collection.
 *
 * add_overhead is true if the latest collection was synchronous
 * (with caml_gc_full_major) and thus the sweep phase counted only the
 * live data (with no floating garbage).
 */

void caml_reset_major_pacing(bool add_overhead)
{
  bool res;
  uintnat target;
  do {
    uintnat alloc = atomic_load(&alloc_counter);
    uintnat work = atomic_load(&work_counter);
    target = alloc;
    if (diffmod(work, alloc) > 0) {
      target = work;
    }
    res = (atomic_compare_exchange_strong(&alloc_counter, &alloc, target) &&
           atomic_compare_exchange_strong(&work_counter, &work, target));
  } while (!res);
  caml_gc_log ("work_counter: reset to %" CAML_PRIuNAT, target);
  uintnat virtual_sweep_work = latest_sweep_allocs;
  if (add_overhead){
    virtual_sweep_work = virtual_sweep_work / 100 * (100 + caml_percent_free);
  }
  work_counter_min_before_mark =
    target + max2 (virtual_sweep_work, caml_small_heap_limit);
}

/* The [log_events] parameter is used to disable writing to the ring for two
   reasons:
   1. To prevent spamming the ring with numerous events generated during
      an opportunistic GC slice.
   2. To avoid logging events when the calling domain is not part of the
      Stop-The-World (STW) participant set. If the domain is not part of
      the STW set, the ring could be torn down concurrently while this domain
      attempts to write to it. */
static void
update_major_slice_work(intnat howmuch,
                        int may_access_gc_phase,
                        int log_events /* log events to the ring? */)
{
  intnat alloc_work, dependent_work, extra_work, new_work;
  intnat my_alloc_count, my_alloc_direct_count, my_dependent_count;
  intnat my_alloc_suspended_count, my_alloc_resumed_count;
  double my_extra_count;
  caml_domain_state *dom_st = Caml_state;
  uintnat heap_words, heap_size, heap_sweep_words, total_cycle_work;
  uintnat percent_free;

  my_alloc_count = dom_st->allocated_words;
  my_alloc_direct_count = dom_st->allocated_words_direct;
  my_alloc_suspended_count = dom_st->allocated_words_suspended;
  my_alloc_resumed_count = dom_st->allocated_words_resumed;
  my_dependent_count = dom_st->dependent_allocated;
  my_extra_count = dom_st->extra_heap_resources;

  dom_st->stat_major_words += dom_st->allocated_words;
  dom_st->current_ramp_up_allocated_words_diff +=
    dom_st->allocated_words_suspended;

  dom_st->allocated_words = 0;
  dom_st->allocated_words_direct = 0;
  dom_st->allocated_words_suspended = 0;
  dom_st->allocated_words_resumed = 0;
  dom_st->dependent_allocated = 0;
  dom_st->extra_heap_resources = 0.0;

  /*
     Free memory at the start of the GC cycle (garbage + free list) (assumed):
                 FM = heap_words * caml_percent_free
                      / (100 + caml_percent_free)

     Assuming steady state and enforcing a constant allocation rate, then
     FM is divided in 2/3 for garbage and 1/3 for free list.
              G = 2 * FM / 3
     G is also the amount of memory that will be used during this cycle
     (still assuming steady state).

     Proportion of G consumed since the previous slice:
              PH = dom_st->allocated_words / G
                = dom_st->allocated_words * 3 * (100 + caml_percent_free)
                  / (2 * heap_words * caml_percent_free)
     Proportion of extra-heap resources consumed since the previous slice:
              PE = dom_st->extra_heap_resources
     Proportion of total work to do in this slice:
              P  = max (PH, PE)
     Amount of marking work for the GC cycle:
              MW = heap_words * 100 / (100 + caml_percent_free)
     Amount of sweeping work for the GC cycle:
              SW = heap_sweep_words
     Amount of total work for the GC cycle:
              TW = MW + SW
              = heap_words * 100 / (100 + caml_percent_free) + heap_sweep_words

     Amount of time to spend on this slice:
                 T = P * TT

     Since we must do TW amount of work in TT time, the amount of work done
     for this slice is:
                 S = P * TW
  */
  heap_size = caml_heap_size(dom_st->shared_heap);
  heap_words = Wsize_bsize(heap_size);
  heap_sweep_words = heap_words;
  percent_free = atomic_load(&caml_percent_free);

  total_cycle_work =
    heap_sweep_words
    + (uintnat) ((double) heap_words * 100.0 / (100.0 + percent_free));

  if (heap_words > 0) {
    double alloc_ratio =
      total_cycle_work
      * 3.0 * (100 + percent_free)
      / heap_words / percent_free / 2.0;
    intnat current_alloc_count =
      my_alloc_count - my_alloc_suspended_count + my_alloc_resumed_count;
    CAMLassert (current_alloc_count >= 0);
    alloc_work = (intnat) (current_alloc_count * alloc_ratio);
  } else {
    alloc_work = 0;
  }

  if (dom_st->dependent_size > 0) {
    double dependent_ratio =
      total_cycle_work
      * (100 + percent_free)
        / (double)dom_st->dependent_size / (double)percent_free;
    dependent_work = (intnat) (my_dependent_count * dependent_ratio);
  }else{
    dependent_work = 0;
  }

  extra_work = (intnat) (my_extra_count * (double) total_cycle_work);

  CAML_GC_MESSAGE(SLICESIZE, "heap_words = %" CAML_PRIuNAT "\n",
                  heap_words);
  CAML_GC_MESSAGE(SLICESIZE, "allocated_words = %" CAML_PRIuNAT "\n",
                   my_alloc_count);
  CAML_GC_MESSAGE(SLICESIZE, "allocated_words_direct = %" CAML_PRIuNAT "\n",
                   my_alloc_direct_count);
  CAML_GC_MESSAGE(SLICESIZE, "allocated_words_suspended = %" CAML_PRIuNAT "\n",
                   my_alloc_suspended_count);
  CAML_GC_MESSAGE(SLICESIZE, "allocated_words_resumed = %" CAML_PRIuNAT "\n",
                   my_alloc_resumed_count);
  CAML_GC_MESSAGE(SLICESIZE, "alloc work-to-do = %" CAML_PRIdNAT "\n",
                   alloc_work);
  CAML_GC_MESSAGE(SLICESIZE, "dependent_words = %" CAML_PRIuNAT "\n",
                   my_dependent_count);
  CAML_GC_MESSAGE(SLICESIZE, "dependent work-to-do = %" CAML_PRIdNAT "\n",
                  dependent_work);
  CAML_GC_MESSAGE(SLICESIZE, "extra_heap_resources = %" CAML_PRIuNAT "u\n",
                  (uintnat) (my_extra_count * 1000000));
  CAML_GC_MESSAGE(SLICESIZE, "extra work-to-do = %" CAML_PRIdNAT "\n",
                  extra_work);

  new_work = max3 (alloc_work, dependent_work, extra_work);
  atomic_fetch_add (&alloc_counter, new_work);

  uintnat work_done_between_slices =
    mark_work_done_between_slices() +
    sweep_work_done_between_slices();
  atomic_fetch_add (&work_counter, work_done_between_slices);

  /* If the work_counter is falling far behind the alloc_counter,
   * artificially catch up some of the difference. This is a band-aid
   * for general GC pacing problems revealed by the mark-delay changes
   * (PR #13580). */
  int64_t pending = diffmod(atomic_load(&alloc_counter),
                             atomic_load(&work_counter));
  if (pending > (int64_t)total_cycle_work * 2) {
    intnat catchup = pending - total_cycle_work;
    CAML_GC_MESSAGE(SLICESIZE,
                    "work counter %"CAML_PRIuNAT" falling behind "
                    "alloc counter %"CAML_PRIuNAT" by more than "
                    "twice a total cycle's work %"CAML_PRIuNAT"; "
                    "catching up by %"CAML_PRIdNAT"\n",
                    atomic_load(&work_counter),
                    atomic_load(&alloc_counter),
                    total_cycle_work, catchup);
    atomic_fetch_add (&work_counter, catchup);
    caml_gc_log ("work_counter: advance to %" CAML_PRIuNAT, work_counter);
  }

  if (howmuch == AUTO_TRIGGERED_MAJOR_SLICE ||
      howmuch == GC_CALCULATE_MAJOR_SLICE) {
    dom_st->slice_target = atomic_load (&alloc_counter);
    dom_st->slice_budget = 0;
  }else{
    /* forced or opportunistic GC slice with explicit quantity */
    dom_st->slice_target = atomic_load (&work_counter);  /* already reached */
    dom_st->slice_budget = howmuch;
  }

  caml_gc_log("Updated major work: [%c] "
              " %" CAML_PRIuNAT " heap_words, "
              " %" CAML_PRIuNAT " allocated, "
              " %" CAML_PRIuNAT " allocated (direct), "
              " %" CAML_PRIuNAT " allocated (suspended), "
              " %" CAML_PRIuNAT " allocated (resumed), "
              " %" CAML_PRIdNAT " alloc_work, "
              " %" CAML_PRIdNAT " dependent_work, "
              " %" CAML_PRIdNAT " extra_work, "
              " %" CAML_PRIuNAT " work counter %s, "
              " %" CAML_PRIuNAT " alloc counter, "
              " %" CAML_PRIuNAT " slice target, "
              " %" CAML_PRIdNAT " slice budget"
              ,
              caml_gc_phase_char(may_access_gc_phase),
              heap_words, my_alloc_count, my_alloc_direct_count,
              my_alloc_suspended_count, my_alloc_resumed_count,
              alloc_work, dependent_work, extra_work,
              atomic_load (&work_counter),
              diffmod (work_counter, alloc_counter) > 0
                ? "[ahead]" : "[behind]",
              atomic_load (&alloc_counter),
              dom_st->slice_target, dom_st->slice_budget
              );

  if (log_events) {
    CAML_EV_COUNTER(EV_C_MAJOR_HEAP_WORDS, (uintnat)heap_words);
    CAML_EV_COUNTER(EV_C_MAJOR_ALLOCATED_WORDS, my_alloc_count);
    /* TODO: add counters for direct, suspended, resumed allocs. */
    CAML_EV_COUNTER(EV_C_MAJOR_ALLOCATED_WORK, alloc_work);
    CAML_EV_COUNTER(EV_C_MAJOR_DEPENDENT_WORK, dependent_work);
    CAML_EV_COUNTER(EV_C_MAJOR_EXTRA_WORK, extra_work);
    CAML_EV_COUNTER(EV_C_MAJOR_WORK_COUNTER, atomic_load (&work_counter));
    CAML_EV_COUNTER(EV_C_MAJOR_ALLOC_COUNTER, atomic_load (&alloc_counter));
    CAML_EV_COUNTER(EV_C_MAJOR_SLICE_TARGET, dom_st->slice_target);
    CAML_EV_COUNTER(EV_C_MAJOR_SLICE_BUDGET, dom_st->slice_budget);
  }
}

/*******************************************************************************
 * Mark stack (allocated/freed per domain; never populated under MMTk)
 ******************************************************************************/

/* The mark stack is no longer used for tracing under always-on MMTk — MMTk owns
   marking. The structure is still allocated in caml_init_major_gc and freed in
   caml_teardown_major_gc, so its definition is retained. */

#define MARK_STACK_INIT_SIZE (1 << 12)

typedef struct {
  value_ptr start;
  value_ptr end;
} mark_entry; /* represents fields in the span [start, end) */

struct mark_stack {
  mark_entry* stack;
  uintnat count;
  uintnat size;
  struct addrmap compressed_stack;
  addrmap_iterator compressed_stack_iter;
};

void caml_darken_cont(value cont)
{
  /* Inert under always-on MMTk: MMTk owns tracing, so darkening is a no-op.
     Still called from fiber.c. */
  (void)cont;
}

void caml_darken(void* state, value v, volatile value* ignored) {
  /* Inert under always-on MMTk: MMTk owns tracing, and weak/ephemeron/finaliser
     liveness is handled by process_weak_refs. Darkening here would push to a stack
     nobody processes and corrupt the stock GC phase counters. No-op.
     Still called from finalise.c, memory.c, weak.c. */
  (void)state; (void)v; (void)ignored;
}

/*******************************************************************************
 * Major GC cycle
 ******************************************************************************/

/* True when some domain wants to enter Phase_sweep_and_mark_main */
atomic_uintnat caml_gc_mark_phase_requested;

void caml_mark_roots_stw (int participant_count,
                          caml_domain_state** barrier_participants)
{
  /* Inert under always-on MMTk: MMTk owns root scanning and tracing. This is
     still called from the minor GC (minor_gc.c) when the stock mark phase is
     requested, but under MMTk caml_gc_mark_phase_requested is never set, so the
     call site never fires. No-op. */
  (void)participant_count; (void)barrier_participants;
}

/*******************************************************************************
 * Major GC slices
 ******************************************************************************/

intnat caml_opportunistic_major_work_available (caml_domain_state* domain_state)
{
  return !domain_state->sweeping_done ||
    (caml_marking_started() && !domain_state->marking_done);
}

void caml_opportunistic_major_collection_slice(intnat howmuch)
{
  /* Inert under always-on MMTk: MMTk owns collection. No-op. */
  (void)howmuch;
}

void caml_major_collection_slice(intnat howmuch)
{
  /* Inert under always-on MMTk: MMTk owns collection. We skip the stock slice
     but STILL record this domain's major-slice epoch. Otherwise
     caml_reset_young_limit keeps observing
     (domain->major_slice_epoch < caml_major_slice_epoch), re-arms the interrupt
     at every safepoint, and the bytecode mutator spins forever in
     caml_poll_gc_work (the native TLAB path early-returns before reaching here,
     so this bites bytecode only). This epoch record is load-bearing. */
  (void)howmuch;
  Caml_state->major_slice_epoch = atomic_load (&caml_major_slice_epoch);
}

/*******************************************************************************
 * Major GC API
 ******************************************************************************/

void caml_finish_major_cycle (int force_compaction)
{
  /* Inert under always-on MMTk: there is no stock major cycle. No-op. */
  (void)force_compaction;
}

#ifdef DEBUG
int caml_mark_stack_is_empty(void)
{
  return Caml_state->mark_stack->count == 0;
}
#endif

void caml_finish_marking (void)
{
  /* No stock marking under MMTk. Mark "done" so caml_domain_terminate's
     marking_and_sweeping_done() is satisfied (the flag inits to 0 and is
     otherwise only advanced by the now-removed stock mark phase). */
  Caml_state->marking_done = 1;
}

void caml_finish_sweeping (void)
{
  /* No stock sweeping under MMTk. Mark "done" (see caml_finish_marking). */
  Caml_state->sweeping_done = 1;
}

int caml_init_major_gc(caml_domain_state* d) {
  d->mark_stack = caml_stat_alloc_noexc(sizeof(struct mark_stack));
  if(d->mark_stack == NULL) {
    return -1;
  }
  d->mark_stack->stack =
    caml_stat_alloc_noexc(MARK_STACK_INIT_SIZE * sizeof(mark_entry));
  if(d->mark_stack->stack == NULL) {
    caml_stat_free(d->mark_stack);
    d->mark_stack = NULL;
    return -1;
  }
  d->mark_stack->count = 0;
  d->mark_stack->size = MARK_STACK_INIT_SIZE;
  caml_addrmap_init(&d->mark_stack->compressed_stack);
  d->mark_stack->compressed_stack_iter =
                  caml_addrmap_iterator(&d->mark_stack->compressed_stack);

  if (caml_gc_phase == Phase_sweep_main) {
    /* This fresh domain will allocate UNMARKED until we start
     * marking, so must mark in this cycle. */
    d->sweeping_done = 1;
    d->marking_done = 0;
    (void)caml_atomic_counter_incr(&num_domains_to_mark);
    (void)caml_atomic_counter_incr(&ephe_cycle_info.num_domains_todo);
  } else {
    /* This fresh domain will allocate MARKED in this cycle,
     * so doesn't need to mark. */
    d->sweeping_done = 1;
    d->marking_done = 1;
  }

  /* Finalisers. Fresh domains participate in updating finalisers. */
  d->final_info = caml_alloc_final_info ();
  if(d->final_info == NULL) {
    caml_stat_free(d->mark_stack->stack);
    caml_stat_free(d->mark_stack);
    return -1;
  }
  d->ephe_info = caml_alloc_ephe_info();
  if(d->ephe_info == NULL) {
    caml_stat_free(d->final_info);
    caml_stat_free(d->mark_stack->stack);
    caml_stat_free(d->mark_stack);
    d->final_info = NULL;
    d->mark_stack = NULL;
    return -1;
  }
  (void)caml_atomic_counter_incr(&num_domains_to_final_update_first);
  (void)caml_atomic_counter_incr(&num_domains_to_final_update_last);

  return 0;
}

void caml_teardown_major_gc(void) {
  caml_domain_state* d = Caml_state;

/* At this point we have been removed from the STW participant set,
   so we may not access the gc phase. */
  int may_access_gc_phase = 0;

  /* Account for latest allocations, but do not write to the event ring since
     we are out of the STW participant set; the ring may be torn down
     concurrently. */
  update_major_slice_work (0, may_access_gc_phase, 0);
  CAMLassert(!caml_addrmap_iter_ok(&d->mark_stack->compressed_stack,
                                   d->mark_stack->compressed_stack_iter));
  caml_addrmap_clear(&d->mark_stack->compressed_stack);
  CAMLassert(d->mark_stack->count == 0);
  caml_stat_free(d->mark_stack->stack);
  caml_stat_free(d->mark_stack);
  d->mark_stack = NULL;
}
