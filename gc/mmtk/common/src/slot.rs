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

use mmtk::util::{Address, ObjectReference};
use mmtk::vm::slot::{MemorySlice, Slot};

// ── FieldSlot ─────────────────────────────────────────────────────────────

/// A slot (address of a memory location) that holds an OCaml value.
///
/// The value may be a tagged integer (no GC action needed) or a heap
/// pointer (must be updated on object movement).
#[derive(Clone, Copy)]
pub struct FieldSlot {
    addr: *mut AtomicUsize,
}

// Raw pointer requires explicit Send; Slot trait bound requires it.
unsafe impl Send for FieldSlot {}

impl FieldSlot {
    #[inline]
    pub fn from_address(address: Address) -> Self {
        Self { addr: address.to_mut_ptr::<AtomicUsize>() }
    }

    #[inline]
    pub fn as_address(&self) -> Address {
        Address::from_mut_ptr(self.addr)
    }

    #[inline]
    fn raw_value(&self) -> usize {
        unsafe { (*self.addr).load(Ordering::Relaxed) }
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
    /// Return the heap object stored in this slot, or None if it holds an immediate.
    fn load(&self) -> Option<ObjectReference> {
        let raw = self.raw_value();
        if raw & 1 == 0 && raw != 0 {
            // LSB=0, non-null → word-aligned heap pointer
            unsafe {
                Some(ObjectReference::from_raw_address_unchecked(Address::from_usize(raw)))
            }
        } else {
            None // tagged integer (LSB=1) or null
        }
    }

    /// Overwrite the slot with a (possibly relocated) object reference.
    fn store(&self, object: ObjectReference) {
        unsafe {
            (*self.addr).store(object.to_raw_address().as_usize(), Ordering::Relaxed);
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
