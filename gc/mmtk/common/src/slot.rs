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
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::OnceLock;

use mmtk::memory_manager;
use mmtk::util::metadata::side_metadata::SideMetadataSpec;
use mmtk::util::{Address, ObjectReference};
use mmtk::vm::slot::{MemorySlice, Slot};

use crate::header::{tag_of, wosize_of, TAG_INFIX, WORD_SIZE};

// ── FieldSlot ─────────────────────────────────────────────────────────────

/// Sentinel `info` value meaning "this slot does not hold a traceable MMTk
/// reference" — i.e. it holds an immediate integer, null, or a pointer to an
/// object outside MMTk's heap (atoms, code, anything allocated before MMTk was
/// enabled). For such slots `load` returns `None` and `store` is never called.
const NOT_TRACEABLE: usize = usize::MAX;

/// DEBUG (MMTK_DEBUG_ROOT_RACE), cached — logs each slot whose cached classification
/// said "traceable pointer" but whose current value is an immediate/null (the
/// concurrent classify-vs-load race, GH#15). Off unless the env var is set.
#[inline]
fn debug_root_race() -> bool {
    static F: OnceLock<bool> = OnceLock::new();
    *F.get_or_init(|| std::env::var_os("MMTK_DEBUG_ROOT_RACE").is_some())
}

/// MMTk's local forwarding-bits side-metadata spec, injected once by the binding
/// at MMTk init via [`set_forwarding_bits_spec`]. Reading it tells us,
/// authoritatively, whether the object at an address has begun forwarding — the
/// analogue of vanilla OCaml's `hd == 0` marker.
///
/// We capture the spec rather than calling the VM-generic
/// `object_forwarding::is_forwarded::<VM>` so this (version-independent) crate
/// stays free of the `VM` type. The binding uses one fixed metadata layout —
/// forwarding *pointer* in the header word, forwarding *bits* on the side — so a
/// single spec suffices, and the read is a plain side-metadata load.
static FORWARDING_BITS_SPEC: OnceLock<SideMetadataSpec> = OnceLock::new();

/// Register MMTk's local forwarding-bits spec
/// (`VMObjectModel::LOCAL_FORWARDING_BITS_SPEC`). Call once, at MMTk init.
pub fn set_forwarding_bits_spec(spec: SideMetadataSpec) {
    let _ = FORWARDING_BITS_SPEC.set(spec);
}

/// The MMTk heap's start address, injected once by the binding at init.
/// Enables the value-range forwarding discriminator below.
static HEAP_RANGE_START: OnceLock<usize> = OnceLock::new();

/// Register the MMTk heap's start address (`vm_layout().heap_start`).
/// Call once, at MMTk init.
pub fn set_heap_range_start(start: usize) {
    let _ = HEAP_RANGE_START.set(start);
}

/// True if the object at `addr` has been (or is being) forwarded by a moving GC.
///
/// MMTk overwrites a forwarded object's header word with the forwarding pointer,
/// so that word is no longer a valid OCaml header (its low byte can even collide
/// with a real tag such as `Infix_tag`, 0xf9). The authoritative "is forwarded"
/// signal is the 2-bit side-metadata state (`0b00` not-triggered, `0b10` being-
/// forwarded, `0b11` forwarded). `addr` must lie in an MMTk space — its metadata
/// is then mapped. Returns `false` before the spec is registered (startup window)
/// and on non-moving builds, where nothing is ever forwarded.
#[inline]
fn is_forwarded(addr: Address) -> bool {
    // Value-range discriminator (preferred): "forwarded" means the header word
    // was overwritten with a forwarding pointer, and a forwarding pointer is
    // necessarily inside the MMTk heap (>= heap start) while every genuine
    // OCaml header `(wosize << 10) | colour | tag` is far below it — stock
    // OCaml's own `hd == 0` idiom, generalized to the value range. This holds
    // in every trace mode: single-tracer (UP) pauses write ONLY the header
    // pointer (the side bits are skipped entirely — HEADER_FORWARDING_SENTINEL),
    // and in multi-worker pauses the pointer store is the completion step, so
    // a header that still reads as a small value is a genuine header. It also
    // needs no side-metadata mapping check (issue #12's SIGSEGV class): the
    // header word itself is always mapped. Plans that never overwrite headers
    // in-place (mark-compact family) always read a genuine header -> false,
    // which is the correct answer there too.
    if let Some(&start) = HEAP_RANGE_START.get() {
        return unsafe { (addr - WORD_SIZE).load::<usize>() } >= start;
    }
    // Startup-window fallback (heap start not yet registered): the historical
    // side-bits read.
    match FORWARDING_BITS_SPEC.get() {
        Some(spec) => {
            // Forwarding-bits side metadata is mapped ONLY by in-place moving spaces
            // (ImmixSpace, CopySpace). LOS/immortal/non-moving spaces never reserve it,
            // so reading it for an infix pointer whose parent lives there dereferences
            // an UNMAPPED metadata page -> SIGSEGV (issue #12). Such objects never move
            // and are never forwarded. Compute the metadata address from the spec's
            // public fields (mirrors mmtk-core address_to_contiguous_meta_address) and
            // read only when that page is mapped; otherwise report not-forwarded.
            const LOG_BITS_IN_BYTE: usize = 3;
            let shift = LOG_BITS_IN_BYTE - spec.log_num_of_bits;
            let meta_addr =
                spec.get_absolute_offset() + ((addr >> spec.log_bytes_in_region) >> shift);
            if !memory_manager::is_mapped_address(meta_addr) {
                return false;
            }
            spec.load_atomic::<u8>(addr, Ordering::SeqCst) != 0
        }
        None => false,
    }
}

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
    /// `true` = re-validate the current value on every `load` (the GH#15 guard).
    /// Set for ROOT slots, which a collection may process while a spawning/
    /// terminating domain concurrently mutates them (the global-root path runs
    /// regardless of mutator-stop). `false` = a heap FIELD slot created during
    /// object scanning: under an STW plan the field cannot change between classify
    /// and load, so `load` can trust the cached classification and skip the re-read
    /// + `is_in_mmtk_spaces` SFT lookup. Only honoured when [`STW_TRUSTED`] is set
    /// (STW plans); concurrent plans keep every load revalidating (mutators run
    /// during their scans, so heap fields can change under the worker).
    checked: bool,
}

/// Process-global: enable the trusted-field-load fast path. Set true at init for
/// stop-the-world plans (GenImmix/Immix/GenCopy/…), where no scan ever runs while
/// mutators mutate the heap. Left false for the concurrent plans (ConcurrentImmix/
/// Bactrian/LXR), which then behave exactly as before (every load revalidates).
pub static STW_TRUSTED: AtomicBool = AtomicBool::new(false);

/// Enable/disable the trusted-field-load fast path (see [`STW_TRUSTED`]). Called
/// once by the binding at MMTk init, after the plan is known.
pub fn set_stw_trusted(enabled: bool) {
    STW_TRUSTED.store(enabled, Ordering::Relaxed);
}

/// Cached heap VA bounds `[start, end)` for the trusted classify fast path (S1):
/// under an STW plan a heap FIELD value that is in this range is a live MMTk
/// object, and one outside it is an OCaml foreign pointer (atom / code / pre-MMTk),
/// so a range compare replaces the `is_in_mmtk_spaces` SFT lookup in `classify`.
/// `end == 0` means "not yet published" → fall back to `is_in_mmtk_spaces`.
/// Sound only on the contiguous (SFTSpaceMap) 64-bit layout, where every space
/// lives within one `[heap_start, heap_end)` reservation — which is what we run.
static HEAP_START: AtomicUsize = AtomicUsize::new(0);
static HEAP_END: AtomicUsize = AtomicUsize::new(0);

/// Publish the MMTk heap VA bounds. Called once by the binding after `mmtk_init`,
/// when `vm_layout()` is fixed. Only consulted on the trusted classify path.
pub fn set_heap_bounds(start: usize, end: usize) {
    HEAP_START.store(start, Ordering::Relaxed);
    HEAP_END.store(end, Ordering::Relaxed);
}

// Raw pointer requires explicit Send; Slot trait bound requires it.
unsafe impl Send for FieldSlot {}

impl FieldSlot {
    #[inline]
    pub fn from_address(address: Address) -> Self {
        let addr = address.to_mut_ptr::<AtomicUsize>();
        let raw = unsafe { (*addr).load(Ordering::Relaxed) };
        // A heap FIELD slot: trusted under STW plans (checked = false). Classify
        // with the range-based foreign-pointer filter when trusted (S1).
        let trusted = STW_TRUSTED.load(Ordering::Relaxed);
        Self { addr, info: Self::classify_inner(raw, trusted), checked: false }
    }

    /// Create a slot for a ROOT value. Identical to [`from_address`] except the
    /// slot is marked `checked`, so `load` always re-validates (roots race with
    /// spawning/terminating domains — GH#15 — even under an STW plan).
    #[inline]
    pub fn from_address_root(address: Address) -> Self {
        let addr = address.to_mut_ptr::<AtomicUsize>();
        let raw = unsafe { (*addr).load(Ordering::Relaxed) };
        // ROOT slot: always full classify (roots race spawn/terminate; GH#15).
        Self { addr, info: Self::classify(raw), checked: true }
    }

    #[inline]
    pub fn as_address(&self) -> Address {
        Address::from_mut_ptr(self.addr)
    }

    #[inline]
    fn raw_value(&self) -> usize {
        unsafe { (*self.addr).load(Ordering::Relaxed) }
    }

    /// Root-slot classify: full `is_in_mmtk_spaces` foreign-pointer filter.
    #[inline]
    fn classify(raw: usize) -> usize {
        Self::classify_inner(raw, false)
    }

    /// Classify the value `raw` once, returning the cached `info`:
    /// `NOT_TRACEABLE`, `0` (ordinary heap reference), or an infix byte offset.
    /// `trusted` (STW plan, heap FIELD slot) uses a heap-range compare in place of
    /// the `is_in_mmtk_spaces` SFT lookup for the foreign-pointer filter (S1); it is
    /// sound because, mutators stopped, a live field's value is either a real MMTk
    /// object (in `[HEAP_START, HEAP_END)`) or an OCaml foreign pointer (outside it).
    #[inline]
    fn classify_inner(raw: usize, trusted: bool) -> usize {
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
        let end = HEAP_END.load(Ordering::Relaxed);
        let in_heap = if trusted && end != 0 {
            // S1 fast path: heap-range compare, no SFT lookup.
            raw >= HEAP_START.load(Ordering::Relaxed) && raw < end
        } else {
            memory_manager::is_in_mmtk_spaces(obj)
        };
        if !in_heap {
            return NOT_TRACEABLE;
        }

        // Infix pointers point *into* a closure block (at an Infix_tag header),
        // not at an object start. The GC must mark/trace/forward the PARENT
        // closure, not the infix object. The infix header's size field is the
        // offset, in words, from the parent closure to the infix object
        // (verified GC paper, Fig. 3): parent = infix - wosize(header) * WORD.
        // CAUTION: during a moving GC `addr` may point to an object that has
        // already been forwarded. MMTk keeps the forwarding pointer *in the header
        // word* (LOCAL_FORWARDING_POINTER_SPEC = in_header(0)), so the word at
        // `addr - WORD_SIZE` can be a forwarding pointer, not an OCaml header — and
        // a forwarding pointer's low byte can equal Infix_tag (0xf9) by coincidence
        // of the destination address, which would make us compute a bogus infix
        // offset. Consult the authoritative forwarding-bits side metadata first
        // (the analogue of vanilla oldify_one checking `hd == 0` before testing
        // Infix_tag, runtime/minor_gc.c): if `addr` is forwarded, treat the slot as
        // an ordinary reference — the trace follows the forwarding pointer and
        // `store` rewrites the slot with the relocated address.
        //
        // For a genuine infix pointer `addr` is interior to a closure (never an
        // object start), so its forwarding bits read not-triggered and we use the
        // real Infix_tag header; this holds because forwarding bits are only ever
        // set at object starts, the same invariant vanilla relies on.
        let header = unsafe { (addr - WORD_SIZE).load::<usize>() };
        if tag_of(header) == TAG_INFIX && !is_forwarded(addr) {
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
        // Trusted fast path (STW plans, heap FIELD slots): the field was classified
        // moments ago on this same worker and, mutators being stopped, cannot have
        // changed — so skip the re-read + `is_in_mmtk_spaces` re-check below. `info`
        // already encodes the (ordinary vs infix) offset. Root slots (`checked`) and
        // the concurrent plans (`!STW_TRUSTED`) fall through to full revalidation.
        if !self.checked && STW_TRUSTED.load(Ordering::Relaxed) {
            let raw = self.raw_value();
            let start = unsafe { Address::from_usize(raw) } - self.info;
            return Some(unsafe { ObjectReference::from_raw_address_unchecked(start) });
        }
        let raw = self.raw_value();
        // GH#15: re-validate the CURRENT value, not just the cached classification.
        // `info` was classified at capture (under roots_mutex / at a safepoint), but a
        // terminating/spawning domain — scanned via the global-root path, which a
        // collection processes regardless of mutator-stop — can concurrently mutate or
        // free+reuse this slot before the worker loads it. If the slot now holds an
        // immediate or null it is not a root, whatever it held at classify; handing it
        // to the tracer panics ("cannot trace object 0x1"). Returning None here is
        // always sound (an immediate is never a heap object to trace/update).
        if raw & 1 != 0 || raw == 0 {
            if debug_root_race() {
                eprintln!(
                    "[ROOT-RACE] slot {:#x}: classified info={:#x} but current value={:#x} \
                     (immediate/null) — skipping",
                    self.as_address().as_usize(),
                    self.info,
                    raw
                );
            }
            return None;
        }
        // raw - infix_offset is the object start (== raw for ordinary slots).
        let start = unsafe { Address::from_usize(raw) } - self.info;
        let obj = unsafe { ObjectReference::from_raw_address_unchecked(start) };
        // GH#15 (cont.): re-check is_in_mmtk_spaces too, not only immediate/null. `info` was
        // classified when the slot held an in-heap object, but a spawning/terminating domain can
        // re-point it to a NON-heap pointer (e.g. a `.data` Stdlib.Domain static — rr-confirmed
        // origin of the LXR multidomain crash) that is non-null and non-immediate, so it passes
        // the bit-check above. The tracing plans survive it (trace_object is SFT-dispatched and
        // bounds-aware), but LXR's RC consumer indexes RC_TABLE side-metadata at the raw address
        // with no mapped-address check -> SIGSEGV. This re-check is exactly the filter `classify`
        // already applies (line ~146); it must hold at LOAD time too since the value can change
        // after classification. Sound for all consumers: a non-MMTk value is never a heap object
        // to trace or refcount. SFT lookup, no dereference of `obj`.
        if !memory_manager::is_in_mmtk_spaces(obj) {
            if debug_root_race() {
                eprintln!(
                    "[ROOT-RACE] slot {:#x}: classified info={:#x} but current value={:#x} \
                     is not in any MMTk space — skipping",
                    self.as_address().as_usize(),
                    self.info,
                    raw
                );
            }
            return None;
        }
        Some(obj)
    }

    /// The address of the slot itself (the field location). Used by the LXR field-logging
    /// write barrier + RC trace to index the per-field unlog-bit side metadata. Overrides
    /// the trait default (which panics for slot reprs the LXR plan doesn't use).
    fn to_address(&self) -> Address {
        self.as_address()
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

// ── OCamlMemorySlice ───────────────────────────────────────────────────────

/// A contiguous run of OCaml value-sized slots, used by the region (array-copy)
/// write barrier of generational plans (GenImmix, StickyImmix). `start` is the
/// address of the first slot; `count` is the number of value-sized slots.
///
/// We use this not only for true array blits but also for scalar field writes
/// (a 1-slot region) — OCaml's `caml_modify` is given only a field address, not
/// the containing object, so the object-remembering barrier doesn't fit; the
/// region barrier remembers the slot itself (matching OCaml's own slot-based
/// minor remembered set).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct OCamlMemorySlice {
    start: Address,
    count: usize,
}

impl OCamlMemorySlice {
    #[inline]
    pub fn from_slots(start: Address, count: usize) -> Self {
        Self { start, count }
    }
}

/// Iterator over the slots of an `OCamlMemorySlice`.
pub struct OCamlSliceIter {
    cur: Address,
    end: Address,
}

impl Iterator for OCamlSliceIter {
    type Item = FieldSlot;
    #[inline]
    fn next(&mut self) -> Option<FieldSlot> {
        if self.cur < self.end {
            let slot = FieldSlot::from_address(self.cur);
            self.cur += WORD_SIZE;
            Some(slot)
        } else {
            None
        }
    }
}

impl MemorySlice for OCamlMemorySlice {
    type SlotType = FieldSlot;
    type SlotIterator = OCamlSliceIter;

    fn iter_slots(&self) -> Self::SlotIterator {
        OCamlSliceIter {
            cur: self.start,
            end: self.start + self.count * WORD_SIZE,
        }
    }

    /// Free-floating region (a field range), not a whole object: return None so
    /// the barrier classifies it by address (`start`).
    fn object(&self) -> Option<ObjectReference> {
        None
    }

    fn start(&self) -> Address {
        self.start
    }

    fn bytes(&self) -> usize {
        self.count * WORD_SIZE
    }

    fn copy(src: &Self, tgt: &Self) {
        debug_assert_eq!(src.count, tgt.count, "MemorySlice::copy size mismatch");
        // Word-wise copy; handles overlap like memmove.
        unsafe {
            std::ptr::copy::<usize>(
                src.start.to_ptr::<usize>(),
                tgt.start.to_mut_ptr::<usize>(),
                src.count,
            );
        }
    }
}
