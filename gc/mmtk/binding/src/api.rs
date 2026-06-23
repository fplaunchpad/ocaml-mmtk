//! C-exported API for the OCaml 5.x MMTk binding.
//!
//! `tls` for a mutator is the address of the domain's `caml_domain_state`
//! struct.  All exported symbols use the `mmtk_ocaml_*` prefix.

use std::ffi::CStr;

use mmtk::memory_manager;
use mmtk::util::alloc::{Allocator, AllocatorSelector, BumpAllocator, ImmixAllocator};
use mmtk::util::opaque_pointer::{OpaquePointer, VMMutatorThread, VMThread};
use mmtk::util::{Address, ObjectReference};
use mmtk::AllocationSemantics;
use mmtk::MMTKBuilder;

use mmtk_ocaml_common::header::{make_header, WORD_SIZE};
use mmtk_ocaml_common::object_model::OBJECT_REF_OFFSET;
use mmtk_ocaml_common::slot::OCamlMemorySlice;

use crate::active_plan::{deregister_by_ptr, register_mutator};
use crate::{mmtk, OCamlVM, SINGLETON};

// ── Init ──────────────────────────────────────────────────────────────────

/// Initialise MMTk.  Call once (from `caml_main`/startup) before any domain
/// is bound. `plan` is a GC plan name: "NoGC", "MarkSweep", "Immix", …
#[no_mangle]
pub extern "C" fn mmtk_ocaml_init(heap_size: usize, plan: *const libc::c_char) {
    let plan_str = unsafe { CStr::from_ptr(plan).to_str().expect("invalid plan string") };

    let mut builder = MMTKBuilder::new();
    assert!(
        memory_manager::process(&mut builder, "plan", plan_str),
        "unknown MMTk plan: {}", plan_str
    );
    assert!(
        memory_manager::process(
            &mut builder,
            "gc_trigger",
            &format!("FixedHeapSize:{}", heap_size)
        ),
        "failed to set gc_trigger/heap_size"
    );
    // GC worker thread count is set via mmtk-core's own MMTK_THREADS env var
    // (read automatically by Options::default → read_env_var_settings), along with
    // the other pass-through knobs (MMTK_STRESS_FACTOR, MMTK_IMMIX_ALWAYS_DEFRAG, …).

    let mmtk_instance = memory_manager::mmtk_init::<OCamlVM>(&builder);
    let constraints = mmtk_instance.get_plan().constraints();
    // Whether this plan ever relocates objects. Only moving plans map the
    // forwarding-bits side metadata, so only they may register the spec below.
    let plan_moves = constraints.moves_objects;
    // Mark-compact-style plans (MarkCompact, Compressor) forward objects AFTER a
    // separate liveness/mark pass rather than in-place during the trace, so they do
    // NOT map LOCAL_FORWARDING_BITS_SPEC (they use their own mark / offset-vector
    // metadata). Reading our forwarding-bits spec under those plans is a wild access.
    let forward_after_liveness = constraints.needs_forward_after_liveness;
    SINGLETON
        .set(mmtk_instance)
        .ok()
        .expect("mmtk_ocaml_init called more than once");

    // Hand the forwarding-bits side-metadata spec to the common crate so
    // FieldSlot::classify can authoritatively distinguish an already-forwarded
    // object's header (now a forwarding pointer) from a genuine Infix_tag header,
    // without this crate needing the VM type. Our object model uses a single fixed
    // layout (forwarding bits on the side), so one spec is all classify needs.
    //
    // ONLY for in-place moving plans that map this spec. Two cases must NOT register
    // it, or classify's is_forwarded() read would be a wild access (SEGV the first
    // time it sees an Infix_tag-looking header):
    //   - non-moving plans (NoGC, MarkSweep): never forward, never map the spec;
    //   - mark-compact plans (MarkCompact, Compressor): forward after the liveness
    //     pass, so during the trace headers are still genuine OCaml headers and the
    //     spec is unmapped. needs_forward_after_liveness flags exactly these.
    // Leaving the spec unset makes is_forwarded() return false, which is correct in
    // both cases: nothing is forwarded in-place during the trace, so every
    // Infix_tag header the scan sees is genuine.
    if plan_moves && !forward_after_liveness {
        mmtk_ocaml_common::slot::set_forwarding_bits_spec(
            *<crate::object_model::VMObjectModel as mmtk::vm::ObjectModel<OCamlVM>>::LOCAL_FORWARDING_BITS_SPEC
                .as_spec()
                .extract_side_spec(),
        );
    }
}

/// Start MMTk GC worker threads.  Call once after `mmtk_ocaml_init`, before
/// any allocation.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_initialize_collection(tls: usize) {
    let tls = VMThread(OpaquePointer::from_address(unsafe { Address::from_usize(tls) }));
    memory_manager::initialize_collection::<OCamlVM>(mmtk(), tls);
}

// ── Mutator (domain) lifecycle ────────────────────────────────────────────

/// Bind a new OCaml 5.x domain as an MMTk mutator.
/// `domain_state_addr` — the address of the domain's `caml_domain_state` struct.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_bind_mutator(domain_state_addr: usize) -> *mut libc::c_void {
    let tls = VMMutatorThread(VMThread(OpaquePointer::from_address(unsafe {
        Address::from_usize(domain_state_addr)
    })));
    let mutator = memory_manager::bind_mutator(mmtk(), tls);
    let raw = Box::into_raw(mutator);
    register_mutator(domain_state_addr, raw);
    raw as *mut libc::c_void
}

/// Destroy the mutator for a terminating OCaml 5.x domain.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_destroy_mutator(mutator: *mut libc::c_void) {
    let mutator_ptr = mutator as *mut mmtk::Mutator<OCamlVM>;
    deregister_by_ptr(mutator_ptr);
    // Reconstruct the Box so the allocation is freed after destroy_mutator runs.
    let mut mutator_box = unsafe { Box::from_raw(mutator_ptr) };
    memory_manager::destroy_mutator(&mut *mutator_box);
    // mutator_box drops here, freeing the Mutator allocation.
}

// ── Allocation ────────────────────────────────────────────────────────────

/// Allocate an OCaml block.  Writes the header and returns a pointer to field 0.
///
/// `wosize` — number of word-sized fields; `tag` — OCaml block tag (0..255);
/// `semantics` — 0 Default, 1 Immortal, 2 Los, 6 NonMoving.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_alloc(
    mutator: *mut libc::c_void,
    wosize: usize,
    tag: usize,
    semantics: usize,
) -> *mut libc::c_void {
    let mutator = unsafe { &mut *(mutator as *mut mmtk::Mutator<OCamlVM>) };
    let total_bytes = (wosize + 1) * WORD_SIZE;
    let semantics = match semantics {
        0 => AllocationSemantics::Default,
        1 => AllocationSemantics::Immortal,
        2 => AllocationSemantics::Los,
        6 => AllocationSemantics::NonMoving,
        _ => AllocationSemantics::Default,
    };

    let alloc_start: Address =
        memory_manager::alloc::<OCamlVM>(mutator, total_bytes, WORD_SIZE, 0, semantics);

    // Heap exhausted: MMTk has already collected, called VMCollection::out_of_memory
    // (which returns rather than aborting), and handed us a null address. Propagate
    // null so the C alloc wrapper raises OCaml's Out_of_memory from a C frame —
    // raising here would longjmp through MMTk's Rust frames.
    if alloc_start.is_zero() {
        return std::ptr::null_mut();
    }

    let header = make_header(wosize, tag as u8);
    unsafe { alloc_start.store(header) };

    let obj_ref = alloc_start + OBJECT_REF_OFFSET;
    let object = unsafe { ObjectReference::from_raw_address_unchecked(obj_ref) };
    memory_manager::post_alloc::<OCamlVM>(mutator, object, total_bytes, semantics);

    obj_ref.to_mut_ptr::<libc::c_void>()
}

/// Native TLAB refill (nursery aliasing).
///
/// Hands the OCaml runtime a contiguous region `[*out_start, *out_end)` to use as
/// its "young" nursery — the region *is* (part of) an MMTk block backing the
/// plan's **Default** allocator. Called from `caml_alloc_small_dispatch` in place
/// of a minor GC when the inlined native fast-path exhausts the current region.
///
/// Two TLAB-capable allocator shapes are supported, selected by the active plan's
/// `Default` allocator mapping:
///   - **`Immix(_)`** (Immix / StickyImmix): the region is an *in-place* Immix
///     block; nursery objects are ordinary Immix-space objects that do not move at
///     a (sticky) collection. Reclaimed by Immix's mark-region sweep.
///   - **`BumpPointer(_)`** (GenImmix / GenCopy): the region is the generational
///     plan's **copy-nursery** (`CopySpace`) bump buffer. At a *minor* GC the
///     nursery survivors are **evacuated** to the mature space and the nursery is
///     reset wholesale; the next refill hands a fresh nursery block. Native young
///     objects therefore MOVE at a minor GC — they are managed objects traced from
///     the domain's roots (registers/stack/`gc_regs` via `caml_scan_stack`), and
///     those roots are reported as updatable `FieldSlot`s on every collection (the
///     same machinery Immix defrag uses for mature objects), so the moving-root
///     fixup already covers the native minor-GC evacuation path.
///
/// Either way we drive the Default allocator's small/bump path to acquire fresh
/// space of at least `min_bytes` (its slow path polls for a GC on exhaustion —
/// for a generational plan that GC is a nursery evacuation), then advance the
/// allocator's cursor to the region limit so MMTk treats the whole region as
/// consumed and direct MMTk allocations (`caml_alloc_shr` → `mmtk_ocaml_alloc`),
/// which share this same Default allocator, do not bump into the region OCaml
/// fills top-down.
///
/// No per-object `post_alloc` is needed: objects OCaml writes into the region are
/// ordinary managed objects, traced from roots (we don't enable the `vo_bit`
/// feature, so `post_alloc` is a no-op anyway). Bump direction is irrelevant to
/// both reclamation models.
///
/// Returns `true` and fills the out-params on success. Returns `false` on heap
/// exhaustion (caller raises `Out_of_memory`) or if the Default allocator is
/// neither an Immix nor a bump allocator (e.g. MarkSweep's free-list — bytecode
/// falls back to the per-object alloc path; native aborts at startup).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_refill_tlab(
    mutator: *mut libc::c_void,
    min_bytes: usize,
    out_start: *mut usize,
    out_end: *mut usize,
) -> bool {
    let mutator = unsafe { &mut *(mutator as *mut mmtk::Mutator<OCamlVM>) };

    let selector =
        memory_manager::get_allocator_mapping::<OCamlVM>(mmtk(), AllocationSemantics::Default);

    // The acquisition logic is identical for both TLAB-capable allocator shapes —
    // probe one word to take the small/bump slow path (its block acquisition polls
    // for a GC on exhaustion; for a generational plan that GC is a nursery
    // evacuation), read `bump_pointer.{cursor,limit}`, eject the rest of the run,
    // and retry if it was too small for the triggering object. The only difference
    // is the concrete allocator type, so a macro stamps out the same loop for each.
    //
    // Why a one-word probe, not `min_bytes`: for `ImmixAllocator` an allocation
    // larger than a line takes the `overflow_alloc` path, which populates the
    // inaccessible `large_bump_pointer` instead of the `pub bump_pointer` we read,
    // handing OCaml a bogus region. A one-word probe always stays on the small
    // path. `BumpAllocator` (GenImmix/GenCopy nursery) has a single `bump_pointer`
    // and no overflow path, but the one-word probe is correct for it too.
    macro_rules! refill_with {
        ($ty:ty) => {{
            let allocator =
                unsafe { mutator.allocator_impl_mut::<$ty>(selector) };
            const PROBE: usize = WORD_SIZE;
            loop {
                let result = Allocator::alloc(allocator, PROBE, WORD_SIZE, 0);
                if result.is_zero() {
                    return false; // heap exhausted after a GC
                }
                let limit = allocator.bump_pointer.limit;
                // Eject the rest of this block/run from MMTk's bump view either
                // way: OCaml owns [result, limit) exclusively if we take it, and a
                // too-small run must be abandoned so the next probe slow-paths to
                // fresh space.
                allocator.bump_pointer.cursor = limit;
                if limit - result >= min_bytes {
                    unsafe {
                        *out_start = result.as_usize();
                        *out_end = limit.as_usize();
                    }
                    return true;
                }
                // Region too small for the triggering object: retry for a larger
                // one. A clean block (32 KiB) satisfies any Max_young_wosize
                // object, so the loop terminates (or returns false on OOM above).
            }
        }};
    }

    match selector {
        // Immix / StickyImmix: in-place Immix block (nursery objects don't move at
        // a collection).
        AllocatorSelector::Immix(_) => refill_with!(ImmixAllocator<OCamlVM>),
        // GenImmix / GenCopy: the copy-nursery CopySpace bump buffer. Nursery
        // survivors are evacuated at a minor GC; the next refill hands a fresh
        // nursery. Native young objects move at the minor GC — their roots
        // (registers/stack/gc_regs) are reported as updatable FieldSlots on every
        // collection, the same machinery Immix defrag uses, so the moving-root
        // fixup already covers this minor-GC evacuation path.
        AllocatorSelector::BumpPointer(_) => refill_with!(BumpAllocator<OCamlVM>),
        // No bump/Immix Default allocator (e.g. MarkSweep's free-list): bytecode
        // falls back to the per-object alloc path; native aborts at startup.
        _ => false,
    }
}

/// Generational write barrier (region form). Records that `count` value-sized
/// slots starting at `start` may now hold pointers into the nursery, so a young
/// collection scans them. Used for both scalar field writes (`count == 1`,
/// remembering the slot — OCaml's `caml_modify` gives a field address, not the
/// object) and array blits. A no-op for non-generational plans (NoBarrier).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_region_barrier(
    mutator: *mut libc::c_void,
    start: usize,
    count: usize,
) {
    let mutator = unsafe { &mut *(mutator as *mut mmtk::Mutator<OCamlVM>) };
    let start = unsafe { Address::from_usize(start) };
    let dst = OCamlMemorySlice::from_slots(start, count);
    // The gen barrier's region path ignores src; pass an empty slice.
    let src = OCamlMemorySlice::from_slots(start, 0);
    memory_manager::memory_region_copy_post::<OCamlVM>(mutator, src, dst);
}

/// Deregister a terminating domain (by its caml_domain_state address) so the
/// stop-the-world code no longer waits for it.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_deregister_domain(domain_state_addr: usize) {
    crate::active_plan::deregister_by_addr(domain_state_addr);
}

// ── GC control ────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn mmtk_ocaml_handle_user_collection_request(domain_state_addr: usize) {
    let tls = VMMutatorThread(VMThread(OpaquePointer::from_address(unsafe {
        Address::from_usize(domain_state_addr)
    })));
    // OCaml's Gc.major/full_major/compact route here. They are FULL-heap collections
    // by contract, so request force=true (collect even if the trigger says not yet)
    // and exhaustive=true (full heap). The exhaustive flag matters under generational
    // plans (StickyImmix/GenImmix): without it a user GC is a nursery collection, so
    // mature/large-object-space objects are never re-traced — a weakly-reachable
    // mature object then never gets reclaimed and its weak ref never clears (e.g.
    // regression/pr5233). Immix is non-generational so every GC is already full; this
    // makes StickyImmix match.
    mmtk().handle_user_collection_request(tls, true, true);
}

// ── Object queries ────────────────────────────────────────────────────────

/// Total number of objects relocated by copying collection so far (Immix
/// defrag, etc.). Lets the runtime confirm/report that movement happened.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_objects_copied() -> usize {
    mmtk_ocaml_common::object_model::OBJECTS_COPIED.load(std::sync::atomic::Ordering::Relaxed)
}

/// Pin an OCaml block so a moving collection will not relocate it. Used for the
/// interim weak/ephemeron handling: `caml_mmtk_scan_ephe_roots` reports the
/// *interior field slots* of ephemeron/weak-array blocks as roots, which would go
/// stale if the block itself were relocated under a moving plan (Immix defrag,
/// GenImmix, …). Pinning the block keeps those slot addresses valid (its fields
/// are still updated in place when their targets move). No-op (returns false) for
/// addresses outside MMTk spaces or under non-moving plans where pinning is inert.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_pin_object(addr: *const libc::c_void) -> bool {
    let address = Address::from_ptr(addr);
    let object = unsafe { ObjectReference::from_raw_address_unchecked(address) };
    if !memory_manager::is_in_mmtk_spaces(object) {
        return false;
    }
    memory_manager::pin_object(object)
}

#[no_mangle]
pub extern "C" fn mmtk_ocaml_is_in_mmtk_spaces(addr: *const libc::c_void) -> bool {
    let addr = Address::from_ptr(addr);
    memory_manager::is_in_mmtk_spaces(unsafe {
        ObjectReference::from_raw_address_unchecked(addr)
    })
}

// ── Finalizers (custom-block Custom_operations.finalize) ───────────────────
// OCaml custom blocks (Bigarray, Int64, channels, marshalled blocks…) carry a C
// `finalize` op that the stock GC runs on sweep. Under MMTk we register each such
// block with MMTk's finalizer queue at allocation (caml_alloc_custom) and, after a
// GC, drain the now-dead ones and run their finalize op. `FinalizableType` is
// `ObjectReference` (see reference_glue.rs), which MMTk keeps alive + forwards
// until retrieved. Gated on MMTK_WEAK_REFS at the C call sites.

/// Register a block with MMTk's finalizer queue. The object is kept alive (and
/// forwarded under a moving plan) until it is unreachable, then returned by
/// `mmtk_ocaml_poll_finalizable`.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_add_finalizer(addr: *const libc::c_void) {
    let object =
        unsafe { ObjectReference::from_raw_address_unchecked(Address::from_ptr(addr)) };
    memory_manager::add_finalizer(mmtk(), object);
}

/// Pop one ready-to-finalize object (unreachable since the last GC), resurrected
/// and valid for the finalize call. Returns its address, or 0 when the queue is
/// empty. The caller runs the block's `finalize` op and drops the reference.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_poll_finalizable() -> usize {
    match memory_manager::get_finalized_object(mmtk()) {
        Some(object) => object.to_raw_address().as_usize(),
        None => 0,
    }
}

// ── Heap statistics (for Gc.stat / Gc.quick_stat) ──────────────────────────
// MMTk accounts memory in pages, so these are page-granular.

/// Total heap size in bytes (the configured/grown heap — Gc.stat `heap_words`).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_total_bytes() -> usize {
    memory_manager::total_bytes(mmtk())
}

/// Bytes currently in use (live + retained pages — a proxy for `live_words`;
/// exact live bytes would need the count-live-bytes GC option).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_used_bytes() -> usize {
    memory_manager::used_bytes(mmtk())
}

/// Free bytes in the heap (Gc.stat `free_words`).
#[no_mangle]
pub extern "C" fn mmtk_ocaml_free_bytes() -> usize {
    memory_manager::free_bytes(mmtk())
}
