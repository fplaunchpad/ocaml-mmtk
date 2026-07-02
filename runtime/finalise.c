/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*           Damien Doligez, projet Moscova, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 2000 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#include <string.h>

#include "caml/callback.h"
#include "caml/runtime_events.h"
#include "caml/fail.h"
#include "caml/finalise.h"
#include "caml/memory.h"
#include "caml/minor_gc.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/mmtk.h"
#include "caml/roots.h"
#include "caml/major_gc.h"

/* [size] is a number of elements for the [to_do.item] array */
static void alloc_todo (caml_domain_state* d, int size)
{
  struct final_todo *result =
    caml_stat_alloc_noexc (sizeof (struct final_todo) +
                           size * sizeof (struct final));
  struct caml_final_info *f = d->final_info;
  if (result == NULL) caml_fatal_error ("out of memory");
  result->next = NULL;
  result->size = size;
  if (f->todo_tail == NULL) {
    f->todo_head = result;
    f->todo_tail = result;
  } else {
    CAMLassert (f->todo_tail->next == NULL);
    f->todo_tail->next = result;
    f->todo_tail = result;
  }
}

/* -- M6: MMTk-native finaliser processing (experimental, MMTK_WEAK_REFS=1)
   ----- The stock caml_final_update_first/last run inside the stock major
   cycle, using the stock mark bits (is_unmarked) + caml_darken. Under MMTk we
   drive the same logic from the binding's Scanning::process_weak_refs using
   MMTk reachability (is_reachable), relocation (forward) and retention
   (retain). Differences from the stock path:
   - No minor/major split under MMTk, so we scan the WHOLE [0, young) range and
     leave both old==young afterwards (every survivor is treated as "old").
   - In MMTk weak-refs mode the root scan reports finaliser *functions* (and the
     run-queue) as roots but NOT the table *values* (do_final_val=0), so a value
     that became unreachable is detectable here. Surviving values are therefore
     not roots, so we must forward them ourselves (caml_mmtk_final_forward). See
     gc/mmtk/NOTES.md (M6 design). The callback typedefs are in caml/mmtk.h. */

/* Normalise a possibly-infix value to its block base (is_reachable/forward
   expect an object start). Finaliser values are rarely infix, but be safe. */
Caml_inline value caml_mmtk_final_base(value v)
{
  if (Is_block(v) && Tag_val(v) == Infix_tag) v -= Infix_offset_val(v);
  return v;
}

/* Move now-unreachable values of [final] to the run queue. For the `first` set
   (retain_value=1) the value is resurrected (retain) and passed to the
   finaliser; for the `last` set (retain_value=0) it is queued as Val_unit and
   may die. The table is compacted to its survivors. Returns the number of
   values retained (non-zero only for the `first` set -- that is "progress" for
   the caller's mark fixpoint, since resurrecting a value extends the live
   closure). */
static uintnat mmtk_final_update_one(caml_domain_state *d,
                                     struct finalisable *final,
                                     int retain_value,
                                     caml_mmtk_ephe_reachable_fn is_reachable,
                                     caml_mmtk_ephe_retain_fn retain, void *ctx)
{
  struct caml_final_info *fi = d->final_info;
  uintnat todo_count = 0;
  for (uintnat i = 0; i < final->young; i++) {
    if (!is_reachable(caml_mmtk_final_base(final->table[i].val)))
      ++todo_count;
  }
  if (todo_count == 0) return 0;

  caml_set_action_pending(d);
  alloc_todo(d, todo_count);
  uintnat j = 0, k = 0;
  for (uintnat i = 0; i < final->young; i++) {
    if (!is_reachable(caml_mmtk_final_base(final->table[i].val))) {
      fi->todo_tail->item[k] = final->table[i];
      if (!retain_value) {
        fi->todo_tail->item[k].val = Val_unit;
        fi->todo_tail->item[k].offset = 0;
      }
      k++;
    } else {
      final->table[j++] = final->table[i];
    }
  }
  final->young = j;
  final->old = j;                 /* every survivor is "old" under MMTk */
  fi->todo_tail->size = k;
  if (retain_value) {
    for (uintnat i = 0; i < k; i++) {
      /* Resurrect the value (and its closure) so it is valid when the finaliser
         runs; record its (possibly relocated) address. May already be live via
         another table entry -- retain is idempotent. */
      value v = fi->todo_tail->item[i].val;
      value base = caml_mmtk_final_base(v);
      fi->todo_tail->item[i].val = retain(ctx, base) + (v - base);
    }
  }
  return retain_value ? k : 0;
}

/* First-set update (Gc.finalise): retain dead values + queue them. Returns 1 if
   any value was retained (caller re-runs the mark fixpoint). Run only after the
   ephemeron mark fixpoint converges, so a value reachable via a live
   ephemeron's retained data is seen as live and not prematurely finalised. */
int caml_mmtk_final_update_first(uintptr_t domain_addr,
                                 caml_mmtk_ephe_reachable_fn is_reachable,
                                 caml_mmtk_ephe_retain_fn retain, void *ctx)
{
  caml_domain_state *d = (caml_domain_state *) domain_addr;
  if (d->final_info == NULL) return 0;
  return mmtk_final_update_one(d, &d->final_info->first, 1,
                               is_reachable, retain, ctx) > 0;
}

/* Cleanup pass (after the mark fixpoint): last-set update (Gc.finalise_last --
   queue dead values as Val_unit, no resurrection) then forward every surviving
   table value, since survivors are not roots in weak-refs mode. */
void caml_mmtk_final_cleanup(uintptr_t domain_addr,
                             caml_mmtk_ephe_reachable_fn is_reachable,
                             caml_mmtk_ephe_forward_fn forward,
                             caml_mmtk_ephe_retain_fn retain, void *ctx)
{
  caml_domain_state *d = (caml_domain_state *) domain_addr;
  struct caml_final_info *fi = d->final_info;
  if (fi == NULL) return;

  mmtk_final_update_one(d, &fi->last, 0, is_reachable, retain, ctx);

  struct finalisable *sets[2] = { &fi->first, &fi->last };
  for (int s = 0; s < 2; s++) {
    struct finalisable *final = sets[s];
    for (uintnat i = 0; i < final->young; i++) {
      value v = final->table[i].val;
      value base = caml_mmtk_final_base(v);
      value fwd = forward(base);
      if (fwd != base) final->table[i].val = fwd + (v - base);
    }
  }
}

/* Call the finalisation functions for the finalising set.
   Note that this function must be reentrant.
*/
caml_result caml_final_do_calls_res(void)
{
  struct final f;
  caml_result res;
  struct caml_final_info *fi = Caml_state->final_info;

  if (fi->running_finalisation_function) return Result_unit;
  /* Drain MMTk's custom-block finalizer queue (Custom_operations.finalize) -- a
     no-op unless MMTK_WEAK_REFS is on. Independent of the OCaml finaliser table
     below. */
  caml_mmtk_run_custom_finalizers();
  if (fi->todo_head != NULL) {
    call_timing_hook(&caml_finalise_begin_hook);
    CAML_GC_MESSAGE(FINALIZE,
                    "Calling finalisation functions.\n");
    while (1) {
      while (fi->todo_head != NULL && fi->todo_head->size == 0) {
        struct final_todo *next_head = fi->todo_head->next;
        caml_stat_free (fi->todo_head);
        fi->todo_head = next_head;
        if (fi->todo_head == NULL) fi->todo_tail = NULL;
      }
      if (fi->todo_head == NULL) break;
      CAMLassert (fi->todo_head->size > 0);
      --fi->todo_head->size;
      f = fi->todo_head->item[fi->todo_head->size];
      fi->running_finalisation_function = 1;
      res = caml_callback_res (f.fun, f.val + f.offset);
      fi->running_finalisation_function = 0;
      if (caml_result_is_exception(res)) return res;
    }
    CAML_GC_MESSAGE(FINALIZE,
                    "Done calling finalisation functions.\n");
    call_timing_hook(&caml_finalise_end_hook);
  }
  return Result_unit;
}

/* Call a scanning_action [f] on [x]. */
#define Call_action(f,d,x) (*(f)) ((d), (x), &(x))

/* Called my major_gc for marking roots */
void caml_final_do_roots
  (scanning_action act, scanning_action_flags fflags, void* fdata,
   caml_domain_state* d, int do_val)
{
  struct caml_final_info *f = d->final_info;

  CAMLassert (f->first.old <= f->first.young);
  for (uintnat i = 0; i < f->first.young; i++) {
    Call_action (act, fdata, f->first.table[i].fun);
    if (do_val)
      Call_action (act, fdata, f->first.table[i].val);
  }

  CAMLassert (f->last.old <= f->last.young);
  for (uintnat i = 0; i < f->last.young; i++) {
    Call_action (act, fdata, f->last.table[i].fun);
    if (do_val)
      Call_action (act, fdata, f->last.table[i].val);
  }

  for (struct final_todo *todo = f->todo_head;
       todo != NULL;
       todo = todo->next) {
    for (uintnat i = 0; i < todo->size; i++) {
      Call_action (act, fdata, todo->item[i].fun);
      Call_action (act, fdata, todo->item[i].val);
    }
  }
}

/* Called by minor gc for marking roots */
void caml_final_do_young_roots
  (scanning_action act, scanning_action_flags fflags, void* fdata,
   caml_domain_state* d, int do_last_val)
{
  struct caml_final_info *f = d->final_info;

  CAMLassert (f->first.old <= f->first.young);
  for (uintnat i = f->first.old; i < f->first.young; i++) {
    Call_action (act, fdata, f->first.table[i].fun);
    Call_action (act, fdata, f->first.table[i].val);
  }

  CAMLassert (f->last.old <= f->last.young);
  for (uintnat i = f->last.old; i < f->last.young; i++) {
    Call_action (act, fdata, f->last.table[i].fun);
    if (do_last_val)
      Call_action (act, fdata, f->last.table[i].val);
  }
}

static void generic_final_minor_update
  (caml_domain_state* d, struct finalisable * final)
{
  uintnat todo_count = 0;
  struct caml_final_info *fi = d->final_info;

  CAMLassert (final->old <= final->young);
  for (uintnat i = final->old; i < final->young; i++){
    CAMLassert (Is_block (final->table[i].val));
    if (Is_young(final->table[i].val) &&
        caml_get_header_val(final->table[i].val) != 0){
      ++ todo_count;
    }
  }

  /** invariant:
      - final->old <= j <= i /\ final->old <= k <= i /\ 0 <= k <= todo_count
      - i : index in final_table, before i all the values are alive
            or the finalizer have been copied in to_do_tl.
      - j : index in final_table, before j all the values are alive,
            next available slot.
      - k : index in to_do_tl, next available slot.
  */
  if (todo_count > 0) {
    uintnat i, j, k;
    caml_set_action_pending(d);
    alloc_todo (d, todo_count);
    k = 0;
    j = final->old;
    for (i = final->old; i < final->young; i++) {
      CAMLassert (Is_block (final->table[i].val));
      CAMLassert (Tag_val (final->table[i].val) != Forward_tag);
      if (Is_young(final->table[i].val) &&
          caml_get_header_val(final->table[i].val) != 0) {
        /** dead */
        fi->todo_tail->item[k] = final->table[i];
        /* The finalisation function is called with unit not with the value */
        fi->todo_tail->item[k].val = Val_unit;
        fi->todo_tail->item[k].offset = 0;
        k++;
      } else {
        /** alive */
        final->table[j++] = final->table[i];
      }
    }
    CAMLassert (i == final->young);
    CAMLassert (k == todo_count);
    final->young = j;
    fi->todo_tail->size = todo_count;
  }

  /** update the minor value to the copied major value */
  for (uintnat i = final->old; i < final->young; i++) {
    CAMLassert (Is_block (final->table[i].val));
    if (Is_young(final->table[i].val)) {
      CAMLassert (caml_get_header_val(final->table[i].val) == 0);
      final->table[i].val = Field(final->table[i].val, 0);
    }
  }
}

void caml_final_update_last_minor (caml_domain_state* d)
{
  generic_final_minor_update(d, &d->final_info->last);
}

void caml_final_empty_young (caml_domain_state* d)
{
  struct caml_final_info *f = d->final_info;
  f->first.old = f->first.young;
  f->last.old = f->last.young;
}

void caml_final_merge_finalisable
  (struct finalisable *source, struct finalisable *target)
{
  uintnat new_size;

  CAMLassert (target->old <= target->young);
  /* to merge the source structure, all its values are in the major heap */
  CAMLassert (source->old == source->young);
  if (target->young + source->young >= target->size) {
    new_size = 2 * (target->young + source->young);
    if (target->table == NULL) {
      target->table = caml_stat_alloc (new_size * sizeof (struct final));
      CAMLassert (target->old == 0);
      CAMLassert (target->young == 0);
      target->size = new_size;
    } else {
      target->table = caml_stat_resize (target->table,
                                       new_size * sizeof (struct final));
      target->size = new_size;
    }
  }
  /* all values from the source are old, we will prepend them
     into the old area of the target */
  memmove(target->table + source->young, target->table,
          target->young * sizeof (struct final));
  memcpy(target->table, source->table,
         source->young * sizeof (struct final));
  /* adjust indices for the prepended values from the source */
  target->old += source->young;
  target->young += source->young;

#ifdef DEBUG
  {
    /** check target is well formed on the values */
    int i;
    for (i = 0; i < target->old; i++) {
      CAMLassert (target->table[i].val); /* no null ptrs */
      CAMLassert (Is_block(target->table[i].val));
      CAMLassert (!Is_young(target->table[i].val));
    };
    for (; i < target->young; i++) {
      CAMLassert (target->table[i].val); /* no null ptrs */
      CAMLassert (Is_block(target->table[i].val));
    }
  }
#endif
}

static void generic_final_register (struct finalisable *final, value f, value v)
{
  uintnat new_size;

  if (!Is_block(v) || Tag_val(v) == Lazy_tag
#ifdef FLAT_FLOAT_ARRAY
      || Tag_val(v) == Double_tag
#endif
      || Tag_val(v) == Forcing_tag
      || Tag_val(v) == Forward_tag) {
    caml_invalid_argument ("Gc.finalise");
  }
  CAMLassert (final->old <= final->young);

  if (final->young >= final->size) {
    if (final->table == NULL) {
      new_size = 30;
      final->table = caml_stat_alloc (new_size * sizeof (struct final));
      CAMLassert (final->old == 0);
      CAMLassert (final->young == 0);
      final->size = new_size;
    } else {
      new_size = final->size * 2;
      final->table = caml_stat_resize (final->table,
                                       new_size * sizeof (struct final));
      final->size = new_size;
    }
  }
  CAMLassert (final->young < final->size);
  final->table[final->young].fun = f;
  if (Tag_val(v) == Infix_tag) {
    final->table[final->young].offset = Infix_offset_val (v);
    final->table[final->young].val = v - Infix_offset_val (v);
  } else {
    final->table[final->young].offset = 0;
    final->table[final->young].val = v;
  }
  ++ final->young;
}


CAMLprim value caml_final_register (value f, value v)
{
  generic_final_register (&Caml_state->final_info->first, f, v);
  return Val_unit;
}

CAMLprim value caml_final_register_called_without_value (value f, value v)
{
  generic_final_register (&Caml_state->final_info->last, f, v);
  return Val_unit;
}

CAMLprim value caml_final_release (value unit)
{
  Caml_state->final_info->running_finalisation_function = 0;
  return Val_unit;
}

struct caml_final_info* caml_alloc_final_info (void)
{
  struct caml_final_info* f =
    caml_stat_alloc_noexc (sizeof(struct caml_final_info));
  if(f != NULL)
    memset (f, 0, sizeof(struct caml_final_info));
  return f;
}
