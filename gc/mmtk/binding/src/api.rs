//! C-exported API for the OCaml 5.x MMTk binding.
//!
//! `tls` for a mutator is the address of the domain's `caml_domain_state`
//! struct.  All exported symbols use the `mmtk_ocaml_*` prefix.

use std::ffi::CStr;

use mmtk::memory_manager;
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
    // Optional: pin the number of GC worker threads (MMTK_GC_THREADS) — useful
    // to make collections deterministic while debugging.
    if let Ok(n) = std::env::var("MMTK_GC_THREADS") {
        assert!(
            memory_manager::process(&mut builder, "threads", &n),
            "failed to set threads={}", n
        );
    }

    let mmtk_instance = memory_manager::mmtk_init::<OCamlVM>(&builder);
    SINGLETON
        .set(mmtk_instance)
        .ok()
        .expect("mmtk_ocaml_init called more than once");
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
    memory_manager::handle_user_collection_request::<OCamlVM>(mmtk(), tls);
}

// ── Object queries ────────────────────────────────────────────────────────

/// Total number of objects relocated by copying collection so far (Immix
/// defrag, etc.). Lets the runtime confirm/report that movement happened.
#[no_mangle]
pub extern "C" fn mmtk_ocaml_objects_copied() -> usize {
    mmtk_ocaml_common::object_model::OBJECTS_COPIED.load(std::sync::atomic::Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn mmtk_ocaml_is_in_mmtk_spaces(addr: *const libc::c_void) -> bool {
    let addr = Address::from_ptr(addr);
    memory_manager::is_in_mmtk_spaces(unsafe {
        ObjectReference::from_raw_address_unchecked(addr)
    })
}
