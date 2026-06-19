//! VMCollection for OCaml 5.x — stop-the-world coordination.
//!
//! Single-domain bring-up (M2): the OCaml main domain is the only mutator.
//! When an allocation can't be satisfied, mmtk-core calls `block_for_gc` on the
//! mutator thread; the mutator parks here and publishes `WORLD_STOPPED`. An MMTk
//! GC worker thread then runs `stop_all_mutators` (which waits for the mutator
//! to park, then visits it so its roots are scanned), marks/sweeps, and finally
//! `resume_mutators` wakes the parked mutator.
//!
//! TODO(multi-domain): this only stops the domain that triggered GC. Supporting
//! several domains needs OCaml's interrupt_word / caml_try_run_on_all_domains
//! machinery so every domain reaches a safepoint.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex};

use mmtk::memory_manager;
use mmtk::util::opaque_pointer::{OpaquePointer, VMMutatorThread, VMThread, VMWorkerThread};
use mmtk::vm::Collection;
use mmtk::vm::GCThreadContext;
use mmtk::Mutator;

use crate::OCamlVM;

pub struct VMCollection;

/// true while the parked mutator should keep blocking (protected by STW_LOCK).
static STW_LOCK: Mutex<bool> = Mutex::new(false);
static STW_COND: Condvar = Condvar::new();
/// Set by the mutator once it has parked in `block_for_gc`; the GC worker spins
/// on this before scanning the mutator's roots.
static WORLD_STOPPED: AtomicBool = AtomicBool::new(false);

impl Collection<OCamlVM> for VMCollection {
    /// Called on a GC worker thread. Wait until the mutator has parked, then
    /// visit each registered mutator (triggers root scanning for it).
    fn stop_all_mutators<F>(_tls: VMWorkerThread, mut mutator_visitor: F)
    where
        F: FnMut(&'static mut Mutator<OCamlVM>),
    {
        // Wait for the triggering mutator to reach block_for_gc and park.
        while !WORLD_STOPPED.load(Ordering::SeqCst) {
            std::hint::spin_loop();
        }
        use mmtk::vm::ActivePlan;
        for mutator in crate::active_plan::VMActivePlan::mutators() {
            mutator_visitor(mutator);
        }
    }

    /// Called on a GC worker thread once collection finishes: wake the mutator.
    fn resume_mutators(_tls: VMWorkerThread) {
        let mut blocked = STW_LOCK.lock().unwrap();
        *blocked = false;
        WORLD_STOPPED.store(false, Ordering::SeqCst);
        STW_COND.notify_all();
    }

    /// Called on the mutator thread when an allocation triggers GC. Park until a
    /// GC worker calls `resume_mutators`.
    fn block_for_gc(_tls: VMMutatorThread) {
        let mut blocked = STW_LOCK.lock().unwrap();
        *blocked = true;
        WORLD_STOPPED.store(true, Ordering::SeqCst);
        while *blocked {
            blocked = STW_COND.wait(blocked).unwrap();
        }
    }

    /// Spawn a Rust thread running the MMTk GC worker loop.
    fn spawn_gc_thread(_tls: VMThread, ctx: GCThreadContext<OCamlVM>) {
        match ctx {
            GCThreadContext::Worker(worker) => {
                std::thread::spawn(move || {
                    // TODO(M3): use a real thread id rather than the sentinel 1.
                    // Safe for non-moving plans: is_mutator() is a registry
                    // lookup, and no domain is bound at address 1.
                    let tls = VMWorkerThread(VMThread(OpaquePointer::from_address(unsafe {
                        mmtk::util::Address::from_usize(1)
                    })));
                    memory_manager::start_worker::<OCamlVM>(crate::mmtk(), tls, worker);
                });
            }
        }
    }
}
