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

> **Status: bring-up.** MMTk backs the **bytecode** runtime, **opt-in** (off by
> default, so a normal build and the compiler bootstrap run on OCaml's stock GC).
> `NoGC`, `MarkSweep`, and `Immix` all work — MarkSweep and Immix collect single-
> and multi-domain, Immix relocates objects, and collection is parallel. Native
> code is unchanged and uses the stock GC (native integration is future work).

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
| — | Parallel collection ✅ verified (marking scales ~8× on 16 threads) | 🟡 |
| next | Weak/ephemeron + finalisers, generational plans, native code, testsuite, benchmarks | ⬜ |

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
| `MMTK_ENABLED` | unset (off) | Set to `1` to let MMTk manage the bytecode heap. |
| `MMTK_PLAN` | `NoGC` | MMTk plan: `NoGC`, `MarkSweep`, `Immix`, … |
| `MMTK_HEAP_SIZE_MB` | `1024` | Fixed heap size, in MiB. |
| `MMTK_GC_THREADS` | core count | Number of parallel GC worker threads. |
| `MMTK_VERBOSE` | unset | Print MMTk init + a GC/objects-copied summary at exit. |

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
`interp.c`, `domain.c`, `domain_state.tbl`, `signals.c`, `intern.c`) and are all
guarded by `#ifndef NATIVE_CODE`, so the native runtime and compiler are
untouched.

## License

Same as OCaml — see [`LICENSE`](LICENSE). MMTk is licensed separately under its
own terms.
