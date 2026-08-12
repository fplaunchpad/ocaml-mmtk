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
//!     A child bound mid-spawn but not yet executing OCaml is therefore not awaited.
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
}
lazy_static! {
    // `HashSet::new` is not const, so the STW state is lazily initialised (the
    // Condvar below is const-constructible and stays a plain static).
    static ref STW: Mutex<StwState> = Mutex::new(StwState {
        gc_active: false,
        running: HashSet::new(),
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
/// Enabled only when MMTK_PAUSE_LOG names an output path; otherwise every pause
/// costs one relaxed atomic load of the disabled flag.
static PAUSE_LOG: Mutex<Vec<PauseRecord>> = Mutex::new(Vec::new());
static PAUSE_LOG_ON: AtomicBool = AtomicBool::new(false);
static PAUSE_LOG_T0: Mutex<Option<Instant>> = Mutex::new(None);

#[derive(Clone, Copy)]
struct PauseRecord {
    /// Start of the pause, nanoseconds since the first recorded pause. Relative
    /// so it can be aligned against the in-mutator probe's own timeline.
    at_nanos: u64,
    dur_nanos: u64,
    full: bool,
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
    MATURE_PRESSURE_FULL_GC_NURSERY_CADENCE_PER_DOMAIN
        * (64 * 1024 * 1024)
        * ndomains
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
    let threshold = baseline
        .saturating_add(baseline.saturating_mul(mature_pressure_overhead_pct()) / 100);
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

/// Park the calling domain (`addr`) until no collection is in progress: mark it
/// STOPPED and wait while `gc_active`. Does NOT re-mark RUNNING — the caller does
/// that via caml_mmtk_become_running once it has re-acquired the domain lock and
/// re-entered OCaml (so RUNNING and the gc_active check stay atomic w.r.t. the next
/// collection).
///
/// Waiting on `gc_active` (the authoritative flag, under the lock) rather than an
/// epoch counter is what makes this race-free against a collection that resumes
/// before the parker arrives: if the collection already finished (gc_active ==
/// false) we return immediately instead of waiting for a resume that already fired
/// (the old epoch-equality wait lost that wakeup and hung — the native burn
/// deadlock). If a collection then (re)starts, stop_all_mutators has poisoned this
/// domain's young_limit, so it traps to its next safepoint and parks again.
fn park_until_resumed(addr: usize) {
    let mut s = STW.lock().unwrap();
    s.running.remove(&addr);
    STW_COND.notify_all();
    while s.gc_active {
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
/// transient reader pointer). One lock acquisition. DORMANT (see above).
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
            while !s.running.is_empty() {
                drop(s);
                for domain in crate::active_plan::domain_addrs() {
                    unsafe { caml_mmtk_interrupt(domain) };
                }
                s = STW.lock().unwrap();
                if s.running.is_empty() {
                    break;
                }
                let (g, _) = STW_COND
                    .wait_timeout(s, Duration::from_millis(1))
                    .unwrap();
                s = g;
            }
        }

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
                mmtk::util::up_trace::set_up_trace(true);
            }
        }

        for mutator in crate::active_plan::VMActivePlan::mutators() {
            mutator_visitor(mutator);
        }
    }

    /// GC worker: collection finished — un-poison every domain and wake them.
    fn resume_mutators(_tls: VMWorkerThread) {
        // Mutators are about to run again: back to full revalidation before any
        // wake (concurrent plans only; see DYNAMIC_TRUSTED).
        if DYNAMIC_TRUSTED.load(Ordering::Relaxed) {
            mmtk_ocaml_common::slot::set_stw_trusted(false);
        }
        // End of the single-tracer window (the lock sequences below publish
        // its plain writes before any mutator observes them).
        mmtk::util::up_trace::set_up_trace(false);

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
            if was_full && sweep_done {
                LAST_FULL_GC_MATURE_PAGES.store(mature, Ordering::Relaxed);
                AWAITING_SWEEP_BASELINE.store(false, Ordering::Relaxed);
            } else if was_full {
                AWAITING_SWEEP_BASELINE.store(true, Ordering::Relaxed);
            } else if AWAITING_SWEEP_BASELINE.load(Ordering::Relaxed) && sweep_done {
                // First post-FinalMark pause with the sweep complete: the
                // mature count is now authoritative.
                LAST_FULL_GC_MATURE_PAGES.store(mature, Ordering::Relaxed);
                AWAITING_SWEEP_BASELINE.store(false, Ordering::Relaxed);
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
                let threshold = baseline.saturating_add(
                    baseline.saturating_mul(mature_pressure_overhead_pct()) / 100,
                );
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
                    g.force_full_heap_collection();
                }
            }
        }

        for domain in crate::active_plan::domain_addrs() {
            unsafe { caml_mmtk_uninterrupt(domain) };
        }
        let mut s = STW.lock().unwrap();
        s.gc_active = false;
        GC_ACTIVE.store(false, Ordering::SeqCst);
        STW_COND.notify_all();
        drop(s);
        if let Some(start) = GC_PAUSE_START.lock().unwrap().take() {
            let dur = start.elapsed();
            GC_NANOS.fetch_add(dur.as_nanos() as u64, Ordering::Relaxed);
            GC_COUNT.fetch_add(1, Ordering::Relaxed);
            // Keep the individual pause too, not just the running sum (#R1).
            if PAUSE_LOG_ON.load(Ordering::Relaxed) {
                let mut t0 = PAUSE_LOG_T0.lock().unwrap();
                let base = *t0.get_or_insert(start);
                let at = start.saturating_duration_since(base).as_nanos() as u64;
                drop(t0);
                PAUSE_LOG.lock().unwrap().push(PauseRecord {
                    at_nanos: at,
                    dur_nanos: dur.as_nanos() as u64,
                    full: was_full,
                });
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
                let _ = std::thread::Builder::new()
                    .name("mmtk-gc-worker".into())
                    .spawn(move || {
                        let tls = VMWorkerThread(VMThread(OpaquePointer::from_address(
                            unsafe { mmtk::util::Address::from_usize(1) },
                        )));
                        memory_manager::start_worker::<OCamlVM>(crate::mmtk(), tls, worker);
                    });
            }
        }
    }
}
