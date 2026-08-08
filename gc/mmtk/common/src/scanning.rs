//! Shared scan_object implementation for all OCaml heap blocks.
//!
//! Every live OCaml block reached during tracing is passed here; we visit
//! every field that may hold a heap pointer.

use mmtk::util::{Address, ObjectReference};
use mmtk::vm::SlotVisitor;

use crate::header::{
    tag_of, wosize_of, TAG_CLOSURE, TAG_CONTINUATION, TAG_FORWARD, TAG_INFIX, TAG_NO_SCAN,
    WORD_SIZE,
};
use crate::slot::FieldSlot;

/// If `object` is a continuation block (Cont_tag), return the address of the
/// suspended fiber `stack_info` it holds in field 0 (`Val_ptr(stack) = stack + 1`),
/// or `None` if it is not a continuation or the stack has been consumed
/// (`caml_continuation_use` leaves field 0 = `Val_unit`).
///
/// MMTk's `scan_ocaml_object` treats a Cont_tag block as an ordinary block, but its
/// field 0 reads as an immediate (low bit set) so the stack is skipped — and that
/// stack is reachable *only* through this block. The binding must therefore scan it
/// via `caml_scan_stack` (the analogue of stock `caml_darken_cont`); this helper
/// recovers the pointer (`Ptr_val`), keeping the layout knowledge in `common`.
pub fn continuation_stack(object: ObjectReference) -> Option<Address> {
    let base = object.to_raw_address();
    let header: usize = unsafe { (base - WORD_SIZE).load() };
    if tag_of(header) != TAG_CONTINUATION {
        return None;
    }
    let field0: usize = unsafe { base.load() }; // Val_ptr(stack), or Val_unit if consumed
    let stack = field0 & !1usize; // Ptr_val: clear the tag bit (Val_unit -> 0)
    if stack == 0 {
        None
    } else {
        Some(unsafe { Address::from_usize(stack) })
    }
}

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
            // OCaml closure layout (runtime/caml/mlvalues.h):
            //   field 0      : code pointer (raw, not a value)
            //   field 1      : closinfo (packed: arity in the top bits, and the
            //                  word offset to the environment; LSB=1 so it reads
            //                  as an immediate)
            //   fields 2..   : for mutually-recursive / multi-arity closures,
            //                  additional code/closinfo pairs and infix headers
            //   [start_env..]: the actual captured environment (the only values)
            //
            // Only the environment holds GC pointers, so scan exactly
            // [start_env, wosize). Everything before it is code/closinfo/infix.
            let closinfo = unsafe { (base + WORD_SIZE).load::<usize>() };
            // Start_env_closinfo(info) = (info << 8) >> 9   (see mlvalues.h)
            let start_env = (closinfo << 8) >> 9;
            for i in start_env..wosize {
                let slot_addr = base + i * WORD_SIZE;
                let raw: usize = unsafe { slot_addr.load() };
                if raw & 1 == 0 && raw != 0 {
                    slot_visitor.visit_slot(FieldSlot::from_address(slot_addr));
                }
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
            // a heap pointer. Pre-filter immediates/null with one load + bit
            // test (vanilla's oldify checks Is_block the same way) instead of
            // constructing a FieldSlot — whose classify pays the full
            // foreign-pointer path — for every tagged int. Roughly half of
            // OCaml fields are immediates; measured on the nursery trace.
            for i in 0..wosize {
                let slot_addr = base + i * WORD_SIZE;
                let raw: usize = unsafe { slot_addr.load() };
                if raw & 1 == 0 && raw != 0 {
                    slot_visitor.visit_slot(FieldSlot::from_address(slot_addr));
                }
            }
        }
    }
}
