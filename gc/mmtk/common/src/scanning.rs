//! Shared scan_object implementation for all OCaml heap blocks.
//!
//! Every live OCaml block reached during tracing is passed here; we visit
//! every field that may hold a heap pointer.

use mmtk::util::ObjectReference;
use mmtk::vm::SlotVisitor;

use crate::header::{
    tag_of, wosize_of, TAG_CLOSURE, TAG_FORWARD, TAG_INFIX, TAG_NO_SCAN, WORD_SIZE,
};
use crate::slot::FieldSlot;

/// Visit all GC-visible pointer fields of an OCaml heap block.
///
/// The caller is responsible for ensuring `object` is a valid, live OCaml block
/// (i.e. not a tagged integer and not null).
pub fn scan_ocaml_object<SV: SlotVisitor<FieldSlot>>(
    object: ObjectReference,
    slot_visitor: &mut SV,
) {
    let base = object.to_raw_address();

    // Read header — one word before the object reference.
    let header: usize = unsafe { (base - WORD_SIZE).load() };
    let tag = tag_of(header);
    let wosize = wosize_of(header);

    // Tags >= TAG_NO_SCAN (Abstract, String, Double, Double_array, Custom) carry
    // no GC-visible pointer fields; nothing to visit.
    if tag >= TAG_NO_SCAN {
        return;
    }

    match tag {
        TAG_INFIX => {
            // An infix block is an interior pointer into a closure.  The parent
            // closure will itself be reached and scanned via its own object reference,
            // so we skip the infix block entirely.
            //
            // TODO: moving/compacting GC must redirect the infix pointer after the
            // parent closure moves.  Implement in copy_object when adding Immix defrag.
        }

        TAG_CLOSURE => {
            // Field 0 is a raw code pointer (address into .text section).
            // It is *not* a GC root and must not be treated as one.
            // Scan fields 1..wosize (the closure environment).
            //
            // TODO: multi-entry closures (arity > 1) have an arity word and
            // additional code-pointer slots interspersed — audit once we have
            // closures in benchmarks.
            for i in 1..wosize {
                let slot_addr = base + i * WORD_SIZE;
                slot_visitor.visit_slot(FieldSlot::from_address(slot_addr));
            }
        }

        TAG_FORWARD => {
            // Forwarding pointer: field 0 holds the new location of a moved object.
            // Visit it so MMTk can update the chain if the target also moves.
            if wosize > 0 {
                slot_visitor.visit_slot(FieldSlot::from_address(base));
            }
        }

        _ => {
            // Ordinary block (tag 0..245), Lazy (246), Object (248):
            // all fields are OCaml values — each may be an immediate int or
            // a heap pointer.  FieldSlot::load() filters out immediates.
            for i in 0..wosize {
                let slot_addr = base + i * WORD_SIZE;
                slot_visitor.visit_slot(FieldSlot::from_address(slot_addr));
            }
        }
    }
}
