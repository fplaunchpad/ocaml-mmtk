//! VMScanning for OCaml 5.x — root enumeration and object tracing.
//!
//! Root enumeration (M2):
//!   OCaml 5.x provides `caml_do_roots(scanning_action f, flags, void* fdata,
//!   caml_domain_state* domain, int do_final)`.  The callback is
//!   `void f(void* fdata, value v, volatile value* slot_addr)`.
//!   We collect every `slot_addr` as a FieldSlot and pass to MMTk.

use mmtk::util::opaque_pointer::VMWorkerThread;
use mmtk::util::ObjectReference;
use mmtk::vm::SlotVisitor;
use mmtk::vm::{RootsWorkFactory, Scanning};
use mmtk::Mutator;

use mmtk_ocaml_common::scanning::scan_ocaml_object;
use mmtk_ocaml_common::slot::FieldSlot;

use crate::OCamlVM;

pub struct VMScanning;

impl Scanning<OCamlVM> for VMScanning {
    /// Enumerate all GC roots for a single OCaml 5.x domain.
    fn scan_roots_in_mutator_thread(
        _tls: VMWorkerThread,
        _mutator: &'static mut Mutator<OCamlVM>,
        _factory: impl RootsWorkFactory<FieldSlot>,
    ) {
        todo!(
            "OCaml 5.x scan_roots_in_mutator_thread: \
             call caml_do_roots per domain with a FieldSlot-collecting callback"
        )
    }

    /// Enumerate global roots not owned by any specific domain.
    fn scan_vm_specific_roots(
        _tls: VMWorkerThread,
        _factory: impl RootsWorkFactory<FieldSlot>,
    ) {
        todo!(
            "OCaml 5.x scan_vm_specific_roots: \
             enumerate caml_global_roots and any shared heap roots"
        )
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
