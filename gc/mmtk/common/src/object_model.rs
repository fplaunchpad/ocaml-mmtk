//! Shared VMObjectModel helpers for OCaml.
//!
//! Object layout in memory:
//!
//!   alloc result  →  [ header word ][ field 0 ][ field 1 ] … [ field N-1 ]
//!   object ref    →                 ^ (one WORD_SIZE past alloc result)
//!
//! The object reference points to field 0; the header is one word before it.
//! MMTk's `ref_to_object_start` must return the header address (= alloc result).

use std::sync::atomic::{AtomicUsize, Ordering};

use mmtk::util::copy::{CopySemantics, GCWorkerCopyContext};
use mmtk::util::{Address, ObjectReference};
use mmtk::vm::VMBinding;

use crate::header::{tag_of, wosize_of, WORD_SIZE};

/// Count of objects relocated by copying collectors (Immix defrag, etc.).
/// Exposed so the runtime can confirm/report that movement actually happened.
pub static OBJECTS_COPIED: AtomicUsize = AtomicUsize::new(0);
/// Whether to count copies (MMTK_VERBOSE / MMTK_PAUSE_LOG runs). A locked
/// fetch_add per copied object costs ~20-40 cycles; at 19M copies per run
/// (binarytrees@16M nursery) that is ~0.4-0.8G cycles of pure telemetry.
pub static COUNT_COPIES: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

/// Byte offset from the MMTk allocation result to the OCaml object reference.
pub const OBJECT_REF_OFFSET: usize = WORD_SIZE;

// ── Address helpers ───────────────────────────────────────────────────────

/// Address of the header word (= alloc result = object start).
#[inline(always)]
pub fn ref_to_header(object: ObjectReference) -> Address {
    object.to_raw_address() - OBJECT_REF_OFFSET
}

/// Address of the first allocated byte (same as the header for OCaml).
#[inline(always)]
pub fn ref_to_object_start(object: ObjectReference) -> Address {
    object.to_raw_address() - OBJECT_REF_OFFSET
}

// ── Header access ─────────────────────────────────────────────────────────

/// Read the header word of a live OCaml object.
///
/// # Safety boundary
/// `object` must be a live `ObjectReference` inside MMTk's managed heap.
/// `ObjectReference` guarantees non-null (`NonZeroUsize`) and `WORD_SIZE`
/// alignment, so `object.to_raw_address() - WORD_SIZE` is always a valid,
/// readable address by OCaml's layout invariant (`Val_hp`).
#[inline(always)]
pub fn read_header(object: ObjectReference) -> usize {
    unsafe { ref_to_header(object).load() }
}

// ── Size ──────────────────────────────────────────────────────────────────

/// Total allocated size of an object in bytes: header word + all fields.
#[inline(always)]
pub fn get_current_size(object: ObjectReference) -> usize {
    let wosize = wosize_of(read_header(object));
    (wosize + 1) * WORD_SIZE // +1 for the header word itself
}

// ── Copy ─────────────────────────────────────────────────────────────────

/// Copy `object` to a new location allocated through `copy_context`.
/// Returns the new `ObjectReference`.
///
/// Used by Immix defragmentation, SemiSpace, GenCopy, and any other
/// copying/moving GC plan.
pub fn copy_object<VM: VMBinding>(
    from: ObjectReference,
    semantics: CopySemantics,
    copy_context: &mut GCWorkerCopyContext<VM>,
) -> ObjectReference {
    let size = get_current_size(from);
    let to_start = copy_context.alloc_copy(from, size, WORD_SIZE, 0, semantics);
    assert!(
        !to_start.is_zero(),
        "alloc_copy returned null for object {:#x} ({} bytes, semantics {:?}): \
         tospace exhausted during GC — heap is full and evacuation cannot complete",
        from.to_raw_address().as_usize(),
        size,
        semantics,
    );
    // TODO(oom-protocol): Replace the assert above with a proper two-way OOM
    // protocol: the GC worker should signal the mutator thread to raise an OCaml
    // `Out_of_memory` exception rather than aborting the process. Until VMCollection
    // STW and root scanning are in place, abort on OOM is correct and safe.

    // Bulk-copy header + all fields to the new location.
    unsafe {
        std::ptr::copy_nonoverlapping(
            ref_to_object_start(from).to_ptr::<u8>(),
            to_start.to_mut_ptr::<u8>(),
            size,
        );
    }

    // The new object reference is one word past the new allocation start.
    let to_ref = unsafe {
        ObjectReference::from_raw_address_unchecked(to_start + OBJECT_REF_OFFSET)
    };
    copy_context.post_copy(to_ref, size, semantics);
    if COUNT_COPIES.load(Ordering::Relaxed) {
        OBJECTS_COPIED.fetch_add(1, Ordering::Relaxed);
    }
    to_ref
}

/// Copy-to variant used by delayed-copy (compacting) collectors.
/// `to` is the destination ObjectReference pre-computed by the forward phase.
/// Returns the address past the end of the copied object.
pub fn copy_to_object(from: ObjectReference, to: ObjectReference) -> Address {
    let size = get_current_size(from);
    let dst = ref_to_object_start(to);
    unsafe {
        std::ptr::copy_nonoverlapping(
            ref_to_object_start(from).to_ptr::<u8>(),
            dst.to_mut_ptr::<u8>(),
            size,
        );
    }
    dst + size
}

// ── Debug ─────────────────────────────────────────────────────────────────

/// Print a one-line description of an OCaml heap object to stderr.
pub fn dump_object(object: ObjectReference) {
    let header = read_header(object);
    eprintln!(
        "OCaml object @ {:#x}: wosize={} tag={}",
        object.to_raw_address().as_usize(),
        wosize_of(header),
        tag_of(header),
    );
}

/// Predict where the object reference will be once the object is copied
/// to `to` (start of reserved region).
#[inline(always)]
pub fn get_reference_when_copied_to(_from: ObjectReference, to: Address) -> ObjectReference {
    debug_assert!(
        !to.is_zero() && to.is_aligned_to(WORD_SIZE),
        "get_reference_when_copied_to: invalid region address {:#x} (null or misaligned)",
        to.as_usize()
    );
    unsafe { ObjectReference::from_raw_address_unchecked(to + OBJECT_REF_OFFSET) }
}
