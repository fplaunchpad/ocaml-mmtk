use mmtk::util::copy::{CopySemantics, GCWorkerCopyContext};
use mmtk::util::{Address, ObjectReference};
use mmtk::vm::ObjectModel;
use mmtk::vm::{
    VMGlobalFieldUnlogBitSpec, VMGlobalLogBitSpec, VMLocalForwardingBitsSpec,
    VMLocalForwardingPointerSpec, VMLocalLOSMarkNurserySpec, VMLocalMarkBitSpec,
    VMLocalPinningBitSpec,
};

use mmtk_ocaml_common::header::WORD_SIZE;
use mmtk_ocaml_common::object_model as common;

use crate::OCamlVM;

pub struct VMObjectModel;

impl ObjectModel<OCamlVM> for VMObjectModel {
    const GLOBAL_LOG_BIT_SPEC: VMGlobalLogBitSpec =
        VMGlobalLogBitSpec::side_first();

    // P4 (LXR): the per-field unlog bit the coalescing field-logging write barrier uses.
    // Laid out `side_after` the per-object log bit so the two occupy disjoint side-metadata
    // regions (the trait default `side_first()` would collide with GLOBAL_LOG_BIT_SPEC).
    // Inert for every non-LXR plan (only the FieldBarrier reads/writes it).
    const GLOBAL_FIELD_UNLOG_BIT_SPEC: VMGlobalFieldUnlogBitSpec =
        VMGlobalFieldUnlogBitSpec::side_after(Self::GLOBAL_LOG_BIT_SPEC.as_spec());

    const LOCAL_FORWARDING_POINTER_SPEC: VMLocalForwardingPointerSpec =
        VMLocalForwardingPointerSpec::in_header(0);

    const LOCAL_FORWARDING_BITS_SPEC: VMLocalForwardingBitsSpec =
        VMLocalForwardingBitsSpec::side_first();

    // Stock OCaml's own minor-GC protocol, made available to mmtk-core's
    // single-tracer (UP) pauses: an OCaml header `(wosize << 10) | colour |
    // tag` is numerically below 2^41 for every real object (forwardable
    // spaces cap object size at 16 KiB anyway), while the heap — and thus any
    // forwarding pointer — starts at vm_layout().heap_start = 0x200_0000_0000
    // = 2^41. So the header word at LOCAL_FORWARDING_POINTER_SPEC (the header
    // itself, in_header(0)) discriminates forwarding state by value range,
    // and UP traces touch NO side forwarding metadata at all — exactly
    // vanilla oldify's header-overwrite discipline. Multi-tracer pauses still
    // use LOCAL_FORWARDING_BITS_SPEC above.
    const HEADER_FORWARDING_SENTINEL: bool = true;

    const LOCAL_MARK_BIT_SPEC: VMLocalMarkBitSpec =
        VMLocalMarkBitSpec::side_after(Self::LOCAL_FORWARDING_BITS_SPEC.as_spec());

    const LOCAL_LOS_MARK_NURSERY_SPEC: VMLocalLOSMarkNurserySpec =
        VMLocalLOSMarkNurserySpec::side_after(Self::LOCAL_MARK_BIT_SPEC.as_spec());

    const LOCAL_PINNING_BIT_SPEC: VMLocalPinningBitSpec =
        VMLocalPinningBitSpec::side_after(Self::LOCAL_LOS_MARK_NURSERY_SPEC.as_spec());

    const OBJECT_REF_OFFSET_LOWER_BOUND: isize = common::OBJECT_REF_OFFSET as isize;

    fn ref_to_object_start(object: ObjectReference) -> Address {
        common::ref_to_object_start(object)
    }

    fn ref_to_header(object: ObjectReference) -> Address {
        common::ref_to_header(object)
    }

    fn get_current_size(object: ObjectReference) -> usize {
        common::get_current_size(object)
    }

    fn get_size_when_copied(object: ObjectReference) -> usize {
        common::get_current_size(object)
    }

    fn get_align_when_copied(_object: ObjectReference) -> usize {
        WORD_SIZE
    }

    fn get_align_offset_when_copied(_object: ObjectReference) -> usize {
        0
    }

    fn copy(
        from: ObjectReference,
        semantics: CopySemantics,
        copy_context: &mut GCWorkerCopyContext<OCamlVM>,
    ) -> ObjectReference {
        common::copy_object::<OCamlVM>(from, semantics, copy_context)
    }

    fn copy_to(from: ObjectReference, to: ObjectReference, _region: Address) -> Address {
        common::copy_to_object(from, to)
    }

    fn get_reference_when_copied_to(from: ObjectReference, to: Address) -> ObjectReference {
        common::get_reference_when_copied_to(from, to)
    }

    fn get_type_descriptor(_reference: ObjectReference) -> &'static [i8] {
        &[]
    }

    fn dump_object(object: ObjectReference) {
        common::dump_object(object);
    }
}
