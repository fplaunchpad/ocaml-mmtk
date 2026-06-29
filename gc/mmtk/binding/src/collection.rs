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

/// Mature-space growth (over the post-full-GC baseline) that forces the next
/// collection to be a full heap GC, as a percentage. 120% ≈ stock OCaml's default
/// `space_overhead` (a full major cycle's worth of mature growth between full GCs).
const MATURE_PRESSURE_OVERHEAD_PCT: usize = 120;

/// Cadence backstop: force a full heap GC after at most this many nursery (minor)
/// GCs since the last full GC, even if the mature heap has not grown enough to trip
/// the space-overhead trigger. Guarantees `Gc.major_collections` keeps advancing and
/// mature-dead weaks/ephemerons/finalisers are reclaimed on a bounded schedule for
/// steady-state-live-set programs (GH#5). 8 mirrors the order of magnitude of minor
/// GCs between full major cycles in stock OCaml's default pacing; measured to leave
/// GenImmix throughput on a mature-growing workload (binarytrees) at parity with
/// Immix, since a full GC at this cadence is cheap whenever the mature heap is small.
const MATURE_PRESSURE_FULL_GC_NURSERY_CADENCE: usize = 8;

/// Floor (in pages) below which the mature-pressure trigger never fires, so tiny /
/// short-lived programs (whose mature heap is a handful of pages) don't thrash on
/// full GCs. 4 MiB / page_size; computed lazily from the runtime page size.
fn mature_pressure_floor_pages() -> usize {
    const FLOOR_BYTES: usize = 4 * 1024 * 1024;
    let pg = mmtk::util::constants::BYTES_IN_PAGE;
    FLOOR_BYTES / pg
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

        for mutator in crate::active_plan::VMActivePlan::mutators() {
            mutator_visitor(mutator);
        }
    }

    /// GC worker: collection finished — un-poison every domain and wake them.
    fn resume_mutators(_tls: VMWorkerThread) {
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
        let was_full = match plan.generational() {
            None => true,
            Some(g) => g.last_collection_full_heap(),
        };
        if was_full {
            FULL_GC_COUNT.fetch_add(1, Ordering::Relaxed);
        }
        if let Some(g) = plan.generational() {
            let mature = g.get_mature_reserved_pages();
            if was_full {
                // A full GC just (re)traced + reclaimed the mature heap. Reset both
                // the mature-pressure baseline and the nursery-GC cadence counter.
                LAST_FULL_GC_MATURE_PAGES.store(mature, Ordering::Relaxed);
                NURSERY_GCS_SINCE_FULL.store(0, Ordering::Relaxed);
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
                let baseline = LAST_FULL_GC_MATURE_PAGES.load(Ordering::Relaxed);
                let floor = mature_pressure_floor_pages();
                let threshold = baseline.saturating_add(
                    baseline.saturating_mul(MATURE_PRESSURE_OVERHEAD_PCT) / 100,
                );
                let by_mature = mature > floor && mature > threshold;
                let by_cadence = n >= MATURE_PRESSURE_FULL_GC_NURSERY_CADENCE;
                if by_mature || by_cadence {
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
            GC_NANOS.fetch_add(start.elapsed().as_nanos() as u64, Ordering::Relaxed);
            GC_COUNT.fetch_add(1, Ordering::Relaxed);
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
                std::thread::spawn(move || {
                    let tls = VMWorkerThread(VMThread(OpaquePointer::from_address(unsafe {
                        mmtk::util::Address::from_usize(1)
                    })));
                    memory_manager::start_worker::<OCamlVM>(crate::mmtk(), tls, worker);
                });
            }
        }
    }
}
