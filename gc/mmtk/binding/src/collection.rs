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
static GC_COUNT: AtomicUsize = AtomicUsize::new(0);
static GC_NANOS: AtomicU64 = AtomicU64::new(0);
static GC_PAUSE_START: Mutex<Option<Instant>> = Mutex::new(None);

#[no_mangle]
pub extern "C" fn mmtk_ocaml_gc_count() -> usize {
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
