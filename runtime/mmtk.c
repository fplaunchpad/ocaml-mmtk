/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*             KC Sivaramakrishnan, FP Launchpad, IIT Madras              */
/*                                                                        */
/*   Copyright 2026 FP Launchpad, IIT Madras                              */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

/* C glue between the OCaml runtime and the in-tree MMTk binding.
   Compiled into both the bytecode and native runtimes (a COMMON source). */

#define CAML_INTERNALS

#include <stdlib.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
/* usleep: caml_mmtk_quiesce_running_domains poll wait */
#include <unistd.h>

#include "caml/config.h"

/* The MMTk glue is compiled into both the bytecode and native runtimes. The few
   bytecode-interpreter-specific bits (caml_mmtk_alloc_small) are harmless when
   linked into native code (unused). */

#include "caml/mlvalues.h"
#include "caml/custom.h"
#include "caml/domain_state.h"
#include "caml/domain.h"
#include "caml/fiber.h"
#include "caml/fail.h"
#include "caml/finalise.h"
#include "caml/misc.h"
#include "caml/roots.h"
#include "caml/signals.h"
#include "caml/weak.h"
#include "caml/mmtk.h"

/* The in-tree MMTk binding's C ABI (gc/mmtk/include/mmtk_ocaml.h). */
#include "../gc/mmtk/include/mmtk_ocaml.h"

/* Link anchor -- force roots.o into the link. The MMTk binding (Rust staticlib,
   scanning.rs) calls caml_do_roots for per-domain root scanning, but with the
   stock GC removed no C code references it anymore. The link line lists
   libcamlrun/libasmrun before the staticlib, so without a C-side reference the
   linker never pulls roots.o out of the archive and fails with "undefined
   reference to caml_do_roots". mmtk.o is always linked (the C runtime calls
   caml_mmtk_*), so referencing caml_do_roots here forces roots.o to be pulled.
   */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((used))
#endif
static void (*const caml_mmtk_link_anchor)(void) =
  (void (*)(void)) caml_do_roots;

/* Collection-suppression counter (see caml/mmtk.h, runtime/intern.c). MMTk's
   gc_trigger consults caml_mmtk_collection_enabled() via the binding's
   VMCollection::is_collection_enabled; while the count is non-zero no
   collection is triggered. Atomic because concurrent domains may bracket their
   own unmarshals and gc_trigger reads it from other mutator threads. */
static atomic_uintnat caml_mmtk_gc_disabled;

void caml_mmtk_disable_collection(void)
{
  atomic_fetch_add(&caml_mmtk_gc_disabled, 1);
}

void caml_mmtk_enable_collection(void)
{
  atomic_fetch_sub(&caml_mmtk_gc_disabled, 1);
}

int caml_mmtk_collection_enabled(void)
{
  return atomic_load(&caml_mmtk_gc_disabled) == 0;
}

/* Native TLAB / nursery-aliasing: MMTk owns the nursery too. The inlined native
   fast-path bumps an MMTk Immix block (handed over by mmtk_ocaml_refill_tlab);
   when it is exhausted the runtime refills another block instead of running a
   minor GC. No OCaml minor GC, no promotion -- every object is an MMTk object
   from birth. Set for native code at domain init; requires an Immix-family plan
   (Immix/StickyImmix/GenImmix). Read on the allocation slow path, so a plain
   int. */
int caml_mmtk_tlab = 0;

static int caml_mmtk_initialised = 0;
static void *caml_mmtk_frontier_warmer(void *arg);
static caml_domain_state *caml_mmtk_warm_dom0;

/* Whether the active plan collects (anything but NoGC). NoGC must NOT start
   collection: forcing a GC it cannot perform would spin/fail. */
static int caml_mmtk_collects = 0;
/* Whether the active plan is generational (needs the mutator write barrier).
   Read on every mutable pointer write, so keep it a plain int. */
static int caml_mmtk_generational = 0;
/* Whether the active plan is the concurrent collector (ConcurrentImmix), which
   needs the SATB (snapshot-at-the-beginning) deletion write barrier. Read on
   every mutable pointer write, so keep it a plain int. */
static int caml_mmtk_concurrent = 0;
/* Whether the active plan (LXR) uses the coalescing field-logging write barrier
   for reference counting. Like the SATB barrier, the per-slot pre-store hook in
   caml_modify routes to the mutator's installed barrier (a FieldBarrier for
   LXR) via mmtk_ocaml_satb_barrier, which logs the field + buffers the RC
   inc/dec. */
static int caml_mmtk_field_log = 0;
static int caml_mmtk_collection_started = 0;

/* M6: MMTk-native weak-reference / ephemeron / finaliser processing via the
   binding's Scanning::process_weak_refs (weak refs clear, ephemeron data
   releases on dead keys, Gc.finalise/finalise_last + custom-block finalizers
   run). Now ON by default; set MMTK_WEAK_REFS=0 to fall back to the
   conservative caml_mmtk_scan_ephe_roots scheme (keep the whole ephemeron graph
   alive -- never clears). The opt-out is transitional, to be removed with the
   conservative scheme in M9 stage 3. See gc/mmtk/NOTES.md. */
int caml_mmtk_weak_refs = 1;

/* Objects this size (bytes) or larger are routed to MMTk's large object
 * space. Conservative: smaller than the smallest line/block in collecting
 * plans, so it is also correct for Immix later. */
#define CAML_MMTK_LOS_THRESHOLD (16 * 1024)

/* AllocationSemantics codes shared with the Rust ABI (see api.rs). */
#define CAML_MMTK_SEM_DEFAULT   0
#define CAML_MMTK_SEM_LOS       2

static size_t caml_mmtk_los_threshold = CAML_MMTK_LOS_THRESHOLD;
/* MMTK_MEDIUM_NONMOVING=1: route "medium" objects (>= 2056 B = stock's
   Max_young_wosize boundary, < the LOS threshold) to the NonMoving space —
   with the marksweep_as_nonmoving build this is a swept FREE-LIST space,
   i.e. size-class pools: stock OCaml's placement for exactly this size band
   (stock allocates >Max_young_wosize blocks straight into major-heap pools).
   Bump placement for these objects is chaotically pitch-sensitive (matmul
   regimes measured at 57M..903M LLC-loads across layouts); pools are the
   robust fix. ON by default (measured, wpret2 battery + overflow-block
   line-phase rotation in the Immix allocator): matmul reaches the vanilla
   LLC floor (58M) and goes nursery-independent at 1.05-1.08x vanilla
   (knob-off swings 1.25-1.89x with nursery size and draws an
   alignment-lottery promoted layout); bt/kb/LU byte- and cycle-neutral.
   This is stock's Max_young_wosize law: the medium band never transits
   the minor heap. Default ON for Bactrian only (set at init once the plan
   is known): other plans map the NonMoving semantic to the common
   mark-sweep space, a placement not yet measured on them.
   MMTK_MEDIUM_NONMOVING=0/1 overrides either way. */
static int caml_mmtk_medium_nonmoving = 0;
#define CAML_MMTK_SEM_NONMOVING 6
/* MMTK_TEST_MALLOC_MEDIUM=1 — MEASUREMENT INSTRUMENT ONLY, NEVER a default.
   Allocates >=2056B blocks via plain malloc as OUT-OF-HEAP (foreign) objects:
   the GC never traces, moves, or frees them (they leak). This gives Bactrian
   byte-for-byte glibc placement for the medium band — the causal test for
   the matmul residual (SHAPE.md round 18): if exact-fit malloc placement is
   the mechanism, the W gap must collapse under this knob. Only sound for
   workloads whose medium objects carry no outgoing heap pointers that the
   GC must trace (matmul: int arrays; 0 GCs at the operating point). */
static int caml_mmtk_test_malloc_medium = 0;
/* Pitch jitter is ON by default (6 bits = 0..63 line pads before >=2KB bump
   allocations). Measured 2026-08-08 (SHAPE.md W-night): removes matmul-768's
   12x LLC-load set-aliasing (739M -> 69M, vanilla 57M), improves the benign
   pitch too (mm800 140M -> 72M), and is neutral on every other panel bench
   (fillers only precede >=2KB allocations). Output-parity gates passed on the
   full panel + par_binarytrees d=4. MMTK_ALLOC_JITTER=0 disables. */
static int caml_mmtk_alloc_jitter = 6;
static uint64_t caml_mmtk_jitter_state = 0x9E3779B97F4A7C15ull;
static _Atomic uint64_t caml_mmtk_jitter_fills = 0;
/* MMTK_TLAB_PREFETCH=1: on each TLAB refill, software-prefetch the fresh
   block with write intent — top 1 KiB into L1, the rest into L2. Rationale
   (SHAPE.md W-night round 3b): the residual W-tax is allocation-frontier
   STORE stalls — bump stores into never-touched cold lines drain through the
   store buffer at RFO latency (mutator loads hit L1 at 98.9%, yet
   stalls_mem_any doubles vs vanilla). OCaml bumps DOWNWARD from young_end,
   so warming proceeds top-down to match store order. */
static int caml_mmtk_tlab_prefetch = 0;

/* --------------------------------------------------------------------------
   D1 mutator-side GC time (MMTK_MUTATOR_GC_TIME=1).

   The D1 CPU-budget comparison needs GC work attributed the same way on both
   runtimes. Vanilla runs ALL of its GC on the mutator, and its runtime_events
   spans capture it there. Under MMTk, per-thread attribution captures only the
   worker pool: the GC work the MUTATOR does — write barriers, TLAB refills,
   LOS allocations — lands in the mutator bucket and flatters MMTk. Measured on
   church (binarytrees-20): the identical program's "mutator" CPU is 2.5 s under
   MMTk vs 1.65 s under vanilla — that ~0.9 s IS this uncounted GC work.

   So: time the mutator-side GC entry points, report at exit, and let the
   harness add this to worker CPU (G) and subtract it from mutator CPU (W).
   All such work funnels through the helpers in this file — native code makes
   no other GC-related C calls — so wrapping them here is complete.

   Mechanics. TSC pairs (~20 ns/pair), accumulated in PER-DOMAIN plain u64
   slots (each domain writes only its own; no atomics on the hot path), summed
   at exit and converted via a monotonic-clock calibration of the TSC rate.
   Off = one predictable branch per call; armed only by MMTK_MUTATOR_GC_TIME.

   The park subtraction is load-bearing: a TLAB refill or LOS allocation can
   BLOCK FOR AN ENTIRE GC (its block acquisition polls, which can trigger a
   collection and park the mutator). TSC measures wall cycles, and a parked
   thread burns wall time but no CPU — without the subtraction a single
   blocking refill would book a whole multi-ms pause as mutator GC *CPU*. The
   alloc wrappers therefore subtract whatever caml_mmtk_park accumulated inside
   their window. Blocked time is D3's business (the pause log), not D1's.

   x86-64 only (TSC); armed on another arch it reports zero and warns. */
#if defined(__x86_64__)
#include <x86intrin.h>
#define MUT_GC_TSC() __rdtsc()
#else
#define MUT_GC_TSC() ((uint64_t)0)
#endif
#define MUT_GC_DOMS 256   /* slots; domain id masked (collision = summed, benign) */
static int caml_mut_gc_timing = 0;
static uint64_t caml_mut_gc_barrier_tsc[MUT_GC_DOMS];
static uint64_t caml_mut_gc_alloc_tsc[MUT_GC_DOMS];
static uint64_t caml_mut_gc_park_tsc[MUT_GC_DOMS];
static uint64_t caml_mut_gc_tsc0;
static double caml_mut_gc_mono0;

static double caml_mut_gc_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void caml_mut_gc_dump(void)
{
  uint64_t barrier = 0, alloc = 0, park = 0;
  double secs = caml_mut_gc_now() - caml_mut_gc_mono0;
  if (caml_mmtk_alloc_jitter)
    fprintf(stderr, "[mmtk] jitter fillers: %llu\n",
            (unsigned long long)atomic_load_explicit(
                &caml_mmtk_jitter_fills, memory_order_relaxed));
  double hz;
  int i;
  for (i = 0; i < MUT_GC_DOMS; i++) {
    barrier += caml_mut_gc_barrier_tsc[i];
    alloc   += caml_mut_gc_alloc_tsc[i];
    park    += caml_mut_gc_park_tsc[i];
  }
  hz = secs > 0 ? (double)(MUT_GC_TSC() - caml_mut_gc_tsc0) / secs : 0;
  if (hz <= 0) {
    fprintf(stderr, "[mmtk] mutator GC time: unavailable (no TSC)\n");
    return;
  }
  fprintf(stderr,
          "[mmtk] mutator GC time: %.3f ms (barrier %.3f ms, alloc %.3f ms; "
          "parked %.3f ms excluded)\n",
          (double)(barrier + alloc) / hz * 1e3,
          (double)barrier / hz * 1e3,
          (double)alloc / hz * 1e3,
          (double)park / hz * 1e3);
}
/* ------------------------------------------------------------------------ */

static void caml_mmtk_report_copied(void);
static void caml_e1_dump(void);  /* E1 write-barrier counter dump (atexit) */
static void caml_mmtk_dump_pause_log(void);  /* #R1 per-pause dump (atexit) */
/* Held from init to atexit; points into the environment, so it stays valid. */
static const char *caml_mmtk_pause_log_path;

void caml_mmtk_init(void)
{
  if (caml_mmtk_initialised) return;

  /* Default to a collecting plan now that MMTk is always on (NoGC can't sustain
     the runtime). The default is **GenImmix**: a copying nursery (CopySpace)
     over an Immix mature space -- the generational, stock-OCaml-faithful plan.
     OCaml allocates a torrent of short-lived data, so a cheap copying nursery
     is the right default (the M8 benchmarking confirmed plain Immix loses on
     allocation-heavy workloads where a nursery collects the young garbage
     cheaply). Caveat: weak-clear timing under generational plans is a known
     tail (see ROADMAP/NOTES); MMTK_WEAK_REFS=0 is the conservative never-clear
     fallback. Override with MMTK_PLAN=<Immix|StickyImmix|ConcurrentImmix|...>.
     */
  const char *plan = getenv("MMTK_PLAN");
  if (plan == NULL || plan[0] == '\0') plan = "GenImmix";

  /* Heap sizing. Default to a DYNAMIC (MemBalancer) heap so the runtime grows
     on demand like stock OCaml -- a CLI tool needs only a few MB of RSS, and
     memory tracks the live set. The old hard-coded fixed 1 GB heap never
     collected until ~1 GB, so allocation-heavy programs used ~15x stock's
     footprint. Pass 0 to mmtk_ocaml_init to request the dynamic default;
     MMTK_HEAP_SIZE_MB=<MB> still pins a fixed heap for benchmarking/repro. */
  /* 0 => dynamic heap (binding chooses min .. physical RAM) */
  size_t heap_bytes = 0;
  const char *heap_env = getenv("MMTK_HEAP_SIZE_MB");
  if (heap_env != NULL && heap_env[0] != '\0') {
    long v = strtol(heap_env, NULL, 10);
    if (v > 0) heap_bytes = (size_t)v * 1024 * 1024;
  }

  mmtk_ocaml_init(heap_bytes, plan);
  caml_mmtk_initialised = 1;
  caml_mmtk_collects = (strcmp(plan, "NoGC") != 0);
  /* Bactrian (RQ7) is BOTH: a copying nursery (generational barrier) and a
     concurrently-marked mature space (SATB deletion barrier + continuation
     snapshot/lock machinery). Both flags on arms both halves of
     write_barrier(). */
  caml_mmtk_generational = (strcmp(plan, "GenImmix") == 0
                           || strcmp(plan, "StickyImmix") == 0
                           || strcmp(plan, "GenCopy") == 0
                           || strcmp(plan, "Bactrian") == 0);
  caml_mmtk_concurrent = (strcmp(plan, "ConcurrentImmix") == 0
                          || strcmp(plan, "Bactrian") == 0);
  caml_mmtk_field_log = (strcmp(plan, "LXR") == 0);

  /* RQ8 (ocaml-mmtk): turn OFF allocation-time zero-fill UNIVERSALLY, for every
     plan including ConcurrentImmix. OCaml fully initializes every block before
     the next GC-observable safepoint, so MMTk eager-zeroing is a redundant
     double-write (~20% of cycles on alloc-heavy code). This is safe for both
     collector families:
     - STW Immix-family plans (GenImmix/Immix/StickyImmix/GenCopy): a collection
       only happens at a safepoint, by which time OCaml has already written
       every field (the unzeroed-minor-heap discipline), so the GC never reads
       garbage.
     - ConcurrentImmix: it is ALLOCATE-BLACK -- newly-allocated objects are born
       marked, and the concurrent marker does NOT field-scan freshly-allocated
     (black) objects, so the header-written / fields-unwritten window is never
             traced. Hence no-zero is safe here too.
     - Bactrian: both arguments compose. Young objects are never traced by the
       concurrent marker (it skips the nursery), nursery collection is STW at a
       safepoint, and mature allocations during marking are born live/black. Set
       before any allocation (this runs at init, before any domain/mutator is
       bound). EXCEPTION: MarkCompact must keep zeroing ON. It is a Lisp-2
       sliding-compaction plan whose mark / compute-forwarding passes
       reconstruct per-object metadata (the per-object VO bit + reserved
       forwarding header word) by reading object fields across the whole heap;
       the unzeroed-minor-heap discipline above does not cover those reads, so
       with no-zero MarkCompact reads garbage and SIGSEGVs on alloc-heavy
       programs (CLBG binarytrees/mandelbrot/knucleotide). It was never
       validated under no-zero (only the Immix family + ConcurrentImmix were).
     */
  mmtk_ocaml_set_alloc_zeroed(strcmp(plan, "MarkCompact") == 0);

  {
    /* On by default; MMTK_WEAK_REFS=0 opts out to the conservative scheme. */
    const char *wr = getenv("MMTK_WEAK_REFS");
    caml_mmtk_weak_refs = (wr == NULL || wr[0] != '0');
  }

  if (getenv("MMTK_VERBOSE") != NULL) {
    if (heap_bytes == 0)
      fprintf(stderr, "[mmtk] initialised: plan=%s heap=dynamic weak_refs=%d\n",
              plan, caml_mmtk_weak_refs);
    else
      fprintf(stderr, "[mmtk] initialised: plan=%s heap=%zuMiB weak_refs=%d\n",
              plan, heap_bytes / (1024 * 1024), caml_mmtk_weak_refs);
    atexit(caml_mmtk_report_copied);
  }

  if (getenv("MMTK_BARRIER_COUNT") != NULL)
    atexit(caml_e1_dump);

  {
    /* Shape experiments: large-object placement (see caml_mmtk_semantics). */
    const char *lt = getenv("MMTK_LOS_THRESHOLD");
    if (lt != NULL && lt[0] != '\0') {
      long v = atol(lt);
      if (v > 0) caml_mmtk_los_threshold = (size_t)v;
    }
    {
      /* Value = entropy BITS for the line-granular pad (0..2^bits-1 lines).
         "1" (the historical on-switch) means the default 5 bits = 32 lines;
         2..8 select the range explicitly (6 -> 64 lines, up to 4 KiB pads). */
      if (getenv("MMTK_FRONTIER_WARMER") != NULL
          && atoi(getenv("MMTK_FRONTIER_WARMER")) > 0) {
        pthread_t t;
        caml_mmtk_warm_dom0 = Caml_state;
        if (pthread_create(&t, NULL, caml_mmtk_frontier_warmer, NULL) == 0)
          pthread_detach(t);
      }
      if (getenv("MMTK_TEST_MALLOC_MEDIUM") != NULL)
        caml_mmtk_test_malloc_medium = atoi(getenv("MMTK_TEST_MALLOC_MEDIUM"));
      /* Default ON under Bactrian (see caml_mmtk_medium_nonmoving); the env
         var overrides in both directions. */
      if (strcmp(plan, "Bactrian") == 0)
        caml_mmtk_medium_nonmoving = 1;
      if (getenv("MMTK_MEDIUM_NONMOVING") != NULL)
        caml_mmtk_medium_nonmoving = atoi(getenv("MMTK_MEDIUM_NONMOVING"));
      if (getenv("MMTK_TLAB_PREFETCH") != NULL)
        caml_mmtk_tlab_prefetch = atoi(getenv("MMTK_TLAB_PREFETCH"));
      const char *jv = getenv("MMTK_ALLOC_JITTER");
      if (jv != NULL && jv[0] != '\0') {
        int bits = atoi(jv);
        /* 0 disables; 1 = the historical on-switch (5 bits); 2..8 explicit. */
        if (bits == 0) caml_mmtk_alloc_jitter = 0;
        else if (bits >= 16 && bits <= 23) caml_mmtk_alloc_jitter = bits;
        else caml_mmtk_alloc_jitter = (bits >= 2 && bits <= 8) ? bits : 5;
      }
    }
  }

  /* D1 mutator-side GC time accounting (see the block comment up top). */
  if (getenv("MMTK_MUTATOR_GC_TIME") != NULL) {
    caml_mut_gc_timing = 1;
    caml_mut_gc_tsc0 = MUT_GC_TSC();
    caml_mut_gc_mono0 = caml_mut_gc_now();
#if !defined(__x86_64__)
    fprintf(stderr, "[mmtk] mutator GC time: no TSC on this arch; "
                    "figures will read 0\n");
#endif
    atexit(caml_mut_gc_dump);
  }

  /* Per-pause STW records (backlog #R1). MMTK_VERBOSE only reports the SUM of
     pause time, which cannot distinguish many small pauses from a few large
     ones — the distinction the GC-shape comparison turns on. Arming here keeps
     the pause path itself free of any I/O: records accumulate in memory and are
     written once at exit. */
  caml_mmtk_pause_log_path = getenv("MMTK_PAUSE_LOG");
  if (caml_mmtk_pause_log_path != NULL && caml_mmtk_pause_log_path[0] != '\0') {
    mmtk_ocaml_pause_log_enable();
    atexit(caml_mmtk_dump_pause_log);
  }
}

/* Write the per-pause STW records collected during the run. */
static void caml_mmtk_dump_pause_log(void)
{
  int64_t n = mmtk_ocaml_pause_log_dump(caml_mmtk_pause_log_path);
  if (n < 0)
    fprintf(stderr, "[mmtk] pause log: could not write %s\n",
            caml_mmtk_pause_log_path);
  else if (getenv("MMTK_VERBOSE") != NULL)
    fprintf(stderr, "[mmtk] pause log: %lld pauses -> %s\n",
            (long long) n, caml_mmtk_pause_log_path);
}

/* Report how many objects copying collection relocated (Immix defrag, etc.).
   Registered with atexit under MMTK_VERBOSE so we can confirm movement actually
   happened during a run (and exercise the infix-pointer fixup path). */
static void caml_mmtk_report_copied(void)
{
  fprintf(stderr,
          "[mmtk] GCs: %zu (full: %zu), GC time: %llu ms, "
          "objects copied: %zu\n",
          mmtk_ocaml_total_gc_count(),
          mmtk_ocaml_gc_count(),
          (unsigned long long) mmtk_ocaml_gc_time_ms(),
          mmtk_ocaml_objects_copied());
}

/* MMTk is this fork's only garbage collector; it initialises unconditionally at
   the first domain's startup. (NoGC can't sustain the runtime, so the default
   plan is Immix.) */
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

#ifdef NATIVE_CODE
  /* Native code inlines a bump allocator over the young region, so MMTk owns
     the nursery via TLAB nursery-aliasing. This requires the plan's Default
     allocator to be an Immix or a plain bump allocator, which OCaml can
     bump-fill directly:
     - Immix/StickyImmix -- an in-place Immix block (young objects don't move);
     - GenImmix/GenCopy -- the copy-nursery CopySpace bump buffer (a minor GC
       evacuates survivors and hands back a fresh nursery, so young objects
       move);
     - SemiSpace -- the to-space CopySpace bump buffer (whole-heap copy);
     - NoGC -- the never-collected bump space. The C side is allocator-agnostic
       -- caml_mmtk_refill_tlab just receives [start,end) -- and the binding
       (mmtk_ocaml_refill_tlab) picks the right allocator from the plan's
       Default mapping. For a moving plan, young objects move at a collection,
       fixed up via the usual updatable-root scan. MarkSweep (free-list) and
       MarkCompact (its bump allocator reserves a per-object header word and the
       space relies on per-object VO bits, neither of which the inlined fast
       path produces) are NOT supported native -- they abort below. (Bytecode
       allocates through C entry points and is all-MMTk directly, so this is
       native-only.) */
  if (caml_mmtk_refill_tlab(dom, Whsize_wosize(0))) {
    caml_mmtk_tlab = 1;
    if (getenv("MMTK_VERBOSE") != NULL)
      fprintf(stderr, "[mmtk] native nursery: TLAB (MMTk-owned %s block)\n",
              caml_mmtk_generational ? "copy-nursery" : "Immix/bump");
  } else {
    caml_fatal_error(
      "MMTk native code requires a plan whose Default allocator is an Immix or "
      "bump allocator "
      "(Immix/StickyImmix/GenImmix/Bactrian/GenCopy/SemiSpace/NoGC); "
      "MMTK_PLAN=%s has no bump/Immix Default allocator (e.g. MarkSweep's "
      "free-list or MarkCompact's per-object-header bump allocator)",
      getenv("MMTK_PLAN") ? getenv("MMTK_PLAN") : "Immix");
  }
#endif
}

/* MMTK_FRONTIER_WARMER=1: a helper thread that prefetches (write-intent) the
   cache lines JUST BELOW domain 0's bump pointer — the lines the mutator is
   about to allocate into. OCaml bumps DOWNWARD, so the warm window is
   [young_ptr - WINDOW, young_ptr). The mutator's allocation stores then hit
   lines already in (or in flight to) the cache hierarchy instead of paying a
   cold RFO each — the store-frontier mechanism measured in SHAPE.md rounds
   3b/6. Reads are racy-by-design (young_ptr moves; prefetch of any mapped
   line is safe) and the thread only issues prefetches — never stores.
   Single-domain experiment: warms domain 0 only. */
static void *caml_mmtk_frontier_warmer(void *arg)
{
  (void)arg;
#ifdef __linux__
  prctl(PR_SET_NAME, "mmtk-warmer", 0, 0, 0);
#endif
  const size_t WINDOW = 24 * 1024;           /* lines ahead of the frontier */
  for (;;) {
    caml_domain_state *d = caml_mmtk_warm_dom0;
    if (d != NULL) {
      char *hi = (char *)d->young_ptr;
      char *lo = (char *)d->young_start;
      if (hi != NULL && lo != NULL && hi > lo) {
        char *from = hi - WINDOW > lo ? hi - WINDOW : lo;
        for (char *a = (char *)((uintptr_t)from & ~63ull); a < hi; a += 64)
          __builtin_prefetch(a, 1, 2);       /* write intent, into L2 */
      }
    }
    {
      struct timespec ts = { 0, 8000 };      /* ~8us cadence */
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

/* Shape experiments (see SHAPE.md, W-tax mechanisms).
   MMTK_LOS_THRESHOLD (bytes) re-routes "large-ish" objects to the LOS instead
   of the nursery bump path. Rationale: bump allocation places big regular
   objects (matmul's 6 KB rows) at a perfectly regular pitch, which at unlucky
   sizes aliases L2 sets (measured: 12.3x LLC-loads at size 768, collapsing to
   2.5x at 800). Vanilla dodges this by placing Max_young_wosize+ objects into
   size-class pools; setting the threshold to 2056 (= Max_young_wosize in
   bytes, +header) mimics that split with one knob.
   MMTK_ALLOC_JITTER=1 instead de-regularizes the pitch in place: before a
   large-ish DEFAULT-semantics allocation, drop a small pseudo-random garbage
   filler block so consecutive big objects stop sharing a stride. The filler is
   unreachable immediately and dies at the next collection; cost is <2% of the
   affected allocation. */
Caml_inline int caml_mmtk_semantics(mlsize_t wosize)
{
  size_t bytes = (size_t)(Whsize_wosize(wosize)) * sizeof(value);
  if (bytes >= caml_mmtk_los_threshold) return CAML_MMTK_SEM_LOS;
  if (caml_mmtk_medium_nonmoving && bytes >= 2056)
    return CAML_MMTK_SEM_NONMOVING;
  return CAML_MMTK_SEM_DEFAULT;
}

/* xorshift64*; deterministic per process, no clock involved. */
Caml_inline uint64_t caml_mmtk_jitter_next(void)
{
  uint64_t x = caml_mmtk_jitter_state;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  caml_mmtk_jitter_state = x;
  return x * 0x2545F4914F6CDD1Dull;
}

/* Pitch-jitter filler: allocate 1..16 words of immediately-dead Abstract_tag
   garbage ahead of a large-ish bump allocation. Abstract_tag = never scanned,
   so the GC treats it as an opaque dead blob and reclaims it next cycle. */
static void caml_mmtk_jitter_pad(size_t bytes, int sem)
{
  if (caml_mmtk_alloc_jitter && bytes >= 2048
      && (sem == CAML_MMTK_SEM_DEFAULT || sem == CAML_MMTK_SEM_NONMOVING)) {
    /* Entropy in LINES, not words: 8-128 B pads (v1) carry <2 lines of
       set-index entropy — enough to break exact pitch alignment (768) but
       enough to CREATE near-alignments where the natural pitch was benign
       (mm800: LLC-loads 140M -> 217M). 0..31 lines spreads consecutive
       large objects across 32 L2 sets; successive pads accumulate, so
       absolute offsets decorrelate as a random walk. */
    mlsize_t pad;
    if (caml_mmtk_alloc_jitter == 24 || caml_mmtk_alloc_jitter == 25) {
      /* Rotating deterministic pad: step the target cache-set position by one
         line per large allocation (mode 24: 64 positions ~ avg 31.5 lines
         waste; mode 25: 16 positions ~ avg 7.5 lines ~ 8% on 6KB objects).
         Deterministic spread without randomness; robust to the 32KB
         block-alignment phase reset that defeats constant pads (each block's
         interior continues the rotation, so blocks get different phases). */
      static _Atomic unsigned caml_mmtk_rot = 0;
      unsigned mask = caml_mmtk_alloc_jitter == 24 ? 63 : 15;
      unsigned k = atomic_fetch_add_explicit(&caml_mmtk_rot, 1,
                                             memory_order_relaxed) & mask;
      if (k == 0) return;
      pad = 8 * (mlsize_t)k - 1;
    } else if (caml_mmtk_alloc_jitter >= 16) {
      /* Deterministic mode (MMTK_ALLOC_JITTER=17..23): a FIXED pad of L =
         (value-16) cache lines before every >=2KB allocation. A constant
         odd-line total pitch steps the cache set index by an odd amount per
         object — coprime with every power-of-two set count (L1/L2/L3) — so
         column walks over same-sized rows cover sets uniformly, and unlike
         the random mode the pitch stays constant (stride-predictable). */
      int lines = caml_mmtk_alloc_jitter - 16;
      if (lines <= 0) return;
      pad = 8 * (mlsize_t)lines - 1;    /* filler + header = exactly L lines */
    } else {
      pad = 1 + 8 * (mlsize_t)(caml_mmtk_jitter_next()
                               & ((1u << caml_mmtk_alloc_jitter) - 1));
    }
    /* The pad must land in the SAME allocation stream as the object it
       jitters: under medium pretenuring the >=2056B band bumps through the
       mature Immix allocator, and a pad diverted to the nursery would leave
       that stream at exact pitch (and litter the nursery with dead blobs). */
    (void)mmtk_ocaml_alloc(Caml_state->mmtk_mutator, pad, Abstract_tag, sem);
    atomic_fetch_add_explicit(&caml_mmtk_jitter_fills, 1,
                              memory_order_relaxed);
  }
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
  /* Minor-words accounting (Gc.minor_words / Gc.counters). The bytecode small
     path allocates straight through MMTk and never touches the young region, so
     caml_gc_minor_words_unboxed's live (young_end - young_ptr) term is always 0
     here -- the only allocation odometer is stat_minor_words. Bump it by this
     block's full size (header + fields). This is the bytecode analogue of the
     native fast path's young_ptr bump (which is accounted at block retirement,
     see caml_mmtk_refill_tlab / caml_mmtk_uninterrupt). One add per object:
     cheap, and bytecode allocation is not a tight native loop. */
  Caml_state->stat_minor_words += Whsize_wosize(wosize);
  return (value)p;
}

value caml_mmtk_alloc_shr(mlsize_t wosize, tag_t tag, reserved_t reserved)
{
  void *p;
  uint64_t t0 = 0, p0 = 0;
  int timed = caml_mut_gc_timing;
  int slot = Caml_state->id & (MUT_GC_DOMS - 1);
  int sem = caml_mmtk_semantics(wosize);
  (void)reserved;
  if (caml_mmtk_test_malloc_medium
      && (size_t)(Whsize_wosize(wosize)) * sizeof(value) >= 2056) {
    header_t *hp = malloc(Bhsize_wosize(wosize));
    if (hp != NULL) {
      *hp = Caml_out_of_heap_header(wosize, tag);
      return Val_hp(hp);
    }
  }
  if (timed) { p0 = caml_mut_gc_park_tsc[slot]; t0 = MUT_GC_TSC(); }
  caml_mmtk_jitter_pad((size_t)Whsize_wosize(wosize) * sizeof(value), sem);
  p = mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag, sem);
  if (timed)
    /* Subtract any time spent PARKED for a GC this allocation triggered:
       parked wall is not mutator CPU (see the block comment up top). */
    caml_mut_gc_alloc_tsc[slot] +=
      (MUT_GC_TSC() - t0) - (caml_mut_gc_park_tsc[slot] - p0);
  if (p == NULL) caml_raise_out_of_memory();
  return (value)p;
}

/* Non-raising variant of caml_mmtk_alloc_shr: returns (value)0 on exhaustion
   instead of raising. The unmarshaller uses it so it can run intern_cleanup
   (freeing its state and re-enabling collection) before raising Out_of_memory,
   exactly as the stock caml_shared_try_alloc path does. */
value caml_mmtk_try_alloc_shr(mlsize_t wosize, tag_t tag)
{
  void *p;
  uint64_t t0 = 0, p0 = 0;
  int timed = caml_mut_gc_timing;
  int slot = Caml_state->id & (MUT_GC_DOMS - 1);
  int sem = caml_mmtk_semantics(wosize);
  if (timed) { p0 = caml_mut_gc_park_tsc[slot]; t0 = MUT_GC_TSC(); }
  caml_mmtk_jitter_pad((size_t)Whsize_wosize(wosize) * sizeof(value), sem);
  p = mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag, sem);
  if (timed)
    caml_mut_gc_alloc_tsc[slot] +=
      (MUT_GC_TSC() - t0) - (caml_mut_gc_park_tsc[slot] - p0);
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
  uint64_t t0 = 0, p0 = 0;
  int timed = caml_mut_gc_timing;
  int slot = dom->id & (MUT_GC_DOMS - 1);
  size_t min_bytes = (size_t)whsize * sizeof(value);
  if (min_bytes == 0) min_bytes = sizeof(value);

  if (timed) { p0 = caml_mut_gc_park_tsc[slot]; t0 = MUT_GC_TSC(); }
  if (!mmtk_ocaml_refill_tlab(dom->mmtk_mutator, min_bytes, &start, &end)) {
    if (timed)
      caml_mut_gc_alloc_tsc[slot] +=
        (MUT_GC_TSC() - t0) - (caml_mut_gc_park_tsc[slot] - p0);
    return 0;
  }
  if (timed)
    caml_mut_gc_alloc_tsc[slot] +=
      (MUT_GC_TSC() - t0) - (caml_mut_gc_park_tsc[slot] - p0);

  /* Minor-words accounting: the block we are about to replace is retired here.
     The words it consumed (young_end - young_ptr, a downward bump from
     young_end) have been reported live by caml_gc_minor_words_unboxed's
     (young_end - young_ptr) term; fold them into stat_minor_words now, BEFORE
     repointing young_* at the fresh block, so the odometer is preserved across
     the swap. Invariant kept by every retirement point: total_minor_words ==
     stat_minor_words + Wsize_bsize(young_end-young_ptr) The new block starts
     with young_ptr == young_end (consumed 0), so the live term reads 0 and the
     words just moved into stat. Guard the first-ever refill (young_end == NULL
     at domain init): nothing consumed yet. */
  if (dom->young_end != NULL)
    dom->stat_minor_words +=
      Wsize_bsize((char*)dom->young_end - (char*)dom->young_ptr);

  if (caml_mmtk_tlab_prefetch) {
    char *base = (char *)start, *top = (char *)end;
    char *l1_floor = top - 1024 > base ? top - 1024 : base;
    char *a = top - 64;
    for (; a >= l1_floor; a -= 64) __builtin_prefetch(a, 1, 3);
    for (; a >= base; a -= 64)     __builtin_prefetch(a, 1, 2);
  }

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
   via ephe_info -- so without this, MMTk would treat them as dead, collect/move
   them, and leave dangling pointers in the lists that crash any later ephemeron
   walk (e.g. Gc.full_major). Until proper MMTk weak-reference processing exists
   (ROADMAP workstream E), keep the whole ephemeron graph alive and
   pointer-updated by reporting each block's fields (link, data, keys) and the
   list heads as ordinary roots. This is memory-safe but conservative: weak
   references never clear (the same tradeoff as keeping finalisable values
   alive).

   Called per domain from the binding's root scan
   (scan_roots_in_mutator_thread), alongside caml_do_roots. */
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

/* -- M6: MMTk-native weak reference / ephemeron processing (experimental)
   ------ Driven by the binding's Scanning::process_weak_refs when
   MMTK_WEAK_REFS=1. Mirrors the stock major GC's two-phase scheme -- ephe_mark
   (major_gc.c) then caml_ephe_clean (weak.c) -- but queries MMTk reachability
   (is_reachable) and relocation (forward) instead of the stock mark bits, and
   resurrects retained data via the MMTk tracer (retain). All callbacks operate
   on whole-object `value`s; the caller (Rust) closes them over the GC worker's
   tracer.

   Object liveness uses the post-strong-closure state: is_reachable(v) is true
   iff the strong transitive closure reached v; forward(v) returns v's current
   address (the new one if a moving plan relocated it, else v); retain(v) traces
   v (keeping it and its closure alive) and returns its current address.
   Interior (infix) pointers are normalised to their block base before querying,
   since MMTk reasons about object starts. The callback typedefs live in
   caml/mmtk.h. */

/* Normalise a possibly-infix pointer to the containing block's base. */
Caml_inline value caml_mmtk_block_base(value v)
{
  if (Is_block(v) && Tag_val(v) == Infix_tag) v -= Infix_offset_val(v);
  return v;
}

/* One marking pass over a single ephemeron list (head at *headp). For each
   reachable ephemeron whose data is non-trivial and not yet retained, retain
   the data iff every block key is reachable. Does NOT clear keys (that is the
   clean pass). Rewrites the list links to forwarded addresses and drops
   unreachable ephemerons from the chain. Returns 1 if any data was newly
   retained. */
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
      /* Keep a dead ephemeron LINKED for now -- a finaliser may resurrect this
         block before the clean pass (PR#5233: a finalised weak array).
         Unlinking it here would orphan it from ephe_info so its weak slots
         would never clear after resurrection, leaving dangling pointers. The
         clean pass (which runs after finaliser resurrection) unlinks the ones
         still dead by then. */
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

/* One clean pass over a single ephemeron list (after the mark fixpoint). For
   each reachable ephemeron: forward surviving block keys; clear (to
   caml_ephe_none) any key whose referent is unreachable, and if any key died,
   clear the data too; otherwise forward the (already-retained) data. Drops
   unreachable ephemerons. */
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
        /* keep infix */
        Field(cur, i) = (fwd == base) ? key : fwd + (key - base);
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

/* Custom-block finalizers (Custom_operations.finalize). Stock OCaml runs these
   on shared-heap sweep; under MMTk we register each finalizable custom block on
   MMTk's finalizer queue at allocation, and drain the now-dead ones at a
   safepoint. Gated by MMTK_WEAK_REFS (M6); a no-op otherwise, so the default
   keeps today's behaviour (custom finalizers don't run under MMTk). See
   gc/mmtk/NOTES.md. */
void caml_mmtk_register_finalizable(value v)
{
  if (caml_mmtk_weak_refs)
    mmtk_ocaml_add_finalizer((const void *) v);
}

/* True iff [v] is currently the subject of a user Gc.finalise on this domain --
   it sits in the first/last finalisable tables or has already been queued into
   the run queue (todo_head) by caml_mmtk_final_update_first this cycle. Used to
   defer the custom-block finalize a cycle when a user finaliser also targets
   the block (see caml_mmtk_run_custom_finalizers). */
static int caml_mmtk_value_has_user_finaliser(value v)
{
  struct caml_final_info *fi = Caml_state->final_info;
  if (fi == NULL) return 0;
  struct finalisable *sets[2] = { &fi->first, &fi->last };
  for (int s = 0; s < 2; s++) {
    struct finalisable *final = sets[s];
    for (uintnat i = 0; i < final->young; i++)
      if (final->table[i].val == v) return 1;
  }
  for (struct final_todo *todo = fi->todo_head; todo != NULL; todo = todo->next)
    for (int i = 0; i < todo->size; i++)
      if (todo->item[i].val == v) return 1;
  return 0;
}

/* Drain MMTk's ready-to-finalize queue and run each block's finalize op. Called
   at a safepoint from caml_final_do_calls (post-GC, via the action-pending flag
   set in caml_mmtk_uninterrupt). The objects are resurrected/valid for the
   call; after it they are dropped and reclaimed on a later GC.

   GH#11: a custom block that ALSO has a user Gc.finalise (e.g. an in_channel
   with `Gc.finalise close_in`) must NOT have its custom finalize run in the
   SAME cycle the user finaliser runs -- stock OCaml spreads them over two sweep
   cycles. Under MMTk both become ready in one cycle: mmtk-core's Finalization
   (FinalRefClosure) queues the dead block BEFORE process_weak_refs
   (VMRefClosure) resurrects it for the user finaliser. Running both here
   (custom queue first, in caml_final_do_calls) makes caml_finalize_channel
   destroy the channel mutex + free the struct, then the user close_in try_locks
   the freed mutex -> EINVAL -> "try_lock: Invalid argument" abort. So if the
   popped block is also pending in a user finaliser, DEFER it: put it back on
   MMTk's finalizer queue (it was resurrected this cycle, so it is live and
   valid to re-register) and skip running its custom finalize now. The user
   finaliser runs this cycle on the still-valid block (close_in only sets fd=-1;
   it does not free the struct/mutex). A later GC, once the user finaliser has
   dropped the reference, re-queues the now-unreachable block and runs its
   custom finalize cleanly -- mirroring stock's two-cycle separation. */
void caml_mmtk_run_custom_finalizers(void)
{
  if (!caml_mmtk_weak_refs) return;
  uintptr_t p;
  while ((p = mmtk_ocaml_poll_finalizable()) != 0) {
    value v = (value) p;
    if (caml_mmtk_value_has_user_finaliser(v)) {
      /* Defer to a later cycle: re-register (push to candidates, not the ready
         queue, so it is not re-popped this drain) and let the user finaliser
         run first. */
      mmtk_ocaml_add_finalizer((const void *) v);
      continue;
    }
    void (*final_fun)(value) = Custom_ops_val(v)->finalize;
    if (final_fun != NULL) final_fun(v);
  }
}

/* Fill the heap-size fields of Gc.stat from MMTk's accounting (page-granular).
   Under MMTk the stock shared-heap counters are ~0 (the stock heap is
   bypassed), so Gc.stat would otherwise report a near-empty heap. `*live_words`
   is the in-use pages (a proxy for live data, not exact live bytes);
   `*collections` is MMTk's GC count. Words = bytes / sizeof(value). */
void caml_mmtk_gc_stats(uintnat *heap_words, uintnat *live_words,
                        uintnat *free_words, uintnat *collections)
{
  *heap_words  = mmtk_ocaml_total_bytes() / sizeof(value);
  *live_words  = mmtk_ocaml_used_bytes()  / sizeof(value);
  *free_words  = mmtk_ocaml_free_bytes()  / sizeof(value);
  *collections = mmtk_ocaml_gc_count();
}

/* Total bytes reserved by MMTk for the heap. Replaces the stock
   caml_heap_size(shared_heap) query now that the stock shared heap is gone. */
uintnat caml_mmtk_heap_size_bytes(void)
{
  return mmtk_ocaml_total_bytes();
}

/* Service an explicit `Gc` collection request (Gc.major / full_major /
   compact). Under MMTk the stock major-GC machinery (caml_finish_major_cycle)
   must NOT run -- it operates on the bypassed stock shared heap and corrupts
   state (observed: channel/custom-block corruption -> crash under a moving
   plan). Instead trigger a real MMTk collection on the calling domain and block
   until it completes. No-op for NoGC (cannot collect) and when MMTk is
   disabled. */
void caml_mmtk_collect(void)
{
  if (caml_mmtk_collects)
    mmtk_ocaml_handle_user_collection_request((uintptr_t) Caml_state);
}

/* Forced MINOR collection (non-exhaustive): under the generational plans this
   is a nursery GC -- enough to promote a global-rooted young value out of the
   nursery, without the whole-heap trace caml_mmtk_collect forces. Used by the
   domain-termination result-promotion path (domain.c sync_and_terminate, GH#3):
   the old exhaustive collect there cost ONE FULL STW GC PER Domain TERMINATION,
   the dominant multi-domain scaling pathology on spawn-heavy programs
   (SCALABILITY.md UPDATE 4/5). */
void caml_mmtk_collect_minor(void)
{
  if (caml_mmtk_collects)
    mmtk_ocaml_handle_user_minor_collection_request((uintptr_t) Caml_state);
}

/* True iff `v` is a heap block currently residing in the generational nursery
   (young space). False for immediates, mature blocks, non-generational plans,
   and NoGC. Used by sync_and_terminate (issue #31) to verify the domain result
   was promoted out of the nursery before it is published to the joiner. */
int caml_mmtk_is_young(value v)
{
  if (!caml_mmtk_collects || !Is_block(v)) return 0;
  return mmtk_ocaml_is_in_nursery((const void *) v) ? 1 : 0;
}

/* LXR (issue #31): durably RC-pin the domain result chain `v` (and its
   transitive children) at domain termination, so it survives this domain's own
   nursery-block sweep/reuse until the joiner reads it via term_sync.state.
   Under LXR the tracing-plan promotion in sync_and_terminate is inert (LXR is
   non-generational: caml_mmtk_is_young always returns 0, so its retry loop is a
   no-op; and the forced caml_mmtk_collect coalesces past the result's
   global-root scan), so without this the result is swept at RC 0 -> SIGSEGV in
   Domain.join. No-op on the tracing/generational plans (they keep the result
   alive via caml_mmtk_collect instead) and when MMTk cannot collect. */
void caml_mmtk_keep_alive(value v)
{
  if (caml_mmtk_collects && Is_block(v))
    mmtk_ocaml_lxr_keep_alive((const void *) v);
}

/* Generational write barrier. Records that `count` value-sized slots starting
   at `start` may now hold pointers into the nursery, so a young collection
   scans them. Called from caml_modify/write_barrier (count 1, slot-based --
   OCaml hands a field address, not the object), caml_initialize, and array
   blits. Self-gated: a no-op unless an MMTk generational plan is active. */
/* E1 (RQ1 finding 1): plan-independent write-barrier instrumentation. Counts
   the program's intrinsic pointer-mutation volume (the LXR field-log barrier
   fires exactly on these) vs init writes, to evidence "OCaml is
   init-write-dominated -> the barrier rarely fires". Plain (non-atomic) longs:
   single-domain measurement only. Dumped at exit when MMTK_BARRIER_COUNT is
   set. caml_e1_modify/caml_e1_init are bumped from runtime/memory.c
   (caml_modify / caml_initialize). */
/* satb_barrier invocations (all mutation paths) */
unsigned long caml_e1_satb_calls = 0;
/* mutated slots = LXR barrier fires (sum of count) */
unsigned long caml_e1_satb_slots = 0;
/* caml_modify calls (the generic pointer-mutation) */
unsigned long caml_e1_modify = 0;
/* caml_initialize calls (mature init writes) */
unsigned long caml_e1_init = 0;

/* Registered via atexit under MMTK_BARRIER_COUNT (a destructor attribute gets
   dead-stripped out of the static libasmrun archive; atexit does not). */
static void caml_e1_dump(void)
{
  fprintf(stderr,
          "[E1-BARRIER] satb_fires=%lu satb_calls=%lu "
          "caml_modify=%lu caml_initialize=%lu\n",
          caml_e1_satb_slots, caml_e1_satb_calls, caml_e1_modify, caml_e1_init);
}

void caml_mmtk_region_barrier(volatile value *start, mlsize_t count)
{
  uint64_t t0 = 0;
  int timed = caml_mut_gc_timing;
  if (timed) t0 = MUT_GC_TSC();
  if (caml_mmtk_generational)
    mmtk_ocaml_region_barrier(Caml_state->mmtk_mutator, (uintptr_t) start,
                              (size_t) count);
  if (timed)
    caml_mut_gc_barrier_tsc[Caml_state->id & (MUT_GC_DOMS - 1)] +=
      MUT_GC_TSC() - t0;
}

/* SATB (snapshot-at-the-beginning) deletion write barrier for the concurrent
   plan (ConcurrentImmix). Greys the OLD referents held in `count` value-sized
   slots at `start` so concurrent marking still reaches an object whose only
   live edge is about to be overwritten. MUST be called BEFORE the store, while
   the slots still hold the old values (the snapshot). Self-gated: a no-op
   unless the concurrent plan is active. Called from write_barrier (caml_modify,
   count 1) and the array-fill paths (before the fill loop). */
void caml_mmtk_satb_barrier(volatile value *start, mlsize_t count)
{
  /* Fires for ConcurrentImmix (SATB delete barrier) AND LXR (field-logging RC
     barrier): both route the pre-store slot to the mutator's installed Barrier
     (SATBBarrier vs FieldBarrier), which dispatches the correct slow path. Only
     called on MUTATIONS (caml_modify / atomic exchange/cas) where the old value
     is valid -- never on caml_initialize, so LXR never logs an initialising
     write as a mutation. */
  uint64_t t0 = 0;
  int timed = caml_mut_gc_timing;
  if (timed) t0 = MUT_GC_TSC();
  /* E1: count every mutation-barrier entry (plan-independent) */
  caml_e1_satb_calls++;
  /* E1: total mutated slots = LXR field-log barrier fires */
  caml_e1_satb_slots += count;
  if (caml_mmtk_concurrent || caml_mmtk_field_log)
    mmtk_ocaml_satb_barrier(Caml_state->mmtk_mutator, (uintptr_t) start,
                            (size_t) count);
  if (timed)
    caml_mut_gc_barrier_tsc[Caml_state->id & (MUT_GC_DOMS - 1)] +=
      MUT_GC_TSC() - t0;
}

/* Per-continuation scan lock (concurrent plan). Held by a GC worker while it
   scans a continuation's suspended fiber stack; the resume path acquires it
   (blocking) before switching onto that stack, so a resume cannot race the
   concurrent scan. Self-gated: a no-op for every non-concurrent plan (STW
   collectors scan stacks at a safepoint with mutators stopped, so no resume can
   run concurrently). `cont` is the continuation block; we key the lock on its
   address. */
void caml_mmtk_cont_lock(value cont)
{
  if (caml_mmtk_concurrent)
    mmtk_ocaml_cont_lock((uintptr_t) cont);
}

void caml_mmtk_cont_unlock(value cont)
{
  if (caml_mmtk_concurrent)
    mmtk_ocaml_cont_unlock((uintptr_t) cont);
}

/* SATB snapshot of a continuation's suspended fiber stack, taken on the resume
   path BEFORE the resume deletes the cont->stack edge (field 0 -> NULL via a
   raw CAS that bypasses the write barrier). Mirrors vanilla's caml_darken_cont,
   which scans the stack itself when a resume finds it not-yet-marked. Without
   this, a continuation resumed during concurrent marking before any GC worker
   reached it would have its stack roots lost (FinalMark does not re-scan
   mutator roots under this SATB collector). We walk the stack with
   caml_scan_stack and grey each slot's value into the SATB buffer
   (caml_mmtk_satb_barrier), so the marker keeps those snapshot roots live.
   Greying is idempotent, so a double snapshot (worker + resume) is harmless.
   Only meaningful while concurrent marking is in flight; the caller gates on
   mmtk_ocaml_concurrent_marking_active(). */
static void caml_mmtk_satb_grey_stack_slot(void *fdata, value v,
                                           volatile value *slot)
{
  (void)fdata; (void)v;
  caml_mmtk_satb_barrier(slot, 1);
}

void caml_mmtk_cont_snapshot(value cont)
{
  if (!caml_mmtk_concurrent) return;
  if (!mmtk_ocaml_concurrent_marking_active()) return;
  {
    value stk = Field(cont, 0);
    if (Ptr_val(stk) != NULL)
      caml_scan_stack(caml_mmtk_satb_grey_stack_slot, 0, NULL,
                      Ptr_val(stk), NULL);
  }
}

/* -- Stop-the-world ---------------------------------------------------- */

/* Global epoch for the ragged safepoint (caml_mmtk_quiesce_running_domains).
   Bumped (release) by a quiescing writer; each domain stores the current value
   into its own caml_domain_state.mmtk_seen_quiesce_epoch at every safepoint
   (caml_mmtk_quiesce_ack, called from caml_poll_gc_work) with no lock on the
   hot path. The writer waits until every domain RUNNING at call time has acked
   an epoch >= its bump (or left RUNNING), which proves each in-flight lock-free
   reader has passed a safepoint and dropped any pre-bump transient pointer.
   LIVE: caml_mmtk_quiesce_ack is called from caml_poll_gc_work (domain.c). */
static atomic_uintnat caml_mmtk_quiesce_epoch;

/* Max domains the quiesce snapshot buffer holds. Domains are capped by
   caml_params->max_domains (default 128); 256 is a safe ceiling that keeps the
   snapshot on the C stack. mmtk_ocaml_snapshot_running truncates to this. */
#define CAML_MMTK_QUIESCE_MAX_DOMAINS 256
/* Poll interval while waiting for acks (microseconds). Rare op; a short sleep
   avoids busy-spinning a core during the (typically sub-ms) grace period. */
#define CAML_MMTK_QUIESCE_POLL_US 50

/* Hot-path ack: record that this domain has reached a safepoint. Plain atomic
   store, no STW lock. Called from caml_poll_gc_work (both the TLAB and bytecode
   paths) right where the domain self-clears its young_limit poison, so a
   poisoned domain that trapped to the safepoint records its passage in the same
   place it un-poisons. Release-ordered so a reader's prior loads of the (now
   stale) shared state are ordered before the ack the writer observes. No-op
   cost when no quiesce is in flight (a single store of the global epoch). */
void caml_mmtk_quiesce_ack(caml_domain_state *d)
{
  atomic_store_release(&d->mmtk_seen_quiesce_epoch,
                       atomic_load_acquire(&caml_mmtk_quiesce_epoch));
}

/* Cooperatively wait out an in-progress collection: drop the domain lock (which
   removes this domain from MMTk's RUNNING set, so the collector no longer
   awaits it -- exactly what a C blocking section does), mark STOPPED and wait
   for the MMTk resume epoch, then re-acquire the lock. Does NOT re-mark RUNNING
   -- the caller does that via caml_mmtk_become_running (so the RUNNING
   transition and the GC-active check stay atomic w.r.t. the next collection).

   (Under always-on MMTk, stop_all_mutators is the sole all-domains rendezvous.
   OCaml's own STW was retired, so there is no second barrier to deadlock
   against and no backup thread to hand participation to -- releasing the lock
   suffices.) */
static void caml_mmtk_cooperative_park(uintnat domain_state_addr)
{
  caml_release_domain_lock();
  mmtk_ocaml_stw_park(domain_state_addr);   /* mark STOPPED, wait for resume */
  caml_acquire_domain_lock();
}

/* Transition this domain to RUNNING (a must-stop STW participant). If a
   collection is active, mmtk_ocaml_try_mark_running refuses and we park
   cooperatively (above) -- releasing the domain lock so the collector does not
   wait on us -- then retry. We must NOT just spin on "GC active" while holding
   the domain lock: that would keep this domain in the RUNNING set, wedging
   stop_all_mutators on running.is_empty() forever. Used on every
   STOPPED->RUNNING edge: resume from park, leave a blocking section, and a
   child starting OCaml. */
void caml_mmtk_become_running(uintnat domain_state_addr)
{
  while (!mmtk_ocaml_try_mark_running(domain_state_addr))
    caml_mmtk_cooperative_park(domain_state_addr);
}

/* Park the calling domain for an in-progress MMTk collection, then resume as a
   RUNNING participant. Called from block_for_gc (the triggering domain) and the
   safepoint poll. */
void caml_mmtk_park(uintnat domain_state_addr)
{
  uint64_t t0 = 0;
  int timed = caml_mut_gc_timing;
  int slot = ((caml_domain_state *)domain_state_addr)->id & (MUT_GC_DOMS - 1);
  if (timed) t0 = MUT_GC_TSC();
  caml_mmtk_cooperative_park(domain_state_addr);
  caml_mmtk_become_running(domain_state_addr);
  if (timed) caml_mut_gc_park_tsc[slot] += MUT_GC_TSC() - t0;
}

/* Called from caml_handle_gc_interrupt at every safepoint. If MMTk has a
   collection in progress, park this domain (roots are already published by the
   safepoint, e.g. Setup_for_event) until the collection finishes. */
void caml_mmtk_stw_poll(void)
{
  if (mmtk_ocaml_stw_active()) {
    caml_mmtk_park((uintnat) Caml_state);
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
   MMTk block on its next allocation. This is essential for correctness: a GC
   may have relocated (moving plans) or reclaimed lines around the objects the
   domain already placed in its current block, so the unused tail [young_start,
   young_ptr) and the block pointers themselves can no longer be trusted. The
   live objects already allocated survived via root tracing (and had their
   references fixed up if moved); we simply stop bumping into the stale block.
   Setting young_ptr = young_start makes the next fast-path allocation trap to
   caml_alloc_small_dispatch, which refills. Safe to do from the GC worker here:
   all mutators are stopped. */
void caml_mmtk_uninterrupt(uintnat domain_state_addr)
{
  caml_domain_state *d = (caml_domain_state *) domain_state_addr;
  if (caml_mmtk_tlab) {
    /* Minor-words accounting: a collection discards this domain's current block
       (the unused tail and the block pointers can no longer be trusted -- see
       below). The words it consumed (young_end - young_ptr) were reported live
       by caml_gc_minor_words_unboxed; fold them into stat_minor_words now so
       the odometer survives the discard.

       Double-count hazard: the discard below sets young_ptr = young_start so
       the next allocation traps and refills. If we left young_end pointing at
       the old block, the live term Wsize_bsize(young_end - young_ptr) would
       then read the WHOLE block (young_end - young_start) -- re-adding the
       consumed words AND counting the never-allocated tail. We therefore also
       collapse the live range by setting young_end = young_start, so the live
       term reads 0 and the invariant total == stat +
       Wsize_bsize(young_end-young_ptr) still holds. The block is fully
       discarded (young_start == young_end == young_ptr); the next fast-path
       alloc traps to caml_alloc_small_dispatch and refills. */
    d->stat_minor_words +=
      Wsize_bsize((char*)d->young_end - (char*)d->young_ptr);
    d->young_end             = d->young_start;
    d->young_ptr             = d->young_start;
    d->young_trigger         = d->young_start;
    d->memprof_young_trigger = d->young_start;
    /* Leave the young region COLLAPSED (young_start == young_end == young_ptr);
       the mutator refills its own TLAB at its next allocation
       (caml_alloc_small_dispatch, a normal mutator safepoint, outside any GC
       lock). We must NOT drive the allocator here. This runs inside
       resume_mutators (binding/src/collection.rs), which MMTk calls from
       on_gc_finished while holding the WorkerMonitorSync lock.
       caml_mmtk_refill_tlab can hit a full space -> Space::acquire ->
       GCTrigger::poll -> request_schedule_collection ->
       WorkerMonitor::make_request, which re-takes that same lock -> a GC worker
       self-deadlocks on a lock it already holds (rr/core-confirmed via the
       deterministic ocamldoc man-gen hang; gc/mmtk/NOTES.md). The sibling MMTk
       bindings (mmtk-openjdk/julia/ruby) all resume WITHOUT touching the
       allocator, for exactly this reason -- resume_mutators only unblocks
       mutators. (The fft poll-trap micro-perf an eager refill once avoided -- a
       collapsed region keeps young_ptr at young_limit so an alloc-light hot
       loop traps at every poll -- must be re-homed to the mutator's OWN resume
       path, caml_mmtk_become_running, not this GC-worker hook. TODO; see
       ROADMAP.) caml_reset_young_limit below is still valid for the collapsed
       region. */
  }
  /* A GC just finished -- MMTk's finalizer queue may now hold dead custom
     blocks. Flag pending actions so this domain drains + runs them
     (caml_final_do_calls -> caml_mmtk_run_custom_finalizers) at its next
     safepoint. */
  if (caml_mmtk_weak_refs) caml_set_action_pending(d);
  caml_reset_young_limit(d);
}

/* Ragged safepoint (excise Phase 2, step 1): block the caller until every OCaml
   domain that was RUNNING OCaml at call time has passed one safepoint OR left
   the RUNNING set (parked / blocked / terminated). No global STW barrier, no
   GC. The two future callers (frametables RCU retire; runtime_events ring
   teardown) publish new state, then call this to drain all in-flight lock-free
   readers of the OLD state before freeing it. LIVE: the runtime_events ring
   teardown (runtime_events.c) is the current caller.

   Protocol:
   1. epoch = ++caml_mmtk_quiesce_epoch (release). Domains store this into their
      own mmtk_seen_quiesce_epoch at each safepoint (caml_mmtk_quiesce_ack).
   2. The caller is itself RUNNING and holds its domain lock; if it spun here it
      could deadlock a concurrent GC (which waits for running.is_empty()). So it
      leaves RUNNING for the wait -- caml_mmtk_enter_blocking(self) -- and
      re-enters cooperatively afterwards (caml_mmtk_become_running(self)), the
      exact handoff a C blocking section uses. As the WRITER it reads nothing of
      the old state between publish and free, so dropping RUNNING is
      reader-safe.
   3. Snapshot the RUNNING set NOW (mmtk_ocaml_snapshot_running). Domains in it
      are the in-flight readers we must wait for. (A domain that re-enters
      RUNNING after the bump via try_mark_running was STOPPED during our
      publish, so it cannot hold a pre-bump pointer -- and it is NOT in our
      snapshot, so it never makes us hang.)
   4. Poison every snapshot domain (caml_mmtk_interrupt) so a running one traps
      to its next safepoint and acks. A domain self-clears its OWN young_limit
      at that safepoint (caml_reset_young_limit), so we never call
      caml_mmtk_uninterrupt -- which would clobber a CONCURRENT GC's poison.
   5. Wait until, for every snapshot domain, EITHER mmtk_seen_quiesce_epoch >=
      epoch (it acked) OR it is no longer RUNNING (mmtk_ocaml_is_running == 0:
      parked / blocked / terminated holds no transient reader pointer). Poll on
      a short sleep, re-poisoning stragglers each round (a domain may have
      cleared its poison at an unrelated safepoint before acking our epoch).

   Latency caveat: a domain spinning in a tight allocation-free, poll-free loop
   never reaches a safepoint; the young_limit poison only bites at the next
   allocation or explicit poll. OCaml's bytecode loop polls and native
   back-edges insert poll points, so ordinary code reaches a safepoint promptly,
   but a hand-rolled C busy-loop with no caml_process_pending_actions is a
   (pre-existing, same as GC STW) bounded-latency exception.

   Re-entrancy: the epoch is a single global monotone counter, so two concurrent
   quiescers are individually correct (each waits for acks >= its OWN bump, and
   a later bump only makes earlier waiters' predicate easier). The future
   callers still serialise themselves with their own retire/teardown lock; this
   primitive does not require it for safety. */
void caml_mmtk_quiesce_running_domains(void)
{
  uintnat self = (uintnat) Caml_state;
  uintnat epoch = atomic_fetch_add(&caml_mmtk_quiesce_epoch, 1) + 1;

  /* Leave RUNNING for the wait so a concurrent GC's running.is_empty() barrier
     does not wait on us (we hold our domain lock and are RUNNING). The backup
     thread covers our OCaml-STW participation while we are stopped. */
  caml_mmtk_enter_blocking(self);

  /* Snapshot the domains RUNNING right now -- the in-flight readers to drain.
     Self was just removed from RUNNING by enter_blocking, so it is not awaited.
     */
  uintnat snap[CAML_MMTK_QUIESCE_MAX_DOMAINS];
  size_t n = mmtk_ocaml_snapshot_running(snap, CAML_MMTK_QUIESCE_MAX_DOMAINS);

  for (;;) {
    int all_done = 1;
    for (size_t i = 0; i < n; i++) {
      caml_domain_state *d = (caml_domain_state *) snap[i];
      if (atomic_load_acquire(&d->mmtk_seen_quiesce_epoch) >= epoch)
        continue;               /* acked a safepoint at/after our bump */
      if (!mmtk_ocaml_is_running(snap[i]))
        continue;             /* parked/blocked/gone: holds no reader */
      /* Still RUNNING and not yet acked -- poison so it traps to a safepoint.
         */
      caml_mmtk_interrupt(snap[i]);
      all_done = 0;
    }
    if (all_done) break;
    usleep(CAML_MMTK_QUIESCE_POLL_US);
  }

  /* Re-enter OCaml as a RUNNING participant (parks cooperatively if a GC is now
     active). We do NOT un-poison anyone: each target self-cleared its own
     young_limit at its safepoint, and a global uninterrupt would clobber a
     concurrent GC's poison. */
  caml_mmtk_become_running(self);
}

/* A domain is entering / leaving a C blocking section. While blocking it is
   safe for GC; on leaving it must wait out any in-progress collection.

   `dom` is the domain's caml_domain_state address, captured by the caller
   (runtime/signals.c) while Caml_state was still bound -- it must NOT be read
   from Caml_state here. The blocking-section hooks release/re-acquire the
   domain lock around these calls, which clears/restores Caml_state
   asymmetrically: `caml_enter_blocking_section` calls enter AFTER the hook
   released the lock (Caml_state is NULL), while `caml_leave_blocking_section`
   calls leave AFTER the hook re-acquired it (Caml_state is valid). The previous
   code read Caml_state_opt directly, so the enter found it NULL and skipped the
   `stopped` increment while leave still decremented it -- underflowing the
   usize count to a huge value, making `stop_all_mutators`'s `stopped >= n`
   barrier always true. The GC then never waited for running domains to reach a
   safepoint and scanned the live, mutating roots of a still-running domain,
   handing an immediate/foreign value to trace_object: the `cannot trace object
   0x1` (Val_int 0) panic in the parallel spawn-burn tests. `dom == 0` (no
   domain bound, e.g. caml_open_descriptor_in during early startup) is a no-op,
   and the `mmtk_mutator != NULL` guard keeps enter/leave balanced across
   binding. */
void caml_mmtk_enter_blocking(uintnat dom)
{
  if (dom != 0 && ((caml_domain_state *) dom)->mmtk_mutator != NULL)
    mmtk_ocaml_enter_blocking(dom);
}

void caml_mmtk_leave_blocking(uintnat dom)
{
  if (dom != 0 && ((caml_domain_state *) dom)->mmtk_mutator != NULL)
    caml_mmtk_become_running(dom);
}

/* Called when a domain terminates: deregister it so collections no longer wait
   for it or scan it.

   No park is needed. By the time caml_domain_terminate calls us, the domain has
   left the runtime's STW participant set (stop_active_domain) and is no longer
   executing OCaml, so it is absent from MMTk's RUNNING set -- a collection in
   flight does not wait for it. Deregistering removes it from both the mutator
   registry and the RUNNING set (active_plan::deregister_by_addr). This subsumes
   the former terminate-specific park special-case (the terminating domain no
   longer needs to park at all). The caller still holds domain_lock continuously
   across teardown to keep the slot from being reused mid-teardown -- unchanged
   and orthogonal to MMTk. */
void caml_mmtk_domain_terminate(caml_domain_state *dom)
{
  if (dom->mmtk_mutator == NULL) return;
  /* Deregister first: this removes the domain from BOTH the mutator registry
     (so a collection started from now on will not scan it) AND the RUNNING set
     (so an in-progress stop_all_mutators that is waiting for all running
     domains to stop no longer waits for this one -- it has left OCaml STW too
     and sits in C teardown with no safepoint, exactly the un-stoppable case bug
     #3b is about). */
  mmtk_ocaml_deregister_domain((uintptr_t) dom);
  /* Then, if a collection is in progress, wait for it to finish before
     returning to caml_domain_terminate, which tears this domain's stack/roots
     down. A collection that snapshotted the registry just BEFORE the deregister
     above still holds this domain's mutator pointer and is scanning its roots;
     tearing them down concurrently traced a freed/garbage slot -> the MMTk
     "cannot trace object" panic (bug #3) seen in the spawn-burn tests. Waiting
     keeps the roots valid until that scan completes. We do NOT release
     domain_lock here: the domain has already left OCaml's STW participant set
     (stop_active_domain), so OCaml STW will not wait for us and cannot
     deadlock, while domain_lock must stay held across teardown to keep the slot
     from being reused mid-teardown (domain_create blocks on the same lock). */
  mmtk_ocaml_wait_collection_done();
  dom->mmtk_mutator = NULL;
}

/* Deregister a domain WITHOUT waiting for an in-flight collection -- used by
   caml_stop_all_domains (excise Phase 3b) when the main domain forcibly cancels
   running peers at process exit. Unlike caml_mmtk_domain_terminate, the caller
   does NOT tear the peer's stack/roots down (it was pthread_cancel'd in an
   unknown state and left in memory), so there is nothing to protect with a
   collection-done wait; we only need to remove the dead peer from MMTk's
   mutator registry + RUNNING set so a stop_all_mutators stops awaiting a thread
   that will never reach a safepoint again. Idempotent / no-op if not bound. */
void caml_mmtk_deregister_domain(caml_domain_state *dom)
{
  if (dom->mmtk_mutator == NULL) return;
  mmtk_ocaml_deregister_domain((uintptr_t) dom);
  dom->mmtk_mutator = NULL;
}

/* Wait out the grace period of any in-flight collection (returns at once if
   none is active). C wrapper so the OCaml runtime can RCU-retire memory that a
   GC worker may have snapshotted as a root before freeing it.
   caml_mmtk_domain_terminate above uses the same primitive to keep a
   terminating domain's OWN roots valid until the scanning collection completes;
   free_domain_ml_values in domain.c uses this wrapper to give the per-spawn
   domain_ml_values global-root block the same guarantee (GH#15 Bug B: a
   collection started AFTER caml_domain_terminate's wait may still hold
   &ml_values->callback/&term_sync -- freeing the block underneath it traced a
   dangling slot -> "cannot trace object" panic). */
void caml_mmtk_wait_collection_done(void)
{
  mmtk_ocaml_wait_collection_done();
}
