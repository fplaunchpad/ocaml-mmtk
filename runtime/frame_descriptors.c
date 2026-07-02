/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*      KC Sivaramakrishnan, Indian Institute of Technology, Madras       */
/*                   Tom Kelly, OCaml Labs Consultancy                    */
/*                 Stephen Dolan, University of Cambridge                 */
/*                                                                        */
/*   Copyright 2019 Indian Institute of Technology, Madras                */
/*   Copyright 2021 OCaml Labs Consultancy Ltd                            */
/*   Copyright 2019 University of Cambridge                               */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include "caml/platform.h"
#include "caml/frame_descriptors.h"
#include "caml/memory.h"
#include "caml/fail.h"
/* GC-cycle RCU retire: tag each retired frametable snapshot with the MMTk
   collection count and free it once a full collection has elapsed (every reader
   re-fetched, every ConcurrentImmix worker finished). NB
   caml_major_cycles_completed is DEAD under MMTk (major_gc.c initialises it to
   0 and never bumps it -- the stock major-GC machinery that incremented it is
   bypassed), so we key on mmtk_ocaml_gc_count() instead. */
#include "../gc/mmtk/include/mmtk_ocaml.h"
#include <stddef.h>

/* The reader-visible frame-descriptor table: an IMMUTABLE {mask, descriptors}
   snapshot. A writer NEVER mutates a published descriptors[] in place -- it
   builds a fresh one and atomically swaps the published pointer (see
   current_frametable / struct frametable_version below). So concurrent
   stack-walkers (the GC root scan, backtrace, signals, tsan, and
   ConcurrentImmix GC workers) read a stable mask+descriptors pair: both come
   from the one pointer they acquire-loaded, so a probe can never mix a new mask
   with an old array. */
struct caml_frame_descrs {
  int mask;
  frame_descr** descriptors;
};
/* Let us call 'capacity' the length of the descriptors array.

   We maintain the following invariants:
     capacity = mask + 1
     capacity = 0 || Is_power_of_2(capacity)
     num_desc <= 2 * num_descr <= capacity

   For an extensible array we would maintain
      num_desc <= capacity,
    but this is a linear-problem hash table, we need to ensure that
    free slots are frequent enough, so we use a twice-larger capacity:
      num_desc * 2 <= capacity
*/

/* Writer-private bookkeeping (NOT reader-visible), protected by writer_lock for
   installs and by `mutex` for the finalizer-callable unregister/zombie
   splicing.

   We keep the list of frametables that was used to build the hashtable; we use
   it when rebuilding. We keep the list of frametables to be removed (zombies);
   an installer applies them (drops them from the build list) before the next
   rebuild. caml_unregister_frametable(s) may be called at any time, even inside
   a STW section / a custom-block finalizer, so it only ever splices the lists
   under `mutex` (no rebuild, no writer_lock). */
struct frame_descrs_state {
  int num_descr;
  caml_frametable_list *frametables;
  caml_frametable_list *zombies;
  caml_plat_mutex mutex;        /* protects frametables/zombies list splicing */
  /* serialises installers (the old STW-leader role) */
  caml_plat_mutex writer_lock;
};

/* A versioned, retire-able snapshot. Memory backing a frametable snapshot is
   only freed once a full MMTk collection has elapsed since it was retired,
   because other threads (mutator stack-walkers AND ConcurrentImmix GC workers)
   read the snapshot at unpredictable times and hold it only for the duration of
   one walk / one object scan. (This is RCU; mirrors Dolan's 2018
   frametable_version design, re-keyed from the dead caml_major_cycles_completed
   onto mmtk_ocaml_gc_count.) */
struct frametable_version {
  caml_frame_descrs table;          /* the published immutable snapshot */
  /* zombie cons cells retired with this version */
  caml_frametable_list *retired;
  /* mmtk_ocaml_gc_count() captured when THIS version was published (i.e. when
     its ->prev chain became stale). Once gc_count() has strictly advanced past
     it, the whole ->prev chain (and their ->retired zombie cells) may be freed.
     Set to No_need_to_free once that chain has been freed. */
  atomic_uintnat free_prev_after_cycle;
  struct frametable_version *prev;  /* chain of older, not-yet-freed versions */
};
#define No_need_to_free ((uintnat)(-1))

/* Defined in code generated by ocamlopt */
extern intnat * caml_frametable[];

/* Note: [cur] is bound by this macro */
#define iter_list(list,cur) \
  for (caml_frametable_list *cur = list; cur != NULL; cur = cur->next)

static frame_descr * next_frame_descr(frame_descr * d) {
  unsigned char num_allocs = 0, *p;
  CAMLassert(d->retaddr >= 4096);
  if (!frame_return_to_C(d)) {
    /* Skip to end of live_ofs */
    p = (unsigned char*)&d->live_ofs[d->num_live];
    /* Skip alloc_lengths if present */
    if (frame_has_allocs(d)) {
      num_allocs = *p;
      p += num_allocs + 1;
    }
    /* Skip debug info if present */
    if (frame_has_debug(d)) {
      /* Align to 32 bits */
      p = Align_to(p, uint32_t);
      p += sizeof(uint32_t) * (frame_has_allocs(d) ? num_allocs : 1);
    }
    /* Align to word size */
    p = Align_to(p, void*);
    return ((frame_descr*) p);
  } else {
    /* This marks the top of an ML stack chunk. Skip over empty
     * frame descriptor */
    /* Skip to address of zero-sized live_ofs */
    CAMLassert(d->num_live == 0);
    p = (unsigned char*)&d->live_ofs[0];
    /* Align to word size */
    p = Align_to(p, void*);
    return ((frame_descr*) p);
  }
}

static intnat count_descriptors(caml_frametable_list *list) {
  intnat num_descr = 0;
  iter_list(list,cur) {
    num_descr += *((intnat*) cur->frametable);
  }
  return num_descr;
}

static caml_frametable_list* frametables_list_tail(caml_frametable_list *list) {
  caml_frametable_list *tail = NULL;
  iter_list(list,cur) {
    tail = cur;
  }
  return tail;
}

static void fill_hashtable(
  caml_frame_descrs *table, caml_frametable_list *frametables)
{
  iter_list(frametables,cur) {
    intnat * tbl = (intnat*) cur->frametable;
    intnat len = *tbl;
    frame_descr * d = (frame_descr *)(tbl + 1);
    for (intnat j = 0; j < len; j++) {
      uintnat h = Hash_retaddr(d->retaddr, table->mask);
      while (table->descriptors[h] != NULL) {
        h = (h+1) & table->mask;
      }
      table->descriptors[h] = d;
      d = next_frame_descr(d);
    }
  }
}

/* Detach the pending zombie list (frametables unregistered since the last
   build). They were already unlinked from state->frametables by
   remove_frame_descriptors, so the next rebuild will not include their
   descriptors; here we just account for the removed descriptors and RETURN the
   zombie cons cells so the caller can defer their free to the grace period (a
   reader mid-walk may still hold a frame_descr* into a just-unregistered
   frametable). Caller holds writer_lock. */
static caml_frametable_list* clean_frame_descriptors(
  struct frame_descrs_state *state)
{
  caml_plat_lock_blocking(&state->mutex);
  caml_frametable_list *zombies = state->zombies;
  state->zombies = NULL;
  caml_plat_unlock(&state->mutex);

  intnat decrease = 0;
  iter_list(zombies, cur)
    decrease += *((intnat*) cur->frametable);
  state->num_descr -= decrease;
  return zombies;
}

/* Prepend new_frametables (may be NULL on a pure rebuild) to
   state->frametables, update state->num_descr, and build a FRESH immutable
   {mask, descriptors} snapshot from the full list. Never mutates or frees any
   published array -- the old snapshot is retired by the caller via the grace
   period. Writes the snapshot into *out and returns 1; returns 0 on OOM
   (state->frametables / num_descr are still updated, which is fine: the list
   grew, only the rebuild failed). Caller holds writer_lock. */
static int build_frame_descrs(
  struct frame_descrs_state *state,
  caml_frametable_list *new_frametables,
  caml_frame_descrs *out)
{
  if (new_frametables != NULL) {
    caml_frametable_list *tail = frametables_list_tail(new_frametables);
    state->num_descr += count_descriptors(new_frametables);
    tail->next = state->frametables;
    state->frametables = new_frametables;
  }

  /* Capacity: power of 2, >= 2 * num_descr, and >= 4. */
  intnat tblsize = 4;
  while (tblsize < 2 * state->num_descr) tblsize *= 2;

  frame_descr **descriptors =
    (frame_descr **) caml_stat_calloc_noexc(tblsize, sizeof(frame_descr *));
  if (descriptors == NULL) return 0;

  out->mask = (int)(tblsize - 1);
  out->descriptors = descriptors;
  fill_hashtable(out, state->frametables);
  return 1;
}

/* Free a retired frametable_version chain: every version's descriptors array
   and, for versions that retired zombies, the zombie cons cells (which carry
   the copy_cons frametable copies). Only called once the grace period has
   elapsed. */
static void free_frametable_versions(struct frametable_version *p)
{
  while (p != NULL) {
    struct frametable_version *next = p->prev;
    caml_frametable_list *z = p->retired;
    while (z != NULL) {
      caml_frametable_list *zn = z->next;
      caml_stat_free(z);   /* frees the copy_cons frametable copy too (it is
                              appended into the same allocation) */
      z = zn;
    }
    caml_stat_free(p->table.descriptors);
    caml_stat_free(p);
    p = next;
  }
}

/* Writer-side bookkeeping (installs hold writer_lock; unregister uses mutex).
   */
static struct frame_descrs_state frame_descrs = {
  0, NULL, NULL,
  CAML_PLAT_MUTEX_INITIALIZER, CAML_PLAT_MUTEX_INITIALIZER
};

/* The currently-published frametable version. Written only under writer_lock
   (and at single-domain startup), but READ without locking by every
   stack-walker, so it is atomic. caml_init_frame_descriptors publishes the
   first version before any mutator or GC worker exists. */
static _Atomic(struct frametable_version*) current_frametable = NULL;

static caml_frametable_list *cons(
  intnat *frametable, caml_frametable_list *tl)
{
  caml_frametable_list *li = caml_stat_alloc(sizeof(caml_frametable_list));
  li->frametable = frametable;
  li->next = tl;
  return li;
}

/* This function not only creates a new caml_frametable_list cell but
   also makes a copy of the new frametable.
   Here, we allocate, in a single malloc call, the space for the cons
   cell and the (appended) frametable copy. This way, we do not have
   to change the code that unregisters the frametable since calling free
   on the cons cell will automatically free the frametable copy at the
   same time.
*/
static caml_frametable_list *copy_cons(
  intnat **frametable, intnat size, caml_frametable_list *tl)
{
  caml_frametable_list *li =
    caml_stat_alloc(sizeof(caml_frametable_list) + size);
  intnat *frametable_copy = (intnat*)(li + 1);
  memcpy(frametable_copy, *frametable, size);
  *frametable = frametable_copy;
  li->frametable = frametable_copy;
  li->next = tl;
  return li;
}

void caml_init_frame_descriptors(void)
{
  caml_frametable_list *frametables = NULL;
  for (int i = 0; caml_frametable[i] != 0; i++)
    frametables = cons(caml_frametable[i], frametables);

  /* Called from caml_init_gc, BEFORE caml_init_domains -- single-domain, before
     any mutator runs and before MMTk's collection workers exist. So we publish
     the first version directly; there is no older version to retire and no
     reader or GC worker to drain. */
  struct frametable_version *ft = caml_stat_alloc(sizeof(*ft));
  if (!build_frame_descrs(&frame_descrs, frametables, &ft->table))
    caml_raise_out_of_memory();
  ft->retired = NULL;
  atomic_store_release(&ft->free_prev_after_cycle, No_need_to_free);
  ft->prev = NULL;
  atomic_store_release(&current_frametable, ft);
}

/* RCU install (replaces the all-domains STW). Serialise installers
   (writer_lock), apply pending zombie removals, build a FRESH snapshot off to
   the side, and atomically publish it. The OLD version is NOT freed here: it is
   chained onto the new version's ->prev and freed lazily (in
   caml_get_frame_descrs) once a full MMTk collection has elapsed
   (free_prev_after_cycle gate), by which time every mutator stack-walker has
   re-fetched (the "valid until next GC" reader contract) AND every
   ConcurrentImmix GC worker scanning a stack has finished. No quiesce, no
   wait_collection_done.

   Sole caller chain: caml_natdynlink_register (dynlink_nat.c) -> RUNNING,
   holding the domain lock, NOT in a blocking section. */
static void install_frametables(caml_frametable_list *new_frametables)
{
  caml_plat_lock_blocking(&frame_descrs.writer_lock);

  /* Detach zombies retired since the last build (their cells are freed with
     this version, not now -- a reader may still hold a descriptor from them).
     */
  caml_frametable_list *retired = clean_frame_descriptors(&frame_descrs);

  struct frametable_version *ft = caml_stat_alloc_noexc(sizeof(*ft));
  if (ft == NULL) {
    /* Splice the zombies back so they are retried at the next install. */
    if (retired != NULL) {
      caml_plat_lock_blocking(&frame_descrs.mutex);
      caml_frametable_list *tail = frametables_list_tail(retired);
      tail->next = frame_descrs.zombies;
      frame_descrs.zombies = retired;
      caml_plat_unlock(&frame_descrs.mutex);
    }
    caml_plat_unlock(&frame_descrs.writer_lock);
    caml_raise_out_of_memory();
  }
  if (!build_frame_descrs(&frame_descrs, new_frametables, &ft->table)) {
    caml_stat_free(ft);
    /* new_frametables / zombies are now part of frame_descrs state; the rebuild
       failed but the list is consistent, so a later install can retry. */
    caml_plat_unlock(&frame_descrs.writer_lock);
    caml_raise_out_of_memory();
  }
  ft->retired = retired;

  struct frametable_version *old = atomic_load_acquire(&current_frametable);
  ft->prev = old;
  /* Tag: free old (and its ->prev chain) once gc_count strictly advances past
     this. Reclaimed lazily in caml_get_frame_descrs (runs on every GC root
     scan). */
  atomic_store_release(&ft->free_prev_after_cycle,
                       (uintnat) mmtk_ocaml_gc_count());
  atomic_store_release(&current_frametable, ft);

  caml_plat_unlock(&frame_descrs.writer_lock);
}

void caml_register_frametables(void **table, int ntables) {
  caml_frametable_list *new_frametables = NULL;
  for (int i = 0; i < ntables; i++)
    new_frametables = cons(table[i], new_frametables);

  install_frametables(new_frametables);
}

void caml_copy_and_register_frametables(
  void **table, int * sizes, int ntables)
{
  caml_frametable_list *new_frametables = NULL;
  for (int i = 0; i < ntables; i++)
    new_frametables = copy_cons((intnat **)(table + i),
                                sizes[i], new_frametables);

  install_frametables(new_frametables);
}

static void remove_frame_descriptors(
  struct frame_descrs_state * state, void ** frametables, int ntables)
{
  void *frametable;
  caml_frametable_list ** previous;

  /* cannot release the domain lock here (e.g. custom block finaliser) */
  caml_plat_lock_blocking(&state->mutex);

  previous = &state->frametables;

  iter_list(state->frametables, current) {
  resume:
    for (int i = 0; i < ntables; i++) {
      if (current->frametable == frametables[i]) {
        *previous = current->next;
        current->next = state->zombies;
        state->zombies = current;
        ntables--;
        if (ntables == 0) goto release;
        current = *previous;
        frametable = frametables[i];
        frametables[i] = frametables[ntables];
        frametables[ntables] = frametable;
        goto resume;
      }
    }
    previous = &current->next;
  }

 release:
  caml_plat_unlock(&state->mutex);
}

void caml_unregister_frametables(void ** frametables, int ntables)
{
  remove_frame_descriptors(&frame_descrs, frametables, ntables);
}

void caml_register_frametable(void * frametables)
{
  caml_register_frametables(&frametables, 1);
}

void* caml_copy_and_register_frametable(void * frametable, int size)
{
  caml_copy_and_register_frametables(&frametable, &size, 1);
  return frametable;
}

void caml_unregister_frametable(void * frametables)
{
  caml_unregister_frametables(&frametables, 1);
}

/* Free the ->prev chain of [ft] (the versions retired before [ft] was
   published) iff a full MMTk collection has elapsed since [ft]'s retire tag --
   i.e. every reader has re-fetched and every ConcurrentImmix worker scan has
   finished. The common case (nothing pending: tag == No_need_to_free, or no
   cycle yet) takes no lock. Mirrors Dolan's free-in-caml_get_frame_descrs,
   keyed on mmtk_ocaml_gc_count. Serialised against concurrent reclaimers /
   unregister by frame_descrs.mutex. */
static void reclaim_retired(struct frametable_version *ft)
{
  if (atomic_load_acquire(&ft->free_prev_after_cycle)
        >= (uintnat) mmtk_ocaml_gc_count())
    return;                /* not yet a full cycle, or nothing pending */
  caml_plat_lock_blocking(&frame_descrs.mutex);
  /* Re-check under the lock (another thread may have reclaimed). */
  if (ft->prev != NULL
      && atomic_load_acquire(&ft->free_prev_after_cycle)
           < (uintnat) mmtk_ocaml_gc_count()) {
    struct frametable_version *chain = ft->prev;
    ft->prev = NULL;
    atomic_store_release(&ft->free_prev_after_cycle, No_need_to_free);
    free_frametable_versions(chain);
  }
  caml_plat_unlock(&frame_descrs.mutex);
}

caml_frame_descrs* caml_get_frame_descrs(void)
{
  struct frametable_version *ft = atomic_load_acquire(&current_frametable);
  CAMLassert(ft != NULL);
  reclaim_retired(ft);
  return &ft->table;
}

frame_descr* caml_find_frame_descr(caml_frame_descrs *fds, uintnat pc)
{
  frame_descr * d;
  uintnat h;

  h = Hash_retaddr(pc, fds->mask);
  while (1) {
    d = fds->descriptors[h];
    if (d == 0) return NULL; /* can happen if some code compiled without -g */
    if (d->retaddr == pc) break;
    h = (h+1) & fds->mask;
  }
  return d;
}
