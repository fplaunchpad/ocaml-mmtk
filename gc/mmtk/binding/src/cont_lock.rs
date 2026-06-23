//! Per-continuation scan lock — mirrors vanilla OCaml's NOT_MARKABLE header-status
//! lock (major_gc.c `caml_darken_cont`), re-created MMTk-side because the OCaml
//! header color bits MMTk does not use.
//!
//! A continuation block points at a suspended fiber stack reachable only through
//! it. Under ConcurrentImmix a GC worker may scan that stack (binding `scan_object`
//! -> `caml_scan_stack`) DURING concurrent marking, while another domain resumes
//! the continuation (`caml_continuation_use_noexc` -> switch onto the stack and
//! mutate it). Marker-reads-while-mutator-mutates = wild pointer / crash.
//!
//! Vanilla serializes the two with a per-continuation 3-state lock on the cont
//! header (UNMARKED -> NOT_MARKABLE("locked/scanning") -> MARKED). Both the marker
//! and the resuming mutator route through the same `caml_darken_cont`:
//!   - marker: try-acquire; winner scans once + releases; others skip.
//!   - resume: BLOCK until released, then take + switch.
//! We reproduce exactly that with an explicit address-keyed lock:
//!   - `try_lock(addr)`  — non-blocking; GC worker scan path (skip stack if false).
//!   - `lock(addr)`      — blocking; resume path, before switching onto the stack.
//!   - `unlock(addr)`    — release + wake any blocked resumer/worker.
//! Collisions (a cont simultaneously scanned + resumed) are rare, so one global
//! Mutex<HashSet> + Condvar suffices; the common path is an uncontended insert.

use std::collections::HashSet;
use std::sync::Mutex;

use lazy_static::lazy_static;

struct LockSet {
    /// Addresses of continuation blocks currently locked (being scanned, or held
    /// by a resuming mutator about to take the stack).
    locked: HashSet<usize>,
}

lazy_static! {
    static ref CONT_LOCKS: Mutex<LockSet> = Mutex::new(LockSet { locked: HashSet::new() });
}

/// Non-blocking acquire. Returns true if the lock was free and is now held by the
/// caller, false if already held (caller must NOT scan the stack). Used by the GC
/// worker scan path: skip-if-held.
pub fn try_lock(addr: usize) -> bool {
    let mut g = CONT_LOCKS.lock().unwrap();
    g.locked.insert(addr) // HashSet::insert returns true iff newly inserted
}

/// Blocking acquire — waits until the continuation is not being scanned, then
/// holds the lock. Used by the resume path before switching onto the fiber, so a
/// resume never races an in-progress worker scan of this same continuation.
///
/// We SPIN (yielding) rather than block on the Condvar: a GC worker holds this lock
/// only for the bounded duration of ONE fiber-stack scan and does NOT need the
/// resuming mutator to park first (concurrent marking does not stop mutators), so
/// the spin terminates quickly. Spinning (vs a kernel block) keeps the resuming
/// domain from sitting un-parkable in a condvar while a FinalMark STW waits for it —
/// the deadlock a blocking wait caused. This mirrors vanilla caml_darken_cont, which
/// SPIN_WAITs on the NOT_MARKABLE header status rather than blocking.
pub fn lock(addr: usize) {
    loop {
        {
            let mut g = CONT_LOCKS.lock().unwrap();
            if g.locked.insert(addr) {
                return;
            }
        }
        std::thread::yield_now();
    }
}

/// Release the lock and wake any waiters. Safe to call even if not held (a no-op
/// remove), though callers always pair it with a successful acquire.
pub fn unlock(addr: usize) {
    let mut g = CONT_LOCKS.lock().unwrap();
    g.locked.remove(&addr);
}
