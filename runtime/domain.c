/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*      KC Sivaramakrishnan, Indian Institute of Technology, Madras       */
/*                 Stephen Dolan, University of Cambridge                 */
/*                   Tom Kelly, OCaml Labs Consultancy                    */
/*                                                                        */
/*   Copyright 2021 OCaml Labs Consultancy Ltd                            */
/*   Copyright 2019 Indian Institute of Technology, Madras                */
/*   Copyright 2019 University of Cambridge                               */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

#define CAML_INTERNALS

#define _GNU_SOURCE  /* For sched.h CPU_ZERO(3) and CPU_COUNT(3) */
#include "caml/config.h"
#include <stdbool.h>
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <string.h>
#include <assert.h>
#ifdef HAS_GNU_GETAFFINITY_NP
#include <sched.h>
#ifdef HAS_PTHREAD_NP_H
#include <pthread_np.h>
#endif
#endif
#ifdef HAS_BSD_GETAFFINITY_NP
#include <pthread_np.h>
#include <sys/cpuset.h>
typedef cpuset_t cpu_set_t;
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sysinfoapi.h>
#include <process.h>
#else
#include <pthread.h>
#endif
#include "caml/alloc.h"
#include "caml/backtrace.h"
#include "caml/backtrace_prim.h"
#include "caml/callback.h"
#include "caml/debugger.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/mmtk.h"
#include "caml/runtime_events.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/finalise.h"
#include "caml/gc_ctrl.h"
#include "caml/globroots.h"
#include "caml/intext.h"
#include "caml/major_gc.h"
#include "caml/minor_gc.h"
#include "caml/memprof.h"
#include "caml/misc.h"
#include "caml/memory.h"
#include "caml/osdeps.h"
#include "caml/platform.h"
#include "caml/signals.h"
#include "caml/startup.h"
#include "caml/startup_aux.h"
#include "caml/sync.h"
#include "caml/weak.h"

/* Check that the domain_state structure was laid out without padding,
   since the runtime assumes this in computing offsets */
static_assert(
    offsetof(caml_domain_state, LAST_DOMAIN_STATE_MEMBER) ==
    (Domain_state_num_fields - 1) * 8,
    "");

/* The runtime can run stop-the-world (STW) sections, during which all
   active domains run the same callback in parallel (with a barrier
   mechanism to synchronize within the callback).

   Stop-the-world sections are used to handle duties such as:
    - minor GC
    - major GC phase changes

   Code within the STW callback can have the guarantee that no mutator
   code runs in parallel -- precisely, the guarantee holds only for
   code that is followed by a barrier. Furthermore, new domains being
   spawned are blocked from running any mutator code while a STW
   section is in progress, and terminating domains cannot stop until
   they have participated to all STW sections currently in progress.

   To provide these guarantees:
    - Domains must register as STW participants before running any
      mutator code.
    - STW sections must not trigger other callbacks into mutator code
      (eg. finalisers or signal handlers).

   See the comments on [caml_try_run_on_all_domains_with_spin_work]
   below for more details on the synchronization mechanisms involved.
*/

/* Under always-on MMTk, the all-domains rendezvous is MMTk's own
   stop_all_mutators (the binding's RUNNING set is the authoritative
   "who must stop"); OCaml's own all-domains STW was retired in the
   "one STW" excision. A domain that releases its domain lock (blocking
   C section, spawn idle-wait, park) is removed from the RUNNING set, so
   the collector does not await it. There is therefore no longer any
   work for a per-domain backup thread to do (it only ever serviced
   OCaml's own STW interrupts), and the whole backup-thread + interrupt
   handoff machinery has been removed. See gc/mmtk/NOTES.md (#20). */

/* control of inter-domain signalling and the spawn handshake */
struct interruptor {
  /* The outermost atomic is for synchronization with
     caml_interrupt_all_signal_safe. The innermost atomic is also for
     cross-domain communication.*/
  _Atomic(atomic_uintnat *) interrupt_word;
  caml_plat_mutex lock;
  caml_plat_cond cond;

  int running;
  int terminating;
  /* unlike the domain ID, this ID number is not reused */
  /* Synchronised with [caml_find_index_of_running_domain] */
  atomic_uintnat unique_id;
};

struct dom_internal {
  /* readonly fields, initialised and never modified */
  int id;
  caml_plat_thread tid;
  caml_domain_state* state;
  struct interruptor interruptor;

  caml_plat_mutex domain_lock;
  bool domain_canceled;
};
typedef struct dom_internal dom_internal;

static CAMLthread_local dom_internal* domain_self;

static caml_plat_mutex all_domains_lock = CAML_PLAT_MUTEX_INITIALIZER;
static caml_plat_cond all_domains_cond = CAML_PLAT_COND_INITIALIZER;
static dom_internal* all_domains;
static atomic_intnat domains_exiting = 0;

CAMLexport atomic_uintnat caml_num_domains_running = 0;

/* Sticky "this process has spawned at least one extra domain" latch. Set the
   first time a domain is spawned and never cleared, even after the spawned
   domain terminates and caml_num_domains_running returns to 1. It backs
   caml_domain_is_multicore (used by Unix.fork / afl to refuse running after
   any domain has been spawned). It replaces the old per-domain backup-thread
   "running" flag, which stock OCaml latched on for the same purpose. */
static atomic_uintnat caml_domains_ever_spawned = 0;

/*
  This structure is protected by all_domains_lock.

  Three regions in the [domains]  array correspond to three states for domains:
   - at indices [0, active_domains) are the 'active' domains,
     which participate in STW sections.
   - [active_domains, parked_domains) are the 'parked' domains,
     in the process of spawning.
   - [parked_domains, caml_params->max_domains) are the 'stopped' domains,
     which are currently unused by the runtime.
 */
static struct {
  int active_domains;
  int parked_domains;
  dom_internal** domains;
} stw_domains = {
  0,
  0,
  NULL
};


static void check_domain_limit(int idx) {
  CAMLassert(0 <= idx && idx <= caml_params->max_domains);
}
static void check_stw_domains(void) {
  check_domain_limit(stw_domains.active_domains);
  check_domain_limit(stw_domains.parked_domains);
  CAMLassert(stw_domains.active_domains
             <= stw_domains.parked_domains);
#ifdef DEBUG
  /* Check here the invariant for early-exit in
     [caml_interrupt_all_signal_safe], because the latter must be
     async-signal-safe and one cannot CAMLassert inside it. */
  bool prev_has_interrupt_word = true;
  for (int i = 0; i < caml_params->max_domains; i++) {
    bool has_interrupt_word = all_domains[i].interruptor.interrupt_word != NULL;
    if (i < stw_domains.active_domains) CAMLassert(has_interrupt_word);
    if (!prev_has_interrupt_word) CAMLassert(!has_interrupt_word);
    prev_has_interrupt_word = has_interrupt_word;
  }
#endif
}

static int find_stw_domain(int start, int end, dom_internal *dom) {
  check_domain_limit(start);
  check_domain_limit(end);
  for (int i = start; i < end; i++)
  {
    if (stw_domains.domains[i] == dom)
      return i;
  }
  caml_fatal_error("find_stw_domain");
}
static int find_active_domain(dom_internal *dom) {
  int start = 0;
  int end = stw_domains.active_domains;
  return find_stw_domain(start, end, dom);
}
static int find_parked_domain(dom_internal *dom) {
  int start = stw_domains.active_domains;
  int end = stw_domains.parked_domains;
  return find_stw_domain(start, end, dom);
}

static void swap_stw_domains(int idx1, int idx2) {
  if (idx1 == idx2) return;
  dom_internal *dom1 = stw_domains.domains[idx1];
  dom_internal *dom2 = stw_domains.domains[idx2];
  stw_domains.domains[idx1] = dom2;
  stw_domains.domains[idx2] = dom1;
}


/* One needs to hold [all_domains_lock] to call any of the
   [stw_domains] transition functions that follow. */

static dom_internal* park_next_stopped_domain(void) {
  if (stw_domains.parked_domains == caml_params->max_domains)
    return NULL;

  dom_internal *dom = stw_domains.domains[stw_domains.parked_domains];
  stw_domains.parked_domains++;
  check_stw_domains();
  return dom;
}

static void stop_parked_domain(dom_internal *dom) {
  int idx = find_parked_domain(dom);
  stw_domains.parked_domains--;
  swap_stw_domains(idx, stw_domains.parked_domains);
  check_stw_domains();
}

static void activate_parked_domain(dom_internal *dom)
{
  int idx = find_parked_domain(dom);
  swap_stw_domains(stw_domains.active_domains, idx);
  stw_domains.active_domains++;
  check_stw_domains();
}

static void stop_active_domain(dom_internal* dom) {
  int idx = find_active_domain(dom);
  stw_domains.active_domains--;
  swap_stw_domains(idx, stw_domains.active_domains);

  idx = stw_domains.active_domains;
  stw_domains.parked_domains--;
  swap_stw_domains(idx, stw_domains.parked_domains);

  check_stw_domains();
}

CAMLexport CAMLthread_local caml_domain_state* caml_state;

#ifndef HAS_FULL_THREAD_VARIABLES
/* Export a getter for caml_state, to be used in DLLs */
CAMLexport caml_domain_state* caml_get_domain_state(void)
{
  return caml_state;
}
#endif

static CAMLthread_local uintnat previous_domain_id = -1;

CAMLexport
bool caml_thread_running_on_expected_domain(uintnat expected_unique_id)
{
  return (Caml_state->unique_id == expected_unique_id &&
          (previous_domain_id == -1 ||
            previous_domain_id == expected_unique_id));
}

CAMLexport void caml_thread_record_domain_id(uintnat domain_id)
{
  previous_domain_id = domain_id;
}

Caml_inline void interrupt_domain(struct interruptor* s)
{
  atomic_uintnat * interrupt_word = atomic_load_relaxed(&s->interrupt_word);
  atomic_store_release(interrupt_word, CAML_UINTNAT_MAX);
}

Caml_inline void interrupt_domain_local(caml_domain_state* dom_st)
{
  atomic_store_relaxed(&dom_st->young_limit, CAML_UINTNAT_MAX);
}

asize_t caml_norm_minor_heap_size (intnat wsize)
{
  asize_t bs;
  if (wsize < Minor_heap_min) wsize = Minor_heap_min;
  bs = caml_mem_round_up_pages(Bsize_wsize (wsize));

  return Wsize_bsize(bs);
}

/* Note [minor heap layout] (always-on MMTk):

  The stock per-domain minor heap and the global minor-heaps address-space
  reservation are GONE. Under always-on MMTk the "minor heap" is an MMTk TLAB
  block (an Immix nursery block) that [caml_mmtk_refill_tlab] hands the domain;
  [young_start/young_end/young_ptr] alias that MMTk-owned block. Bytecode
  allocates straight into MMTk. Nothing is allocated in a reserved
  [caml_minor_heaps_start, caml_minor_heaps_end) range -- that range and its
  per-domain segments no longer exist -- so [Is_young] is always false
  (address_class.h) and the reservation machinery
  (reserve/unreserve/resize_minor_heaps_reservation, the per-domain
  minor_heap_reservation_{start,end} fields, the [caml_mem_map]/[caml_mem_unmap]
  of the reservation) has been retired.

  [caml_minor_heap_max_wsz] survives as a plain scalar cap on the per-domain
  minor-heap word size requestable via [Gc.set] (it sizes no arena/table; it is
  only compared against and asserted in gc_ctrl.c). [caml_update_minor_heap_max]
  raises it with a plain store; no memory is reserved and no STW is taken.
*/

/* Size of the (nominal) minor heap, per domain -- a scalar cap; see above. */
uintnat caml_minor_heap_max_wsz;

Caml_inline void check_minor_heap(void) {
  caml_domain_state* domain_state = Caml_state;

  caml_gc_log(
      "young_start: %p,"
      " young_end: %p,"
      " minor_heap_wsz: %" CAML_PRIuSZT " words",
      domain_state->young_start,
      domain_state->young_end,
      domain_state->minor_heap_wsz);

  /* Under always-on MMTk the "minor heap" is an MMTk TLAB block (Immix
     nursery), not the stock per-domain minor arena. caml_mmtk_refill_tlab
     repoints young_start/young_end/young_ptr at the MMTk-owned block, and after
     a minor collection young_ptr is reset to young_start (not young_end). So
     the stock "minor heap fully drained" invariant (young_ptr == young_end)
     does not hold here, and there is no longer any reservation to bounds-check
     against. The stock asserts are gone; the caml_gc_log above is kept for
     diagnostics. (DEBUG-only; release and all-plans behaviour unchanged.) */
}


/* The stock per-domain minor-heap arena AND the minor-heaps address-space
   reservation are gone under always-on MMTk: the minor heap is an MMTk TLAB
   block set up by caml_mmtk_refill_tlab (called from caml_mmtk_domain_init), so
   allocate/free/reallocate_minor_heap_arena and the
   reserve/unreserve/resize_minor_heaps_reservation machinery are removed.
   Is_young is now a folded constant false (address_class.h). */

/* Minor heap max-size cap: raise it (no memory is reserved, no STW). */

void caml_update_minor_heap_max(uintnat requested_wsz) {
  caml_gc_log("Changing heap_max_wsz from %" CAML_PRIuNAT
              " to %" CAML_PRIuNAT ".",
              caml_minor_heap_max_wsz, requested_wsz);
  /* Under always-on MMTk [caml_minor_heap_max_wsz] is a plain scalar cap on the
     Gc.set path: it sizes no arena and no address-space reservation. The stock
     all-domains STW (stw_resize_minor_heaps_reservation) only emptied the minor
     heaps (GC-dead: MMTk's TLAB owns the nursery) and stored the cap under the
     final-domain barrier, so the lone observable effect is this store. No STW
     needed; the former while-loop only retried on losing the STW leader race.
     */
  if (requested_wsz > caml_minor_heap_max_wsz) {
    caml_minor_heap_max_wsz = requested_wsz;
  }
  check_minor_heap();
}


/* Domain creation. */

/* This variable is owned by [all_domains_lock]. */
static uintnat next_domain_unique_id = 0;

/* Precondition: you must own [all_domains_lock].

   Specification:
   - returns 0 on the first call
     (we want the main domain to have unique_id 0)
   - returns distinct ids unless there is an overflow
   - never returns 0 again, even in presence of overflow.
 */
static uintnat fresh_domain_unique_id(void) {
    uintnat next = next_domain_unique_id++;

    /* On 32-bit systems, there is a risk of wraparound of the unique
       id counter. We have decided to let that happen and live with
       it, but we still ensure that id 0 is not reused, to avoid
       having new domains believe that they are the main domain. */
    if (next_domain_unique_id == 0)
      next_domain_unique_id++;

    return next;
}

/* must be run on the domain's thread */
static void domain_create(uintnat initial_minor_heap_wsize,
                          caml_domain_state *parent)
{
  dom_internal* d = 0;
  caml_domain_state* domain_state;
  struct interruptor* s;
  uintnat stack_wsize = caml_get_init_stack_wsize();

  CAMLassert (domain_self == 0);

  /* take the all_domains_lock so that we can alter the STW participant
     set atomically */
  caml_plat_lock_blocking(&all_domains_lock);

  /* No all-domains STW under always-on MMTk, so no in-progress STW section to
     wait out here (excise Phase 3c): with [all_domains_lock] held we can alter
     the STW participant set and park a slot directly. */
  d = park_next_stopped_domain();

  if (d == NULL)
    goto domain_parking_failure;

  s = &d->interruptor;
  CAMLassert(!s->running);

  /* If the chosen domain slot has not been previously used, allocate a fresh
     domain state. Otherwise, reuse it.

     Reusing the slot ensures that the GC stats are not lost:
     - Heap stats are moved to the free list on domain termination,
       so we don't reuse those stats (caml_init_shared_heap will reset them)
     - But currently there is no orphaning process for allocation stats,
       we just reuse the previous stats from the previous domain
       with the same index.
  */
  if (d->state == NULL) {
    /* FIXME: Never freed. Not clear when to. */
    domain_state = (caml_domain_state*)
      caml_stat_calloc_noexc(1, sizeof(caml_domain_state));
    if (domain_state == NULL)
      goto domain_state_init_failure;
    d->state = domain_state;
  } else {
    domain_state = d->state;
  }

  /* Note: until we take d->domain_lock, the domain_state may still be
   * shared with a domain which is terminating (see
   * caml_domain_terminate). */

  caml_plat_lock_blocking(&d->domain_lock);

  /* This is the first thing we do after acquiring the domain lock,
     so that [caml_domain_alone()] returns accurate result even
     during domain initialization. */
  atomic_fetch_add(&caml_num_domains_running, 1);

  /* Set domain_self if we have successfully allocated the
   * caml_domain_state. Otherwise domain_self will be NULL and it's up
   * to the caller to deal with that. */

  domain_self = d;
  caml_state = domain_state;

  domain_state->young_limit = 0;
  /* Synchronized with [caml_interrupt_all_signal_safe], so that the
     initializing write of young_limit happens before any
     interrupt. */
  atomic_store_explicit(&s->interrupt_word, &domain_state->young_limit,
                        memory_order_release);

  domain_state->id = d->id;

  /* Tell memprof system about the new domain before either (a) new
   * domain can allocate anything or (b) parent domain can go away. */
  CAMLassert(domain_state->memprof == NULL);
  caml_memprof_new_domain(parent, domain_state);
  if (!domain_state->memprof) {
    goto init_memprof_failure;
  }

  domain_state->extra_heap_resources = 0.0;
  domain_state->extra_heap_resources_minor = 0.0;

  domain_state->dependent_size = 0;
  domain_state->dependent_allocated = 0;

  domain_state->sweep_work_done_between_slices = 0;
  domain_state->mark_work_done_between_slices = 0;

  /* the minor heap arena will be initialized by
     [allocate_minor_heap_arena] below. */
  domain_state->minor_heap_wsz = 0;
  domain_state->young_start = NULL;
  domain_state->young_end = NULL;
  domain_state->young_ptr = NULL;
  domain_state->young_trigger = NULL;

  domain_state->minor_tables = caml_alloc_minor_tables();
  if(domain_state->minor_tables == NULL) {
    goto alloc_minor_tables_failure;
  }

  /* Always-on MMTk: the stock shared heap is gone (MMTk does all allocation).
     The shared_heap field is kept (it is generated from domain_state.tbl and
     removing it would shift struct offsets baked into the native code
     generator), but it is left NULL and never dereferenced. */
  d->state->shared_heap = NULL;

  if (caml_init_major_gc(domain_state) < 0) {
    goto init_major_gc_failure;
  }

  /* No stock minor-heap arena under always-on MMTk: the minor heap is an MMTk
     TLAB block, set up by caml_mmtk_domain_init (below) via
     caml_mmtk_refill_tlab, which points young_start/end/ptr at the MMTk-owned
     block. We only record the nominal minor-heap size (reported by
     Gc.stat/Gc.get + used to size the minor tables); young_* stay NULL until
     the refill, and nothing allocates an OCaml value in between (the setup
     below is all caml_stat/mmap, no minor-heap allocation). */
  domain_state->minor_heap_wsz =
    caml_norm_minor_heap_size(initial_minor_heap_wsize);

  domain_state->dls_root = Val_unit;
  caml_register_generational_global_root(&domain_state->dls_root);

  domain_state->stack_cache = caml_alloc_stack_cache();
  if(domain_state->stack_cache == NULL) {
    goto create_stack_cache_failure;
  }

  domain_state->extern_state = NULL;

  domain_state->intern_state = NULL;

  domain_state->current_stack =
      caml_alloc_main_stack(stack_wsize);
  if(domain_state->current_stack == NULL) {
    goto alloc_main_stack_failure;
  }

  /* No remaining failure cases: domain creation is going to succeed,
   * so we can update globally-visible state without needing to unwind
   * it. */
  s->unique_id = fresh_domain_unique_id();
  domain_state->unique_id = s->unique_id;
  s->running = 1;

  domain_state->c_stack = NULL;
  domain_state->exn_handler = NULL;

  domain_state->action_pending = 0;

  domain_state->gc_regs_buckets = NULL;
  domain_state->gc_regs = NULL;

  domain_state->allocated_words = 0;
  domain_state->allocated_words_direct = 0;
  domain_state->allocated_words_suspended = 0;
  domain_state->allocated_words_resumed = 0;
  domain_state->current_ramp_up_allocated_words_diff = 0;
  domain_state->swept_words = 0;

  domain_state->local_roots = NULL;

  domain_state->backtrace_buffer = NULL;
  domain_state->backtrace_last_exn = Val_unit;
  domain_state->backtrace_active = 0;
  caml_register_generational_global_root(&domain_state->backtrace_last_exn);

  domain_state->compare_unordered = 0;
  domain_state->oo_next_id_local = 0;

  domain_state->requested_major_slice = 0;
  domain_state->requested_minor_gc = 0;
  domain_state->major_slice_epoch = 0;
  domain_state->requested_external_interrupt = 0;

  domain_state->parser_trace = 0;

  bool bt_enabled = parent
    ? parent->backtrace_active
    : caml_params->backtrace_enabled;
  caml_record_backtraces(bt_enabled);

#ifndef NATIVE_CODE
  domain_state->external_raise = NULL;
  domain_state->trap_sp_off = 1;
  domain_state->trap_barrier_off = 0;
  domain_state->trap_barrier_block = -1;
#endif

  /* MMTk: bind this domain as a mutator now that its state is fully
     initialised (minor/shared heap, stacks, roots). Enables the MMTk major heap
     for both the bytecode and native runtimes. */
  caml_mmtk_domain_init(domain_state);

  activate_parked_domain(d);
  goto domain_init_complete;

alloc_main_stack_failure:
create_stack_cache_failure:
  caml_remove_generational_global_root(&domain_state->dls_root);
  caml_teardown_major_gc();
init_major_gc_failure:
  /* No stock shared heap to orphan/free under always-on MMTk. */
  caml_free_minor_tables(domain_state->minor_tables);
  domain_state->minor_tables = NULL;
alloc_minor_tables_failure:
  caml_memprof_delete_domain(domain_state);
init_memprof_failure:
  caml_plat_unlock(&d->domain_lock);
  domain_self = NULL;

  atomic_fetch_add(&caml_num_domains_running, -1);

domain_state_init_failure:
  stop_parked_domain(d);
domain_parking_failure:

domain_init_complete:
  caml_gc_log("domain init complete");
  caml_plat_unlock(&all_domains_lock);
}

CAMLexport void caml_reset_domain_lock(void)
{
  dom_internal* self = domain_self;
  // This is only used to reset the domain_lock state on fork.
  /* FIXME: initializing an already-initialized mutex and cond
     variable is UB (especially mutexes that are locked).

     * On systhreads, this is best-effort but at least the error
       conditions should be checked and reported.

     * If there is only one thread, it is sensible to fork but the
       mutex should still not be initialized while locked. On Linux it
       seems that the mutex remains valid and locked
       (https://man7.org/linux/man-pages/man2/fork.2.html). For
       portability on POSIX the lock should be released and destroyed
       prior to calling fork and then init afterwards in both parent
       and child. */
  caml_plat_mutex_reinit(&self->domain_lock);

  return;
}

void caml_init_domains(uintnat max_domains, uintnat minor_heap_wsz)
{
  atomic_store_relaxed(&domains_exiting, 0);
  atomic_store_relaxed(&caml_num_domains_running, 0);

  /* Use [caml_stat_calloc_noexc] to zero initialize [all_domains]. */
  all_domains = caml_stat_calloc_noexc(max_domains, sizeof(dom_internal));
  if (all_domains == NULL)
    caml_fatal_error("Failed to allocate all_domains");

  stw_domains.domains =
      caml_stat_calloc_noexc(max_domains, sizeof(dom_internal*));
  if (stw_domains.domains == NULL)
    caml_fatal_error("Failed to allocate stw_domains.domains");

  /* No minor-heaps address-space reservation under always-on MMTk: the minor
     heap is an MMTk TLAB block per domain (see Note [minor heap layout]). */

  for (int i = 0; i < max_domains; i++) {
    struct dom_internal* dom = &all_domains[i];

    stw_domains.domains[i] = dom;

    dom->id = i;

    dom->interruptor.interrupt_word = NULL;
    caml_plat_mutex_init(&dom->interruptor.lock);
    caml_plat_cond_init(&dom->interruptor.cond);
    dom->interruptor.running = 0;
    dom->interruptor.terminating = 0;
    dom->interruptor.unique_id = 0;

    caml_plat_mutex_init(&dom->domain_lock);
    dom->domain_canceled = false;
  }

  domain_create(minor_heap_wsz, NULL);
  if (!domain_self) caml_fatal_error("Failed to create main domain");
  CAMLassert (domain_self->state->unique_id == 0);

  caml_init_signal_handling();
}

void caml_init_domain_self(int domain_id) {
  CAMLassert(0 <= domain_id);
  CAMLassert(domain_id < caml_params->max_domains);
  domain_self = &all_domains[domain_id];
  caml_state = domain_self->state;
}

enum domain_status { Dom_starting, Dom_started, Dom_failed };

struct domain_ml_values {
  value callback;
  value term_sync;
  /* The domain's `Finished(...)` result, kept alive as a generational global
     root from the moment it is built (sync_and_terminate) until ml_values is
     freed (after the joiner has consumed it). Global roots are scanned by EVERY
     MMTk collection in scan_vm_specific_roots -- unconditionally, independent
     of which domain triggered the GC and of mutator-park timing -- so this both
     keeps the result alive across the terminating domain's nursery teardown AND
     lets the coalescing-prone caml_mmtk_collect() promote it reliably (issue
     #31 / GH#3). Val_unit until set in sync_and_terminate. */
  value result;
};

/* stdlib/domain.ml */
#define Term_state(sync) (&Field(sync, 0))
#define Term_mutex(sync) (Mutex_val(Field(sync, 1)))
#define Term_condition(sync) (Condition_val(Field(sync, 2)))

static void init_domain_ml_values(struct domain_ml_values* ml_values,
                                  value callback, value term_sync)
{
  ml_values->callback = callback;
  ml_values->term_sync = term_sync;
  ml_values->result = Val_unit;
  caml_register_generational_global_root(&ml_values->callback);
  caml_register_generational_global_root(&ml_values->term_sync);
  caml_register_generational_global_root(&ml_values->result);
}

/* [retire_after_gc] must be true on the terminating path (domain_thread_func)
   and false on the spawn-failure path (caml_domain_spawn). See the GH#15 Bug B
   note below the root removals. */
static void free_domain_ml_values(struct domain_ml_values* ml_values,
                                  bool retire_after_gc)
{
  caml_remove_generational_global_root(&ml_values->callback);
  caml_remove_generational_global_root(&ml_values->term_sync);
  caml_remove_generational_global_root(&ml_values->result);
  /* GH#15 Bug B (use-after-free of a global root racing the GC root scan). On
     the terminating path this domain has ALREADY been MMTk-deregistered by
     caml_domain_terminate, so it now runs concurrently with collections. A GC
     that started AFTER caml_domain_terminate's own wait_collection_done can
     have snapshotted these two global-root slots (&callback / &term_sync) into
     a ProcessEdges packet while iterating caml_global_roots_old. Freeing
     ml_values now would let that worker later load a freed/reused slot and hand
     garbage to trace_object -> "cannot trace object" panic. Having removed the
     roots above (so no NEW collection can snapshot them), wait out any
     in-flight collection's grace period before the free: an RCU-style retire,
     the same guarantee caml_mmtk_domain_terminate gives the domain's own
     stack/roots. The spawn-failure caller passes retire_after_gc=false: there
     the freeing thread is the parent, a registered RUNNING mutator, which is
     stopped across any collection and so cannot reach this free while a GC
     still holds the snapshot (no UAF) -- and a blocking wait on a running
     mutator could deadlock against stop_all_mutators. */
  if (retire_after_gc)
    caml_mmtk_wait_collection_done();
  caml_stat_free(ml_values);
}

/* This is the structure of the data exchanged between the parent
   domain and child domain during domain_spawn. Some fields are 'in'
   parameters, passed from the parent to the child, others are 'out'
   parameters returned to the parent by the child.
*/
struct domain_startup_params {
  dom_internal *parent; /* in */
  enum domain_status status; /* in+out:
                                parent and child synchronize on this value. */
  struct domain_ml_values* ml_values; /* in */
  uintnat unique_id; /* out */
  const char *error; /* out: set iff status is Dom_failed */
};

static value caml_domain_initialize_default_exn(void)
{
  return Val_unit;
}

static void caml_domain_stop_default(void)
{
  return;
}

static void caml_domain_external_interrupt_hook_default(void)
{
  return;
}

CAMLexport value (*caml_domain_initialize_hook_exn)(void) =
   caml_domain_initialize_default_exn;

CAMLexport void (*caml_domain_stop_hook)(void) =
   caml_domain_stop_default;

CAMLexport void (*caml_domain_external_interrupt_hook)(void) =
   caml_domain_external_interrupt_hook_default;

CAMLexport _Atomic caml_timing_hook caml_domain_terminated_hook =
  (caml_timing_hook)NULL;

static value make_finished(caml_result result)
{
  CAMLparam0();
  CAMLlocal2(res, bt);
  if (caml_result_is_exception(result)) {
    res = result.data; /* Ensure that [result.data] is rooted before
                          subsequent allocations */
    /* res = exn */
    bt = caml_get_exception_raw_backtrace(Val_unit);
    res = caml_alloc_2(0, res, bt);
    /* res = (exn, bt) */
    res = caml_alloc_1(1 /* Error */, res);
    /* res = Error (exn, bt) */
    res = caml_alloc_1(0 /* Finished */, res);
    /* res = Finished(Error(exn, bt)) */
  } else {
    res = caml_alloc_1(0 /* Ok */,result.data);
    /* res = Ok v */
    res = caml_alloc_1(0, res);
    /* res = Finished(Ok v) */
  }
  CAMLreturn(res);
}

static void handshake_success(struct domain_startup_params *p)
{
  caml_plat_lock_blocking(&p->parent->interruptor.lock);
  p->status = Dom_started;
  p->unique_id = domain_self->interruptor.unique_id;
  caml_plat_broadcast(&p->parent->interruptor.cond);
  caml_plat_unlock(&p->parent->interruptor.lock);
}

static void handshake_failure(struct domain_startup_params *p, const char *str)
{
  caml_plat_lock_blocking(&p->parent->interruptor.lock);
  p->status = Dom_failed;
  p->error = str;
  caml_plat_broadcast(&p->parent->interruptor.cond);
  caml_plat_unlock(&p->parent->interruptor.lock);
  caml_gc_log("Failed to create domain");
}

/* Synchronize with joining domains. */
static void sync_result(value term_sync, value res)
{
  CAMLparam2(term_sync, res);

  /* We only call functions that do not raise exceptions, because this
     would be bad for us at this point. */

  /* Synchronize with joining domains. To avoid deadlocks, we must not
     block. In particular, systhreads are still running on this
     domain. */
  caml_plat_lock_non_blocking(Term_mutex(term_sync));

  /* Store result */
  volatile value *state = Term_state(term_sync);
  CAMLassert(!Is_block(*state));
  caml_modify(state, res);

  /* Signal all the waiting domains to be woken up */
  caml_plat_broadcast(Term_condition(term_sync));

  /* The mutex is unlocked after the domain is destroyed; we must
     release the local roots before this happens. */
  CAMLreturn0;
}

static void sync_and_terminate(struct domain_ml_values *ml_values,
                               caml_result res)
{
  CAMLparam0();
  CAMLlocal1(v);
  /* Allocate the result value. */
  v = make_finished(res);
  /* MMTk (issue #31): promote the just-allocated result to stable MMTk-traced
     space BEFORE we publish it to the joiner and tear this domain down.

     `make_finished` allocates the `Finished(Ok v)` chain on THIS (terminating)
     domain's young TLAB region. Stock OCaml's domain-terminate minor collection
     (caml_empty_minor_heap_promote) used to oldify all young survivors into the
     major heap, so the result was stable before teardown. Under always-on MMTk
     that routine is neutered to a bare `young_ptr = young_start` discard (it
     does NOT promote -- see runtime/minor_gc.c), so without help the result
     stays YOUNG while it is published to the joiner and the domain proceeds to
     deregister and tear down its young region. A collection on another domain
     landing in that window relocates/reclaims the result's young block out from
     under the joiner, which then dereferences a corrupted `Finished` chain ->
     SIGSEGV in Domain.join (issue #31; intermittent, all moving Immix-family
     plans).

     Forcing a collection here, while the result is rooted and this domain is
     still a registered, running STW participant, traces the result into stable
     space (and, for the generational plans, promotes it out of the nursery).
     After this the result survives the deregister/teardown edge. A MINOR
     collection suffices: `v` is a global root (below), and nursery GCs scan and
     promote global-root targets. The previous whole-heap (exhaustive) collect
     here was measured to be the dominant multi-domain scaling pathology -- one
     full STW GC per Domain termination, i.e. per spawn on spawn-per-round
     programs (SCALABILITY.md UPDATE 4/5). Self-gated: caml_mmtk_collect_minor
     is a no-op for NoGC / when MMTk cannot collect.

     ROOT THE RESULT AS A GLOBAL ROOT FIRST (issue #31 / GH#3). A single
     caml_mmtk_collect() against a CAMLlocal-only `v` does NOT reliably promote
     it under heavy multi-domain join: the user collection request COALESCES
     onto a peer domain's in-flight GC (gc_trigger request_flag), and per-domain
     (mutator-local) root scanning is subject to park timing -- an in-flight GC
     can have already scanned this domain before we park into it, so
     caml_mmtk_collect() returns without having promoted `v` (the ~9% SIGSEGV in
     Domain.join on a 28-core spawn/terminate+join storm). Publishing it into
     ml_values->result -- a *generational global root* -- changes that: global
     roots are scanned by EVERY collection in scan_vm_specific_roots,
     unconditionally and independent of park timing, so the collection that
     caml_mmtk_collect() waits out promotes the result via that path. The global
     root is also kept registered until ml_values is freed (after the joiner has
     consumed the result), so even in the residual edge where the awaited
     collection had already passed its global-root scan before we stored `v`,
     the result is never reclaimed by the nursery teardown: it stays live until
     the next collection promotes it, and thereafter survives reachably via
     term_sync->state. A naive collect-retry loop instead livelocks here (every
     collect coalesces under sustained contention); the global root is the
     correct, livelock-free fix. */
  caml_modify_generational_global_root(&ml_values->result, v);
  /* LXR (issue #31): the tracing/generational promotion below does NOT save the
     result under the reference-counting plan. LXR is non-generational, so
     caml_mmtk_is_young is always 0 -- the retry loop is a dead no-op -- and the
     forced caml_mmtk_collect coalesces onto a peer GC that already passed its
     global-root scan, so it never promotes `v`. The result's clean nursery
     block (BlockState::Unallocated, all-RC-zero) is then reclaimed by the RC
     nursery sweep and reused before the joiner dereferences term_sync.state ->
     SIGSEGV in Domain.join (rr-confirmed). RC-pin the whole Finished(Ok v)
     chain HERE, synchronously and independent of any collection: it gives `v`
     and its transitive children RC >= 1 (sparing their blocks) and marks them
     mature. A no-op on the tracing/generational plans, where the collect +
     global root below is the load-bearing promotion. Do it BEFORE the collect
     so a peer GC that snapshots this domain's roots sees a consistent,
     RC-pinned result. */
  caml_mmtk_keep_alive(v);
  caml_mmtk_collect_minor();
  /* Confirm the result is actually out of the nursery before publishing. The
     first caml_mmtk_collect() can coalesce onto a peer GC that had already run
     its global-root scan before we stored `v`, returning without promoting it.
     Because `v` is now a GLOBAL root it is kept alive regardless (so this can
     never livelock the way a CAMLlocal-only retry does -- every collection
     scans global roots, so each iteration converges), but it may still be
     young; loop a bounded number of fresh collects until it is promoted. After
     the first collect returns the GC request flag is clear, so the next
     caml_mmtk_collect() schedules a FRESH collection whose global-root scan
     sees `v`. The bound is a safety valve only; in practice this exits in 0-1
     extra iterations. caml_mmtk_is_young is false for non-generational plans /
     NoGC, so the loop is a no-op there (the Immix-family result is made live in
     place by the collect + global root). */
  {
    int tries = 0;
    while (caml_mmtk_is_young(ml_values->result) && tries++ < 1000)
      caml_mmtk_collect_minor();
  }
  /* re-read through the (now-forwarded, promoted) global root */
  v = ml_values->result;
  sync_result(ml_values->term_sync, v);
  /* This domain currently holds a lock for [mut], which is kept alive
     by a global root inside ml_values. */
  caml_plat_mutex *mut = Term_mutex(ml_values->term_sync);
  /* Join all systhreads on this domain and release the runtime state. */
  caml_domain_terminate(false);
  /* This domain has signaled all the waiting domains to be woken up.
     We unlock [mut] to release the joining domains. The unlock is
     done after [caml_domain_terminate] to ensure that this domain has
     released all of its runtime state. The domain no longer exists at
     this point but we can use [caml_plat_unlock]. */
  caml_plat_unlock(mut);
  caml_plat_assert_all_locks_unlocked();
  CAMLreturn0;
}

static CAML_THREAD_FUNCTION
domain_thread_func(void* v)
{
  struct domain_startup_params* p = v;
  struct domain_ml_values *ml_values = p->ml_values;
  /* This thread now owns ml_values */

  /* Create domain and do handshake with parent */
#ifndef _WIN32
  void * signal_stack = caml_init_signal_stack();
  if (signal_stack == NULL) {
    handshake_failure(p, "failed to allocate domain: signal stack");
    goto out1;
  }
#endif

  domain_create(caml_params->init_minor_heap_wsz, p->parent->state);
  if (domain_self == NULL) {
    handshake_failure(p, "failed to allocate domain: domain_create");
    goto out2;
  }
  domain_self->tid = caml_plat_thread_self();
  /* this domain is now part of the STW participant set */
  handshake_success(p);

  /* v and p must no longer be accessed */
  v = NULL;
  p = NULL;

  /* The child is now about to run OCaml: mark it a must-stop MMTk participant.
     leave_blocking waits out any in-progress collection first, so we never flip
     RUNNING while a collection is scanning this domain (see caml/mmtk.h). It
     was born STOPPED at bind (caml_mmtk_domain_init); this is its first RUNNING
     edge. */
  caml_mmtk_leave_blocking((uintnat) domain_self->state);

  caml_gc_log("Domain starting (unique_id = %" CAML_PRIuNAT ")",
              domain_self->interruptor.unique_id);
  CAML_EV_LIFECYCLE(EV_DOMAIN_SPAWN, getpid());

  value exn = caml_domain_initialize_hook_exn();
  if (Is_exception_result(exn)) {
    sync_and_terminate(ml_values, Result_exception(exn));
    goto out2;
  }

  /* release callback early;
     see the [note about callbacks and GC] in callback.c */
  value unrooted_callback = ml_values->callback;
  caml_modify_generational_global_root(&ml_values->callback, Val_unit);
  caml_result res = caml_callback_res(unrooted_callback, Val_unit);
  sync_and_terminate(ml_values, res);
  /* fall through */

 out2:
#ifndef _WIN32
  caml_free_signal_stack(signal_stack);
 out1:
#endif
  /* [ml_values] must be freed after unlocking its [term_sync] mutex.
     This ensures that the [term_sync] field is only removed from the
     root set after the mutex is unlocked. Otherwise, there is a risk
     of it being destroyed by [caml_mutex_finalize] while it remains
     locked, leading to undefined behaviour. */
  free_domain_ml_values(ml_values, /*retire_after_gc=*/true);
  return 0;
}

/* Note: [caml_domain_spawn] and [caml_domain_alone()].

   The use of [caml_domain_alone()] to implement sequential fast-path
   requires that no other domain is operating in parallel. This is
   indeed the case when [caml_domain_alone()] is observed while
   holding the domain lock:

   1. When a domain exits, it is careful to decrement
      [caml_num_domains_running] as the very last step, so that
      [caml_domain_alone()] does not return [true] while its mutator
      or domain-termination cleanup logic are still in progress.

   2. When a domain starts, it increments [caml_num_domains_running]
      immediately after taking the domain lock, and its parent domain
      blocks waiting for the child set the [Dom_started] flag, which
      happens after this increment. Neither the parent nor the child
      can wrongly observe [caml_domain_alone()] while the other may be
      running code with its domain lock held.
*/


CAMLprim value caml_domain_spawn(value callback, value term_sync)
{
  CAMLparam2 (callback, term_sync);
  struct domain_startup_params p;
  caml_plat_thread th;
  int err;

  if (atomic_load_relaxed(&domains_exiting) != 0) {
    caml_failwith("domain creation not allowed during shutdown");
  }

#ifndef NATIVE_CODE
  if (caml_debugger_in_use)
    caml_fatal_error("ocamldebug does not support spawning multiple domains");
#endif

  domain_self->tid = caml_plat_thread_self();
  /* Latch that the process has now gone multicore (never cleared, even after
     the spawned domain terminates). Backs caml_domain_is_multicore so that
     e.g. Unix.fork refuses after any domain has been spawned. */
  atomic_store_release(&caml_domains_ever_spawned, 1);

  p.parent = domain_self;
  p.status = Dom_starting;

  p.ml_values =
      (struct domain_ml_values*) caml_stat_alloc(
                                    sizeof(struct domain_ml_values));
  init_domain_ml_values(p.ml_values, callback, term_sync);

  err = caml_plat_thread_create(&th, 0, domain_thread_func, (void*)&p);
  if (err) {
    /* retire_after_gc=false: the parent (this thread) is a registered, running
       mutator -- stopped across any collection, so it cannot reach this free
       while a GC still holds a snapshot of these roots; no UAF and no wait
       needed. */
    free_domain_ml_values(p.ml_values, /*retire_after_gc=*/false);
    caml_check_error(err, "failed to create domain thread: "
                     "caml_plat_thread_create");
  }

  /* p.ml_values is now owned by the new domain */

  /* Handshake with the new domain: idle-wait for the child to start up
     (woken via interruptor->cond from handshake_success/_failure). There is
     no OCaml all-domains STW to service here any more (it was retired; MMTk's
     stop_all_mutators is the sole rendezvous), so no interrupt-servicing arm
     is needed. */
  struct interruptor *interruptor = &domain_self->interruptor;
  caml_plat_lock_blocking(&interruptor->lock);
  while (p.status == Dom_starting) {
    /* Idle-wait for the child. Mark this (parent) domain safe-stopped for MMTk
       while we block here -- otherwise a collection triggered by another domain
       would wait for us forever, since we hold no safepoint in this wait (bug
       #3b). Use the blocking-section hooks (no pending-action processing, so no
       raise can escape mid-handshake). interruptor->lock is independent of
       domain_lock (which the enter hook releases), so drop it around the
       section and re-test p.status under it to avoid a lost wakeup. */
    caml_domain_state *self = domain_self->state;
    caml_plat_unlock(&interruptor->lock);
    caml_enter_blocking_section_hook();
    caml_mmtk_enter_blocking((uintnat) self);
    caml_plat_lock_blocking(&interruptor->lock);
    if (p.status == Dom_starting)
      caml_plat_wait(&interruptor->cond, &interruptor->lock);
    caml_plat_unlock(&interruptor->lock);
    caml_leave_blocking_section_hook();
    caml_mmtk_leave_blocking((uintnat) self);
    caml_plat_lock_blocking(&interruptor->lock);
  }
  caml_plat_unlock(&interruptor->lock);

  if (p.status == Dom_started) {
    /* successfully created a domain.
       p.ml_values is now owned by that domain */
    caml_plat_thread_detach(th);
  } else {
    CAMLassert (p.status == Dom_failed);
    /* failed */
    caml_plat_thread_join(th);
    caml_failwith(p.error);
  }

  CAMLreturn (Val_long(p.unique_id));
}

CAMLprim value caml_ml_domain_id(value unit)
{
  CAMLnoalloc;
  return Val_long(domain_self->interruptor.unique_id);
}

CAMLprim value caml_ml_domain_index(value unit)
{
  CAMLnoalloc;
  return Val_long(domain_self->id);
}

#ifdef DEBUG
int caml_domain_is_in_stw(void) {
  return Caml_state->inside_stw_handler;
}
#endif

void caml_interrupt_self(void)
{
  interrupt_domain_local(Caml_state);
}

/*  This function is async-signal-safe as [all_domains] and
    [caml_params->max_domains] are set before signal handlers are installed and
    do not change afterwards. */
void caml_interrupt_all_signal_safe(void)
{
  for (dom_internal *d = all_domains;
       d < &all_domains[caml_params->max_domains];
       d++) {
    /* [all_domains] is an array of values. So we can access
       [interrupt_word] directly without synchronisation other than
       with other people who access the same [interrupt_word].*/
    atomic_uintnat * interrupt_word =
      atomic_load_acquire(&d->interruptor.interrupt_word);
    /* Early exit: if the current domain was never initialized, then
       neither have been any of the remaining ones. */
    if (interrupt_word == NULL) return;
    interrupt_domain(&d->interruptor);
  }
}

/*  This function can be called from arbitrary code, possibly running
    concurrently with the OCaml runtime, as long as it synchronized
    with the runtime startup which initialized [all_domains] and
    [caml_params].
    Returns [-1] on failure -- if the provided unique id is
    not assigned to a currently-running domain.
*/
intnat caml_find_index_of_running_domain(uintnat dom_unique_id)
{
  for (int i = 0; i < caml_params->max_domains; i++) {
    dom_internal *d = &all_domains[i];

    /* See [caml_interrupt_all_signal_safe] above for synchronization
       and early-exit comments. */
    atomic_uintnat * interrupt_word =
      atomic_load_acquire(&d->interruptor.interrupt_word);
    if (interrupt_word == NULL) return -1;

    if (d->interruptor.unique_id == dom_unique_id) return i;
  }
  return -1;
}

/* To avoid any risk of forgetting an action through a race,
   [caml_reset_young_limit] is the only way (apart from setting
   young_limit to -1 for immediate interruption) through which
   [young_limit] can be modified. We take care here of possible
   races. */
void caml_reset_young_limit(caml_domain_state * dom_st)
{
  /* An interrupt might have been queued in the meanwhile; the
     atomic_exchange achieves the proper synchronisation with the
     reads that follow (an atomic_store is not enough). */
  value *trigger = dom_st->young_trigger > dom_st->memprof_young_trigger ?
          dom_st->young_trigger : dom_st->memprof_young_trigger;
  CAMLassert ((uintnat)dom_st->young_ptr >=
              (uintnat)dom_st->memprof_young_trigger);
  CAMLassert ((uintnat)dom_st->young_ptr >=
              (uintnat)dom_st->young_trigger);
  /* An interrupt might have been queued in the meanwhile; this
     achieves the proper synchronisation. */
  atomic_exchange(&dom_st->young_limit, (uintnat)trigger);

  /* For non-delayable asynchronous actions, we immediately interrupt
     the domain again. */
  if (dom_st->requested_minor_gc
      || dom_st->requested_major_slice
      || dom_st->major_slice_epoch < atomic_load (&caml_major_slice_epoch)) {
    interrupt_domain_local(dom_st);
  }
  /* We might be here due to a recently-recorded signal or forced
     systhread switching, so we need to remember that we must run
     signal handlers or systhread's yield. In addition, in the case of
     long-running C code (that may regularly poll with
     caml_process_pending_actions), we want to force a query of all
     callbacks at every minor collection or major slice (similarly to
     the OCaml behaviour). */
  caml_set_action_pending(dom_st);
}

void caml_update_young_limit_after_c_call(caml_domain_state * dom_st)
{
  if (CAMLunlikely(dom_st->action_pending)) interrupt_domain_local(dom_st);
}

Caml_inline void advance_global_major_slice_epoch (caml_domain_state* d)
{
  uintnat old_value;

  CAMLassert (atomic_load (&caml_major_slice_epoch) <=
              atomic_load (&caml_minor_collections_count));

  old_value = atomic_exchange (&caml_major_slice_epoch,
                               atomic_load (&caml_minor_collections_count));

  if (old_value != atomic_load (&caml_minor_collections_count)) {
    /* This domain is the first one to use up half of its minor heap arena
        in this minor cycle. Trigger major slice on other domains. */
    caml_interrupt_all_signal_safe();
  }
}

void caml_poll_gc_work(void)
{
  CAMLalloc_point_here;

  caml_domain_state* d = Caml_state;

  /* TLAB mode: MMTk owns the entire heap, so there is no OCaml minor GC and no
     OCaml major slice. Consume any pending minor-GC / major-slice requests (so
     caml_reset_young_limit below doesn't immediately re-interrupt the domain),
     reset the young_limit, and return. The young region is refilled on demand
     in caml_alloc_small_dispatch, and MMTk collections fire from the refill /
     STW path. */
  if (caml_mmtk_tlab) {
    d->requested_minor_gc = 0;
    d->requested_major_slice = 0;
    d->requested_global_major_slice = 0;
    /* Ragged-safepoint ack (caml_mmtk_quiesce_running_domains): record that
       this domain has passed a safepoint. Plain atomic store, no lock. */
    caml_mmtk_quiesce_ack(d);
    caml_reset_young_limit(d);
    /* An EXHAUSTED TLAB re-arms the safepoint trap: young_ptr sits at/below
       young_limit and only the allocation path (caml_alloc_small_dispatch)
       ever refills. An allocation-free compute phase after a draining
       allocation then traps on EVERY loop back-edge poll, through the whole
       caml_call_gc machinery, doing nothing — measured 453M traps / 82G
       mutator instructions on matmul-768 with a 16MB nursery ("the 8.9x
       catastrophe", SHAPE.md round 20). Refill here to clear the trap; if
       the refill fails (heap genuinely exhausted) leave state as-is — the
       next real allocation raises Out_of_memory as before. */
    if (Caml_check_gc_interrupt(d))
      (void)caml_mmtk_refill_tlab(d, Whsize_wosize(0));
    return;
  }

  if ((uintnat)d->young_ptr - Bhsize_wosize(Max_young_wosize) <
      (uintnat)d->young_trigger) {

    if (d->young_trigger == d->young_start) {
      /* Trigger minor GC */
      d->requested_minor_gc = 1;
    } else {
      CAMLassert (d->young_trigger ==
                  d->young_start + (d->young_end - d->young_start) / 2);
      /* We have used half of our minor heap arena. Request a major slice on
         this domain. */
      advance_global_major_slice_epoch (d);
      /* Advance the [young_trigger] to [young_start] so that the allocation
         fails when the minor heap arena is full. */
      d->young_trigger = d->young_start;
    }
  } else if (d->requested_minor_gc) {
    /* This domain has _not_ used up half of its minor heap arena, but a minor
       collection has been requested. Schedule a major collection slice so as
       to not lag behind. */
    advance_global_major_slice_epoch (d);
  }

  if (d->major_slice_epoch < atomic_load (&caml_major_slice_epoch)) {
    d->requested_major_slice = 1;
  }

  if (d->requested_minor_gc) {
    /* out of minor heap or collection forced */
    d->requested_minor_gc = 0;
    /* excise Phase 3a: the all-domains minor-empty STW is gone. Reset THIS
       domain's young region directly (the STW's only load-bearing residue) and
       run THIS domain's minor-cycle bookkeeping. Reached only in bytecode
       (native takes the caml_mmtk_tlab early-return above and never gets here),
       so bump_count=1 here is the single caml_minor_collections_count bump per
       minor GC; native stays 0. */
    caml_minor_gc_reset_young_region(d);
    caml_minor_gc_domain_bookkeeping(d, /*bump_count=*/1);
  }

  if (d->requested_major_slice || d->requested_global_major_slice) {
    /* No EV_MAJOR span here: under always-on MMTk caml_major_collection_slice
       is inert (it only records this domain's major-slice epoch; MMTk owns
       collection), so the span reported a fictional major-GC pause around a
       no-op to olly/runtime_events. */
    d->requested_major_slice = 0;
    caml_major_collection_slice(AUTO_TRIGGERED_MAJOR_SLICE);
  }

  if (d->requested_global_major_slice) {
    /* Stock OCaml broadcast the major-slice request to all domains via an async
       all-domains STW (stw_global_major_slice, which just set each peer's local
       requested_major_slice). Under always-on MMTk caml_major_collection_slice
       is inert (it only records this domain's major-slice epoch; MMTk owns
       collection), so the broadcast ran a no-op on every peer. The requesting
       domain's own slice already fired at the block above (line ~1950 tests
       requested_global_major_slice). So just satisfy and clear the request
       locally, with no STW. */
    d->requested_global_major_slice = 0;
  }

  /* Ragged-safepoint ack (caml_mmtk_quiesce_running_domains): record that this
     domain has passed a safepoint. Plain atomic store, no lock. */
  caml_mmtk_quiesce_ack(d);
  caml_reset_young_limit(d);
}

void caml_handle_gc_interrupt(void)
{
  CAMLalloc_point_here;

  /* MMTk multi-domain STW: if a collection is in progress, park this domain at
     the safepoint (roots are published) until it finishes. */
  caml_mmtk_stw_poll();

  caml_poll_gc_work();
}

/* Preemptive systhread switching */
void caml_process_external_interrupt(void)
{
  if (atomic_load_acquire(&Caml_state->requested_external_interrupt)) {
    caml_domain_external_interrupt_hook();
  }
}

CAMLexport intnat caml_domain_is_multicore (void)
{
  /* True once more than one domain runs, or once any extra domain has ever
     been spawned (latched, never cleared -- so this stays true even after the
     spawned domain terminates). The latch replaces the old per-domain
     backup-thread "running" flag that stock OCaml used for this. */
  return (!caml_domain_alone()
          || atomic_load_acquire(&caml_domains_ever_spawned));
}

CAMLexport void caml_acquire_domain_lock(void)
{
  dom_internal* self = domain_self;
  caml_plat_lock_blocking(&self->domain_lock);
  caml_state = self->state;
}

CAMLexport void caml_release_domain_lock(void)
{
  dom_internal* self = domain_self;
  caml_state = NULL;
  caml_plat_unlock(&self->domain_lock);
}

/* default handler for unix_fork, will be called by unix_fork. */
static void caml_atfork_default(void)
{
  caml_reset_domain_lock();
  caml_acquire_domain_lock();
  /* FIXME: For best portability, the IO channel locks should be
     reinitialised as well. (See comment in
     caml_reset_domain_lock.) */
}

CAMLexport void (*caml_atfork_hook)(void) = caml_atfork_default;

static inline int domain_terminating(dom_internal *d) {
  return d->interruptor.terminating;
}

int caml_domain_terminating (caml_domain_state *dom_st)
{
  return domain_terminating(&all_domains[dom_st->id]);
}

int caml_domain_is_terminating (void)
{
  return domain_terminating(domain_self);
}

static bool marking_and_sweeping_done(caml_domain_state *domain_state)
{
  return (domain_state->marking_done
          && domain_state->sweeping_done);
}

void caml_domain_terminate(bool last)
{
  caml_domain_state* domain_state = domain_self->state;
  struct interruptor* s = &domain_self->interruptor;
  int finished = 0;

  caml_gc_log("Domain terminating");
  s->terminating = 1;

  /* Join ongoing systhreads, if necessary, and then run user-defined
     termination hooks. No OCaml code can run on this domain after
     this. */
  caml_domain_stop_hook();
  call_timing_hook(&caml_domain_terminated_hook);

  while (!finished) {
    caml_finish_sweeping();

    /* excise Phase 3a: the all-domains minor-empty STW is gone, so the
       terminate flush no longer joins it (the surrounding loop's
       all_domains_lock / interrupt-draining already handles any ongoing STW).
       Reset this domain's young region directly and run its terminate
       bookkeeping explicitly, since the STW no longer clears this domain's
       sampled_gc_stats slot. bump_count=0: terminate must not bump
       caml_minor_collections_count (keeps native at 0).
       caml_collect_gc_stats_sample_stw sees terminating==1 and zeroes the slot,
       as the comment near caml_domain_terminate's stats teardown requires. */
    caml_minor_gc_reset_young_region(domain_state);
    caml_minor_gc_domain_bookkeeping(domain_state, /*bump_count=*/0);

    if (last)
      caml_finish_major_cycle(0);

    caml_finish_marking();

    /* (Dropped the stock `caml_gc_phase != Phase_sweep_main` assert: MMTk is
       the only collector and does not drive caml_gc_phase, so that invariant no
       longer applies.) */
    caml_orphan_ephemerons(domain_state);
    caml_orphan_finalisers(domain_state);

    /* Orphaning ephemerons and finalizers may create new marking or
       sweeping work, so we may need to mark and/or sweep again. */

    /* No need to check for interrupts if we are the last domain running. */
    if (last) {
      CAML_EV_LIFECYCLE(EV_DOMAIN_TERMINATE, getpid());
      break;
    }

    /* If new marking or sweeping work appeared during orphaning,
       run a new loop iteration. */
    if (!marking_and_sweeping_done(domain_state))
      continue;

    /* No stock shared heap to orphan under always-on MMTk. */
    CAMLassert(marking_and_sweeping_done(domain_state));

    /* GH#15 fix: leave MMTk's RUNNING set right HERE -- after the flush body
       above (which ran while RUNNING, so a concurrent collection waited for it
       and scanned this domain's roots/minor-tables consistently) and right
       before the UNBOUNDED block on all_domains_lock below. The
       all_domains_lock may be held
       (transitively) by a spawning domain blocked on a terminating peer's
                      domain_lock, which that peer holds while waiting in
                      mmtk_ocaml_wait_collection_done for a collection that
                      stop_all_mutators is wedging on US being RUNNING -- the
                      4-way lock-order deadlock GH#15. Leaving RUNNING here
                      breaks the cycle: the GC stops awaiting us and finishes.
                      We stay in the MUTATOR REGISTRY (roots still scanned, and
                      now stable -- the flush is done, teardown hasn't started)
                      until caml_mmtk_domain_terminate deregisters us at the
                      end. NB it must be HERE, not at the top of
                      caml_domain_terminate: marking STOPPED before the flush
                      body would leave this domain STOPPED-but-still-registered
                      across the flush, adding a window where a GC scans it
                      while the flush mutates its minor tables. (That is
                      distinct from GH#15 "Bug B" -- a pre-existing
                      terminating-domain root-scan panic, 'cannot trace object',
                      that fires during spawn/terminate independent of this
                      placement; fixing the lock cycle here UNMASKS it. Bug B is
                      tracked in GH#15 as the remaining blocker.) */
    caml_mmtk_enter_blocking((uintnat) domain_state);

    /* Take the all_domains_lock to try and exit the STW participant set
       without racing with a STW section being triggered. */
    caml_plat_lock_blocking(&all_domains_lock);

    /* Leave the STW participant set. (Stock OCaml gated this on no pending
       OCaml all-domains STW interrupt; that rendezvous is retired, so the
       departure is unconditional.) */
    {
      finished = 1;
      s->terminating = 0;
      s->running = 0;

      /* Remove this domain from stw_domains.
         (This will only be observed after [all_domains_lock] is released.) */
      stop_active_domain(domain_self);

      /* No stock minor-heap arena to free under always-on MMTk: the domain's
         young region is an MMTk TLAB block, returned to MMTk when the mutator
         is deregistered (caml_mmtk_domain_terminate). */

      /* We must signal domain termination before releasing [all_domains_lock]:
         after that, this domain will no longer take part in STWs and emitting
         an event could race with runtime events teardown. */
      CAML_EV_LIFECYCLE(EV_DOMAIN_TERMINATE, getpid());
    }
    caml_plat_unlock(&all_domains_lock);
  }

  /* Now the minor heap has been fully flushed (survivors promoted into MMTk via
     a valid mutator) and the domain has left the STW participant set:
     deregister it from MMTk so future collections don't wait for it. Must come
     AFTER the flush loop above -- the final caml_empty_minor_heaps_once
     promotes through Caml_state->mmtk_mutator, so it must still be live there.
     */
  caml_mmtk_domain_terminate(domain_state);

  /* [domain_state] may be reused by a fresh domain here, now that we
     have done [stop_active_domain] and released the
     [all_domains_lock]. In particular, we cannot touch
     [domain_self->interruptor] after here because it may be reused.

     However, [domain_create()] won't touch the domain state until
     it has claimed the [domain_lock], so we hang onto that while we are
     tearing down the state. */

  /* Delete the domain state from statmemprof after any promotion
   * (etc) done by this domain: any remaining memprof state will be
   * handed over to surviving domains. */
  caml_memprof_delete_domain(domain_state);

  caml_remove_generational_global_root(&domain_state->dls_root);
  caml_remove_generational_global_root(&domain_state->backtrace_last_exn);
  caml_stat_free(domain_state->final_info);
  caml_stat_free(domain_state->ephe_info);
  caml_free_intern_state();
  caml_free_extern_state();
  caml_teardown_major_gc();

  /* Under always-on MMTk there is no stock shared heap to adopt/finalise/orphan
     or free on domain termination: MMTk owns all heap objects and runs custom
     finalisers itself (caml_mmtk_run_custom_finalizers). The shared_heap field
     is left NULL. */
  caml_free_minor_tables(domain_state->minor_tables);
  domain_state->minor_tables = NULL;

  /* At this point, the stats of the domain must be empty.
     - heap stats were orphaned by [caml_orphan_shared_heap]
     - alloc stats were orphaned by [caml_orphan_alloc_stats]
     - the sampled copy in [sampled_gc_stats] was cleared by the minor
       collection performed by [caml_empty_minor_heaps_once()], see
       the termination-specific logic in
       [caml_collect_gc_stats_sample_stw].
  */

  /* TODO: can this ever be NULL? can we remove this check? */
  if(domain_state->current_stack != NULL) {
    caml_free_stack(domain_state->current_stack);
  }
  caml_free_backtrace_buffer(domain_state->backtrace_buffer);
  caml_free_gc_regs_buckets(domain_state->gc_regs_buckets);

  caml_plat_unlock(&domain_self->domain_lock);

  /* This is the last thing we do because we need to be able to rely
     on caml_domain_alone (which uses caml_num_domains_running) in at least
     the shared_heap lockfree fast paths. Also, we don't want to decrement
     it back to zero when the last domain exits, for caml_domain_alone()
     to remain accurate. */
  if (!last)
    atomic_fetch_add(&caml_num_domains_running, -1);
}

/* Stop every domain other than the main one when the program exits with peers
   still running (i.e. domains were never joined). This used to run a callback
   on each domain via OCaml's all-domains STW (caml_try_run_on_all_domains +
   stw_terminate_domain). excise Phase 3b removes that: MMTk is the sole STW
   rendezvous, so caml_stop_all_domains no longer joins OCaml's STW. Instead the
   main domain iterates the running peers itself and forcibly cancels each one.

   We are not in a state where we can safely release a peer's resources: a
   cancelled peer may have been anywhere (mid-allocation, holding its domain
   lock, inside C). So, exactly as before (PR #12964), we do NOT touch a peer's
   heap or roots and do NOT wait for it to terminate; the best we can do is
   cancel it. The one thing we MUST do for each cancelled peer is deregister it
   from MMTk: a pthread_cancel'd peer will never reach a GC safepoint again, so
   if it stayed in MMTk's mutator registry / RUNNING set, a stop_all_mutators in
   flight (or the one a final collection starts) would block forever on
   running.is_empty(). */
void caml_stop_all_domains(void)
{
  /* Blocks any new domain spawn from here on (checked in caml_domain_spawn). */
  atomic_store_relaxed(&domains_exiting, 1);

  /* all_domains_lock guards stw_domains membership and the interruptor.running
     flag transitions, so under it the active region [0, active_domains) is
     exactly the set of domains currently running OCaml. We never take an MMTk
     lock -> all_domains_lock anywhere, and the MMTk deregister path never
     reaches back into all_domains_lock (stop_all_mutators takes only MMTk locks
     and calls into C solely via caml_mmtk_interrupt/uninterrupt, pure atomic
     stores), so holding all_domains_lock here while deregistering cannot invert
     lock order. */
  caml_plat_lock_blocking(&all_domains_lock);
  for (int i = 0; i < stw_domains.active_domains; i++) {
    dom_internal *d = stw_domains.domains[i];
    if (d == domain_self)
      continue;

    /* Forcibly cancel the peer. The cancel request comes from the main
       domain. */
    (void)caml_plat_thread_cancel(d->tid);

    /* Load-bearing: drop the peer from MMTk's mutator registry AND RUNNING set
       BEFORE we stop waiting on it, so a collection's stop_all_mutators can
       reach running.is_empty() instead of hanging on a thread that will never
       hit a safepoint again. Deregister-only (no collection-done wait): we do
       not tear the peer's roots down, so there is nothing to protect. */
    caml_mmtk_deregister_domain(d->state);

    /* The peer was cancelled in an unknown state, so its domain_lock may be
       held or half-released: mark it so caml_free_domains() does NOT free that
       lock. We intentionally do not wait for the peer to terminate, do not
       decrement caml_num_domains_running, and do not unlock its domain_lock. */
    d->domain_canceled = true;
  }
  caml_plat_unlock(&all_domains_lock);

  caml_plat_unlock(&domain_self->domain_lock);

  caml_plat_assert_all_locks_unlocked();
}

bool caml_free_domains(void)
{
  bool result = true;

  for (int i = 0; i < caml_params->max_domains; i++) {
    struct dom_internal* dom = &all_domains[i];

    dom->interruptor.interrupt_word = NULL;
    caml_plat_mutex_free(&dom->interruptor.lock);
    caml_plat_cond_free(&dom->interruptor.cond);

    if (dom->domain_canceled)
      result = false;
    else
      caml_plat_mutex_free(&dom->domain_lock);
  }

#ifdef WITH_THREAD_SANITIZER
  /* When running with TSan, there will be reports of races between
     freeing the all_domains synchronization objects and domain threads
     accessing them, even though we wait first for the domain threads to
     have terminated in the above loop. */
  result = false;
#endif

  return result;
}

CAMLprim value caml_ml_domain_cpu_relax(value t)
{
  /* No inter-domain interrupts to service here any more (the OCaml all-domains
     STW was retired); just yield the CPU. */
  cpu_relax ();
  return Val_unit;
}

CAMLprim value caml_domain_dls_set(value t)
{
  CAMLnoalloc;
  caml_modify_generational_global_root(&Caml_state->dls_root, t);
  return Val_unit;
}

CAMLprim value caml_domain_dls_get(value unused)
{
  CAMLnoalloc;
  return Caml_state->dls_root;
}

CAMLprim value caml_domain_dls_compare_and_set(value old, value new)
{
  CAMLnoalloc;
  value current = Caml_state->dls_root;
  if (current == old) {
    caml_modify_generational_global_root(&Caml_state->dls_root, new);
    return Val_true;
  } else {
    return Val_false;
  }
}

CAMLprim value caml_domain_count(value unused)
{
  return Val_long(atomic_load_relaxed(&caml_num_domains_running));
}

CAMLprim value caml_recommended_domain_count(value unused)
{
  intnat n = -1;

#if defined(HAS_GNU_GETAFFINITY_NP) || defined(HAS_BSD_GETAFFINITY_NP)
  cpu_set_t cpuset;

  CPU_ZERO(&cpuset);
  /* error case fallsback into next method */
  if (pthread_getaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0)
    n = CPU_COUNT(&cpuset);
#endif /* HAS_GNU_GETAFFINITY_NP || HAS_BSD_GETAFFINITY_NP */

#ifdef _SC_NPROCESSORS_ONLN
  if (n == -1)
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif /* _SC_NPROCESSORS_ONLN */

#ifdef _WIN32
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  n = sysinfo.dwNumberOfProcessors;
#endif /* _WIN32 */

  /* At least one, even if system says zero */
  if (n <= 0)
    n = 1;
  else if (n > caml_params->max_domains)
    n = caml_params->max_domains;

  return (Val_long(n));
}
