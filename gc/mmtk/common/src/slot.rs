//! OCaml tagged-pointer slot (VMSlot) and memory-slice placeholder.
//!
//! OCaml value encoding:
//!   LSB = 1  →  immediate integer  (not a GC root; Slot::load returns None)
//!   LSB = 0  →  pointer to a heap block  (IS a GC root; Slot::load returns Some)
//!
//! `FieldSlot` stores the *address* of the value slot so MMTk can update
//! the pointer in-place when an object moves.

use std::fmt;
use std::hash::{Hash, Hasher};
use std::sync::atomic::{AtomicUsize, Ordering};

use mmtk::memory_manager;
use mmtk::util::{Address, ObjectReference};
use mmtk::vm::slot::{MemorySlice, Slot};

use crate::header::{tag_of, wosize_of, TAG_INFIX, WORD_SIZE};

// ── FieldSlot ─────────────────────────────────────────────────────────────

/// Sentinel `info` value meaning "this slot does not hold a traceable MMTk
/// reference" — i.e. it holds an immediate integer, null, or a pointer to an
/// object outside MMTk's heap (atoms, code, anything allocated before MMTk was
/// enabled). For such slots `load` returns `None` and `store` is never called.
const NOT_TRACEABLE: usize = usize::MAX;

/// A slot (address of a memory location) that holds an OCaml value.
///
/// The value may be a tagged integer (no GC action needed) or a heap pointer
/// (must be updated on object movement). For interior (infix) pointers — which
/// point *into* a closure block rather than at an object start — `info` records
/// the byte offset from the parent object's start to the interior pointer, so a
/// moving GC can re-derive the interior pointer as `new_parent + offset` after
/// the parent is forwarded.
///
/// `info` is classified once, at slot creation (during scanning, before any
/// object moves), and cached: the old object — including its infix header — may
/// be gone by the time `store` runs, so the offset cannot be recomputed then.
/// `info` is one of: `NOT_TRACEABLE`, `0` (ordinary reference), or a non-zero
/// infix byte offset.
#[derive(Clone, Copy)]
pub struct FieldSlot {
    addr: *mut AtomicUsize,
    info: usize,
}

// Raw pointer requires explicit Send; Slot trait bound requires it.
unsafe impl Send for FieldSlot {}

impl FieldSlot {
    #[inline]
    pub fn from_address(address: Address) -> Self {
        let addr = address.to_mut_ptr::<AtomicUsize>();
        let raw = unsafe { (*addr).load(Ordering::Relaxed) };
        Self { addr, info: Self::classify(raw) }
    }

    #[inline]
    pub fn as_address(&self) -> Address {
        Address::from_mut_ptr(self.addr)
    }

    #[inline]
    fn raw_value(&self) -> usize {
        unsafe { (*self.addr).load(Ordering::Relaxed) }
    }

    /// Classify the value `raw` once, returning the cached `info`:
    /// `NOT_TRACEABLE`, `0` (ordinary heap reference), or an infix byte offset.
    #[inline]
    fn classify(raw: usize) -> usize {
        if raw & 1 != 0 || raw == 0 {
            return NOT_TRACEABLE; // tagged integer (LSB=1) or null
        }
        let addr = unsafe { Address::from_usize(raw) };
        let obj = unsafe { ObjectReference::from_raw_address_unchecked(addr) };

        // Only objects MMTk actually manages are references it can trace. OCaml
        // has pointers outside any MMTk space — atoms (static zero-size blocks),
        // code addresses, objects allocated before MMTk was enabled. Tracing
        // those would make mmtk-core panic, and reading their "header" to test
        // for an infix tag would be a wild read; filter them out here.
        if !memory_manager::is_in_mmtk_spaces(obj) {
            return NOT_TRACEABLE;
        }

        // Infix pointers point *into* a closure block (at an Infix_tag header),
        // not at an object start. The GC must mark/trace/forward the PARENT
        // closure, not the infix object. The infix header's size field is the
        // offset, in words, from the parent closure to the infix object
        // (verified GC paper, Fig. 3): parent = infix - wosize(header) * WORD.
        let header = unsafe { (addr - WORD_SIZE).load::<usize>() };
        if tag_of(header) == TAG_INFIX {
            wosize_of(header) * WORD_SIZE
        } else {
            0
        }
    }
}

impl fmt::Debug for FieldSlot {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "FieldSlot({:#x} → {:#x})", self.as_address(), self.raw_value())
    }
}

impl PartialEq for FieldSlot {
    fn eq(&self, other: &Self) -> bool { self.addr == other.addr }
}
impl Eq for FieldSlot {}

impl Hash for FieldSlot {
    fn hash<H: Hasher>(&self, state: &mut H) { self.addr.hash(state); }
}

impl Slot for FieldSlot {
    /// Return the heap object this slot refers to, or None for a non-traceable
    /// value (immediate, null, or foreign pointer). Interior (infix) pointers
    /// are redirected to the parent object, which is what MMTk traces/forwards.
    fn load(&self) -> Option<ObjectReference> {
        if self.info == NOT_TRACEABLE {
            return None;
        }
        // raw - infix_offset is the object start (== raw for ordinary slots).
        let start = unsafe { Address::from_usize(self.raw_value()) } - self.info;
        Some(unsafe { ObjectReference::from_raw_address_unchecked(start) })
    }

    /// Overwrite the slot with a (possibly relocated) object reference,
    /// re-applying the infix offset so an interior pointer keeps pointing into
    /// the relocated parent at the same word offset.
    fn store(&self, object: ObjectReference) {
        let new_raw = object.to_raw_address().as_usize() + self.info;
        unsafe {
            (*self.addr).store(new_raw, Ordering::Relaxed);
        }
    }
}

// ── UnimplementedMemorySlice ───────────────────────────────────────────────

/// Placeholder for write-barrier memory-slice operations.
///
/// Required by the `VMMemorySlice` associated type on `VMBinding`.
/// Must be implemented to support generational plans (GenImmix, GenCopy)
/// that use array-copy write barriers.  Panics if called until then.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct UnimplementedMemorySlice;

impl MemorySlice for UnimplementedMemorySlice {
    type SlotType = FieldSlot;
    type SlotIterator = std::iter::Empty<FieldSlot>;

    fn iter_slots(&self) -> Self::SlotIterator {
        unimplemented!("MemorySlice::iter_slots — implement for generational GC")
    }
    fn object(&self) -> Option<ObjectReference> { unimplemented!() }
    fn start(&self) -> Address { unimplemented!() }
    fn bytes(&self) -> usize { unimplemented!() }
    fn copy(_src: &Self, _tgt: &Self) { unimplemented!() }
}
