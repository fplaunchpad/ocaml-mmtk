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
    /// DEBUG (moving-GC bug hunt): a domain's bytecode value-stack live range
    /// [*lo, *hi). Used by the post-GC stale-root check.
    fn caml_mmtk_debug_stack_range(domain: usize, lo: *mut *mut usize, hi: *mut *mut usize);
}

#[inline]
fn weak_refs_enabled() -> bool {
    unsafe { caml_mmtk_weak_refs != 0 }
}

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

/// Weak-processing callbacks handed to the C ephemeron walk (runtime/mmtk.c).
/// `is_reachable`/`forward` are context-free; `retain` carries the GC worker's
/// tracer through `ctx` (a `*mut &mut dyn FnMut(usize) -> usize`).
extern "C" fn ephe_is_reachable(v: usize) -> i32 {
    // Foreign / immediate values are never collected → always "reachable" (1), so
    // the walk never clears a key/data that points at one.
    managed_obj(v).map_or(1, |o| o.is_reachable() as i32)
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
    buf.push(FieldSlot::from_address(Address::from_mut_ptr(slot)));
}

/// DEBUG (MMTK_DEBUG_STACK_CHECK): handed to caml_do_roots / caml_scan_global_roots
/// in process_weak_refs (after the strong closure forwarded everything, before
/// release). Flags any enumerated root slot whose referent is *forwarded but not
/// updated* — the signature of the partial-move relocation bug, wherever the slot
/// lives (value stack, local roots, finalisable, globals). Uses FieldSlot so infix
/// roots resolve to their (possibly forwarded) parent.
extern "C" fn check_root_slot(_data: *mut c_void, _v: usize, slot: *mut usize) {
    let fs = FieldSlot::from_address(Address::from_mut_ptr(slot));
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
        unsafe {
            caml_scan_global_roots(
                collect_root_slot,
                (&mut buf as *mut Vec<FieldSlot>).cast::<c_void>(),
            );
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

        // DEBUG (moving-GC bug hunt): after the root scan has forwarded everything
        // it found, re-walk each domain's bytecode value stack raw. Any slot still
        // holding a pointer to a *forwarded* object is a stack root the scan failed
        // to update — the missed-root bug. Prints the slot so we can map it to the
        // interpreter frame. Gated on MMTK_DEBUG_STACK_CHECK to stay off by default.
        if std::env::var_os("MMTK_DEBUG_STACK_CHECK").is_some() {
            for &d in &domains {
                let (mut lo, hi): (*mut usize, *mut usize) = unsafe {
                    let mut lo = core::ptr::null_mut();
                    let mut hi = core::ptr::null_mut();
                    caml_mmtk_debug_stack_range(d, &mut lo, &mut hi);
                    (lo, hi)
                };
                while !lo.is_null() && (lo as usize) < (hi as usize) {
                    let word = unsafe { *lo };
                    if let Some(obj) = managed_obj(word) {
                        if let Some(fwd) = obj.get_forwarded_object() {
                            let new = fwd.to_raw_address().as_usize();
                            if new != word {
                                eprintln!(
                                    "[STALE-ROOT/fwd] stack slot {:p} = {:#x} -> forwarded {:#x}",
                                    lo, word, new
                                );
                            }
                        } else if !obj.is_reachable() {
                            // Slot points to an unreachable (collected) object: a
                            // dangling stack root the scan never traced.
                            let hdr = unsafe { *((word as *const usize).offset(-1)) };
                            eprintln!(
                                "[STALE-ROOT/dead] stack slot {:p} = {:#x} unreachable (hdr {:#x})",
                                lo, word, hdr
                            );
                        }
                    }
                    lo = unsafe { lo.add(1) };
                }
            }
            // Generalised: check ALL enumerated roots (value stack, local roots,
            // finalisable, globals) for a slot still pointing at a forwarded object
            // — catches the partial-move relocation bug wherever the missed slot is,
            // not just on the raw value stack above.
            for &d in &domains {
                unsafe {
                    caml_do_roots(
                        check_root_slot,
                        0,
                        core::ptr::null_mut(),
                        d as *mut c_void,
                        0,
                    );
                }
            }
            unsafe { caml_scan_global_roots(check_root_slot, core::ptr::null_mut()) };
        }
        false
    }

    fn notify_initial_thread_scan_complete(_partial_scan: bool, _tls: VMWorkerThread) {}

    fn supports_return_barrier() -> bool {
        false
    }

    fn prepare_for_roots_re_scanning() {}
}
