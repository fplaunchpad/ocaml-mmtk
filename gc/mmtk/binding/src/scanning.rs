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

use mmtk::util::opaque_pointer::VMWorkerThread;
use mmtk::util::{Address, ObjectReference};
use mmtk::vm::SlotVisitor;
use mmtk::vm::{RootsWorkFactory, Scanning};
use mmtk::Mutator;

use mmtk_ocaml_common::scanning::scan_ocaml_object;
use mmtk_ocaml_common::slot::FieldSlot;

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
            // Keep this domain's weak arrays / ephemerons alive + updated (interim;
            // they are otherwise unreachable and would dangle — see runtime/mmtk.c).
            caml_mmtk_scan_ephe_roots(collect_root_slot, buf_ptr, domain);
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

    fn notify_initial_thread_scan_complete(_partial_scan: bool, _tls: VMWorkerThread) {}

    fn supports_return_barrier() -> bool {
        false
    }

    fn prepare_for_roots_re_scanning() {}
}
