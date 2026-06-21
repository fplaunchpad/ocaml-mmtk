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
#include "caml/custom.h"
#include "caml/domain_state.h"
#include "caml/domain.h"
#include "caml/fail.h"
#include "caml/misc.h"
#include "caml/roots.h"
#include "caml/signals.h"
#include "caml/weak.h"
#include "caml/mmtk.h"

/* The in-tree MMTk binding's C ABI (gc/mmtk/include/mmtk_ocaml.h). */
#include "../gc/mmtk/include/mmtk_ocaml.h"

/* MMTk is the garbage collector for this fork — the only one. The flag is 0 only
   during the brief early-startup window before MMTk is initialised and the first
   domain's mutator is bound (a handful of pre-init allocations take the stock
   path); it is set to 1 in caml_mmtk_domain_init and stays 1 for the rest of the
   process. There is no opt-out. */
int caml_mmtk_enabled = 0;

/* Native TLAB / nursery-aliasing: MMTk owns the nursery too. The inlined native
   fast-path bumps an MMTk Immix block (handed over by mmtk_ocaml_refill_tlab);
   when it is exhausted the runtime refills another block instead of running a
   minor GC. No OCaml minor GC, no promotion — every object is an MMTk object from
   birth. Set for native code at domain init; requires an Immix-family plan
   (Immix/StickyImmix/GenImmix). Read on the allocation slow path, so a plain int. */
int caml_mmtk_tlab = 0;

static int caml_mmtk_initialised = 0;
/* Whether the active plan collects (anything but NoGC). NoGC must NOT start
   collection: forcing a GC it cannot perform would spin/fail. */
static int caml_mmtk_collects = 0;
/* Whether the active plan is generational (needs the mutator write barrier).
   Read on every mutable pointer write, so keep it a plain int. */
static int caml_mmtk_generational = 0;
static int caml_mmtk_collection_started = 0;

/* M6 (experimental, gated by MMTK_WEAK_REFS=1): drive MMTk-native weak-reference
   / ephemeron processing via the binding's Scanning::process_weak_refs instead of
   the conservative caml_mmtk_scan_ephe_roots scheme (which keeps the whole
   ephemeron graph alive — weak refs never clear). Off by default until validated
   against the weak/ephemeron/finaliser testsuite; see gc/mmtk/NOTES.md (M6 design).
   Read by the binding (scanning.rs) at the start of weak processing. */
int caml_mmtk_weak_refs = 0;

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

  /* Default to a collecting plan now that MMTk is always on (NoGC can't sustain
     the runtime). Immix is the most-validated plan that supports native TLAB
     nursery aliasing; StickyImmix (generational) is the likely perf default, to
     be switched after the benchmarking phase confirms it. */
  const char *plan = getenv("MMTK_PLAN");
  if (plan == NULL || plan[0] == '\0') plan = "Immix";

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

  {
    const char *wr = getenv("MMTK_WEAK_REFS");
    caml_mmtk_weak_refs = (wr != NULL && wr[0] == '1');
  }

  if (getenv("MMTK_VERBOSE") != NULL) {
    fprintf(stderr, "[mmtk] initialised: plan=%s heap=%zuMiB weak_refs=%d\n",
            plan, heap_mb, caml_mmtk_weak_refs);
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

/* MMTk is this fork's only garbage collector; it initialises unconditionally at
   the first domain's startup. (NoGC can't sustain the runtime, so the default plan
   is Immix.) */
void caml_mmtk_domain_init(caml_domain_state *dom)
{
  caml_mmtk_init();
  dom->mmtk_mutator = mmtk_ocaml_bind_mutator((uintptr_t)dom);
  /* For collecting plans, spawn the GC worker threads once (must happen before
     an allocation can trigger a collection). */
  if (caml_mmtk_collects && !caml_mmtk_collection_started) {
    mmtk_ocaml_initialize_collection((uintptr_t)dom);
    caml_mmtk_collection_started = 1;
  }
  caml_mmtk_enabled = 1;

#ifdef NATIVE_CODE
  /* Native code inlines a bump allocator over the young region, so MMTk owns the
     nursery via TLAB nursery-aliasing. This requires the plan to expose an Immix
     Default allocator (Immix / StickyImmix / GenImmix) — the supported set for
     native. (Bytecode allocates through C entry points and is all-MMTk directly,
     so this is native-only.) */
  if (caml_mmtk_refill_tlab(dom, Whsize_wosize(0))) {
    caml_mmtk_tlab = 1;
    if (getenv("MMTK_VERBOSE") != NULL)
      fprintf(stderr, "[mmtk] native nursery: TLAB (MMTk-owned Immix block)\n");
  } else {
    caml_fatal_error(
      "MMTk native code requires an Immix-family plan (Immix/StickyImmix/GenImmix); "
      "MMTK_PLAN=%s has no Immix Default allocator", getenv("MMTK_PLAN") ? getenv("MMTK_PLAN") : "Immix");
  }
#endif
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
      /* Pin the ephemeron block so a moving collection can't relocate it: we
         report its interior fields as root slots just below, and those slot
         addresses must stay valid through the collection. (Inert/no-op under
         non-moving plans.) */
      mmtk_ocaml_pin_object((const void *) e);
      mlsize_t wo = Wosize_val(e);  /* fields: 0 link, 1 data, 2.. keys */
      for (mlsize_t i = 0; i < wo; i++) {
        volatile value *slot = Op_val(e) + i;
        f(fdata, *slot, slot);
      }
    }
  }
}

/* ── M6: MMTk-native weak reference / ephemeron processing (experimental) ──────
   Driven by the binding's Scanning::process_weak_refs when MMTK_WEAK_REFS=1.
   Mirrors the stock major GC's two-phase scheme — ephe_mark (major_gc.c) then
   caml_ephe_clean (weak.c) — but queries MMTk reachability (is_reachable) and
   relocation (forward) instead of the stock mark bits, and resurrects retained
   data via the MMTk tracer (retain). All callbacks operate on whole-object
   `value`s; the caller (Rust) closes them over the GC worker's tracer.

   Object liveness uses the post-strong-closure state: is_reachable(v) is true iff
   the strong transitive closure reached v; forward(v) returns v's current address
   (the new one if a moving plan relocated it, else v); retain(v) traces v (keeping
   it and its closure alive) and returns its current address. Interior (infix)
   pointers are normalised to their block base before querying, since MMTk reasons
   about object starts. The callback typedefs live in caml/mmtk.h. */

/* Normalise a possibly-infix pointer to the containing block's base. */
Caml_inline value caml_mmtk_block_base(value v)
{
  if (Is_block(v) && Tag_val(v) == Infix_tag) v -= Infix_offset_val(v);
  return v;
}

/* One marking pass over a single ephemeron list (head at *headp). For each
   reachable ephemeron whose data is non-trivial and not yet retained, retain the
   data iff every block key is reachable. Does NOT clear keys (that is the clean
   pass). Rewrites the list links to forwarded addresses and drops unreachable
   ephemerons from the chain. Returns 1 if any data was newly retained. */
static int caml_mmtk_ephe_mark_list(value *headp,
                                    caml_mmtk_ephe_reachable_fn is_reachable,
                                    caml_mmtk_ephe_forward_fn forward,
                                    caml_mmtk_ephe_retain_fn retain, void *ctx)
{
  int progress = 0;
  value *linkp = headp;
  value e = *linkp;
  while (e != (value) NULL) {
    int live = is_reachable(e);
    value cur = live ? forward(e) : e;   /* dead objects don't move */
    value next = Ephe_link(cur);
    if (!live) {
      /* Keep a dead ephemeron LINKED for now — a finaliser may resurrect this
         block before the clean pass (PR#5233: a finalised weak array). Unlinking
         it here would orphan it from ephe_info so its weak slots would never clear
         after resurrection, leaving dangling pointers. The clean pass (which runs
         after finaliser resurrection) unlinks the ones still dead by then. */
      linkp = &Ephe_link(cur);
      e = next;
      continue;
    }
    *linkp = cur;                                      /* fix forwarded link */

    value data = Ephe_data(cur);
    if (data != caml_ephe_none && Is_block(data) && !is_reachable(data)) {
      int all_keys_live = 1;
      mlsize_t size = Wosize_val(cur);
      for (mlsize_t i = CAML_EPHE_FIRST_KEY; i < size; i++) {
        value key = Field(cur, i);
        if (key != caml_ephe_none && Is_block(key)
            && !is_reachable(caml_mmtk_block_base(key))) {
          all_keys_live = 0;
          break;
        }
      }
      if (all_keys_live) {
        Ephe_data(cur) = retain(ctx, data);
        progress = 1;
      }
    }
    linkp = &Ephe_link(cur);
    e = next;
  }
  return progress;
}

/* One clean pass over a single ephemeron list (after the mark fixpoint). For each
   reachable ephemeron: forward surviving block keys; clear (to caml_ephe_none) any
   key whose referent is unreachable, and if any key died, clear the data too;
   otherwise forward the (already-retained) data. Drops unreachable ephemerons. */
static void caml_mmtk_ephe_clean_list(value *headp,
                                      caml_mmtk_ephe_reachable_fn is_reachable,
                                      caml_mmtk_ephe_forward_fn forward)
{
  value *linkp = headp;
  value e = *linkp;
  while (e != (value) NULL) {
    int live = is_reachable(e);
    value cur = live ? forward(e) : e;
    value next = Ephe_link(cur);
    if (!live) { *linkp = next; e = next; continue; }
    *linkp = cur;

    int released = 0;
    mlsize_t size = Wosize_val(cur);
    for (mlsize_t i = CAML_EPHE_FIRST_KEY; i < size; i++) {
      value key = Field(cur, i);
      if (key == caml_ephe_none || !Is_block(key)) continue;
      value base = caml_mmtk_block_base(key);
      if (is_reachable(base)) {
        value fwd = forward(base);
        Field(cur, i) = (fwd == base) ? key : fwd + (key - base); /* keep infix */
      } else {
        Field(cur, i) = caml_ephe_none;
        released = 1;
      }
    }
    value data = Ephe_data(cur);
    if (data != caml_ephe_none && Is_block(data)) {
      if (released)
        atomic_store_relaxed(Ephe_data_addr(cur), caml_ephe_none);
      else
        atomic_store_relaxed(Ephe_data_addr(cur), forward(data));
    }
    linkp = &Ephe_link(cur);
    e = next;
  }
}

/* Public entry points for the binding. A domain's ephemerons live on two lists
   (todo + live); process both. */
int caml_mmtk_ephe_mark_pass(uintptr_t domain_addr,
                             caml_mmtk_ephe_reachable_fn is_reachable,
                             caml_mmtk_ephe_forward_fn forward,
                             caml_mmtk_ephe_retain_fn retain, void *ctx)
{
  caml_domain_state *domain = (caml_domain_state *) domain_addr;
  struct caml_ephe_info *ei = domain->ephe_info;
  if (ei == NULL) return 0;
  int p = 0;
  p |= caml_mmtk_ephe_mark_list(&ei->todo, is_reachable, forward, retain, ctx);
  p |= caml_mmtk_ephe_mark_list(&ei->live, is_reachable, forward, retain, ctx);
  return p;
}

void caml_mmtk_ephe_clean_pass(uintptr_t domain_addr,
                               caml_mmtk_ephe_reachable_fn is_reachable,
                               caml_mmtk_ephe_forward_fn forward)
{
  caml_domain_state *domain = (caml_domain_state *) domain_addr;
  struct caml_ephe_info *ei = domain->ephe_info;
  if (ei == NULL) return;
  caml_mmtk_ephe_clean_list(&ei->todo, is_reachable, forward);
  caml_mmtk_ephe_clean_list(&ei->live, is_reachable, forward);
}

/* Custom-block finalizers (Custom_operations.finalize). Stock OCaml runs these on
   shared-heap sweep; under MMTk we register each finalizable custom block on MMTk's
   finalizer queue at allocation, and drain the now-dead ones at a safepoint. Gated
   by MMTK_WEAK_REFS (M6); a no-op otherwise, so the default keeps today's behaviour
   (custom finalizers don't run under MMTk). See gc/mmtk/NOTES.md. */
void caml_mmtk_register_finalizable(value v)
{
  if (caml_mmtk_weak_refs)
    mmtk_ocaml_add_finalizer((const void *) v);
}

/* Drain MMTk's ready-to-finalize queue and run each block's finalize op. Called at
   a safepoint from caml_final_do_calls (post-GC, via the action-pending flag set in
   caml_mmtk_uninterrupt). The objects are resurrected/valid for the call; after it
   they are dropped and reclaimed on a later GC. */
void caml_mmtk_run_custom_finalizers(void)
{
  if (!caml_mmtk_weak_refs) return;
  uintptr_t p;
  while ((p = mmtk_ocaml_poll_finalizable()) != 0) {
    value v = (value) p;
    void (*final_fun)(value) = Custom_ops_val(v)->finalize;
    if (final_fun != NULL) final_fun(v);
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
  /* A GC just finished — MMTk's finalizer queue may now hold dead custom blocks.
     Flag pending actions so this domain drains + runs them (caml_final_do_calls →
     caml_mmtk_run_custom_finalizers) at its next safepoint. */
  if (caml_mmtk_weak_refs) caml_set_action_pending(d);
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
