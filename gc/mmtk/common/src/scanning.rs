//! Shared scan_object implementation for all OCaml heap blocks.
//!
//! Every live OCaml block reached during tracing is passed here; we visit
//! every field that may hold a heap pointer.

use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

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

/// Skip fields that hold an immediate (tagged int, LSB = 1) instead of handing
/// them to the slot visitor. Set once at init by the binding
/// ([`set_skip_immediate_slots`]); off by default.
///
/// Why: every visited field becomes a 24-byte `FieldSlot` in a work-packet buffer
/// (4096 entries = 96 KiB), and an immediate's slot is only discarded later, at
/// `FieldSlot::load` (its cached `info` is `NOT_TRACEABLE`). Scanning int-heavy
/// blocks therefore built transient GC memory ~4.6x the scanned data (matmul 768:
/// +33 MiB of slot buffers at peak) and loaded every slot only to drop it.
/// mmtk-core's `Scanning` contract lets a VM omit non-reference fields
/// ("a tagged non-reference value such as small integer").
///
/// Sound wherever a slot's only consumer is `load` (+ `store` of what it loaded):
/// a `from_address` slot whose value was immediate at scan time is cached as
/// `NOT_TRACEABLE`, so `load` already returns `None` for it in EVERY plan, even if
/// a concurrent mutator later writes a pointer into the field — skipping it at scan
/// time drops exactly the slots that were already dead. (That later write is the
/// SATB barrier's business: the deletion barrier greys the OLD value, an immediate
/// = nothing to grey, and the NEW value is young or already marked by
/// allocate-black — unchanged by this.) NOT sound for LXR: its RC trace visits
/// every field of a freshly promoted object to UNLOG the field (`slot.to_address()`
/// in `scan_nursery_object` / the keep-alive scan), including immediate fields; a
/// skipped field would stay logged, so a later pointer store into it would bypass
/// the field barrier and its increment. The binding leaves this off for LXR.
pub static SKIP_IMMEDIATE_SLOTS: AtomicBool = AtomicBool::new(false);

/// Enable/disable [`SKIP_IMMEDIATE_SLOTS`]. Called once by the binding at init.
pub fn set_skip_immediate_slots(enabled: bool) {
    SKIP_IMMEDIATE_SLOTS.store(enabled, Ordering::Relaxed);
}

/// Visit the value fields `[from, to)` of the block at `base`.
#[inline(always)]
fn visit_fields<SV: SlotVisitor<FieldSlot>>(
    base: Address,
    from: usize,
    to: usize,
    slot_visitor: &mut SV,
) {
    if SKIP_IMMEDIATE_SLOTS.load(Ordering::Relaxed) {
        for i in from..to {
            let slot_addr = base + i * WORD_SIZE;
            // One load per field: classify from the value we just read.
            let raw = unsafe { (*slot_addr.to_ptr::<AtomicUsize>()).load(Ordering::Relaxed) };
            if raw & 1 != 0 {
                continue; // immediate: never a reference (see SKIP_IMMEDIATE_SLOTS)
            }
            slot_visitor.visit_slot(FieldSlot::from_address_with_value(slot_addr, raw));
        }
    } else {
        for i in from..to {
            let slot_addr = base + i * WORD_SIZE;
            slot_visitor.visit_slot(FieldSlot::from_address(slot_addr));
        }
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
            visit_fields(base, start_env, wosize, slot_visitor);
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
            // a heap pointer. Immediates are skipped here when
            // SKIP_IMMEDIATE_SLOTS is on, else filtered by FieldSlot::load().
            visit_fields(base, 0, wosize, slot_visitor);
        }
    }
}
