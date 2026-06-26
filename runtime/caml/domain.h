/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*      KC Sivaramakrishnan, Indian Institute of Technology, Madras       */
/*                   Stephen Dolan, University of Cambridge               */
/*                                                                        */
/*   Copyright 2019 Indian Institute of Technology, Madras                */
/*   Copyright 2019 University of Cambridge                               */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#ifndef CAML_DOMAIN_H
#define CAML_DOMAIN_H

#ifdef CAML_INTERNALS

#include <stdbool.h>

#include "camlatomic.h"
#include "config.h"
#include "mlvalues.h"
#include "domain_state.h"

/* See caml_c_thread_register_in_domain_index */
CAMLextern bool caml_thread_running_on_expected_domain(uintnat);
CAMLextern void caml_thread_record_domain_id(uintnat);

#ifdef ARCH_SIXTYFOUR
#define Max_domains_def 128
#else
#define Max_domains_def 16
#endif

/* Upper limit for the number of domains. Chosen to be arbitrarily large. Used
 * for sanity checking [max_domains] value in OCAMLRUNPARAM. */
#define Max_domains_max 4096

/* is the minor heap full or an external interrupt has been triggered */
Caml_inline int caml_check_gc_interrupt(caml_domain_state * dom_st)
{
  CAMLalloc_point_here;
  uintnat young_limit = atomic_load_relaxed(&dom_st->young_limit);
  if ((uintnat)dom_st->young_ptr < young_limit) {
    /* Synchronise for the case when [young_limit] was used to interrupt
       us. */
    atomic_thread_fence(memory_order_acquire);
    return 1;
  }
  return 0;
}

#define Caml_check_gc_interrupt(dom_st)           \
  (CAMLunlikely(caml_check_gc_interrupt(dom_st)))

asize_t caml_norm_minor_heap_size (intnat);
void caml_update_minor_heap_max(uintnat minor_heap_wsz);

/* is there a STW interrupt queued that needs servicing */
int caml_incoming_interrupts_queued(void);

void caml_poll_gc_work(void);
void caml_handle_gc_interrupt(void);
void caml_process_external_interrupt(void);
void caml_handle_incoming_interrupts(void);

CAMLextern void caml_interrupt_self(void);
void caml_interrupt_all_signal_safe(void);
void caml_reset_young_limit(caml_domain_state *);
void caml_update_young_limit_after_c_call(caml_domain_state *);

CAMLextern void caml_reset_domain_lock(void);
CAMLextern int caml_bt_is_in_blocking_section(void);
CAMLextern int caml_bt_is_self(void);
CAMLextern intnat caml_domain_is_multicore (void);
CAMLextern void caml_bt_enter_ocaml(void);
CAMLextern void caml_bt_exit_ocaml(void);
CAMLextern void caml_acquire_domain_lock(void);
CAMLextern void caml_release_domain_lock(void);

/* These hooks are not modified after other domains are spawned. */
CAMLextern void (*caml_atfork_hook)(void);
CAMLextern value (*caml_domain_initialize_hook_exn)(void);
CAMLextern void (*caml_domain_stop_hook)(void);
CAMLextern void (*caml_domain_external_interrupt_hook)(void);

CAMLextern void caml_init_domains(uintnat max_domains, uintnat minor_heap_wsz);
CAMLextern void caml_init_domain_self(int);

CAMLextern uintnat caml_minor_heap_max_wsz;

CAMLextern atomic_uintnat caml_num_domains_running;

/*  Given domain unique id, return the index of the domain.
 *  If the domain unique id is unknown, return -1.
*/
CAMLextern intnat caml_find_index_of_running_domain(uintnat dom_unique_id);

/* When [caml_domain_alone()] is true, there is a single domain
   running. In particular, if the test passes while holding the domain
   lock, then we know that no other domain is running concurrently,
   and we can use fast paths with fewer synchronization operations.

      // if you hold the domain lock:
      if (caml_domain_alone()) {
        // sequential fast path
        ...
      } else {
        // slower concurrent version
        ...
      }
*/
Caml_inline intnat caml_domain_alone(void)
{
  return atomic_load_acquire(&caml_num_domains_running) == 1;
}

/* The index of the current domain. It is an integer unique among
   currently-running domains, in the interval [0; N-1] where N is the
   peak number of domains running simultaneously so far. The index of
   a terminated domain may be reused for a new domain.

   This function requires the domain lock to be held.
*/
Caml_inline int caml_domain_index(void)
{
  return Caml_state->id;
}

#ifdef DEBUG
int caml_domain_is_in_stw(void);
#endif

int caml_domain_terminating(caml_domain_state *);
int caml_domain_is_terminating(void);
void caml_domain_terminate(bool last);

/*
 * Termination helpers.
 */

/* Force all other domains to stop their operation. */
void caml_stop_all_domains(void);

/* Try and release all synchronisation resources set up by
   caml_init_domains(). Returns whether all resources could be released. */
bool caml_free_domains(void);

#endif /* CAML_INTERNALS */

#endif /* CAML_DOMAIN_H */
