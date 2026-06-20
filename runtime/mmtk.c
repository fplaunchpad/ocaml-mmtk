/**************************************************************************/
/*                                                                        */
/*                MMTk garbage collector glue (bytecode)                  */
/*                                                                        */
/**************************************************************************/

/* C glue between the OCaml runtime and the in-tree MMTk binding. Compiled into
 * both the bytecode and native runtimes (a COMMON source). Currently the body is
 * behind #ifndef NATIVE_CODE, so the native object is empty; native enablement
 * is in progress (see ROADMAP M5). */

#define CAML_INTERNALS

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "caml/config.h"

/* The MMTk glue is compiled into both the bytecode and native runtimes. The few
   bytecode-interpreter-specific bits (caml_mmtk_alloc_small) are harmless when
   linked into native code (unused). */

#include "caml/mlvalues.h"
#include "caml/domain_state.h"
#include "caml/domain.h"
#include "caml/fail.h"
#include "caml/misc.h"
#include "caml/roots.h"
#include "caml/weak.h"
#include "caml/mmtk.h"

/* The in-tree MMTk binding's C ABI (gc/mmtk/include/mmtk_ocaml.h). */
#include "../gc/mmtk/include/mmtk_ocaml.h"

int caml_mmtk_enabled = 0;

/* Experimental "vanilla minor heap + MMTk major heap" mode (MMTK_VANILLA_MINOR=1):
   keep OCaml's stock nursery + minor GC, but redirect promotion to MMTk and let
   MMTk own the major heap. When 0 (default), MMTk backs every allocation and the
   minor heap is bypassed. Read on the allocation fast path, so a plain int. */
int caml_mmtk_vanilla_minor = 0;

/* Native TLAB / nursery-aliasing mode (MMTK_TLAB=1): MMTk owns the nursery too.
   The inlined native fast-path bumps an MMTk Immix block (handed over by
   mmtk_ocaml_refill_tlab); when it is exhausted the runtime refills another
   block instead of running a minor GC. There is no OCaml minor GC and no
   promotion in this mode — every object is an MMTk object from birth — so the
   nested-STW hazard of the vanilla-minor model cannot occur. Requires an
   Immix-family plan (Immix/GenImmix/StickyImmix); falls back to vanilla-minor
   otherwise. Read on the allocation slow path, so a plain int. */
int caml_mmtk_tlab = 0;

static int caml_mmtk_initialised = 0;
/* Whether the active plan collects (anything but NoGC). NoGC must NOT start
   collection: forcing a GC it cannot perform would spin/fail. */
static int caml_mmtk_collects = 0;
/* Whether the active plan is generational (needs the mutator write barrier).
   Read on every mutable pointer write, so keep it a plain int. */
static int caml_mmtk_generational = 0;
static int caml_mmtk_collection_started = 0;

/* Objects this size (bytes) or larger are routed to MMTk's large object
 * space. Conservative: smaller than the smallest line/block in collecting
 * plans, so it is also correct for Immix later. */
#define CAML_MMTK_LOS_THRESHOLD (16 * 1024)

/* AllocationSemantics codes shared with the Rust ABI (see api.rs). */
#define CAML_MMTK_SEM_DEFAULT   0
#define CAML_MMTK_SEM_LOS       2

static void caml_mmtk_report_copied(void);

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
  caml_mmtk_generational = (strcmp(plan, "GenImmix") == 0
                           || strcmp(plan, "StickyImmix") == 0
                           || strcmp(plan, "GenCopy") == 0);

  if (getenv("MMTK_VERBOSE") != NULL) {
    fprintf(stderr, "[mmtk] initialised: plan=%s heap=%zuMiB\n", plan, heap_mb);
    atexit(caml_mmtk_report_copied);
  }
}

/* Report how many objects copying collection relocated (Immix defrag, etc.).
   Registered with atexit under MMTK_VERBOSE so we can confirm movement actually
   happened during a run (and exercise the infix-pointer fixup path). */
static void caml_mmtk_report_copied(void)
{
  fprintf(stderr,
          "[mmtk] GCs: %zu, GC time: %llu ms, objects copied: %zu\n",
          mmtk_ocaml_gc_count(),
          (unsigned long long) mmtk_ocaml_gc_time_ms(),
          mmtk_ocaml_objects_copied());
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
  {
    const char *vm = getenv("MMTK_VANILLA_MINOR");
    caml_mmtk_vanilla_minor = (vm != NULL && vm[0] != '\0' && strcmp(vm, "0") != 0);
    const char *tl = getenv("MMTK_TLAB");
    caml_mmtk_tlab = (tl != NULL && tl[0] != '\0' && strcmp(tl, "0") != 0);
  }
  dom->mmtk_mutator = mmtk_ocaml_bind_mutator((uintptr_t)dom);
  /* For collecting plans, spawn the GC worker threads once (must happen before
     an allocation can trigger a collection). */
  if (caml_mmtk_collects && !caml_mmtk_collection_started) {
    mmtk_ocaml_initialize_collection((uintptr_t)dom);
    caml_mmtk_collection_started = 1;
  }
  caml_mmtk_enabled = 1;

  /* TLAB mode: repoint the domain's young region at a fresh MMTk Immix block now,
     so the very first allocation lands in MMTk-owned memory rather than the stock
     minor arena (which then sits unused). If the active plan has no Immix Default
     allocator (e.g. NoGC/MarkSweep), TLAB is unsupported — fall back to the
     validated vanilla-minor model. */
  if (caml_mmtk_tlab) {
    if (!caml_mmtk_refill_tlab(dom, Whsize_wosize(0))) {
      caml_mmtk_tlab = 0;
      caml_mmtk_vanilla_minor = 1;
      if (getenv("MMTK_VERBOSE") != NULL)
        fprintf(stderr, "[mmtk] MMTK_TLAB requested but plan has no Immix TLAB; "
                        "falling back to vanilla-minor\n");
    } else if (getenv("MMTK_VERBOSE") != NULL) {
      fprintf(stderr, "[mmtk] TLAB nursery aliasing active (young = MMTk block)\n");
    }
  }
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
  void *p = mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag,
                             CAML_MMTK_SEM_DEFAULT);
  /* NULL => heap exhausted after collection. Raise from this C frame (safe to
     longjmp; raising inside MMTk's Rust alloc path would not be). */
  if (p == NULL) caml_raise_out_of_memory();
  return (value)p;
}

value caml_mmtk_alloc_shr(mlsize_t wosize, tag_t tag, reserved_t reserved)
{
  (void)reserved;
  void *p = mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag,
                             caml_mmtk_semantics(wosize));
  if (p == NULL) caml_raise_out_of_memory();
  return (value)p;
}

/* TLAB refill: hand the domain a fresh MMTk Immix block as its young region,
   in place of a minor GC. `whsize` is the size (in words, header included) the
   triggering allocation needs; the block returned is at least that large.

   Repoints young_start/young_end/young_ptr at the block and (no half-heap major
   trigger, no memprof sampling in this mode) sets both triggers to young_start
   so caml_reset_young_limit makes the fast-path bump the whole block before the
   next refill. Returns 1 on success, 0 on heap exhaustion or if the plan has no
   Immix Default allocator (caller decides whether to raise or fall back). The
   refill may itself trigger an MMTk GC (its block acquisition polls); that is
   safe here because the old young region is exhausted (nothing to lose) and the
   roots are published at this safepoint. */
int caml_mmtk_refill_tlab(caml_domain_state *dom, mlsize_t whsize)
{
  uintptr_t start = 0, end = 0;
  size_t min_bytes = (size_t)whsize * sizeof(value);
  if (min_bytes == 0) min_bytes = sizeof(value);

  if (!mmtk_ocaml_refill_tlab(dom->mmtk_mutator, min_bytes, &start, &end))
    return 0;

  dom->young_start          = (value*)start;
  dom->young_end            = (value*)end;
  dom->young_ptr            = (value*)end;
  dom->young_trigger        = (value*)start;
  dom->memprof_young_trigger = (value*)start;
  caml_reset_young_limit(dom);
  return 1;
}

/* Report every ephemeron / weak-array field in this domain as a strong root.

   OCaml links weak arrays and ephemerons into per-domain lists
   (domain->ephe_info->{todo,live}); the stock major GC (major_gc.c) is what
   normally marks, updates, and weakly-clears them. We bypass that GC, and these
   blocks are Abstract_tag (so scan_object skips them) and are reachable *only*
   via ephe_info — so without this, MMTk would treat them as dead, collect/move
   them, and leave dangling pointers in the lists that crash any later ephemeron
   walk (e.g. Gc.full_major). Until proper MMTk weak-reference processing exists
   (ROADMAP workstream E), keep the whole ephemeron graph alive and
   pointer-updated by reporting each block's fields (link, data, keys) and the
   list heads as ordinary roots. This is memory-safe but conservative: weak
   references never clear (the same tradeoff as keeping finalisable values alive).

   Called per domain from the binding's root scan (scan_roots_in_mutator_thread),
   alongside caml_do_roots. */
void caml_mmtk_scan_ephe_roots(scanning_action f, void *fdata,
                               caml_domain_state *domain)
{
  struct caml_ephe_info *ei = domain->ephe_info;
  if (ei == NULL) return;

  value *heads[2];
  heads[0] = &ei->todo;
  heads[1] = &ei->live;

  for (int h = 0; h < 2; h++) {
    value *headp = heads[h];
    if (*headp != (value) NULL) f(fdata, *headp, headp);
    for (value e = *headp; e != (value) NULL; e = Ephe_link(e)) {
      mlsize_t wo = Wosize_val(e);  /* fields: 0 link, 1 data, 2.. keys */
      for (mlsize_t i = 0; i < wo; i++) {
        volatile value *slot = Op_val(e) + i;
        f(fdata, *slot, slot);
      }
    }
  }
}

/* Service an explicit `Gc` collection request (Gc.major / full_major / compact).
   Under MMTk the stock major-GC machinery (caml_finish_major_cycle) must NOT run
   — it operates on the bypassed stock shared heap and corrupts state (observed:
   channel/custom-block corruption → crash under a moving plan). Instead trigger a
   real MMTk collection on the calling domain and block until it completes. No-op
   for NoGC (cannot collect) and when MMTk is disabled. */
void caml_mmtk_collect(void)
{
  if (caml_mmtk_enabled && caml_mmtk_collects)
    mmtk_ocaml_handle_user_collection_request((uintptr_t) Caml_state);
}

/* Generational write barrier. Records that `count` value-sized slots starting
   at `start` may now hold pointers into the nursery, so a young collection
   scans them. Called from caml_modify/write_barrier (count 1, slot-based —
   OCaml hands a field address, not the object), caml_initialize, and array
   blits. Self-gated: a no-op unless an MMTk generational plan is active. */
void caml_mmtk_region_barrier(volatile value *start, mlsize_t count)
{
  if (caml_mmtk_enabled && caml_mmtk_generational)
    mmtk_ocaml_region_barrier(Caml_state->mmtk_mutator, (uintptr_t) start,
                              (size_t) count);
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

/* Reset a domain's young_limit (un-poison) after the collection.

   In TLAB mode, also discard the domain's young region so it refills a fresh
   MMTk block on its next allocation. This is essential for correctness: a GC may
   have relocated (moving plans) or reclaimed lines around the objects the domain
   already placed in its current block, so the unused tail [young_start, young_ptr)
   and the block pointers themselves can no longer be trusted. The live objects
   already allocated survived via root tracing (and had their references fixed up
   if moved); we simply stop bumping into the stale block. Setting
   young_ptr = young_start makes the next fast-path allocation trap to
   caml_alloc_small_dispatch, which refills. Safe to do from the GC worker here:
   all mutators are stopped. */
void caml_mmtk_uninterrupt(uintnat domain_state_addr)
{
  caml_domain_state *d = (caml_domain_state *) domain_state_addr;
  if (caml_mmtk_tlab) {
    d->young_ptr             = d->young_start;
    d->young_trigger         = d->young_start;
    d->memprof_young_trigger = d->young_start;
  }
  caml_reset_young_limit(d);
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
