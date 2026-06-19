//! VMReferenceGlue for OCaml 5.x — weak references and finalizers.
//!
//! Deferred until basic per-domain GC works (post-M2).

use mmtk::util::opaque_pointer::VMWorkerThread;
use mmtk::util::ObjectReference;
use mmtk::vm::ReferenceGlue;

use crate::OCamlVM;

pub struct VMReferenceGlue;

impl ReferenceGlue<OCamlVM> for VMReferenceGlue {
    type FinalizableType = ObjectReference;

    fn clear_referent(_new_reference: ObjectReference) {
        unimplemented!("OCaml finalizers — implement after basic GC works")
    }

    fn get_referent(_object: ObjectReference) -> Option<ObjectReference> {
        unimplemented!()
    }

    fn set_referent(_reff: ObjectReference, _referent: ObjectReference) {
        unimplemented!()
    }

    fn enqueue_references(_references: &[ObjectReference], _tls: VMWorkerThread) {
        unimplemented!()
    }
}
