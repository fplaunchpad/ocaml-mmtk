//! OCaml block header encoding and tag constants.
//!
//! Header word layout (64-bit):
//!   bits 63..10  Wosize  — number of word-sized fields
//!   bits  9.. 8  Color   — GC colour (managed by OCaml's own GC; MMTk uses side metadata)
//!   bits  7.. 0  Tag     — block type tag
//!
//! References:
//!   runtime/caml/mlvalues.h  Hd_val, Wosize_val, Tag_val, Color_val

pub const WORD_SIZE: usize = std::mem::size_of::<usize>();

/// Build a header from field count and tag (color bits = 0).
#[inline(always)]
pub fn make_header(wosize: usize, tag: u8) -> usize {
    (wosize << 10) | (tag as usize)
}

/// Extract the field count (Wosize) from a header word.
#[inline(always)]
pub fn wosize_of(header: usize) -> usize {
    header >> 10
}

/// Extract the tag byte from a header word.
#[inline(always)]
pub fn tag_of(header: usize) -> u8 {
    (header & 0xFF) as u8
}

// ── Block tags (from runtime/caml/mlvalues.h) ─────────────────────────────
// Tags 0..245     ordinary blocks    all fields are OCaml values
pub const TAG_FORCING:      u8 = 244;
pub const TAG_CONTINUATION: u8 = 245;
pub const TAG_LAZY:         u8 = 246; // lazy thunk; field 0 = thunk or value
pub const TAG_CLOSURE:      u8 = 247; // function closure; field 0 = code ptr (NOT a GC root)
pub const TAG_OBJECT:       u8 = 248; // OO object; all fields are values
pub const TAG_INFIX:        u8 = 249; // interior pointer into a closure block
pub const TAG_FORWARD:      u8 = 250; // forwarding pointer; field 0 = new location
pub const TAG_ABSTRACT:     u8 = 251; // opaque; no GC-visible fields
pub const TAG_STRING:       u8 = 252; // byte string; no pointer fields
pub const TAG_DOUBLE:       u8 = 253; // 64-bit float; no pointer fields
pub const TAG_DOUBLE_ARRAY: u8 = 254; // flat float array; no pointer fields
pub const TAG_CUSTOM:       u8 = 255; // custom block with finaliser; no GC pointer fields

/// Tags >= NO_SCAN carry no GC-visible pointer fields and must not be scanned.
pub const TAG_NO_SCAN: u8 = TAG_ABSTRACT;

/// A non-heap value: LSB=1 means immediate integer, not a GC root.
#[inline(always)]
pub fn is_immediate(val: usize) -> bool {
    val & 1 == 1
}
