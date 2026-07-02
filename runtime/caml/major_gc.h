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

#ifndef CAML_MAJOR_GC_H
#define CAML_MAJOR_GC_H

#ifdef CAML_INTERNALS

#include <stdbool.h>
#include "mlvalues.h"

typedef enum {
  Phase_sweep_main,
  Phase_sweep_and_mark_main,
  Phase_mark_final,
  Phase_sweep_ephe
} gc_phase_t;

extern gc_phase_t caml_gc_phase;

Caml_inline int caml_marking_started(void)
{
  return caml_gc_phase != Phase_sweep_main;
}

extern atomic_uintnat caml_gc_mark_phase_requested;
/* auto-triggered slice from within the GC */
#define AUTO_TRIGGERED_MAJOR_SLICE -1
/* external triggered slice, but GC will compute the amount of work */
#define GC_CALCULATE_MAJOR_SLICE 0
void caml_major_collection_slice (intnat);
void caml_finish_sweeping(void);
void caml_finish_marking (void);
int caml_init_major_gc(caml_domain_state*);
void caml_teardown_major_gc(void);
void caml_darken(void*, value, volatile value* ignored);
void caml_darken_cont(value);
void caml_finish_major_cycle(int force_compaction);
void caml_init_major_pacing (void);
void caml_orphan_ephemerons(caml_domain_state*);
void caml_orphan_finalisers(caml_domain_state*);

/* This variable is only written with the world stopped,
   so it need not be atomic */
extern uintnat caml_major_cycles_completed;

/* -- Mark-status colours -------------------------------------------------
   These header-colour helpers and the global colour-cycle state used to live
   in shared_heap.h (deleted under always-on MMTk). They remain live because
   weak/ephemeron/finaliser processing still reads mark bits, so they have
   been relocated here. */

/* always readable by all threads
   written only by a single thread during STW periods */
typedef uintnat status;
struct global_heap_state {
  status MARKED, UNMARKED, GARBAGE;
};
extern struct global_heap_state caml_global_heap_state;

/* CR mshinwell: ensure this matches [Emitaux] */
enum {NOT_MARKABLE = 3 << HEADER_COLOR_SHIFT};

Caml_inline int Has_status_hd(header_t hd, status s) {
  return Color_hd(hd) == s;
}

Caml_inline int Has_status_val(value v, status s) {
  return Has_status_hd(Hd_val(v), s);
}

Caml_inline int is_unmarked(value v) {
  return Has_status_val(v, caml_global_heap_state.UNMARKED);
}

Caml_inline int is_marked(value v) {
  return Has_status_val(v, caml_global_heap_state.MARKED);
}

Caml_inline int is_not_markable(value v) {
  return Has_status_val(v, NOT_MARKABLE);
}

#endif /* CAML_INTERNALS */

#endif /* CAML_MAJOR_GC_H */
