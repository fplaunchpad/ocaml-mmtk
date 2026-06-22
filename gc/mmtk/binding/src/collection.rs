//! VMCollection for OCaml 5.x — multi-domain stop-the-world coordination.
//!
//! When MMTk collects, every OCaml domain (mutator) must be at a safe point
//! before marking. A domain is "safe" if it is either parked at a bytecode
//! safepoint or inside a C blocking section (not mutating OCaml objects; its sp
//! is published). We track a single `stopped` count covering both:
//!
//!   - park (block_for_gc on the triggering domain; the poll hook on others):
//!       stopped += 1; wait for the collection to finish; stopped -= 1.
//!   - blocking section: enter -> stopped += 1; leave -> wait if a collection
//!       is active, then stopped -= 1.
//!
//! `stop_all_mutators` (GC worker) sets GC active, poisons every domain's
//! young_limit so running domains trap to a safepoint and park, then waits until
//! `stopped >= number_of_mutators` (recomputed, so domains terminating mid-cycle
//! don't wedge it), and visits each mutator. `resume_mutators` un-poisons and
//! wakes everyone.

use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

use mmtk::memory_manager;
use mmtk::util::alloc::AllocationError;
use mmtk::util::opaque_pointer::{OpaquePointer, VMMutatorThread, VMThread, VMWorkerThread};
use mmtk::vm::Collection;
use mmtk::vm::GCThreadContext;
use mmtk::Mutator;

use crate::OCamlVM;

pub struct VMCollection;

/// True for the duration of a collection. Read at safepoints (incl. the C hook).
static GC_ACTIVE: AtomicBool = AtomicBool::new(false);

struct StwState {
    /// Bumped once per completed collection; parked domains wait for a change.
    epoch: u64,
    /// Domains currently safe-stopped (parked or in a blocking section).
    stopped: usize,
}
static STW: Mutex<StwState> = Mutex::new(StwState { epoch: 0, stopped: 0 });
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
    /// participation to its backup thread, waits for the MMTk resume epoch, then
    /// re-enters OCaml. Used so MMTk's STW can't deadlock against OCaml's own.
    fn caml_mmtk_park();
    /// Non-zero iff collection is currently allowed. The runtime drops it to 0
    /// around critical sections that must not see a GC — notably `intern_rec`
    /// (the unmarshaller fills a half-built structure through raw C pointers).
    fn caml_mmtk_collection_enabled() -> i32;
}

/// Park the calling domain at a safepoint until the current collection finishes.
fn park_until_resumed() {
    let mut s = STW.lock().unwrap();
    let my_epoch = s.epoch;
    s.stopped += 1;
    STW_COND.notify_all();
    while s.epoch == my_epoch {
        s = STW_COND.wait(s).unwrap();
    }
    s.stopped -= 1;
}

/// True if a collection is in progress (queried from the C safepoint hook).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_stw_active() -> bool {
    GC_ACTIVE.load(Ordering::SeqCst)
}

/// Park the calling domain at a safepoint (called from caml_handle_gc_interrupt).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_stw_park() {
    park_until_resumed();
}

/// A domain is entering a C blocking section: count it as safe-stopped.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_enter_blocking() {
    let mut s = STW.lock().unwrap();
    s.stopped += 1;
    STW_COND.notify_all();
}

/// A domain is leaving a blocking section and wants to run OCaml again. If a
/// collection is active it must wait for it to finish first.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_leave_blocking() {
    let mut s = STW.lock().unwrap();
    while GC_ACTIVE.load(Ordering::SeqCst) {
        s = STW_COND.wait(s).unwrap();
    }
    s.stopped -= 1;
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

    /// GC worker: stop every domain, then visit each so its roots are scanned.
    fn stop_all_mutators<F>(_tls: VMWorkerThread, mut mutator_visitor: F)
    where
        F: FnMut(&'static mut Mutator<OCamlVM>),
    {
        use mmtk::vm::ActivePlan;

        *GC_PAUSE_START.lock().unwrap() = Some(Instant::now());
        GC_ACTIVE.store(true, Ordering::SeqCst);

        // Poison every domain so running ones trap to a safepoint and park.
        for domain in crate::active_plan::domain_addrs() {
            unsafe { caml_mmtk_interrupt(domain) };
        }

        // Wait until all domains are safe-stopped. Recompute the count each time
        // (a domain may deregister as it terminates) and re-poison stragglers.
        loop {
            let n = crate::active_plan::VMActivePlan::number_of_mutators();
            let stopped = STW.lock().unwrap().stopped;
            if stopped >= n {
                break;
            }
            for domain in crate::active_plan::domain_addrs() {
                unsafe { caml_mmtk_interrupt(domain) };
            }
            let guard = STW.lock().unwrap();
            let _ = STW_COND.wait_timeout(guard, Duration::from_millis(1));
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
        GC_ACTIVE.store(false, Ordering::SeqCst);
        s.epoch = s.epoch.wrapping_add(1);
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
    /// hence the C helper rather than park_until_resumed directly.
    fn block_for_gc(_tls: VMMutatorThread) {
        unsafe { caml_mmtk_park() };
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
