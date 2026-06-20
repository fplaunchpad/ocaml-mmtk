# OCaml + MMTk (`mmtk-ocaml`)

A fork of [OCaml](https://github.com/ocaml/ocaml) whose garbage collector is
provided by [MMTk](https://www.mmtk.io), the Memory Management Toolkit. The goal
is for **normal OCaml programs to run on a normally-built compiler whose heap is
managed by MMTk**, eventually distributable as an `ocaml-variants.5.x+mmtk` opam
switch.

- **Base:** OCaml `5.5.0-rc1` (branch `5.5+mmtk`).
- **MMTk binding:** in-tree at [`gc/mmtk/`](gc/mmtk), depending on
  [`mmtk-core`](https://github.com/mmtk/mmtk-core) `0.32` from crates.io
  (not vendored).
- The upstream OCaml README is preserved at
  [`README.upstream.adoc`](README.upstream.adoc).
- **Plan & status: [`ROADMAP.md`](ROADMAP.md)** — milestones, GC-plan tiers, and
  the full workstream list (start here to continue the project).
- Design background and rationale: [`fork-handoff.md`](fork-handoff.md).
- Design notes & deferred investigations: [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md).

> **Status: bring-up.** MMTk backs both the **bytecode** and **native** runtimes,
> **opt-in** (off by default, so a normal build and the compiler bootstrap run on
> OCaml's stock GC). `NoGC`, `MarkSweep`, `Immix`, `GenImmix`, and `StickyImmix`
> all work — the collecting plans collect single- and multi-domain, the moving
> plans relocate objects, the generational plans use a write barrier, and
> collection is parallel. Known limitation: weak arrays / ephemerons are only safe
> under non-moving `MarkSweep` for now (parked — see `ROADMAP.md`).
>
> **Native** code runs on MMTk for **single-domain** programs two ways: the
> default keeps the stock minor heap and promotes to MMTk (`MMTK_VANILLA_MINOR`);
> the proper all-MMTk path (`MMTK_TLAB=1`, Immix/StickyImmix) makes **MMTk own the
> nursery too** — the inlined native fast-path bumps an MMTk Immix block, with no
> OCaml minor GC and no promotion. Multi-domain native (`Domain.spawn`) deadlocks
> under GC pressure on domain-termination STW coordination (root cause known, fix
> deferred); see `ROADMAP.md`.

## Why bytecode first?

OCaml's native code *inlines* a bump-pointer allocation sequence at every
allocation site, so the GC cannot be swapped by replacing a C function. The
bytecode interpreter, by contrast, allocates through ordinary C entry points
(`Alloc_small`, `caml_alloc_shr`), which *can* be redirected. Targeting bytecode
first lets us bring up the whole MMTk integration (init, allocation, and later
root scanning + stop-the-world) without touching code generation. Native
integration comes later.

## Roadmap

| Milestone | Description | Status |
|-----------|-------------|--------|
| M0 | Build skeleton: in-tree binding links into the bytecode runtime | ✅ done |
| M1 | MMTk **NoGC** backs every bytecode allocation | ✅ done |
| M2 | **MarkSweep**: precise root scanning + stop-the-world, incl. multi-domain (`Domain.spawn`) | ✅ done |
| M3 | **Immix** (moving): infix-pointer fixup, clean `Out_of_memory` | ✅ done |
| M4 | **Generational** (GenImmix / StickyImmix): mutator write barrier | ✅ done |
| — | Parallel collection ✅ verified (marking scales ~8× on 16 threads) | 🟡 |
| next | Native code, testsuite, benchmarks (weak/ephemeron + finalisers parked) | ⬜ |

GC-plan bring-up ladder: `NoGC` → `MarkSweep` → `Immix`. Collections are parallel
and stop-the-world. **See [`ROADMAP.md`](ROADMAP.md) for the full plan, GC-plan
tiers, and current workstreams.**

## Dependencies

- A working C toolchain and the usual prerequisites to build OCaml (see
  [`INSTALL.adoc`](INSTALL.adoc)).
- **Rust + Cargo** (stable; tested with 1.96). The build invokes `cargo` to
  compile the MMTk binding into a static library.
- macOS: the binding's static library transitively needs
  `-lobjc -framework IOKit -framework CoreFoundation -liconv` (wired up
  automatically in [`Makefile.mmtk`](Makefile.mmtk)).

## Building

Exactly like upstream OCaml — the MMTk static library is built and linked
automatically:

```sh
./configure
make            # builds the world; the gc/mmtk staticlib is built via cargo
```

This produces a normal OCaml toolchain. By default MMTk is **not** active; the
runtime uses OCaml's stock GC.

## Running a program under MMTk

Set `MMTK_ENABLED=1` to have the bytecode runtime manage its heap with MMTk:

```sh
# compile a bytecode program with the freshly built toolchain
OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun ./ocamlc myprog.ml -o myprog.byte

# run it on MMTk (NoGC by default)
MMTK_ENABLED=1 OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun myprog.byte
```

### Environment knobs

| Variable | Default | Meaning |
|----------|---------|---------|
| `MMTK_ENABLED` | unset (off) | Set to `1` to let MMTk manage the heap. |
| `MMTK_PLAN` | `NoGC` | MMTk plan: `NoGC`, `MarkSweep`, `Immix`, `StickyImmix`, … |
| `MMTK_HEAP_SIZE_MB` | `1024` | Fixed heap size, in MiB. |
| `MMTK_VERBOSE` | unset | Print MMTk init + a GC/objects-copied summary at exit. |

The **native nursery mode is chosen automatically** from the plan — no knob: an
Immix-family plan (`Immix`/`StickyImmix`/`GenImmix`) uses all-MMTk nursery aliasing
(MMTk owns the nursery, no OCaml minor GC); any other plan falls back to the stock
minor heap + promotion. mmtk-core's own `MMTK_*` options also work
(`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, `MMTK_IMMIX_ALWAYS_DEFRAG`, …).

> MMTk's own options are also read from the environment, e.g.
> `MMTK_IMMIX_ALWAYS_DEFRAG=true MMTK_IMMIX_DEFRAG_EVERY_BLOCK=true` forces Immix
> to relocate objects (useful for exercising the moving path).

> `MarkSweep` and `Immix` collect (single- and multi-domain); `Immix` also
> relocates objects. Under `NoGC`, memory is never reclaimed — long-running or
> allocation-heavy programs (including the OCaml compiler) will exhaust the heap;
> that is expected, so the compiler bootstrap runs on the stock GC.

## Repository layout

```
gc/mmtk/                in-tree MMTk binding (a self-contained Cargo workspace)
├── common/             version-independent OCaml value layout (header, slot,
│                       scanning, object model)
├── binding/            VMBinding impl + C ABI (libmmtk_ocaml.a)
└── include/            mmtk_ocaml.h — the C ABI consumed by the runtime
runtime/mmtk.c          C glue between the bytecode runtime and the binding
runtime/caml/mmtk.h     glue declarations
Makefile.mmtk           build glue (cargo + link flags)
_references/            external repos kept for study only (git-ignored)
```

The runtime patches are concentrated in `runtime/` (`memory.h`, `memory.c`,
`minor_gc.c`, `interp.c`, `domain.c`, `domain_state.tbl`, `signals.c`, `intern.c`).
`mmtk.c`/`mmtk.h` is the glue, compiled into both runtimes. All MMTk paths are
gated at runtime by `caml_mmtk_enabled` (and `caml_mmtk_vanilla_minor` /
`caml_mmtk_tlab` for the native modes), so with MMTk off the stock GC runs
unchanged and the compiler bootstrap is unaffected.

## License

Same as OCaml — see [`LICENSE`](LICENSE). MMTk is licensed separately under its
own terms.
