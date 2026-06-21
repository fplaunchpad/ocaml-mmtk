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

> **Status: MMTk is the GC.** As of M9 it is **on by default** for both the
> **bytecode** and **native** runtimes — a normal `./configure && make` builds and
> self-hosts (the compiler bootstrap reaches its fixpoint) entirely on MMTk. The
> stock GC is being excised — MMTk is the only collector, there is no opt-out
> (benchmark against stock via a separate vanilla OCaml 5.5 opam switch). `NoGC`,
> `MarkSweep`, `Immix`,
> `GenImmix`, `StickyImmix` all work — collecting plans collect single- **and**
> multi-domain (`Domain.spawn`), moving plans relocate, generational plans use a
> write barrier, collection is parallel. **Native** uses TLAB nursery aliasing
> (MMTk owns the nursery; no OCaml minor GC), so the native fast-path is unchanged
> and there are no code-generator changes (validated on x86-64 Linux; arm64/macOS
> not yet exercised). The default plan is **Immix**.
>
> Known limitations (see `ROADMAP.md`): weak arrays / ephemerons / finalisers /
> lazy are not yet supported (parked; their tests are disabled); performance is
> ~1.4–1.8× of the stock GC on GC-heavy workloads today (tuning in progress).

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
| M5 | **Native-code integration** — all-MMTk via TLAB/nursery-aliasing (Immix-family plans), single- **and** multi-domain; staticlib auto-linked | ✅ done |
| M6 | Runtime features: weak arrays, ephemerons, finalisers — `process_weak_refs` **on by default** (`MMTK_WEAK_REFS=0` opts out, transitional). Weak-clear, ephemeron-release, `Gc.finalise`/`finalise_last`, **and custom-block finalizers** all work under Immix **and** StickyImmix — `pr3612` + `pr5233` pass; full bootstrap clean; no regressions (remaining testsuite failures are non-M6: memprof, runtime-events, `Gc.stat`). | 🟢 done |
| M7 | Pass the OCaml testsuite — full bytecode suite under StickyImmix: **1476/1524 pass**; failures are unsupported features (weak/finaliser — fixed by `MMTK_WEAK_REFS`; `Gc.stat`/memprof/runtime-events) + 1 multidomain-StickyImmix SIGSEGV (bug #3). Default Immix clean | 🟡 |
| M8 | **Benchmark + optimise** vs. the stock GC — first baseline ~1.4–1.8× slower on GC-heavy native bench; optimisation levers identified | 🟡 started |
| M9 | **MMTk-only: excise the stock GC** — MMTk always-on (done), then delete the stock minor/major GC + shared heap | 🟡 in progress |
| — | Parallel collection ✅ verified (marking scales ~8× on 16 threads) | ✅ |

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

This produces an OCaml toolchain whose runtime is managed by MMTk (the default).

## Running a program

MMTk is on by default — just build and run as usual:

```sh
OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun ./ocamlc myprog.ml -o myprog.byte
OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun myprog.byte          # runs on MMTk (Immix)
```

> On Linux, run under `setarch "$(uname -m)" -R` (disables ASLR) to avoid an
> occasional MMTk start-up abort (`failed to mmap meta memory`) — a known
> fixed-address-metadata interaction, see `ROADMAP.md`.

### Environment knobs

| Variable | Default | Meaning |
|----------|---------|---------|
| `MMTK_PLAN` | `Immix` | MMTk plan: `Immix`, `StickyImmix`, `MarkSweep`, `NoGC`, … (native requires an Immix-family plan). |
| `MMTK_HEAP_SIZE_MB` | `1024` | Fixed heap size, in MiB. |
| `MMTK_VERBOSE` | unset | Print MMTk init + a GC/objects-copied summary at exit. |

mmtk-core's own `MMTK_*` options also work (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`,
`MMTK_IMMIX_ALWAYS_DEFRAG`, …). For **native** code, MMTk owns the nursery via TLAB
nursery-aliasing (no OCaml minor GC), which requires an Immix-family plan
(`Immix`/`StickyImmix`/`GenImmix`) — a non-Immix plan is a fatal error for native.
Bytecode allocates through C entry points and works with any plan.

> MMTk's own options are also read from the environment, e.g.
> `MMTK_IMMIX_ALWAYS_DEFRAG=true MMTK_IMMIX_DEFRAG_EVERY_BLOCK=true` forces Immix
> to relocate objects (useful for exercising the moving path).

> `MarkSweep` and `Immix` collect (single- and multi-domain); `Immix` also
> relocates objects. Under `NoGC`, memory is never reclaimed — long-running or
> allocation-heavy programs (including the OCaml compiler) will exhaust the heap;
> that is expected — build or bootstrap the compiler under a collecting plan (the
> default `Immix`), not `NoGC`.

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
`mmtk.c`/`mmtk.h` is the glue, compiled into both runtimes. As of M9 **MMTk is
always-on**: allocation, the write barrier, and domain init go unconditionally to
MMTk (the old `caml_mmtk_vanilla_minor` native mode has been removed — native
always uses TLAB nursery-aliasing). MMTk is the only collector — there is no
opt-out; the stock GC code is being deleted (M9). Benchmark against stock via a
separate vanilla OCaml 5.5 opam switch.

## License

Same as OCaml — see [`LICENSE`](LICENSE). MMTk is licensed separately under its
own terms.
