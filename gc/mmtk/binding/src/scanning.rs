//! VMScanning for OCaml 5.x — root enumeration and object tracing.
//!
//! Roots mirror what OCaml's own major-GC mark phase scans (major_gc.c):
//!   - per domain: `caml_do_roots` (local roots, the value stack, the
//!     scan-roots hook, and finalisable values);
//!   - once globally: `caml_scan_global_roots` (caml_globals[] and registered
//!     C global roots).
//! Both take a `scanning_action` callback `(void* data, value v, value* slot)`;
//! we turn each `slot` address into a FieldSlot (which filters immediates) and
//! hand the batch to MMTk via `create_process_roots_work`.

use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

use mmtk::memory_manager;
use mmtk::scheduler::GCWorker;
use mmtk::util::opaque_pointer::VMWorkerThread;
use mmtk::util::{Address, ObjectReference};
use mmtk::vm::slot::Slot;
use mmtk::vm::SlotVisitor;
use mmtk::vm::{ObjectTracer, ObjectTracerContext, RootsWorkFactory, Scanning};
use mmtk::Mutator;

use mmtk_ocaml_common::scanning::{continuation_stack, scan_ocaml_object};
use mmtk_ocaml_common::slot::FieldSlot;

use crate::active_plan::domain_addrs;
use crate::OCamlVM;

// OCaml runtime entry points (resolved when the binding's staticlib is linked
// into the bytecode runtime). `caml_do_roots` and `caml_scan_global_roots` are
// CAMLexport functions in runtime/roots.c and runtime/globroots.c.
type ScanningAction = extern "C" fn(*mut c_void, usize, *mut usize);
extern "C" {
    fn caml_do_roots(
        f: ScanningAction,
        flags: i32,
        data: *mut c_void,
        domain: *mut c_void,
        do_final_val: i32,
    );
    fn caml_scan_global_roots(f: ScanningAction, data: *mut c_void);
    /// Scan a suspended fiber stack (and its parent chain) — used to trace the stack
    /// held by a continuation block (runtime/fiber.c). `f` is called per root slot.
    fn caml_scan_stack(
        f: ScanningAction,
        fflags: i32,
        fdata: *mut c_void,
        stack: *mut c_void,
        v_gc_regs: *mut usize,
    );
    /// Report a domain's weak arrays / ephemerons as strong roots (interim, until
    /// proper weak-reference processing) so they cannot dangle under MMTk.
    fn caml_mmtk_scan_ephe_roots(f: ScanningAction, data: *mut c_void, domain: *mut c_void);

    // M6 (gated by MMTK_WEAK_REFS): MMTk-native weak/ephemeron processing. See
    // runtime/mmtk.c and gc/mmtk/NOTES.md (M6 design).
    static caml_mmtk_weak_refs: i32;
    fn caml_mmtk_ephe_mark_pass(
        domain: usize,
        is_reachable: extern "C" fn(usize) -> i32,
        forward: extern "C" fn(usize) -> usize,
        retain: extern "C" fn(*mut c_void, usize) -> usize,
        ctx: *mut c_void,
    ) -> i32;
    fn caml_mmtk_ephe_clean_pass(
        domain: usize,
        is_reachable: extern "C" fn(usize) -> i32,
        forward: extern "C" fn(usize) -> usize,
    );
    fn caml_mmtk_final_update_first(
        domain: usize,
        is_reachable: extern "C" fn(usize) -> i32,
        retain: extern "C" fn(*mut c_void, usize) -> usize,
        ctx: *mut c_void,
    ) -> i32;
    fn caml_mmtk_final_cleanup(
        domain: usize,
        is_reachable: extern "C" fn(usize) -> i32,
        forward: extern "C" fn(usize) -> usize,
        retain: extern "C" fn(*mut c_void, usize) -> usize,
        ctx: *mut c_void,
    );
    /// Adopt finalisers orphaned by terminated domains into the given live domain,
    /// so the passes above then process them (runtime/major_gc.c). Drains the
    /// orphan list — a no-op after the first call in a GC's mark fixpoint.
    fn caml_mmtk_adopt_orphaned_finalisers(
        domain: usize,
        retain: extern "C" fn(*mut c_void, usize) -> usize,
        ctx: *mut c_void,
    );
    /// Scan finalisers orphaned by terminated domains as roots (runtime/major_gc.c).
    /// Mirrors caml_final_do_roots over every orphaned final_info still awaiting
    /// adoption, so their fun/val slots are reported here too — otherwise the
    /// orphaned nursery values dangle between orphaning and adoption
    /// (finaliser_handover use-after-free). `do_val` matches the per-domain scan.
    fn caml_mmtk_scan_orphaned_finalisers(
        f: ScanningAction,
        fflags: i32,
        data: *mut c_void,
        do_val: i32,
    );
}

#[inline]
fn weak_refs_enabled() -> bool {
    unsafe { caml_mmtk_weak_refs != 0 }
}

/// DEBUG flag (MMTK_DEBUG_STACK_CHECK), cached — gates the mis-forward detector.
#[inline]
fn debug_check_enabled() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| std::env::var_os("MMTK_DEBUG_STACK_CHECK").is_some())
}

/// DEBUG (MMTK_DEBUG_STACK_CHECK): snapshot of (root slot, its pre-GC object),
/// taken during root scanning and checked after the closure to catch a MIS-FORWARD
/// — a root updated to a valid-but-WRONG object (`new != forward(old)`), the
/// suspected control-flow-desync root cause. Empty/untouched unless the flag is on.
static ROOT_SNAPSHOT: Mutex<Vec<(FieldSlot, ObjectReference)>> = Mutex::new(Vec::new());

/// Map an OCaml `value` (a block's field-0 pointer == its ObjectReference raw
/// address) to a managed ObjectReference, or None if it is an immediate, null, or
/// a pointer outside any MMTk space (atom / pre-init / foreign — never collected).
#[inline]
fn managed_obj(v: usize) -> Option<ObjectReference> {
    let obj = ObjectReference::from_raw_address(unsafe { Address::from_usize(v) })?;
    if memory_manager::is_in_mmtk_spaces(obj) {
        Some(obj)
    } else {
        None
    }
}

/// Set for the duration of a generational NURSERY (minor) GC's weak/ephemeron
/// processing. While set, `ephe_is_reachable` treats any referent NOT resident in
/// the nursery as live: a minor GC only traces [0,young), so a mature object
/// reachable only through the mature heap is never visited and its liveness bit is
/// stale (set at the last FULL GC). Reading that stale bit as "dead" would let the
/// clean pass clear a still-reachable weak/ephemeron key or data. Mirrors stock
/// OCaml, where a minor collection clears only dead *young* referents. Clear (false)
/// for full GCs and for non-generational plans → behaviour is byte-identical there.
static NURSERY_GC: AtomicBool = AtomicBool::new(false);

/// True iff `o` resides in the generational nursery (young) space. False for
/// mature objects and for non-generational plans (no nursery). Consults the active
/// plan via its `GenerationalPlan` view (callable through the vtable; the trait is
/// crate-sealed in mmtk-core but its methods resolve on the trait object — same as
/// `.concurrent()` in api.rs).
fn in_nursery(o: ObjectReference) -> bool {
    crate::mmtk()
        .get_plan()
        .generational()
        .map_or(false, |g| g.is_object_in_nursery(o))
}

/// Weak-processing callbacks handed to the C ephemeron walk (runtime/mmtk.c).
/// `is_reachable`/`forward` are context-free; `retain` carries the GC worker's
/// tracer through `ctx` (a `*mut &mut dyn FnMut(usize) -> usize`).
extern "C" fn ephe_is_reachable(v: usize) -> i32 {
    // Foreign / immediate values are never collected → always "reachable" (1), so
    // the walk never clears a key/data that points at one.
    let o = match managed_obj(v) {
        Some(o) => o,
        None => return 1,
    };
    // During a generational nursery (minor) GC, only [0,young) was traced. A mature
    // referent was not visited this GC, so its mark/liveness bit is stale; do NOT
    // read it as dead and clear a still-reachable key/data. Treat any non-nursery
    // referent as live here, clearing only genuinely-dead nursery referents. Off
    // (NURSERY_GC=false) for full GCs / non-generational plans → unchanged there.
    if NURSERY_GC.load(Ordering::Relaxed) && !in_nursery(o) {
        return 1;
    }
    o.is_reachable() as i32
}

extern "C" fn ephe_forward(v: usize) -> usize {
    managed_obj(v)
        .and_then(|o| o.get_forwarded_object())
        .map_or(v, |o| o.to_raw_address().as_usize())
}

extern "C" fn ephe_retain(ctx: *mut c_void, v: usize) -> usize {
    let retain = unsafe { &mut *(ctx as *mut &mut dyn FnMut(usize) -> usize) };
    retain(v)
}

/// Callback handed to `caml_do_roots`/`caml_scan_global_roots`. `data` points to
/// the `Vec<FieldSlot>` being filled; `slot` is the address of a root value.
extern "C" fn collect_root_slot(data: *mut c_void, _v: usize, slot: *mut usize) {
    let buf = unsafe { &mut *(data as *mut Vec<FieldSlot>) };
    // ROOT slot: `checked` (always revalidates) — roots race with spawning/
    // terminating domains even under STW (GH#15). See slot.rs `from_address_root`.
    let fs = FieldSlot::from_address_root(Address::from_mut_ptr(slot));
    buf.push(fs);
    // DEBUG: record this root's pre-GC object so the post-closure pass can verify
    // it was forwarded (not mis-forwarded to a different valid object).
    if debug_check_enabled() {
        if let Some(old) = fs.load() {
            ROOT_SNAPSHOT.lock().unwrap().push((fs, old));
        }
    }
}

/// DEBUG (MMTK_DEBUG_STACK_CHECK): handed to caml_do_roots / caml_scan_global_roots
/// in process_weak_refs (after the strong closure forwarded everything, before
/// release). Flags any enumerated root slot whose referent is *forwarded but not
/// updated* — the signature of the partial-move relocation bug, wherever the slot
/// lives (value stack, local roots, finalisable, globals). Uses FieldSlot so infix
/// roots resolve to their (possibly forwarded) parent.
extern "C" fn check_root_slot(_data: *mut c_void, _v: usize, slot: *mut usize) {
    let fs = FieldSlot::from_address_root(Address::from_mut_ptr(slot));
    if let Some(obj) = fs.load() {
        if let Some(fwd) = obj.get_forwarded_object() {
            if fwd.to_raw_address() != obj.to_raw_address() {
                eprintln!(
                    "[STALE-ROOT] slot {:p} -> {:#x} forwarded to {:#x}",
                    slot,
                    obj.to_raw_address().as_usize(),
                    fwd.to_raw_address().as_usize()
                );
            }
        }
    }
}

/// Callback handed to `caml_scan_stack` while tracing a continuation block: feed
/// each fiber-stack slot to the GC worker's slot visitor. `data` points to a
/// `&mut dyn SlotVisitor<FieldSlot>`.
extern "C" fn visit_cont_stack_slot(data: *mut c_void, _v: usize, slot: *mut usize) {
    let visitor = unsafe { &mut *(data as *mut &mut dyn SlotVisitor<FieldSlot>) };
    visitor.visit_slot(FieldSlot::from_address(Address::from_mut_ptr(slot)));
}

pub struct VMScanning;

impl Scanning<OCamlVM> for VMScanning {
    /// Enumerate the roots of a single OCaml domain (the parked mutator).
    fn scan_roots_in_mutator_thread(
        _tls: VMWorkerThread,
        mutator: &'static mut Mutator<OCamlVM>,
        mut factory: impl RootsWorkFactory<FieldSlot>,
    ) {
        // The mutator's tls is the address of this domain's caml_domain_state,
        // which is exactly what caml_do_roots wants as its `domain` argument.
        let domain = mutator.mutator_tls.0 .0.to_address().to_mut_ptr::<c_void>();

        let mut buf: Vec<FieldSlot> = Vec::new();
        let buf_ptr = (&mut buf as *mut Vec<FieldSlot>).cast::<c_void>();
        // do_final_val: with MMTk-native finalisers (weak-refs mode) we must let
        // finalisable *values* become unreachable so process_weak_refs can detect
        // and run them — so 0 there (the root scan still keeps finaliser functions
        // and the run-queue alive). Off (default): 1, keeping all finalisable
        // values alive (finalisers never run — the conservative interim).
        let do_final_val = if weak_refs_enabled() { 0 } else { 1 };
        unsafe {
            caml_do_roots(
                collect_root_slot,
                0, // darken_scanning_flags: scan everything
                buf_ptr,
                domain,
                do_final_val,
            );
            // Weak arrays / ephemerons. With MMTK_WEAK_REFS off (default), keep the
            // whole ephemeron graph alive + updated by rooting the domain's
            // ephe_info lists (conservative interim — see runtime/mmtk.c). With it
            // on, do NOT root them here: process_weak_refs gives them proper weak
            // semantics (rooting them would defeat clearing by keeping all alive).
            if !weak_refs_enabled() {
                caml_mmtk_scan_ephe_roots(collect_root_slot, buf_ptr, domain);
            }
        }
        if !buf.is_empty() {
            factory.create_process_roots_work(buf);
        }
    }

    /// Enumerate program-wide roots not owned by a domain: caml_globals[] and
    /// registered C global roots.
    fn scan_vm_specific_roots(
        _tls: VMWorkerThread,
        mut factory: impl RootsWorkFactory<FieldSlot>,
    ) {
        let mut buf: Vec<FieldSlot> = Vec::new();
        let buf_ptr = (&mut buf as *mut Vec<FieldSlot>).cast::<c_void>();
        // Same gate as scan_roots_in_mutator_thread's caml_do_roots call: with
        // MMTk-native finalisers (weak-refs mode) leave finalisable *values*
        // unrooted (0) so dead ones can be detected and their finalisers run;
        // otherwise keep them alive (1). The run-queue values are always rooted
        // inside caml_mmtk_scan_orphaned_finalisers regardless.
        let do_final_val = if weak_refs_enabled() { 0 } else { 1 };
        unsafe {
            caml_scan_global_roots(collect_root_slot, buf_ptr);
            // Finalisers orphaned by terminated domains are referenced only from
            // [orph_structs] until adopted — no domain root scan covers them, so
            // root their fun/val slots here too (fixes finaliser_handover UAF).
            caml_mmtk_scan_orphaned_finalisers(collect_root_slot, 0, buf_ptr, do_final_val);
        }
        if !buf.is_empty() {
            factory.create_process_roots_work(buf);
        }
    }

    /// Trace all pointer fields of a live OCaml heap block.
    fn scan_object<SV: SlotVisitor<FieldSlot>>(
        _tls: VMWorkerThread,
        object: ObjectReference,
        slot_visitor: &mut SV,
    ) {
        // A continuation block holds a suspended fiber stack (field 0) that is
        // reachable only through this block; scan_ocaml_object treats Cont_tag as an
        // ordinary block and skips it (field 0 reads as an immediate). Scan that
        // stack (and its parent chain) via caml_scan_stack — the analogue of stock
        // caml_darken_cont — feeding each stack slot to the slot visitor. Without
        // this, a GC taken while a callback/continuation has detached the parent
        // fiber chain reclaims live stack objects (crash on resume; cf. nested_fiber).
        // Per-continuation scan lock (mirrors vanilla's NOT_MARKABLE header lock in
        // caml_darken_cont). Under ConcurrentImmix this scan runs on a GC worker
        // DURING concurrent marking, while another domain may resume this very
        // continuation (caml_continuation_use_noexc -> switch onto the fiber and
        // mutate it). We must TRY-LOCK the cont FIRST, then read field 0 UNDER the
        // lock: that closes the window where the worker reads the stack pointer and
        // begins scanning just as a resume takes the stack (nulls field 0) and
        // switches onto it. If we win the lock and field 0 still holds a stack, scan
        // it (suspended/immutable while locked); if we lose the lock (a resuming
        // mutator or another worker holds it) -> SKIP. A skipped-because-resumed cont
        // becomes the resuming domain's RUNNING stack + is snapshotted by the resume
        // path's SATB scan, so no root is lost; and STW does not scale with fiber
        // count (suspended fibers are scanned here, concurrently). (STW collectors
        // scan at a safepoint with mutators stopped, so try_lock always wins -> inert.)
        if continuation_stack(object).is_some() {
            let cont_addr = object.to_raw_address().as_usize();
            if crate::cont_lock::try_lock(cont_addr) {
                // Re-read field 0 under the lock: a resume cannot have taken it now.
                if let Some(stack) = continuation_stack(object) {
                    let mut dyn_visitor: &mut dyn SlotVisitor<FieldSlot> = slot_visitor;
                    let data = (&mut dyn_visitor as *mut &mut dyn SlotVisitor<FieldSlot>)
                        .cast::<c_void>();
                    unsafe {
                        caml_scan_stack(
                            visit_cont_stack_slot,
                            0,
                            data,
                            stack.to_mut_ptr::<c_void>(),
                            core::ptr::null_mut(),
                        );
                    }
                }
                crate::cont_lock::unlock(cont_addr);
            }
        }
        scan_ocaml_object(object, slot_visitor);
    }

    /// MMTk-native weak / ephemeron processing (M6, gated by MMTK_WEAK_REFS).
    /// Called after the strong transitive closure. Runs the ephemeron mark
    /// fixpoint — each invocation does one mark round across all domains, retaining
    /// the data of ephemerons whose keys are all reachable; returning `true` makes
    /// MMTk re-run us after the retained closure settles. When a round retains
    /// nothing new (fixpoint), we run the clean pass (clear dead keys/data, forward
    /// survivors) and return `false`. When the flag is off, this is a no-op and the
    /// conservative caml_mmtk_scan_ephe_roots scheme (in root scanning) applies.
    fn process_weak_refs(
        worker: &mut GCWorker<OCamlVM>,
        tracer_context: impl ObjectTracerContext<OCamlVM>,
    ) -> bool {
        if !weak_refs_enabled() {
            return false;
        }
        // Mark whether THIS GC is a generational nursery (minor) collection, so
        // ephe_is_reachable conservatively treats mature (non-nursery) referents as
        // live: a minor GC traces only [0,young), so a mature object's liveness bit
        // is stale and must not drive weak/ephemeron clearing (mirrors stock's
        // minor rule). false for full GCs and non-generational plans. process_weak_refs
        // runs on a single GC worker during STW, so a plain relaxed store is enough.
        //
        // Bactrian refinement: its FinalMark pause is nursery-anchored
        // (is_current_gc_nursery() is true — the modbuf/promotion machinery needs
        // that), but by the time this weak stage runs the concurrent marking cycle
        // has fully drained, so mature mark state IS complete and mature-dead
        // weaks/ephemerons must be judged (and cleared) normally — that is the whole
        // point of the cycle. current_pause_finishes_mark() distinguishes the
        // cycle-completing pause from a mid-cycle nursery pause (stale marks).
        let plan = crate::mmtk().get_plan();
        let marks_complete = plan
            .concurrent()
            .map_or(false, |c| c.current_pause_finishes_mark());
        NURSERY_GC.store(
            plan.generational()
                .map_or(false, |g| g.is_current_gc_nursery())
                && !marks_complete,
            Ordering::Relaxed,
        );
        let domains = domain_addrs();

        // One retention round, with access to the tracer for resurrecting objects.
        // Two phases, ordered so finalisers never fire prematurely:
        //   1. ephemeron marking — retain data of all-keys-reachable ephemerons;
        //   2. ONLY once ephemeron marking has converged this round, finalise-first
        //      — retain + queue now-unreachable Gc.finalise values.
        // Either phase making progress returns `true`, so MMTk re-runs us after the
        // retained closure settles (a retained finaliser value may revive an
        // ephemeron key, and vice-versa — the joint fixpoint handles both).
        let progress = tracer_context.with_tracer(worker, |tracer| {
            let mut retain = |v: usize| -> usize {
                match managed_obj(v) {
                    Some(o) => tracer.trace_object(o).to_raw_address().as_usize(),
                    None => v,
                }
            };
            let mut retain_dyn: &mut dyn FnMut(usize) -> usize = &mut retain;
            let ctx = (&mut retain_dyn as *mut &mut dyn FnMut(usize) -> usize).cast::<c_void>();

            // Adopt finalisers orphaned by terminated domains into the first live
            // domain before the finaliser pass runs, so they are processed (and
            // their dead values queued) like any other. Drains the orphan list, so
            // later fixpoint rounds — and GCs with no orphans — are no-ops.
            if let Some(&d0) = domains.first() {
                unsafe { caml_mmtk_adopt_orphaned_finalisers(d0, ephe_retain, ctx) };
            }

            let mut ephe = false;
            for &d in &domains {
                ephe |= unsafe {
                    caml_mmtk_ephe_mark_pass(d, ephe_is_reachable, ephe_forward, ephe_retain, ctx)
                } != 0;
            }
            if ephe {
                return true; // keep marking ephemerons before touching finalisers
            }

            let mut fin = false;
            for &d in &domains {
                fin |= unsafe {
                    caml_mmtk_final_update_first(d, ephe_is_reachable, ephe_retain, ctx)
                } != 0;
            }
            fin
        });

        if progress {
            return true; // re-run after the VMRefClosure bucket drains
        }

        // Fixpoint reached. Clean up (no resurrection happens here):
        //   - ephemerons: clear dead keys/data, forward survivors;
        //   - finalisers: queue dead finalise_last values (as unit), forward the
        //     surviving table values (they are not roots in weak-refs mode).
        for &d in &domains {
            unsafe {
                caml_mmtk_ephe_clean_pass(d, ephe_is_reachable, ephe_forward);
                caml_mmtk_final_cleanup(
                    d,
                    ephe_is_reachable,
                    ephe_forward,
                    ephe_retain,
                    core::ptr::null_mut(),
                );
            }
        }

        // DEBUG (moving-GC bug hunt, MMTK_DEBUG_STACK_CHECK): verify each root that
        // was snapshotted pre-GC was *forwarded*, not MIS-forwarded. After the
        // closure, each root's object must equal forward(old); a *different valid*
        // object = a mis-forward (Immix in-place partial defrag updating a reference
        // to the wrong object) — the suspected control-flow-desync root cause. Print
        // the offending slot, then clear the snapshot for the next GC.
        if debug_check_enabled() {
            if let Ok(mut snap) = ROOT_SNAPSHOT.lock() {
                for &(fs, old) in snap.iter() {
                    let expected = old.get_forwarded_object().unwrap_or(old);
                    match fs.load() {
                        Some(n) if n == expected => {}
                        got => eprintln!(
                            "[MIS-FORWARD] slot {:#x}: old {:#x} expected fwd {:#x} got {:#x}",
                            fs.as_address().as_usize(),
                            old.to_raw_address().as_usize(),
                            expected.to_raw_address().as_usize(),
                            got.map(|o| o.to_raw_address().as_usize()).unwrap_or(0),
                        ),
                    }
                }
                snap.clear();
            }
        }
        false
    }

    fn notify_initial_thread_scan_complete(_partial_scan: bool, _tls: VMWorkerThread) {}

    fn supports_return_barrier() -> bool {
        false
    }

    fn prepare_for_roots_re_scanning() {}
}
