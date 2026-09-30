//! VMActivePlan for OCaml 5.x — domain (mutator) registry.
//!
//! In OCaml 5.x the unit of parallelism is a *domain*, represented by
//! `caml_domain_state*`.  Each domain is one MMTk mutator.
//! A domain is registered on creation and deregistered on termination.

use std::collections::HashMap;
use std::sync::{Mutex, RwLock};

use lazy_static::lazy_static;

use mmtk::util::opaque_pointer::{VMMutatorThread, VMThread};
use mmtk::vm::ActivePlan;
use mmtk::plan::MutatorContext;
use mmtk::Mutator;

use crate::OCamlVM;

pub struct VMActivePlan;

/// Newtype wrapper around a raw mutator pointer so we can implement Send+Sync.
/// SAFETY: Access is always serialised by the surrounding RwLock.
struct MutatorPtr(*mut Mutator<OCamlVM>);
unsafe impl Send for MutatorPtr {}
unsafe impl Sync for MutatorPtr {}

lazy_static! {
    /// Global domain registry: domain_state_addr → raw pointer to Mutator<OCamlVM>.
    static ref DOMAIN_REGISTRY: RwLock<HashMap<usize, MutatorPtr>> =
        RwLock::new(HashMap::new());
    /// The mutator set frozen for the pause in progress (`None` outside a pause).
    /// A domain spawning or terminating during a pause is not awaited by
    /// stop_all_mutators, so the registry can change while GC packets run. The
    /// core reads the set twice per Release stage — `number_of_mutators()` when
    /// MarkSweepSpace::release arms its `pending_release_packets` handshake, then
    /// `mutators()` to create the ReleaseMutator packets that decrement it — and a
    /// registry change in between leaves the counter short or wraps it to
    /// usize::MAX (the domain_parallel_spawn_burn abort). Both reads use this
    /// snapshot while it is set, so every pause sees one consistent set. Mutator
    /// allocations are leaked at termination, so the pointers stay valid.
    static ref PAUSE_MUTATORS: Mutex<Option<Vec<MutatorPtr>>> = Mutex::new(None);
}

/// Freeze the mutator set for the pause that is starting. Called by
/// stop_all_mutators once every running domain has stopped.
pub fn freeze_mutators_for_pause() {
    let ptrs: Vec<MutatorPtr> = DOMAIN_REGISTRY
        .read()
        .unwrap()
        .values()
        .map(|p| MutatorPtr(p.0))
        .collect();
    *PAUSE_MUTATORS.lock().unwrap() = Some(ptrs);
}

/// Drop the pause snapshot. Called by resume_mutators before domains wake.
pub fn thaw_mutators_after_pause() {
    *PAUSE_MUTATORS.lock().unwrap() = None;
}

/// Raw mutator pointers: the pause snapshot while one is set, else the registry.
fn mutator_ptrs() -> Vec<*mut Mutator<OCamlVM>> {
    if let Some(snapshot) = PAUSE_MUTATORS.lock().unwrap().as_ref() {
        return snapshot.iter().map(|p| p.0).collect();
    }
    DOMAIN_REGISTRY.read().unwrap().values().map(|p| p.0).collect()
}

/// Register a mutator for the given domain address.
pub fn register_mutator(domain_state_addr: usize, mutator: *mut Mutator<OCamlVM>) {
    let n = {
        let mut map = DOMAIN_REGISTRY.write().unwrap();
        let prev = map.insert(domain_state_addr, MutatorPtr(mutator));
        assert!(
            prev.is_none(),
            "register_mutator: domain 0x{:x} already registered — bind_mutator called twice",
            domain_state_addr
        );
        map.len()
    };
    update_nursery_scale(n);
}

/// Per-domain nursery-budget scaling (stock parity: stock gives each domain its own
/// 2 MiB minor heap, so total nursery capacity grows with the domain count; a flat
/// shared budget instead makes minor-GC FREQUENCY scale with the aggregate allocation
/// rate — SCALABILITY.md UPDATE 5 culprit 1). Scale the shared Bounded budget by the
/// registered-domain count. LAZY by construction: the budget is a pure accounting
/// number consulted at trigger checks, so this store does no eager work — a spawn
/// just widens what the NEXT trigger evaluation allows, and a termination shrinks it
/// (if usage already exceeds the shrunk budget, the next poll triggers the minor GC).
/// Skipped when the user pinned MMTK_NURSERY, or with MMTK_NURSERY_PER_DOMAIN=0.
fn update_nursery_scale(ndomains: usize) {
    if crate::api::nursery_per_domain_scaling() {
        crate::mmtk()
            .get_plan()
            .base()
            .gc_trigger
            .set_nursery_scale(ndomains.max(1));
    }
}

/// Addresses (caml_domain_state*) of all registered domains. Used by the STW
/// code to interrupt every domain.
pub fn domain_addrs() -> Vec<usize> {
    DOMAIN_REGISTRY.read().unwrap().keys().copied().collect()
}

/// Remove a domain from the registry by its caml_domain_state address. Called
/// when a domain terminates so it is no longer counted by stop-the-world.
/// (The Mutator allocation itself is intentionally leaked here — retiring it
/// safely w.r.t. an in-progress collection is left for later.)
pub fn deregister_by_addr(domain_state_addr: usize) {
    let removed = DOMAIN_REGISTRY.write().unwrap().remove(&domain_state_addr);
    // FLUSH THE DYING MUTATOR'S BARRIER BUFFERS before it becomes invisible.
    // Once out of the registry the mutator is never visited by StopMutators
    // again, so any REMEMBERED-SET entries still sitting in its thread-local
    // modbufs would be lost — and a mature->young edge written shortly before
    // termination then never gets re-traced at a minor GC. That is exactly the
    // domain-termination result publish (`sync_result`: mature term_sync.state
    // := young Finished(...)): the global root promotes the Finished block but
    // the un-remembered slot keeps the stale young address, whose memory the
    // nursery then recycles -> Domain.join reads garbage (GH#3 residual;
    // rr/lldb-confirmed 2026-07-02: joiner's `res` pointing into the CopySpace
    // region with float-array contents). The old whole-heap collect per
    // termination masked this by re-tracing every slot without needing the
    // remset. Flushing here is safe off-pause: the packets land in the closed
    // Closure bucket and are consumed by the next collection.
    if let Some(m) = removed {
        // SAFETY: the mutator pointer is valid (the allocation is deliberately
        // leaked at termination) and this runs on the dying domain's own thread
        // before any further mutator activity. No pause may flush this mutator
        // concurrently: ScanMutatorRoots flushes every mutator of the pause's
        // frozen set on a GC worker, so the terminate path
        // (caml_mmtk_domain_terminate) calls this holding a binding slot, which
        // excludes an active collection (GH issue 39: two concurrent flushes
        // took the same buffer and the packets freed it twice). The
        // stop_all_domains path (caml_mmtk_deregister_domain) holds no slot.
        unsafe { (*m.0).flush() };
    }
    update_nursery_scale(DOMAIN_REGISTRY.read().unwrap().len());
    // Also drop it from the stop-the-world RUNNING set so a collection in flight
    // does not wait for a domain that has terminated (it has left the runtime's
    // STW participant set and is no longer executing OCaml).
    crate::collection::remove_running(domain_state_addr);
}

/// Deregister the mutator by pointer match. Panics if the pointer is not registered.
pub fn deregister_by_ptr(mutator_ptr: *mut Mutator<OCamlVM>) {
    let mut map = DOMAIN_REGISTRY.write().unwrap();
    let key = map
        .iter()
        .find(|(_k, v)| v.0 == mutator_ptr)
        .map(|(k, _v)| *k)
        .expect("deregister_by_ptr: mutator pointer not found — double-free or unregistered pointer");
    map.remove(&key);
    drop(map);
    crate::collection::remove_running(key);
}

impl ActivePlan<OCamlVM> for VMActivePlan {
    // TODO: spawn_gc_thread currently passes Address::from_usize(1) as the worker TLS.
    // This is safe here because is_mutator uses registry lookup (not a sentinel check),
    // so a worker with tls=1 correctly returns false as long as no domain is bound at
    // address 1. Fix spawn_gc_thread to use a real thread id before moving to a copying plan.

    /// True if `tls` corresponds to a registered OCaml 5.x domain.
    fn is_mutator(tls: VMThread) -> bool {
        let addr = tls.0.to_address().as_usize();
        let map = DOMAIN_REGISTRY.read().unwrap();
        map.contains_key(&addr)
    }

    /// Return the Mutator for the given domain.
    fn mutator(tls: VMMutatorThread) -> &'static mut Mutator<OCamlVM> {
        let addr = tls.0 .0.to_address().as_usize();
        let map = DOMAIN_REGISTRY.read().unwrap();
        let ptr = map
            .get(&addr)
            .unwrap_or_else(|| panic!("OCaml mutator: unknown domain addr 0x{:x}", addr))
            .0;
        // SAFETY: MMTk guarantees this is only called during STW when the mutator is live.
        unsafe { &mut *ptr }
    }

    /// Iterator over all live domain mutators.
    fn mutators<'a>() -> Box<dyn Iterator<Item = &'a mut Mutator<OCamlVM>> + 'a> {
        // Pointers are collected under the lock (pause snapshot or registry),
        // and the lock is dropped before iterating.
        let ptrs = mutator_ptrs();
        // SAFETY: Each pointer is live (mutators are never freed) and MMTk calls
        // this during STW only.
        let iter = ptrs.into_iter().map(|p| unsafe { &mut *p });
        Box::new(iter)
    }

    /// Count of mutators: the pause snapshot while one is set, else the
    /// currently registered domains. Must agree with `mutators()`.
    fn number_of_mutators() -> usize {
        mutator_ptrs().len()
    }
}
