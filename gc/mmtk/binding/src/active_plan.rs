//! VMActivePlan for OCaml 5.x — domain (mutator) registry.
//!
//! In OCaml 5.x the unit of parallelism is a *domain*, represented by
//! `caml_domain_state*`.  Each domain is one MMTk mutator.
//! A domain is registered on creation and deregistered on termination.

use std::collections::HashMap;
use std::sync::RwLock;

use lazy_static::lazy_static;

use mmtk::util::opaque_pointer::{VMMutatorThread, VMThread};
use mmtk::vm::ActivePlan;
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
}

/// Register a mutator for the given domain address.
pub fn register_mutator(domain_state_addr: usize, mutator: *mut Mutator<OCamlVM>) {
    let mut map = DOMAIN_REGISTRY.write().unwrap();
    let prev = map.insert(domain_state_addr, MutatorPtr(mutator));
    assert!(
        prev.is_none(),
        "register_mutator: domain 0x{:x} already registered — bind_mutator called twice",
        domain_state_addr
    );
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
    DOMAIN_REGISTRY.write().unwrap().remove(&domain_state_addr);
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
        let map = DOMAIN_REGISTRY.read().unwrap();
        // Collect pointers under the lock, then drop the lock before iterating.
        let ptrs: Vec<*mut Mutator<OCamlVM>> = map.values().map(|p| p.0).collect();
        drop(map);
        // SAFETY: Each pointer is live and MMTk calls this during STW only.
        let iter = ptrs.into_iter().map(|p| unsafe { &mut *p });
        Box::new(iter)
    }

    /// Count of currently registered domains.
    fn number_of_mutators() -> usize {
        DOMAIN_REGISTRY.read().unwrap().len()
    }
}
