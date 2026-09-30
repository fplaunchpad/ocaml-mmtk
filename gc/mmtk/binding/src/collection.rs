//! VMCollection for OCaml 5.x — multi-domain stop-the-world coordination.
//!
//! When MMTk collects, every OCaml domain (mutator) that is *running OCaml* must
//! be at a safe point before marking. The crux is "running OCaml": a domain is a
//! must-stop participant only while it holds its domain lock and is executing
//! mutator code. A domain that is parked at a safepoint, inside a C blocking
//! section, still booting (bound but not yet started), or terminating (left the
//! runtime's STW set) is, by construction, NOT running OCaml — it has no live,
//! mutating roots the GC could trip over — and must NOT be awaited.
//!
//! We track this with a per-mutator RUNNING state held in a set of the
//! `caml_domain_state` addresses currently running OCaml (`RUNNING`), tied to the
//! same lifecycle the runtime already drives:
//!
//!   - bind (register_mutator): a domain is born STOPPED (absent from the set).
//!     A child bound mid-spawn but not yet executing OCaml is therefore not
//!     awaited as RUNNING, but its creation holds a BINDING slot
//!     (`mmtk_ocaml_try_begin_bind`, taken in `domain_create` before a domain
//!     slot is claimed and released at its single exit). A slot is refused
//!     while a collection is active, and `stop_all_mutators` waits for held
//!     slots to drain, so registration and the first TLAB refill never
//!     overlap a pause.
//!   - STOPPED edges (enter blocking section, park, deregister) remove the domain
//!     from the set immediately.
//!   - RUNNING edges (leave blocking section, resume from park, child boot) go
//!     through `caml_mmtk_become_running` (runtime/mmtk.c), which calls
//!     `mmtk_ocaml_try_mark_running` — that REFUSES while a collection is active
//!     and the caller then parks COOPERATIVELY (releasing the domain lock so its
//!     backup thread answers OCaml's own STW) before retrying. A domain therefore
//!     never spins on GC-active while holding its domain lock (which deadlocked
//!     OCaml's minor-heap STW), and never joins the awaited set of a collection
//!     that already passed its barrier (the check+insert are one critical section).
//!   - destroy/deregister (terminate): removed from the set (and the registry).
//!
//! Set semantics make every transition idempotent (re-mark RUNNING / re-mark
//! STOPPED / remove-absent are all no-ops), so the accounting cannot underflow
//! (the old `stopped: usize` counter could — see bug #3) and a transitioning
//! domain is simply absent rather than wedging the barrier (bug #3b).
//!
//! `stop_all_mutators` (GC worker) sets GC active, poisons every domain's
//! young_limit so running domains trap to a safepoint and park, then waits until
//! the RUNNING set is empty, and visits each registered mutator. `resume_mutators`
//! un-poisons and wakes everyone.

use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

use lazy_static::lazy_static;
use mmtk::memory_manager;
use mmtk::util::alloc::AllocationError;
use mmtk::util::opaque_pointer::{OpaquePointer, VMMutatorThread, VMThread, VMWorkerThread};
use mmtk::vm::Collection;
use mmtk::vm::GCThreadContext;
use mmtk::Mutator;

use crate::OCamlVM;

pub struct VMCollection;

/// True for the duration of a collection. This is a lock-free MIRROR of
/// `StwState::gc_active`, read at OCaml safepoints (the C hook
/// `mmtk_ocaml_stw_active`) without taking the STW lock. The authoritative copy
/// used for the parked-domain wait predicate is the `gc_active` field below, which
/// is always written under the STW lock so a parker cannot miss a transition.
static GC_ACTIVE: AtomicBool = AtomicBool::new(false);

/// Phase-dynamic slot trusting for CONCURRENT plans: their field slots must be
/// revalidated only while marking runs alongside mutators; inside an STW pause
/// (all minors, Initial/FinalMark, Full) the world is stopped and the same S1
/// trusted fast path used by STW plans is sound — one heap-range compare per
/// slot instead of two SFT lookups (classify + load-time revalidation), a
/// measured ~15% of nursery-trace worker cycles. stop_all_mutators sets
/// trusted=true once running.is_empty(); resume_mutators clears it BEFORE any
/// mutator wakes. Armed at init only for concurrent plans (STW plans keep the
/// static true; MMTK_NO_TRUSTED_LOADS forces everything off).
pub(crate) static DYNAMIC_TRUSTED: AtomicBool = AtomicBool::new(false);

struct StwState {
    /// True for the duration of a collection (authoritative; written under the
    /// lock by stop_all_mutators / resume_mutators). Parked domains wait for it to
    /// go false. Mirrored lock-free into `GC_ACTIVE` for the safepoint hint.
    gc_active: bool,
    /// `caml_domain_state` addresses currently RUNNING OCaml — i.e. the domains
    /// `stop_all_mutators` must wait for. A domain is added when it (re)enters
    /// OCaml and removed when it parks, blocks, or deregisters. The set (rather
    /// than a count) makes every transition idempotent and self-cleaning.
    running: HashSet<usize>,
    /// Domains currently being created (`domain_create` between claiming a
    /// binding slot and finishing `caml_mmtk_domain_init`). A booting domain
    /// is not RUNNING OCaml, but it registers its mutator and takes its first
    /// TLAB block, and neither may overlap a pause: registering mid-pause
    /// changed the mutator set between MarkSweepSpace::release arming
    /// `pending_release_packets` and the ReleaseMutator packets being created
    /// (the counter wrapped), and a TLAB taken mid-pause was never released,
    /// so the domain later bumped into lines the sweep had freed (stale-buffer
    /// SIGSEGVs and wrong output in native CLBG mandelbrot). A slot is only
    /// granted while no collection is active, and `stop_all_mutators` waits for
    /// the count to drain along with the RUNNING set.
    binding: usize,
    /// Completed pauses, bumped by `resume_mutators` in the same critical
    /// section that clears `gc_active`. A parker snapshots it on entry and
    /// returns once it moves, so a wait is bounded by the first pause that
    /// completes after the park began (see `park_until_resumed`).
    epoch: u64,
}
lazy_static! {
    // `HashSet::new` is not const, so the STW state is lazily initialised (the
    // Condvar below is const-constructible and stays a plain static).
    static ref STW: Mutex<StwState> = Mutex::new(StwState {
        gc_active: false,
        running: HashSet::new(),
        binding: 0,
        epoch: 0,
    });
}
static STW_COND: Condvar = Condvar::new();

/// GC-pause accounting: number of collections and total stop-the-world wall time
/// (the span from stop_all_mutators to resume_mutators). Lets the runtime report
/// GC time separately from mutator/allocation time — e.g. to see whether parallel
/// marking actually scales. Reported at exit under MMTK_VERBOSE.
///
/// `GC_COUNT` counts EVERY collection (nursery + full). `FULL_GC_COUNT` counts only
/// FULL (major) collections — for a generational plan a nursery (minor) GC is NOT a
/// full collection, so it must not bump the count `Gc.major_collections` reports
/// (GH#5: the test's `while major_collections < N` window, and stock OCaml's own
/// `major_collections` semantics, count only full cycles — a nursery GC is the
/// MMTk analogue of a stock minor collection). For non-generational plans every GC
/// is full, so the two stay equal there. Both are bumped together in
/// `resume_mutators`, keyed on `last_collection_full_heap()`.
static GC_COUNT: AtomicUsize = AtomicUsize::new(0);
static FULL_GC_COUNT: AtomicUsize = AtomicUsize::new(0);
static GC_NANOS: AtomicU64 = AtomicU64::new(0);
static GC_PAUSE_START: Mutex<Option<Instant>> = Mutex::new(None);

/// Per-pause records for the GC space-time shape work (backlog #R1).
///
/// `GC_NANOS` above is a SUM: it answers "how much stop-the-world in total" and
/// nothing about the distribution. Two collectors with identical totals can have
/// entirely different pause profiles — many small pauses versus a few large ones
/// — which is exactly the vanilla-vs-Bactrian question, so the per-pause value
/// is what the latency dimension needs. It was already being computed in
/// `resume_mutators` (`start.elapsed()`) and immediately discarded into the sum.
///
/// Kept in memory and dumped at exit rather than written per pause: the pause
/// path must not do I/O, and a mutator is parked waiting on this. A pause is 24
/// bytes, so even a run with a million collections costs 24 MB.
///
/// Enabled only when MMTK_PAUSE_LOG is set; otherwise every pause costs one
/// relaxed atomic load of the disabled flag. Two modes (runtime/mmtk.c picks):
///  - `MMTK_PAUSE_LOG=1`: one `[mmtk-pause]` line per pause on stderr, written
///    by the GC worker AFTER the mutators have been woken (outside the measured
///    span, so no mutator waits on the write), plus a `[mmtk-pause-summary]`
///    line at exit (`mmtk_ocaml_pause_log_summary`).
///  - `MMTK_PAUSE_LOG=<path>`: the records are written as NDJSON at exit.
/// Only the GC worker running stop_all_mutators / resume_mutators writes any of
/// this state (one pause at a time), so multi-domain runs do not race on it.
static PAUSE_LOG: Mutex<Vec<PauseRecord>> = Mutex::new(Vec::new());
static PAUSE_LOG_ON: AtomicBool = AtomicBool::new(false);
static PAUSE_LOG_STDERR: AtomicBool = AtomicBool::new(false);
static PAUSE_LOG_T0: Mutex<Option<Instant>> = Mutex::new(None);
/// Bound on the in-memory record vector (24 MB at 24 bytes a record). Pauses
/// past the cap are still counted in the exact totals of `PAUSE_STATS`, but
/// not in the percentiles; the summary says `capped=1` when that happened.
const PAUSE_LOG_CAP: usize = 1 << 20;
static PAUSE_STATS: Mutex<PauseStats> = Mutex::new(PauseStats {
    n: 0,
    full: 0,
    total_nanos: 0,
    max_nanos: 0,
    dropped: 0,
});
/// The pause in flight: filled by stop_all_mutators once the world is stopped,
/// consumed by resume_mutators. Touched only when the pause log is armed.
static PAUSE_CUR: Mutex<PauseCur> = Mutex::new(PauseCur {
    stopped: None,
    kind: "",
    domains: 0,
});

#[derive(Clone, Copy)]
struct PauseRecord {
    /// Start of the pause, nanoseconds since the first recorded pause. Relative
    /// so it can be aligned against the in-mutator probe's own timeline.
    at_nanos: u64,
    dur_nanos: u64,
    full: bool,
}

struct PauseStats {
    n: u64,
    full: u64,
    total_nanos: u64,
    max_nanos: u64,
    dropped: u64,
}

struct PauseCur {
    /// When the rendezvous completed (no domain RUNNING any more).
    stopped: Option<Instant>,
    /// Pause kind if the plan names it at stop time (concurrent plans); ""
    /// means "decide at resume from last_collection_full_heap".
    kind: &'static str,
    /// Mutators visited for root scanning in this pause.
    domains: usize,
}

/// The kind of the pause that is starting, for plans whose pause kind is known
/// at stop time (concurrent plans: `current_pause` is set by
/// `schedule_collection`, which runs before StopMutators, and cleared again by
/// `end_of_gc` before resume). A sliced-marking plan (Bactrian) runs a mark
/// quantum inside every nursery pause of a marking cycle, and a sweep quantum
/// while the deferred sweep is pending; those are tagged `+mark` / `+sweep`.
fn pause_kind_at_stop() -> &'static str {
    use mmtk::plan::concurrent::Pause;
    let plan = crate::mmtk().get_plan();
    let Some(c) = plan.concurrent() else {
        return "";
    };
    match c.current_pause() {
        Some(Pause::Full) => "full",
        Some(Pause::InitialMark) => "initial",
        Some(Pause::FinalMark) => "final",
        Some(Pause::Nursery) => {
            let mark = c.marking_confined_to_pauses() && c.concurrent_work_in_progress();
            let sweep = !c.sweep_drained();
            match (mark, sweep) {
                (false, false) => "nursery",
                (true, false) => "nursery+mark",
                (false, true) => "nursery+sweep",
                (true, true) => "nursery+mark+sweep",
            }
        }
        None => "",
    }
}

/// Mature (major-heap) reserved pages right after the last FULL collection — the
/// baseline for the mature-space-pressure full-GC trigger (GH#5). After a full GC
/// reclaims the mature heap, a generational plan otherwise runs ONLY nursery GCs
/// until the mature space is nearly exhausted, so mature-DEAD weaks/ephemerons/
/// finalisable values are never reclaimed (their referents never get re-traced).
/// Mirroring stock OCaml's `space_overhead` pacing, once mature reserved pages
/// have grown past this baseline by `MATURE_PRESSURE_OVERHEAD_PCT`% we force the
/// next collection to be a full heap GC (see `resume_mutators`). 0 = no full GC
/// has happened yet (every collection so far has been a nursery GC).
static LAST_FULL_GC_MATURE_PAGES: AtomicUsize = AtomicUsize::new(0);

/// Off-heap custom-block bytes allocated since the last completed full GC,
/// credited by the runtime from alloc_custom_gen (raw byte sizes). Reported to
/// mmtk-core via `Collection::vm_live_bytes`, so memory held OUTSIDE the MMTk
/// heap (Bigarrays, GMP limbs, video-frame pools) counts toward heap-full
/// checks and the space-overhead heap sizing — stock's custom_major_ratio
/// pacing, in MMTk terms. Flow approximation: reset when a full GC's sweep
/// completes, i.e. when the dead customs' finalizers have run and released
/// their memory. Without this, a program whose OCaml-side live set is tiny
/// never triggers a collection while its off-heap pool grows without bound
/// (measured: a ~180MB-under-stock frame pool reached 61GB).
static OFFHEAP_BYTES_SINCE_FULL: AtomicUsize = AtomicUsize::new(0);

/// C entry: credit off-heap custom-block bytes (called from the runtime's
/// caml_mmtk_custom_mem_pressure alongside the pacing tick).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_offheap_credit(bytes: usize) {
    OFFHEAP_BYTES_SINCE_FULL.fetch_add(bytes, Ordering::Relaxed);
}

/// Number of nursery (minor) GCs since the last FULL collection. A pure
/// mature-size trigger starves on a steady-state-live-set program that churns the
/// nursery heavily (weaklifetime: a near-constant live set, so mature barely grows
/// past the baseline → full GCs become arbitrarily rare → `Gc.major_collections`
/// stalls and dead mature weaks never clear). A bounded nursery-GC cadence
/// guarantees a full collection runs at least every N nursery GCs regardless, the
/// way stock OCaml's pacing bounds the minor GCs between full major cycles.
static NURSERY_GCS_SINCE_FULL: AtomicUsize = AtomicUsize::new(0);

/// Incremental sweep (Bactrian sliced mode): FinalMark ended but its deferred
/// sweep is still draining — the mature-pressure baseline reset is postponed
/// until the first pause that reports the sweep queue empty.
static AWAITING_SWEEP_BASELINE: AtomicBool = AtomicBool::new(false);

/// Mature-space growth (over the post-full-GC baseline) that forces the next
/// collection to be a full heap GC, as a percentage. 120% ≈ stock OCaml's default
/// `space_overhead` (a full major cycle's worth of mature growth between full GCs).
/// D2 calibration (SHAPE round 28): the margin is calibrated so the mark-
/// cycle PERIOD matches vanilla's at the stock-parity config — bt@2M sweeps
/// 120%->15 cycles, 20%->26, 15%->28, 12%->30 against vanilla's 29 at
/// space_overhead=500, so the default is 14%. The nominal percentage is far
/// below vanilla's o=500 because the two laws measure different bases: our
/// post-cycle baseline includes the marking window's floating garbage and
/// promotions land mature continuously, so a small margin over a large
/// baseline fires at the same allocation period as vanilla's large margin
/// over its swept-live base. MMTK_MATURE_OVERHEAD_PCT overrides.
///
/// ROUND 30: the margin is one half of a two-tier law — concurrent plans
/// additionally clamp the trigger to a fraction of the heap limit
/// (`conc_trigger_pct`), because a margin target above the heap limit can
/// never fire and every major then degrades to an emergency STW Full.
/// The margin governs small live sets; the clamp governs big-live-set and
/// fixed/tight-heap regimes.
fn mature_pressure_overhead_pct() -> usize {
    static V: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("MMTK_MATURE_OVERHEAD_PCT")
            .ok()
            .and_then(|v| v.parse::<usize>().ok())
            .filter(|p| (5..=2000).contains(p))
            .unwrap_or(MATURE_PRESSURE_OVERHEAD_PCT_DEFAULT)
    })
}
// Recalibrated 2026-08-12 (round 30): the incremental-sweep work moved the
// baseline to POST-SWEEP reserved pages (≈ live, compact) — the old 14%
// margin was calibrated against the inflated post-FinalMark baseline and
// became razor-thin over the new base (bt-def stormed 46 fulls). Joint
// sweep on the new base: 150% gives bt@2M 28 cycles (vanilla o=500: 29)
// and bt-def 13 fulls (historical 10).
const MATURE_PRESSURE_OVERHEAD_PCT_DEFAULT: usize = 150;

/// Cadence backstop: force a full heap GC after at most this many nursery (minor)
/// GCs since the last full GC, even if the mature heap has not grown enough to trip
/// the space-overhead trigger. Guarantees `Gc.major_collections` keeps advancing and
/// mature-dead weaks/ephemerons/finalisers are reclaimed on a bounded schedule for
/// steady-state-live-set programs (GH#5).
///
/// ALLOCATION-PACED RETUNE (SCALABILITY.md UPDATE 4, 2026-07-02): this was 8, and
/// together with the old 4 MiB growth floor it MANUFACTURED domain-scaled major
/// cycles — with N domains the shared nursery fills ~N× faster, so an 8-minor
/// cadence fires ~N× more often per unit work (measured: 71→328→807 cycles at
/// d1/8/24 on par_spectralnorm, up to 92–98% STW-wall on par_binarytrees, while
/// vanilla — allocated-words-paced — completed ZERO major cycles on every cell).
/// The cadence is now PER-DOMAIN-SCALED (see `cadence_threshold`): minors/sec
/// scales with the domain count (the shared nursery fills proportionally
/// faster), so a fixed minor-count cadence makes the forced-full frequency
/// scale with domains. 8 x ndomains keeps the single-domain reclamation
/// timing identical to the GH#5-validated behaviour (weaklifetime's
/// major_collections wait) while making the forced-full rate per wall-second
/// domain-invariant.
const MATURE_PRESSURE_FULL_GC_NURSERY_CADENCE_PER_DOMAIN: usize = 8;

/// Allocation actually run through the nursery since the last full GC, in
/// bytes (each minor adds the nursery's max capacity — the trigger fired
/// because the nursery filled, so capacity ~= bytes collected).
static NURSERY_BYTES_SINCE_FULL: std::sync::atomic::AtomicUsize =
    std::sync::atomic::AtomicUsize::new(0);

/// The configured nursery max in bytes (what one minor collects).
fn nursery_max_bytes() -> usize {
    use mmtk::util::options::NurserySize;
    match *crate::mmtk().get_options().nursery {
        NurserySize::Bounded { max, .. } => max,
        NurserySize::ProportionalBounded { .. } => 64 * 1024 * 1024,
        NurserySize::Fixed(b) => b,
    }
}

/// GH#5 backstop budget, denominated in ALLOCATION rather than minors.
///
/// The old law ("a full GC every 8 minors per domain") made the forced-full
/// rate scale INVERSELY with nursery size: at MMTK_NURSERY=Fixed:2MiB it fired
/// a whole-heap collection every ~16 MiB allocated — 186 fulls on binarytrees
/// where the default config does 6, i.e. a manufactured full-GC storm (W-night
/// 2026-08-08, SHAPE.md: 141G cycles at 2 MiB, 46G with the storm suppressed).
/// Vanilla's analogue paces majors by allocated words and is nursery-invariant.
/// The budget below reproduces the GH#5-validated timing at the DEFAULT
/// config (8 minors x 64 MiB per domain) and keeps it whatever the nursery is.
///
/// LXR note: this whole trigger block is gated on plan.generational(); LXR
/// returns None there and is untouched — its reclamation is RC-driven and must
/// never be forced through this path.
fn cadence_budget_bytes() -> usize {
    let ndomains = crate::active_plan::domain_addrs().len().max(1);
    let floor = MATURE_PRESSURE_FULL_GC_NURSERY_CADENCE_PER_DOMAIN
        * (64 * 1024 * 1024)
        * ndomains;
    // Scale the backstop with the heap: a fixed 512MiB budget forced a
    // whole-heap collection every 512MiB allocated REGARDLESS of heap size —
    // at a 13GiB heap that meant 40 full marks of a growing 10GiB live set
    // where the margin law alone needs ~8 (macro ydump: 137s of GC in a
    // 175s run, 3.5x stock; backstop suppressed: 78s, 1.58x). One
    // heap-worth of allocation is the principled budget: stock completes
    // roughly one full cycle per heap-worth allocated (space_overhead), and
    // the margin law already reproduces that pacing — so anything tighter
    // makes the backstop a second, competing pacer instead of the GH#5
    // safety net it is. The 512MiB floor keeps small-heap (GH#5,
    // weaklifetime-class) timing unchanged: it dominates below a 512MiB
    // heap, and at CLBG scale (192MiB pins) behaviour is byte-identical.
    // Denominate the heap-worth in the POST-FULL LIVE BASELINE, not the
    // current heap: the current heap includes the off-heap credit
    // (vm_live_bytes), so a custom-memory-heavy program would grow the heap
    // with its own uncollected pool and thereby defer the very fulls that
    // clear it (measured: a 179MB-under-stock 4K frame pool reached 9.3GB at
    // defaults with the current-heap denomination). The baseline is what the
    // last full sweep actually kept — off-heap accumulation cannot inflate
    // it. x2 approximates one live-derived heap-worth (space_overhead 2.2).
    let baseline_bytes = LAST_FULL_GC_MATURE_PAGES.load(Ordering::Relaxed)
        * mmtk::util::constants::BYTES_IN_PAGE;
    floor.max(baseline_bytes * 2)
}

/// Cadence threshold for the current run: 8 minors per registered domain.
fn cadence_threshold() -> usize {
    let ndomains = crate::active_plan::domain_addrs().len().max(1);
    // Shape experiment (MMTK_FULL_GC_CADENCE): override the per-domain cadence.
    //
    // Matching vanilla's D2 pacing needs BOTH knobs. MMTK_NURSERY sets the
    // minor rate (vanilla: ~2 MiB/minor; the fork's default cap: ~61 MiB), but
    // pinning a stock-parity 2 MiB nursery would leave the default cadence
    // firing a FULL collection every 8 minors = every ~16 MiB allocated, where
    // vanilla's allocated-words law runs a major per ~57 MiB. The cadence has
    // to scale with the nursery or the backstop becomes the pacer. Read once
    // and cached: this is polled after every nursery GC.
    static CADENCE: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    let per_domain = *CADENCE.get_or_init(|| {
        std::env::var("MMTK_FULL_GC_CADENCE")
            .ok()
            .and_then(|v| v.parse::<usize>().ok())
            .filter(|&v| v > 0)
            .unwrap_or(MATURE_PRESSURE_FULL_GC_NURSERY_CADENCE_PER_DOMAIN)
    });
    per_domain * ndomains
}

/// Floor (in pages) below which the mature-pressure trigger never fires. This is
/// the allocation budget a program must actually promote/allocate into the mature
/// space before we consider a major cycle at all, so it is the effective pacer for
/// small-live-set programs. Was 4 MiB — small enough that a few domain-scaled
/// minors' junk promotion tripped it (the manufactured-cycles pathology above).
/// Now max(32 MiB, the max nursery size): at least a whole nursery's worth of
/// survivors must have been promoted since the last cycle, making the budget scale
/// with the same knob that scales promotion volume. Stock's analogue is its
/// allocated-words slice budget against `space_overhead`.
fn mature_pressure_floor_pages() -> usize {
    // D5 (SHAPE round 29): the floor gates how small a live set the pressure
    // trigger can serve. At the historical 32 MiB, any bench with live below
    // ~28 MiB was paced ONLY by the allocation backstop and its mature
    // ballooned to ~10x live (kb: 24 MiB touched vs 2.5 MiB live), where
    // vanilla's o-law keeps overhead uniform at any live size. Sweep (round 29): 32->8 gives kb -8% RSS (5 fulls
    // vs 3, wall flat), saturates below 8 (block-occupancy slack dominates);
    // sp/LU/bt neutral. Default 8.
    static V: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        let mb = std::env::var("MMTK_MATURE_FLOOR_MB")
            .ok()
            .and_then(|v| v.parse::<usize>().ok())
            .filter(|m| (1..=1024).contains(m))
            .unwrap_or(8);
        let pg = mmtk::util::constants::BYTES_IN_PAGE;
        let nursery_pages = crate::mmtk()
            .get_plan()
            .base()
            .gc_trigger
            .get_max_nursery_pages();
        (mb * 1024 * 1024 / pg).max(nursery_pages)
    })
}

/// Evaluate the compaction law at a post-sweep baseline latch (stock's
/// `Gc.max_overhead` analog, round 30). The inputs are the mature Immix
/// space's post-sweep RESERVED bytes vs the LIVE bytes its last major
/// marking epoch actually marked — the only pair that separates the three
/// regimes: a dense growing heap (binarytrees: reserved ≈ live, never
/// fires), an effectively swept heap (fragmed: reserved ≈ live, never
/// fires), and line-pinned waste (mature_mutation: 6.6MB live pinning
/// 17-46MB of lines whose 256B granularity cannot free interleaved dead
/// cells — fires, and the COMPACT-ALL evacuation is the only mechanism
/// that reclaims it). Post-compaction, reserved collapses to ≈live and the
/// law is quiet until waste re-accumulates — self-limiting cadence.
fn note_swept_baseline(_baseline_pages: usize) {
    let pct = compact_overhead_pct();
    if pct == 0 {
        return;
    }
    let plan = crate::mmtk().get_plan();
    let Some(c) = plan.concurrent() else { return };
    // MONOLITHIC-FULL REGIME ONLY (round 30d, wcomp7 lesson): the
    // reserved-vs-marked-live ratio is exact only when the major was a
    // single STW Full — one pause, complete trace, in-pause sweep. A
    // concurrent cycle's post-sweep reserved additionally carries SATB
    // floating garbage and the marking window's promotions (marked by
    // post_copy, NOT in the trace tally), which inflated the ratio into
    // false compact-alls: bt@2M grew a 193ms monolithic evacuation of its
    // 140MB live set, and fragmed's tick-paced cycles overfired to 11x
    // vanilla. A monolithic Full is `previous_pause_started_cycle() &&
 // previous_pause_finished_mark()` (a cycle's FinalMark finishes but
    // did not start it).
    if !(c.previous_pause_started_cycle() && c.previous_pause_finished_mark()) {
        return;
    }
    let Some((reserved, live)) = c.mature_footprint_and_live() else {
        return;
    };
    if live == 0 {
        return;
    }
    let floor = mature_pressure_floor_pages() * mmtk::util::constants::BYTES_IN_PAGE;
    let threshold = live.saturating_add(live.saturating_mul(pct) / 100);
    if reserved > floor && reserved > threshold {
        if std::env::var_os("MMTK_PACE_DEBUG").is_some() {
            eprintln!(
                "[compact] mature {}KB reserved vs {}KB live (+{}%) — compact-all next major",
                reserved / 1024,
                live / 1024,
                pct
            );
        }
        c.request_mature_compaction();
        if let Some(g) = plan.generational() {
            g.force_full_heap_collection();
        }
    }
}

/// Mature-compaction overhead threshold, percent (stock's `Gc.max_overhead`
/// analog, round 30): when mature reserved pages exceed the live estimate
/// by this margin, request a COMPACT-ALL Full (every block a defrag source
/// — hole-based selection cannot see intra-line waste). 0 disables.
/// Measured driver: mature_mutation's dynamic-heap RSS staircased to 120MB
/// over ~8MB truly live. MMTK_COMPACT_OVERHEAD_PCT overrides.
fn compact_overhead_pct() -> usize {
    static V: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("MMTK_COMPACT_OVERHEAD_PCT")
            .ok()
            .and_then(|v| v.parse::<usize>().ok())
            .filter(|p| *p <= 10000)
            .unwrap_or(100)
    })
}


/// Concurrent early-trigger clamp, as a percentage of the CURRENT heap
/// limit (round 30). The margin law alone breaks on big-live-set programs:
/// with `live x (1+margin)` above the heap limit the pressure target can
/// never be reached, the heap physically fills first, and every major
/// degrades to an emergency monolithic STW Full (binarytrees@192M: all 13
/// majors ran as 80-140ms Fulls — the concurrent cycle path never fired;
/// same on every dynamic heap, whose 120% growth budget is below the 150%
/// margin). Every concurrent collector starts its cycle before exhaustion —
/// stock OCaml's slice pacing likewise targets cycle completion before
/// heap-full. The trigger therefore fires at
///   min(baseline x (1+margin), heap_limit x MMTK_CONC_TRIGGER_PCT%)
/// for concurrent plans (a thrash guard keeps the clamp at least one
/// nursery of growth above the baseline: a live set parked at ~clamp size
/// must actually allocate before a new cycle fires). Non-concurrent plans
/// (GenImmix) keep the pure margin law — firing early buys a monolithic
/// STW plan nothing. 0 disables the clamp.
fn conc_trigger_pct() -> usize {
    static V: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *V.get_or_init(|| {
        std::env::var("MMTK_CONC_TRIGGER_PCT")
            .ok()
            .and_then(|v| v.parse::<usize>().ok())
            .filter(|p| *p <= 100)
            .unwrap_or(80)
    })
}

/// The mature-pressure threshold with the concurrent early-trigger clamp
/// applied (see `conc_trigger_pct`). Shared by the post-minor path and the
/// mature-direct allocation tick.
fn effective_mature_threshold_pages(baseline: usize) -> usize {
    let margin = baseline.saturating_add(
        baseline.saturating_mul(mature_pressure_overhead_pct()) / 100,
    );
    let pct = conc_trigger_pct();
    let plan = crate::mmtk().get_plan();
    if pct == 0 || plan.concurrent().is_none() {
        return margin;
    }
    let trig = &plan.base().gc_trigger;
    let clamp = trig.policy.get_current_heap_size_in_pages() * pct / 100;
    // Thrash guard: at least one nursery of growth over the baseline.
    let clamp = clamp.max(baseline.saturating_add(trig.get_max_nursery_pages()));
    margin.min(clamp)
}

/// Tell the plan which pacing site fired the cycle just requested (the
/// mature-direct tick vs the post-minor path); it decides slicing vs a
/// monolithic Full. Slice sizing is the plan's own (runway frozen at
/// InitialMark, measured mark rate), so no quantum or debt estimate is passed.
fn note_cycle_origin(tick_origin: bool) {
    if let Some(c) = crate::mmtk().get_plan().concurrent() {
        c.set_cycle_tick_origin(tick_origin);
    }
}

/// Mature-direct allocation tick (pretenured >= 2056B band + LOS): the
/// pacing law's counters were historically advanced only at minors, so a
/// pretenure-heavy workload (fragmed) grew mature to the space-full edge
/// before any cycle fired. The C alloc path calls this every ~2 MiB of
/// mature-direct allocation; it advances the SAME allocation budget the
/// post-minor path uses and evaluates the SAME two triggers (pressure %
/// over baseline, allocation budget), requesting a cycle via
/// force_full_heap_collection — honored at poll time by
/// Bactrian::collection_required, no minor required.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_mature_alloc_tick(bytes: usize) {
    let plan = crate::mmtk().get_plan();
    let Some(g) = plan.generational() else { return };
    let nb = NURSERY_BYTES_SINCE_FULL.fetch_add(bytes, Ordering::Relaxed) + bytes;
    let mature = g.get_mature_reserved_pages();
    let baseline = LAST_FULL_GC_MATURE_PAGES.load(Ordering::Relaxed);
    let floor = mature_pressure_floor_pages();
    let threshold = effective_mature_threshold_pages(baseline);
    let by_mature = mature > floor && mature > threshold;
    let by_cadence = if std::env::var_os("MMTK_FULL_GC_CADENCE").is_some() {
        false // minor-count override is post-minor-only by definition
    } else {
        nb >= cadence_budget_bytes()
    };
    if by_mature || by_cadence {
        if std::env::var_os("MMTK_PACE_DEBUG").is_some() {
            eprintln!(
                "[pace] mature-alloc tick trigger by_mature={} by_cadence={} mature={}p baseline={}p nb={}MB",
                by_mature, by_cadence, mature, baseline, nb / (1 << 20)
            );
        }
        note_cycle_origin(true);
        g.force_full_heap_collection();
    }
    // In-flight cycle/sweep: this allocation must also DRIVE the quanta
    // (stock runs major slices off major-heap allocation). Without this, a
    // no-minor workload's cycle floats forever (fragmed OOM, SHAPE round 30).
    if let Some(c) = plan.concurrent() {
        c.request_progress_pause();
    }
}

/// Number of collections reported as `Gc.major_collections` (and the field tests
/// poll to confirm a *major* cycle ran). Full GCs only — see `FULL_GC_COUNT`.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_gc_count() -> usize {
    FULL_GC_COUNT.load(Ordering::Relaxed)
}

/// Total number of collections — nursery + full. For MMTK_VERBOSE reporting (so the
/// pause/throughput readout still reflects EVERY stop-the-world, not just full GCs).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_total_gc_count() -> usize {
    GC_COUNT.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn mmtk_ocaml_gc_time_ms() -> u64 {
    GC_NANOS.load(Ordering::Relaxed) / 1_000_000
}

/// Arm per-pause recording (backlog #R1). Called once at init when
/// MMTK_PAUSE_LOG is set; off by default, and off costs one relaxed load per
/// pause.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_pause_log_enable() {
    PAUSE_LOG_ON.store(true, Ordering::Relaxed);
}

/// Arm per-pause recording in stderr mode (`MMTK_PAUSE_LOG=1`): as
/// `mmtk_ocaml_pause_log_enable`, plus one `[mmtk-pause]` line per pause.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_pause_log_enable_stderr() {
    PAUSE_LOG_STDERR.store(true, Ordering::Relaxed);
    PAUSE_LOG_ON.store(true, Ordering::Relaxed);
}

/// Print the `[mmtk-pause-summary]` line to stderr (atexit, stderr mode).
/// count/total/max/full are exact over every pause; the percentiles are over
/// the recorded pauses (all of them unless `capped=1`). Nearest-rank
/// percentiles, all durations in microseconds.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_pause_log_summary() {
    if !PAUSE_LOG_ON.load(Ordering::Relaxed) {
        return;
    }
    let st = PAUSE_STATS.lock().unwrap();
    let mut d: Vec<u64> = PAUSE_LOG.lock().unwrap().iter().map(|r| r.dur_nanos).collect();
    d.sort_unstable();
    let pct = |p: f64| -> f64 {
        if d.is_empty() {
            return 0.0;
        }
        let rank = ((p / 100.0) * d.len() as f64).ceil() as usize;
        d[rank.clamp(1, d.len()) - 1] as f64 / 1e3
    };
    let us = |ns: u64| ns as f64 / 1e3;
    let mean = if st.n == 0 { 0.0 } else { us(st.total_nanos) / st.n as f64 };
    use std::io::Write;
    let line = format!(
        "[mmtk-pause-summary] count={} full={} total_us={:.1} mean_us={:.1} p50_us={:.1} \
         p95_us={:.1} p99_us={:.1} max_us={:.1} capped={}\n",
        st.n,
        st.full,
        us(st.total_nanos),
        mean,
        pct(50.0),
        pct(95.0),
        pct(99.0),
        us(st.max_nanos),
        (st.dropped > 0) as u8
    );
    let _ = std::io::stderr().write_all(line.as_bytes());
}

/// Write the recorded pauses as NDJSON to `path` (NUL-terminated C string).
/// Called from the runtime's atexit handler. Returns the number written, or
/// -1 if the path could not be opened.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn mmtk_ocaml_pause_log_dump(path: *const std::os::raw::c_char) -> i64 {
    use std::io::Write;
    if path.is_null() || !PAUSE_LOG_ON.load(Ordering::Relaxed) {
        return 0;
    }
    let p = match std::ffi::CStr::from_ptr(path).to_str() {
        Ok(s) => s,
        Err(_) => return -1,
    };
    let recs = PAUSE_LOG.lock().unwrap();
    let f = match std::fs::File::create(p) {
        Ok(f) => f,
        Err(_) => return -1,
    };
    let mut w = std::io::BufWriter::new(f);
    for r in recs.iter() {
        // at/dur in seconds so the stream lines up with the probe's float
        // timeline without either side having to know the other's units.
        if writeln!(
            w,
            "{{\"kind\":\"pause\",\"at\":{:.9},\"dur\":{:.9},\"full\":{}}}",
            r.at_nanos as f64 / 1e9,
            r.dur_nanos as f64 / 1e9,
            r.full
        )
        .is_err()
        {
            return -1;
        }
    }
    if w.flush().is_err() {
        return -1;
    }
    recs.len() as i64
}

/// Book one completed pause (per-pause log armed). `start` = entry to
/// stop_all_mutators, `dur` = start -> mutators woken (the same span GC_NANOS
/// sums), `resume_at` = entry to resume_mutators. Runs on the GC worker after
/// the wake-up, so the stderr write is outside the measured pause.
fn record_pause(
    start: Instant,
    dur: Duration,
    resume_at: Instant,
    was_full: bool,
    gen_plan: bool,
    epoch: u64,
) {
    let (stopped, kind, domains) = {
        let mut cur = PAUSE_CUR.lock().unwrap();
        let r = (cur.stopped.take(), cur.kind, cur.domains);
        cur.kind = "";
        cur.domains = 0;
        r
    };
    let kind = match (kind, was_full, gen_plan) {
        ("", true, true) => "full",
        ("", false, true) => "nursery",
        // No nursery/full distinction: every pause is a whole-heap GC.
        ("", _, false) => "gc",
        (k, _, _) => k,
    };
    let dur_ns = dur.as_nanos() as u64;
    let seq = {
        let mut st = PAUSE_STATS.lock().unwrap();
        st.n += 1;
        st.full += was_full as u64;
        st.total_nanos += dur_ns;
        st.max_nanos = st.max_nanos.max(dur_ns);
        let mut log = PAUSE_LOG.lock().unwrap();
        if log.len() < PAUSE_LOG_CAP {
            let mut t0 = PAUSE_LOG_T0.lock().unwrap();
            let base = *t0.get_or_insert(start);
            drop(t0);
            log.push(PauseRecord {
                at_nanos: start.saturating_duration_since(base).as_nanos() as u64,
                dur_nanos: dur_ns,
                full: was_full,
            });
        } else {
            st.dropped += 1;
        }
        st.n
    };
    if PAUSE_LOG_STDERR.load(Ordering::Relaxed) {
        let us = |d: Duration| d.as_nanos() as f64 / 1e3;
        let (ttsp, gc) = match stopped {
            Some(s) => (
                us(s.saturating_duration_since(start)),
                us(resume_at.saturating_duration_since(s)),
            ),
            None => (0.0, 0.0),
        };
        // Format first, then ONE write(2): eprintln! may split a line into
        // several writes, which interleave with the C runtime's own stderr
        // output from other threads (seen with MMTK_VERBOSE on 8 domains).
        use std::io::Write;
        let line = format!(
            "[mmtk-pause] n={} kind={} stw_us={:.1} ttsp_us={:.1} gc_us={:.1} domains={} epoch={}\n",
            seq,
            kind,
            us(dur),
            ttsp,
            gc,
            domains,
            epoch
        );
        let _ = std::io::stderr().write_all(line.as_bytes());
    }
}

extern "C" {
    fn caml_mmtk_interrupt(domain: usize);
    fn caml_mmtk_uninterrupt(domain: usize);
    /// Cooperative park (runtime side): hands this domain's OCaml-STW
    /// participation to its backup thread, waits for the collection to finish,
    /// then re-enters OCaml and re-marks itself RUNNING (parking again if a new
    /// collection started meanwhile). Used so MMTk's STW can't deadlock against
    /// OCaml's own. Called from block_for_gc (the triggering domain).
    fn caml_mmtk_park(domain: usize);
    /// Non-zero iff collection is currently allowed. The runtime drops it to 0
    /// around critical sections that must not see a GC — notably `intern_rec`
    /// (the unmarshaller fills a half-built structure through raw C pointers).
    fn caml_mmtk_collection_enabled() -> i32;
    /// Non-zero in a process created by fork() from an OCaml process. MMTk's
    /// GC worker threads do not survive fork, so no collection can run there
    /// (GH issue 33).
    fn caml_mmtk_in_forked_child() -> i32;
}

/// Mark domain `addr` as STOPPED (no longer running OCaml). Idempotent.
/// Called when a domain enters a blocking section or deregisters.
///
/// Domains transition *to* RUNNING ONLY via `mmtk_ocaml_try_mark_running`, which
/// refuses while a collection is active. The C side then parks COOPERATIVELY
/// (releasing the domain lock so the backup thread answers OCaml's own STW) and
/// retries — see caml_mmtk_become_running in runtime/mmtk.c. This is why the
/// STOPPED->RUNNING edge here never spins on GC_ACTIVE while holding the domain
/// lock (doing so deadlocked OCaml's minor-heap STW, which a running domain may be
/// leading — see the burn deadlock in gc/mmtk/NOTES.md).
fn mark_stopped(addr: usize) {
    let mut s = STW.lock().unwrap();
    s.running.remove(&addr);
    STW_COND.notify_all();
}

/// Remove a domain from the RUNNING set as it deregisters/terminates. Same as
/// `mark_stopped`; named for clarity at the deregistration call site.
pub fn remove_running(addr: usize) {
    mark_stopped(addr);
}

/// True in a forked child, where no collection can run (GH issue 33).
pub(crate) fn in_forked_child() -> bool {
    unsafe { caml_mmtk_in_forked_child() != 0 }
}

/// Park the calling domain (`addr`): remove it from RUNNING and wait until no
/// collection is pending or in progress, or until a pause that was pending or
/// in progress when the park began has completed, whichever comes first. Does
/// NOT re-mark RUNNING -- the caller does that via caml_mmtk_become_running
/// once it has re-acquired the domain lock and re-entered OCaml (so RUNNING
/// and the gc_active check stay atomic w.r.t. the next collection).
///
/// "Pending" is MMTk's request flag (`is_collection_requested`): it is set
/// before the collection is scheduled and cleared only after
/// stop_all_mutators has returned, i.e. after `gc_active` was set under this
/// lock. So `requested || gc_active` has no gap between a request and its
/// pause. Waiting on `gc_active` alone returned from block_for_gc before a
/// just-requested collection had started: Gc.major/full_major/compact came
/// back with the collection still to run.
///
/// Why this completes, and cannot lose a wakeup:
///  - If the caller was RUNNING when it took the lock (every mutator that
///    requests or triggers a collection is), no pause can have passed its
///    `running.is_empty()` barrier yet, so a set request flag or `gc_active`
///    means a pause that will pass the barrier after we leave RUNNING here,
///    then bump `epoch` and notify in resume_mutators.
///  - If nothing is pending or active (a pause already finished before we
///    got here), both predicates are false and we return at once. An
///    epoch-equality wait captured too late would instead wait forever for a
///    pause that is never requested (the old native burn deadlock).
///  - The `epoch` bound makes the wait end at the first pause that completes
///    after the park began, so a domain is not held back by a stream of new
///    requests made by other domains after that pause.
///
/// If a collection then (re)starts, stop_all_mutators poisons this domain's
/// young_limit, so it traps to its next safepoint and parks again.
///
/// In a forked child (GH issue 33) no GC worker exists, so a pending request
/// never becomes a pause: the request flag is ignored there and only
/// `gc_active` is waited on, as before this wait was added. Explicit requests
/// return without collecting in a forked child.
fn park_until_resumed(addr: usize) {
    let forked = in_forked_child();
    let mut s = STW.lock().unwrap();
    let start = s.epoch;
    s.running.remove(&addr);
    STW_COND.notify_all();
    while s.epoch == start && (s.gc_active || (!forked && crate::mmtk().is_collection_requested()))
    {
        s = STW_COND.wait(s).unwrap();
    }
}

/// True if a collection is in progress (queried from the C safepoint hook).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_stw_active() -> bool {
    GC_ACTIVE.load(Ordering::SeqCst)
}

/// Park the calling domain at a safepoint (called via caml_mmtk_park, e.g. from
/// caml_handle_gc_interrupt or the spawn-handshake idle wait). `addr` is the
/// domain's caml_domain_state address.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_stw_park(addr: usize) {
    park_until_resumed(addr);
}

/// Wait until no collection is in progress, without touching the RUNNING set.
/// Used by the terminate path AFTER the domain has deregistered (so the GC no
/// longer awaits it) to keep its roots valid until any collection that snapshotted
/// the registry before deregistration has finished scanning — only then may the
/// caller tear the domain's stack/roots down. Returns immediately if no collection
/// is active.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_wait_collection_done() {
    let mut s = STW.lock().unwrap();
    while s.gc_active {
        s = STW_COND.wait(s).unwrap();
    }
}

/// Ragged safepoint (excise Phase 2, step 1) — snapshot the set of domains
/// currently RUNNING OCaml into `buf` (caller-provided, `len` entries). Returns
/// the number written. If more than `len` domains are RUNNING the result is
/// truncated to `len` (the C caller sizes `buf` at >= caml_params->max_domains,
/// so truncation does not happen in practice). One lock acquisition; the
/// returned addresses are the in-flight lock-free readers a quiescing writer
/// must wait to drain. DORMANT: only caml_mmtk_quiesce_running_domains calls it,
/// which has no callers yet.
///
/// # Safety
/// `buf` must point to `len` writable `usize` slots.
#[no_mangle]
pub unsafe extern "C" fn mmtk_ocaml_snapshot_running(buf: *mut usize, len: usize) -> usize {
    let s = STW.lock().unwrap();
    let mut n = 0;
    for &addr in s.running.iter() {
        if n >= len {
            break;
        }
        *buf.add(n) = addr;
        n += 1;
    }
    n
}

/// Ragged safepoint — true iff domain `addr` is currently RUNNING OCaml. A
/// quiescing writer uses this to drop a snapshot domain that has since parked /
/// blocked / terminated (it left the RUNNING set, so it holds no pre-bump
/// transient reader pointer). One lock acquisition. Also the query behind the
/// runtime's RUNNING-set check (GH issue 24, `caml_mmtk_check_running` in
/// runtime/mmtk.c), which calls it holding its domain lock; STW is a leaf
/// lock, so that adds no lock-order edge.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_is_running(addr: usize) -> i32 {
    let s = STW.lock().unwrap();
    s.running.contains(&addr) as i32
}

/// Atomically try to transition domain `addr` to RUNNING. Fails (returns 0) iff a
/// collection is currently active — in which case the caller must park
/// cooperatively (release the domain lock, let its backup thread service OCaml's
/// own STW) and retry, rather than block here while holding the lock. Succeeds
/// (returns 1) and inserts `addr` into the RUNNING set otherwise. The check and
/// the insert are one critical section, so a domain never joins the awaited set of
/// a collection that has already passed its `running.is_empty()` barrier.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_try_mark_running(addr: usize) -> i32 {
    let mut s = STW.lock().unwrap();
    if s.gc_active {
        return 0;
    }
    s.running.insert(addr);
    1
}

/// Claim a binding slot for a domain being created (see `StwState::binding`).
/// Returns 1 and counts the slot when no collection is active; returns 0 when
/// one is, and the caller must drop its runtime locks, wait with
/// `mmtk_ocaml_wait_collection_done`, and retry. The check and the increment
/// are one critical section with `stop_all_mutators` setting `gc_active`, so a
/// slot is never granted to a domain that a started pause would not wait for.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_try_begin_bind() -> i32 {
    let mut s = STW.lock().unwrap();
    if s.gc_active {
        return 0;
    }
    s.binding += 1;
    1
}

/// Release a binding slot claimed with `mmtk_ocaml_try_begin_bind`.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_end_bind() {
    let mut s = STW.lock().unwrap();
    debug_assert!(s.binding > 0, "end_bind without a matching begin");
    s.binding = s.binding.saturating_sub(1);
    STW_COND.notify_all();
}

/// A domain is entering a C blocking section: it is now safe-stopped.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_enter_blocking(addr: usize) {
    mark_stopped(addr);
}

impl Collection<OCamlVM> for VMCollection {
    /// Consulted by MMTk's gc_trigger before starting a collection. The runtime
    /// suppresses GC (count > 0) around critical sections that hold raw, un-rooted
    /// pointers into half-built objects — chiefly `intern_rec` (the unmarshaller).
    /// Vanilla OCaml upholds this implicitly by reserving the whole block up front;
    /// under MMTk's per-object allocation we must say so explicitly.
    fn is_collection_enabled() -> bool {
        unsafe { caml_mmtk_collection_enabled() != 0 }
    }

    /// Off-heap custom-block memory counts toward reserved pages, so it drives
    /// heap-full checks and the space-overhead heap sizing like heap memory
    /// does (see OFFHEAP_BYTES_SINCE_FULL).
    fn vm_live_bytes() -> usize {
        OFFHEAP_BYTES_SINCE_FULL.load(Ordering::Relaxed)
    }

    /// GC worker: stop every running domain, then visit each registered mutator
    /// so its roots are scanned.
    fn stop_all_mutators<F>(_tls: VMWorkerThread, mut mutator_visitor: F)
    where
        F: FnMut(&'static mut Mutator<OCamlVM>),
    {
        use mmtk::vm::ActivePlan;

        *GC_PAUSE_START.lock().unwrap() = Some(Instant::now());
        // Mark the collection active UNDER the STW lock (and mirror it lock-free
        // into GC_ACTIVE for the safepoint hint) so a domain in park_until_resumed
        // / try_mark_running cannot observe "no collection" and slip past — the
        // gc_active flag and the parker's wait predicate are the same critical
        // section.
        {
            let mut s = STW.lock().unwrap();
            s.gc_active = true;
            GC_ACTIVE.store(true, Ordering::SeqCst);
        }

        // Poison every domain so running ones trap to a safepoint and park.
        for domain in crate::active_plan::domain_addrs() {
            unsafe { caml_mmtk_interrupt(domain) };
        }

        // Wait until no domain is RUNNING OCaml. The RUNNING set is exactly the
        // domains that hold their domain lock and are executing mutator code; a
        // domain that is booting, parked, in a blocking section, or terminating
        // is absent and is therefore not awaited (this is what fixed bug #3b: a
        // domain transitioning through spawn/terminate is by construction not
        // running, so the barrier no longer waits for it forever). Re-poison
        // stragglers each iteration in case a poison was cleared concurrently.
        {
            let mut s = STW.lock().unwrap();
            while !s.running.is_empty() || s.binding > 0 {
                drop(s);
                for domain in crate::active_plan::domain_addrs() {
                    unsafe { caml_mmtk_interrupt(domain) };
                }
                s = STW.lock().unwrap();
                if s.running.is_empty() && s.binding == 0 {
                    break;
                }
                let (g, _) = STW_COND
                    .wait_timeout(s, Duration::from_millis(1))
                    .unwrap();
                s = g;
            }
        }

        // Rendezvous complete (per-pause log: ttsp = pause start -> here).
        let stopped_at = if PAUSE_LOG_ON.load(Ordering::Relaxed) {
            Some(Instant::now())
        } else {
            None
        };

        // World stopped: field slots cannot change until resume — enable the
        // trusted classify/load fast path for this pause (concurrent plans).
        if DYNAMIC_TRUSTED.load(Ordering::Relaxed) {
            mmtk_ocaml_common::slot::set_stw_trusted(true);
        }

        // UP-trace: with exactly one GC worker, mutators quiesced, and no
        // concurrent-marking window in flight or being opened, the trace hot
        // path may use plain operations (see mmtk::util::up_trace). Fused
        // marking pauses (InitialMark/FinalMark) stay atomic when marking runs
        // WORKER-CONCURRENT: their mark state is shared with the concurrent
        // window that follows/precedes. Under SLICED-STW marking
        // (marking_confined_to_pauses) no marking packet ever runs while
        // mutators run — every quantum executes on this single worker inside a
        // stopped-world pause, pause-end fences publish between quanta — so the
        // tracer is single across the whole cycle and UP stays armed for ALL
        // pauses, marking quanta included.
        {
            use mmtk::plan::concurrent::Pause;
            let single = crate::mmtk().worker_count() == 1;
            let marking_safe = match crate::mmtk().get_plan().concurrent() {
                None => true,
                Some(c) => {
                    c.marking_confined_to_pauses()
                        || (!c.concurrent_work_in_progress()
                            && !matches!(
                                c.current_pause(),
                                Some(Pause::InitialMark) | Some(Pause::FinalMark)
                            ))
                }
            };
            if std::env::var_os("MMTK_UP_DEBUG").is_some() {
                eprintln!(
                    "[up-debug] single={} marking_safe={} pause={:?}",
                    single,
                    marking_safe,
                    crate::mmtk().get_plan().concurrent().and_then(|c| c.current_pause())
                );
            }
            if single && marking_safe {
                crate::mmtk().set_up_trace(true);
            }
        }

        // Every running domain has stopped: freeze the mutator set for this
        // pause so `mutators()` and `number_of_mutators()` agree for its whole
        // duration (see active_plan::PAUSE_MUTATORS).
        crate::active_plan::freeze_mutators_for_pause();
        let mut visited = 0usize;
        for mutator in crate::active_plan::VMActivePlan::mutators() {
            mutator_visitor(mutator);
            visited += 1;
        }
        if let Some(stopped) = stopped_at {
            let mut cur = PAUSE_CUR.lock().unwrap();
            cur.stopped = Some(stopped);
            cur.kind = pause_kind_at_stop();
            cur.domains = visited;
        }
    }

    /// GC worker: collection finished — un-poison every domain and wake them.
    fn resume_mutators(_tls: VMWorkerThread) {
        // Per-pause log: end of the GC work proper (gc_us = stopped -> here).
        let resume_at = if PAUSE_LOG_ON.load(Ordering::Relaxed) {
            Some(Instant::now())
        } else {
            None
        };
        crate::active_plan::thaw_mutators_after_pause();
        // Mutators are about to run again: back to full revalidation before any
        // wake (concurrent plans only; see DYNAMIC_TRUSTED).
        if DYNAMIC_TRUSTED.load(Ordering::Relaxed) {
            mmtk_ocaml_common::slot::set_stw_trusted(false);
        }
        // End of the single-tracer window (the lock sequences below publish
        // its plain writes before any mutator observes them).
        crate::mmtk().set_up_trace(false);

        // Collection accounting + the GH#5 mature-space-pressure full-GC trigger.
        // This runs on the GC worker AFTER `Scheduler::end_of_gc` (which set
        // `next_gc_full_heap`) and BEFORE any mutator resumes, so reading plan state
        // and calling `force_full_heap_collection` here is race-free, and our
        // force-store is sequenced after end_of_gc's so it is not clobbered.
        //
        // `last_collection_full_heap()` reflects the GC that just ran (`end_of_gc`
        // touches only `next_gc_full_heap`, not `gc_full_heap`). For a
        // non-generational plan `.generational()` is None, so EVERY GC counts as a
        // full GC and the trigger is inert — behaviour is unchanged there.
        let plan = crate::mmtk().get_plan();
        let gen_plan = plan.generational().is_some();
        // A completed CONCURRENT cycle (FinalMark just ended) is a major
        // collection for pacing purposes, exactly like a STW Full: the mature
        // heap was retraced + swept, so the pressure baseline and the
        // allocation-denominated cadence counters must reset. Stock OCaml's
        // pacing likewise resets when its (incremental) major cycle completes.
        // Without this, sliced/concurrent cycles never reset the counters and
        // the trigger law diverges from the STW mode's.
        let was_full = match plan.generational() {
            None => true,
            Some(g) => {
                g.last_collection_full_heap()
                    || plan
                        .concurrent()
                        .is_some_and(|c| c.previous_pause_finished_mark())
            }
        };
        if was_full {
            FULL_GC_COUNT.fetch_add(1, Ordering::Relaxed);
        }
        // Off-heap credit reset for NON-generational plans (Immix, MarkSweep,
        // SemiSpace, MarkCompact, ConcurrentImmix, ...). The generational
        // branch below resets it when a full cycle's sweep completes; without
        // this, every other collecting plan only ever added to it, so
        // vm_live_bytes (and with it every heap-full and heap-sizing decision)
        // grew without bound on a custom-block-heavy program. Every GC of a
        // STW non-generational plan is a whole-heap GC; a concurrent one
        // completes its cycle only at the pause that finishes marking, never
        // at InitialMark, whose trace has not yet found any dead block. As in
        // the generational case, the dead blocks' finalizers run at the
        // mutators' next safepoint, just after this reset.
        if plan.generational().is_none()
            && plan
                .concurrent()
                .is_none_or(|c| c.previous_pause_finished_mark() && c.sweep_drained())
        {
            OFFHEAP_BYTES_SINCE_FULL.store(0, Ordering::Relaxed);
        }
        if let Some(g) = plan.generational() {
            let mature = g.get_mature_reserved_pages();
            // Cycle-START reset for the allocation-denominated cadence: stock's
            // pacing measures the budget from cycle start to next cycle start,
            // so allocation during a (sliced/concurrent) marking window counts
            // toward the NEXT trigger. A STW Full is start+finish in one pause
            // (both resets fire together — the historical behaviour, unchanged).
            let started_cycle = plan
                .concurrent()
                .is_some_and(|c| c.previous_pause_started_cycle());
            if started_cycle || (was_full && plan.concurrent().is_none()) {
                NURSERY_GCS_SINCE_FULL.store(0, Ordering::Relaxed);
                NURSERY_BYTES_SINCE_FULL.store(0, Ordering::Relaxed);
            }
            // INCREMENTAL SWEEP: with the sweep deferred into quanta, the
            // mature page count at FinalMark still contains the whole cycle's
            // garbage — resetting the pressure baseline there would inflate it
            // and stretch the period. Latch "cycle finished, awaiting sweep"
            // and reset the baseline at the first pause whose sweep queue is
            // drained (plans that sweep in-pause report drained immediately,
            // so the STW mode and Full pauses keep the historical behaviour).
            let sweep_done = plan.concurrent().map(|c| c.sweep_drained()).unwrap_or(true);
            // Baseline for the cycle-start laws: after a sliced cycle the swept
            // mature size still holds everything promoted during the cycle and
            // its sweep window (born black, never tested); the margin (x2.5) and
            // cadence (2x baseline) laws then wait for multiples of that
            // (eio_conc: baseline 0.4 -> 1.7 -> 2.8 -> 6.1 GB for a ~1 GB live
            // set). Use the cycle's marked live size instead; a STW Full reports
            // 0 and keeps the swept size, which is honest there.
            let baseline_of = |mature: usize| -> usize {
                match plan.concurrent().map(|c| c.last_cycle_marked_bytes()) {
                    Some(m) if m > 0 => m.div_ceil(mmtk::util::constants::BYTES_IN_PAGE),
                    _ => mature,
                }
            };
            if was_full && sweep_done {
                LAST_FULL_GC_MATURE_PAGES.store(baseline_of(mature), Ordering::Relaxed);
                AWAITING_SWEEP_BASELINE.store(false, Ordering::Relaxed);
                note_swept_baseline(mature);
                // Full cycle + sweep complete: dead custom blocks have been
                // finalized and their off-heap memory released.
                OFFHEAP_BYTES_SINCE_FULL.store(0, Ordering::Relaxed);
            } else if was_full {
                AWAITING_SWEEP_BASELINE.store(true, Ordering::Relaxed);
            } else if AWAITING_SWEEP_BASELINE.load(Ordering::Relaxed) && sweep_done {
                // First post-FinalMark pause with the sweep complete: the
                // mature count is now authoritative.
                LAST_FULL_GC_MATURE_PAGES.store(baseline_of(mature), Ordering::Relaxed);
                AWAITING_SWEEP_BASELINE.store(false, Ordering::Relaxed);
                note_swept_baseline(mature);
                OFFHEAP_BYTES_SINCE_FULL.store(0, Ordering::Relaxed);
            }
            if was_full {
                // (kept: was_full also feeds the pause-log flag below)
            } else {
                // Nursery (minor) GC. Force the NEXT collection to be a full heap GC
                // if EITHER trigger fires:
                //   - mature pressure: the mature heap has grown past the post-full-GC
                //     baseline by the space-overhead margin (catches mature-growing
                //     workloads — e.g. binarytrees — promptly), OR
                //   - cadence: at least MATURE_PRESSURE_FULL_GC_NURSERY_CADENCE nursery
                //     GCs have run since the last full GC. This is the backstop for a
                //     steady-state-live-set program that churns the nursery without
                //     growing mature (weaklifetime), where the mature trigger never
                //     fires; without it `Gc.major_collections` would stall and dead
                //     mature weaks/ephemerons/finalisers would never clear (GH#5).
                //     A full GC here is cheap precisely when this is the firing
                //     trigger (small mature heap), so it does not hurt throughput.
                let n = NURSERY_GCS_SINCE_FULL.fetch_add(1, Ordering::Relaxed) + 1;
                let nb = NURSERY_BYTES_SINCE_FULL
                    .fetch_add(nursery_max_bytes(), Ordering::Relaxed)
                    + nursery_max_bytes();
                let baseline = LAST_FULL_GC_MATURE_PAGES.load(Ordering::Relaxed);
                let floor = mature_pressure_floor_pages();
                let threshold = effective_mature_threshold_pages(baseline);
                let by_mature = mature > floor && mature > threshold;
                // MMTK_FULL_GC_CADENCE (a minor count) remains the explicit
                // experiment override; the DEFAULT backstop is allocation-
                // denominated (see cadence_budget_bytes).
                let by_cadence = if std::env::var_os("MMTK_FULL_GC_CADENCE").is_some() {
                    n >= cadence_threshold()
                } else {
                    nb >= cadence_budget_bytes()
                };
                if by_mature || by_cadence {
                    // MMTK_PACE_DEBUG: which pacing law fired (mature-pressure
                    // vs allocation-cadence) — for cadence attribution against
                    // stock's space_overhead pacing.
                    if std::env::var_os("MMTK_PACE_DEBUG").is_some() {
                        eprintln!(
                            "[pace] trigger by_mature={} by_cadence={} mature={}p baseline={}p n={} nb={}MB",
                            by_mature, by_cadence, mature, baseline, n, nb / (1 << 20)
                        );
                    }
                    note_cycle_origin(false);
                    g.force_full_heap_collection();
                }
            }
        }

        for domain in crate::active_plan::domain_addrs() {
            unsafe { caml_mmtk_uninterrupt(domain) };
        }
        // Count the pause before any parked mutator can return (as with
        // FULL_GC_COUNT above), so a caller of an explicit Gc request reads
        // the updated count as soon as the request returns.
        GC_COUNT.fetch_add(1, Ordering::Relaxed);
        let mut s = STW.lock().unwrap();
        s.epoch = s.epoch.wrapping_add(1);
        let epoch = s.epoch;
        s.gc_active = false;
        GC_ACTIVE.store(false, Ordering::SeqCst);
        STW_COND.notify_all();
        drop(s);
        if let Some(start) = GC_PAUSE_START.lock().unwrap().take() {
            let dur = start.elapsed();
            GC_NANOS.fetch_add(dur.as_nanos() as u64, Ordering::Relaxed);
            // Keep the individual pause too, not just the running sum (#R1).
            if let Some(resume_at) = resume_at {
                record_pause(start, dur, resume_at, was_full, gen_plan, epoch);
            }
        }
    }

    /// The domain whose allocation triggered GC parks here until collection
    /// ends. It holds its domain lock (it was running OCaml), so it must park
    /// cooperatively (backup thread covers it for any concurrent OCaml STW) —
    /// hence the C helper rather than park_until_resumed directly. The helper
    /// passes the domain address to mmtk_ocaml_stw_park.
    fn block_for_gc(tls: VMMutatorThread) {
        let addr = tls.0 .0.to_address().as_usize();
        unsafe { caml_mmtk_park(addr) };
    }

    /// The heap is full and a collection could not free enough space. The
    /// default impl panics (aborts the process); instead we return, so the
    /// allocation hands a null pointer back to `mmtk_ocaml_alloc`, and the C
    /// alloc wrapper raises OCaml's `Out_of_memory` from a C frame (a raise here
    /// would longjmp through MMTk's Rust frames). Called on the mutator thread.
    fn out_of_memory(_tls: VMThread, _err_kind: AllocationError) {}

    fn spawn_gc_thread(_tls: VMThread, ctx: GCThreadContext<OCamlVM>) {
        match ctx {
            GCThreadContext::Worker(worker) => {
                // NAMED, so /proc/<pid>/task/<tid>/comm identifies GC workers.
                //
                // This is what makes the D1 CPU budget measurable without any
                // privileges. The proper instrument is perf symbol attribution,
                // but perf needs perf_event_paranoid lowered, which needs root —
                // not available on every host we measure on. With named threads,
                // per-thread utime+stime from /proc separates GC CPU from mutator
                // CPU directly, because on this binding GC work runs on threads
                // the mutator never uses.
                //
                // Linux truncates comm to 15 bytes; this name is 14.
                // Fail fast if the OS refuses the thread: MMTk would otherwise
                // count a worker that never started and a later collection
                // could wait for it forever.
                std::thread::Builder::new()
                    .name("mmtk-gc-worker".into())
                    .spawn(move || {
                        let tls = VMWorkerThread(VMThread(OpaquePointer::from_address(
                            unsafe { mmtk::util::Address::from_usize(1) },
                        )));
                        memory_manager::start_worker::<OCamlVM>(crate::mmtk(), tls, worker);
                    })
                    .expect("failed to spawn an MMTk GC worker thread");
            }
        }
    }
}
