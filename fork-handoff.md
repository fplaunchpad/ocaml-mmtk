# MMTk + OCaml: Fork Handoff & Learnings

A cold-start brief for building an **MMTk-backed OCaml compiler** as a fork of OCaml.
Written for an agent (or engineer) starting from an empty repo. It assumes no prior
conversation context. It captures *why* the project is structured the way it is, the
hard runtime facts you must know before writing code, what to reuse from the prior
prototype, and a de-risked milestone plan.

> **Prior prototype:** a standalone binding prototype exists at
> `/Users/kc/repos/mmtk-ocaml` (crates: `mmtk-ocaml-common`, `mmtk-ocaml-4`,
> `mmtk-ocaml-5`). It proves the MMTk allocation C ABI in isolation under the **NoGC**
> plan, on a *stock* compiler, via synthetic FFI stubs. It does **not** integrate with
> the OCaml runtime. Treat it as a parts bin, not a foundation — see §6.

---

## 1. Goal

Make [MMTk](https://www.mmtk.io) serve as OCaml's garbage collector, so that **normal
OCaml programs run on a normally-built compiler** whose heap is managed by MMTk. The
deliverable is a fork of OCaml that builds end-to-end and is eventually distributable as
an `ocaml-variants.5.x+mmtk` opam switch.

The success test is *not* "the binding's C functions return pointers." It is "`ocamlopt`
compiles a real program, and that program runs correctly with MMTk managing its heap,
including collection."

---

## 2. The architecture decision (and why)

**Build a fork of OCaml with the MMTk binding in-tree; depend on `mmtk-core`, do not
vendor it; target OCaml 5.x first.**

Three concrete choices:

1. **Fork OCaml; binding lives inside the compiler tree.** The integration (allocation
   fast path, root scanning, stop-the-world, build/link glue) physically lives in
   `runtime/`, `asmcomp/`, `domain_state.tbl`, and the build system. It *cannot* be
   expressed from an external binding repo. A runtime ABI change and the binding change
   that depends on it must land in one commit. This is also how every other MMTk binding
   is shaped (a VM fork + binding, e.g. `mmtk-julia` + a `julia` fork).

2. **Depend on `mmtk-core`, do not vendor it.** `mmtk-core` is large, fast-moving, and its
   `VMBinding` trait evolves between releases. Vendoring (copying source in-tree) means
   painful manual re-syncs and lost provenance for no benefit. The `mmtk` crate is on
   crates.io. Use, in order of preference:
   - `mmtk = "0.32"` (crates.io) if the released trait suffices;
   - `mmtk = { git = "https://github.com/mmtk/mmtk-core", rev = "<pinned sha>" }` if you
     need unreleased fixes;
   - a submodule only if you must carry local patches to mmtk-core itself.
   (The prototype used a `path = "../../mmtk-core"` dep with `features = ["object_pinning"]`
   — convenient for local dev, wrong for a shippable repo.)

3. **Target OCaml 5.x first.** Multicore is where MMTk's parallelism fits (domain = mutator;
   the domain-state pointer is already in a register at every allocation site). It is the
   forward-looking target and the more complete half of the prototype. Carrying 4.14 forward
   doubles the rebase-maintenance burden for the version on its way out; treat it as a
   separate later fork if ever needed.

**The one real cost, eyes open:** a fork means you own the rebase against upstream OCaml
until the work is upstreamed. Mitigation: keep the compiler diff *small and concentrated*.
Both the technical approach (§4) and the milestone plan (§7) are chosen to minimize that diff.

---

## 3. Essential background: how OCaml allocation actually works

**You must internalize this before touching anything.** OCaml native code does not call an
allocation function in the common case — the compiler *inlines* a bump-pointer sequence into
every allocation site. Verified against OCaml 5.4.1 sources
(`.../ocaml-compiler.5.4.1/asmcomp/amd64/`; note 5.4.1 still uses `asmcomp/`, newer trees use
`backend/`).

### Reserved registers (amd64, `asmcomp/amd64/proc.ml`)

```
r14   domain state pointer   (Caml_state — the OCaml 5 addition)
r15   allocation pointer      (young_ptr; grows downward)
```

### The inlined fast path (`asmcomp/amd64/emit.mlp`, `Lop(Ialloc { bytes = n })`)

```asm
    sub   $n, %r15                       ; bump young_ptr down by n bytes
    cmp   young_limit(%r14), %r15        ; compare with Caml_state->young_limit
    jb    L_call_gc                      ; underflow -> out-of-line slow path
L_after:
    lea   8(%r15), res                   ; OCaml value = young_ptr + 8 (skip header word)
```

Three instructions, no call, no register spill. `L_call_gc` is emitted out-of-line and
calls `caml_call_gc`; the non-fast path calls `caml_alloc1/2/3/N` (sizes 16/24/32/other).
The slow-path entry points are in `runtime/amd64.S`.

### Safepoints share the same lever (`Lop(Ipoll)`)

```asm
    cmp   young_limit(%r14), %r15
    jbe   L_call_gc            ; poll point — a safepoint is "compare alloc ptr vs limit"
```

To **stop the world**, the runtime poisons `young_limit` (sets it high) so the next bump
*or* poll branches into the runtime. This is the single STW lever for both versions:
- OCaml 4.14: flip `caml_young_limit` to `caml_young_end`.
- OCaml 5.x: write each domain's `interrupt_word` / `young_limit`, coordinated by
  `caml_try_run_on_all_domains` in `runtime/domain.c`.

### Field offsets are baked into codegen

`young_ptr` / `young_limit` are fields of `Caml_state` (the `caml_domain_state` struct).
Their offsets are defined in `runtime/caml/domain_state.tbl` and **hard-coded into emitted
code**. If you change that struct's layout you change the ABI and must rebuild everything.

### Consequence — the rule that drives every design choice

You **cannot** redirect allocation by swapping a C function, because allocation isn't a
function call in the hot path. Anything that turns every allocation into a call to
`mmtk_alloc(...)` (a full C call + MMTk allocator dispatch + `post_alloc`) is a non-starter
for a language that allocates as hard as OCaml.

---

## 4. The two viable integration strategies

### Strategy A — alias the bump pointer (full nursery ownership)

MMTk's own fast-path allocators (Immix, bump-pointer MarkSweep) are *the same shape*: bump a
cursor, compare a limit, overflow → slow path. So **make `Caml_state->young_ptr`/`young_limit`
be an MMTk bump allocator's cursor and limit** — point them at the same memory; reconcile at
slow-path boundaries. Then:
- Leave `emit.mlp`'s instruction selection essentially unchanged (you change what *backs* the
  registers, not the sequence).
- Retarget the slow path (`caml_call_gc` / `caml_allocN`) to MMTk's `alloc_slow` + poll-for-GC.
- STW is free: poisoning `young_limit` already forces mutators into the runtime.

This is the endgame. It is also the higher-risk piece (touches the runtime allocation glue and
the `Domainstate` offset assumptions).

### Strategy B — major-heap-only (the pragmatic v1)

Keep OCaml's copying **minor heap entirely**. The inlined fast path is untouched — **zero
`emit.mlp` changes.** Route only `caml_alloc_shr` (large/old allocations) and minor→major
promotion into MMTk. You get an MMTk-backed major heap without touching codegen at all.

**Start with B.** It de-risks the build/link integration, `caml_startup` init, root scanning,
and STW without the emitter problem. Layer A on later as an optimization once real programs run.

> What you do *not* want: rewriting `Ialloc` to emit `call mmtk_alloc`. That's the one option
> nobody should take.

---

## 5. The MMTk side: what a `VMBinding` is

`mmtk-core` is language-agnostic. To use it you implement the `VMBinding` trait — a bundle of
associated types through which MMTk asks the VM the questions a collector needs answered:

| `VMBinding` member | MMTk is asking | Status in prototype |
|---|---|---|
| `VMObjectModel` | object size? header location? copy it. | ✅ complete (reusable) |
| `VMSlot` | read/write one pointer field. | ✅ complete (reusable, **critical**) |
| `VMScanning` | enumerate roots; trace an object's fields. | `scan_object` ✅; **roots = TODO** |
| `VMActivePlan` | which threads/domains are mutators? | OCaml-5 registry ✅; OCaml-4 TODO |
| `VMCollection` | stop / resume the world; spawn GC worker. | scaffolding only; STW = TODO |
| `VMReferenceGlue` | weak refs & finalizers. | unimplemented (deferred) |
| `VMMemorySlice` | bulk-copy a field range (gen. barriers). | stub |

Data flow is **bidirectional**: the patched runtime calls a small C ABI to allocate and to
request GC; mmtk-core's GC worker calls *back* into the `VMBinding` to stop the world, scan
roots, and scan objects.

---

## 6. What to reuse from the prototype vs. drop

### Carry forward — these are correct and version-independent

From `mmtk-ocaml-common/` (depends only on the OCaml *value layout*, which multicore did not
change). Bring these in as a subdir of the fork.

- **`header.rs` — block header encoding.** 64-bit header: bits `63..10` = `wosize` (field
  count), `9..8` = color (ignore — MMTk uses side metadata), `7..0` = tag.
  - `make_header(wosize, tag) = (wosize << 10) | tag`
  - `wosize_of(h) = h >> 10`, `tag_of(h) = h & 0xFF`
  - Tag constants: ordinary `0..=245`; Lazy 246; Closure 247; Object 248; Infix 249; Forward
    250; then no-scan: Abstract 251, String 252, Double 253, Double_array 254, Custom 255.
    `TAG_NO_SCAN = 251`.

- **`slot.rs` — `FieldSlot` (the most safety-critical code).** OCaml values are tagged:
  **LSB=1 → immediate int (not a pointer); LSB=0 → heap pointer.** `FieldSlot` stores the
  *address of a field* (so a moving GC can patch it) and filters on load:
  ```rust
  fn load(&self) -> Option<ObjectReference> {
      let raw = self.raw_value();
      if raw & 1 == 0 && raw != 0 {        // LSB=0, non-null -> real heap pointer
          Some(ObjectReference::from_raw_address_unchecked(Address::from_usize(raw)))
      } else { None }                       // immediate or null -> MMTk ignores
  }
  ```
  Mis-tracing an immediate as a pointer corrupts the heap. This filter is non-negotiable.
  (Note: prototype uses `Ordering::Relaxed` — fine under STW, revisit for concurrent plans.)

- **`scanning.rs` — `scan_ocaml_object`.** Tag dispatch for tracing a live block:
  - tag `>= 251` (no-scan): visit nothing.
  - Closure (247): **skip field 0** (raw code pointer, not a root); scan `1..wosize`.
  - Infix (249): visit nothing (parent closure is scanned via its own ref).
  - Forward (250): visit field 0.
  - everything else (ordinary, Lazy, Object): scan all `wosize` fields; the `FieldSlot`
    load-filter drops integer fields.
  - Open TODOs: redirect infix pointers under a moving GC; multi-entry closures (arity > 1)
    interleave extra code pointers.

- **`object_model.rs` — layout & sizing.** The crucial offset: **the OCaml value points at
  field 0; the header is one word before it.** `OBJECT_REF_OFFSET = WORD_SIZE`;
  `ref_to_object_start`/`ref_to_header` subtract one word; `get_current_size = (wosize+1) *
  WORD_SIZE`. `copy_object`/`copy_to_object` (bulk memcpy + re-derive the value) are ready for
  moving plans. Side-metadata specs (mark bit side-after forwarding bits, etc.) are declared
  here.

Also reusable as a starting skeleton: the OCaml-5 **domain registry**
(`active_plan.rs`: `RwLock<HashMap<usize, MutatorPtr>>` keyed on `caml_domain_state*`), the
STW atomics (`collection.rs`: `WANTS_TO_STOP`/`WORLD_HAS_STOPPED`/`NUM_STOPPED`), the C ABI
shape and `mmtk_ocaml.h`, the `AllocationSemantics` mapping (0=Default, 1=Immortal, 2=Los,
6=NonMoving), and the `VMBinding` consts (`MIN_ALIGNMENT = MAX_ALIGNMENT = WORD_SIZE`,
`USE_ALLOCATION_OFFSET = false`).

### Drop

- The synthetic test harness (`tests/`): lazy `mmtk_init` from a `CAMLprim` stub, mutator
  keyed on `pthread_self()`, allocations that are never installed as live values. Replaced by
  "run real programs."
- The notion of `mmtk_alloc(...)` as the *fast path* — see §3/§4.
- Bring-up scaffolding: the `tls = Address::from_usize(1)` GC-worker sentinel; the per-alloc
  `eprintln!` in `api.rs` and `dump_object`; OOM-aborts-the-process in `copy_object`.

---

## 7. Milestone roadmap

Ordered to de-risk the build/link integration first and defer the emitter work as long as
possible.

- **M0 — Build skeleton.** Fork OCaml 5.x. Add `gc/mmtk/{common,binding}` (Rust). Pin
  `mmtk-core`. Wire `cargo build` into the OCaml build so the staticlib links into
  `libasmrun`. Compiler still uses its own GC. Goal: it builds and `hello world` runs.
- **M1 — MMTk owns the major heap (Strategy B).** `mmtk_init` from `caml_startup`. Route
  `caml_alloc_shr` + minor→major promotion to MMTk. Implement root scanning
  (`caml_do_roots` → `RootsWorkFactory`) and STW (`young_limit` poison + the barrier
  atomics). Keep OCaml's minor heap. **Goal: a real program triggers a major collection and
  survives.** This is the milestone that proves the whole integration.
- **M2 — MMTk owns the nursery (Strategy A).** Alias `young_ptr`/`young_limit` to an MMTk
  bump allocator; retarget the slow path. Goal: the inlined fast path allocates into MMTk.
- **M3 — Moving plan.** Enable Immix/copying: write barriers (`VMMemorySlice`), infix-pointer
  fixup, the proper OOM protocol (raise `Out_of_memory`, not abort). `copy_object` is already
  in place.
- **M4 — Packaging.** Ship as an `ocaml-variants.5.x+mmtk` opam switch.

Bring-up plan ladder for the *GC plan* itself: `NoGC` → `MarkSweep` → `Immix`.

---

## 8. Proposed repo structure

```
ocaml/                         # fork of github.com/ocaml/ocaml, branch 5.x+mmtk
├── runtime/                   #   patched: alloc glue, caml_startup hook, STW
│   └── caml/domain_state.tbl  #   (touch only if nursery layout changes — ABI!)
├── asmcomp/amd64/emit.mlp     #   untouched in M1; Strategy A work lands here in M2
├── gc/mmtk/                   # NEW — the binding, in-tree
│   ├── Cargo.toml             #   mmtk = "0.32" (crates.io) or git+rev; NOT vendored
│   ├── common/                #   from prototype mmtk-ocaml-common (header, slot, scan, model)
│   ├── binding/               #   OCaml5VM VMBinding impl + C ABI (api.rs) + domain registry
│   └── include/mmtk_ocaml.h   #   C ABI contract consumed by runtime/
└── ... rest of the OCaml tree
```

Keep the binding a coherent Rust unit (`common` + `binding`) even though it lives in the
compiler tree, so it stays reviewable and the diff against upstream OCaml is concentrated.

---

## 9. Gotchas observed while building the prototype

- **`mmtk-core` path resolution.** A workspace `path = "../../mmtk-core"` resolves relative to
  the *workspace root*, i.e. it landed in the home dir, not a repo sibling. Moot once you pin
  the dep instead of using a path.
- **macOS:** dynamic libs are `.dylib`, not `.so`; drop `-ldl` (those symbols are in
  `libSystem`, there is no separate `libdl`).
- **Rust crates don't need OCaml installed** — the binding reproduces the header/tag layout in
  Rust and exports a C ABI; it never `#include`s `caml/*.h`. Only the (now-dropped) test
  programs needed an opam switch.
- **Symbol naming:** the prototype's OCaml-4 ABI is bare `mmtk_*` while OCaml-5 is
  `mmtk_ocaml5_*` — inconsistent. Pick one scheme (`mmtk_ocaml_*`) from the start.

---

## 10. References

- **Prior prototype:** `/Users/kc/repos/mmtk-ocaml` — see its `docs/setup.md` and
  `docs/binding-design.md` for the full prototype walkthrough.
- **OCaml 5.4.1 compiler sources (read these):**
  `/Users/kc/.opam/default/.opam-switch/sources/ocaml-compiler.5.4.1/`
  - `asmcomp/amd64/proc.ml` — register conventions (r14/r15).
  - `asmcomp/amd64/emit.mlp` — `Lop(Ialloc)` (~line 607) and `Lop(Ipoll)` allocation/safepoint.
  - `runtime/amd64.S` — `caml_call_gc`, `caml_alloc1/2/3/N`.
  - `runtime/caml/{mlvalues.h, domain_state.h, domain_state.tbl}` — value layout, `Caml_state`.
  - `runtime/{minor_gc.c, major_gc.c, domain.c}` — collection + STW you are replacing.
- **Installed runtime headers:** `/Users/kc/.opam/default/lib/ocaml/caml/`
- **mmtk-core:** <https://github.com/mmtk/mmtk-core> (target v0.32.0); `mmtk` crate on crates.io.
- **Reference binding to study:** `mmtk-julia` (a VM fork + binding, the same shape you want).
  Julia sources are checked out at `/Users/kc/repos/mmtk-ocaml/_references/julia`.
- **Other MMTk bindings for patterns:** `mmtk-ruby`, `mmtk-openjdk`, `mmtk-v8`.
