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
use mmtk::vm::SlotVisitor;
use mmtk::vm::{ObjectTracer, ObjectTracerContext, RootsWorkFactory, Scanning};
use mmtk::Mutator;

use mmtk_ocaml_common::scanning::scan_ocaml_object;
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
        unsafe {
            caml_do_roots(
                collect_root_slot,
                0, // darken_scanning_flags: scan everything
                buf_ptr,
                domain,
                1, // keep finalisable values alive (we don't run finalisers yet)
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

        // One mark round across all domains, with access to the tracer for
        // retaining (resurrecting) the data of fully-reachable-key ephemerons.
        let progress = tracer_context.with_tracer(worker, |tracer| {
            let mut retain = |v: usize| -> usize {
                match managed_obj(v) {
                    Some(o) => tracer.trace_object(o).to_raw_address().as_usize(),
                    None => v,
                }
            };
            let mut retain_dyn: &mut dyn FnMut(usize) -> usize = &mut retain;
            let ctx = (&mut retain_dyn as *mut &mut dyn FnMut(usize) -> usize).cast::<c_void>();
            let mut any = false;
            for &d in &domains {
                let p = unsafe {
                    caml_mmtk_ephe_mark_pass(d, ephe_is_reachable, ephe_forward, ephe_retain, ctx)
                };
                any |= p != 0;
            }
            any
        });

        if progress {
            return true; // re-run after the VMRefClosure bucket drains
        }

        // Fixpoint reached: clear dead keys/data and forward survivors.
        for &d in &domains {
            unsafe { caml_mmtk_ephe_clean_pass(d, ephe_is_reachable, ephe_forward) };
        }
        false
    }

    fn notify_initial_thread_scan_complete(_partial_scan: bool, _tls: VMWorkerThread) {}

    fn supports_return_barrier() -> bool {
        false
    }

    fn prepare_for_roots_re_scanning() {}
}
