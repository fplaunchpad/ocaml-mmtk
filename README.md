# OCaml + MMTk (`mmtk-ocaml`)

A fork of [OCaml](https://github.com/ocaml/ocaml) whose garbage collector is
[MMTk](https://www.mmtk.io), the Memory Management Toolkit. A normally-built
compiler runs ordinary OCaml programs on an MMTk-managed heap. The eventual goal
is to ship this as an `ocaml-variants.5.x+mmtk` opam switch.

- **Base:** OCaml `5.5.0-rc1`.
- **Garbage collector:** MMTk is the **only** collector and is on by default —
  there is no opt-out, and OCaml's stock GC has been removed. The default plan is
  **Immix**.
- **Binding:** in-tree at [`gc/mmtk/`](gc/mmtk), built against
  [`mmtk-core`](https://github.com/mmtk/mmtk-core) `0.32` from crates.io.

A normal `./configure && make` builds the world and self-hosts — the compiler
bootstraps and the documentation builds — entirely on MMTk, for both the
**bytecode** and **native** runtimes. Native code uses TLAB nursery-aliasing (MMTk
owns the nursery), so there are no code-generator changes. Collection is parallel
and stop-the-world; moving plans relocate objects, generational plans use a write
barrier, and single- and multi-domain (`Domain.spawn`) programs are supported.

> Developed and tested on **x86-64 Linux**; native code on macOS is not yet
> exercised. On GC-heavy workloads MMTk is currently ~1.4–1.8× the stock GC, with
> tuning ongoing.

**Learn more:** the plan and current status live in [`ROADMAP.md`](ROADMAP.md);
design notes and investigations in [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md); project
background in [`fork-handoff.md`](fork-handoff.md). The upstream OCaml README is
preserved at [`README.upstream.adoc`](README.upstream.adoc).

## Building

Exactly like upstream OCaml — the MMTk static library is compiled with `cargo` and
linked in automatically:

```sh
./configure
make            # builds the world; gc/mmtk is built and linked for you
```

Beyond the usual OCaml build prerequisites (see [`INSTALL.adoc`](INSTALL.adoc)) you
need:

- **Rust + Cargo** (stable) — the build runs `cargo` to produce the binding.
- On macOS the binding links `-lobjc -framework IOKit -framework CoreFoundation
  -liconv` (wired up for you in [`Makefile.mmtk`](Makefile.mmtk)).

## Running

MMTk is on by default — build and run as usual:

```sh
OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun ./ocamlc myprog.ml -o myprog.byte
OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun myprog.byte
```

> **Linux:** run under `setarch "$(uname -m)" -R` (disables ASLR) to avoid an
> occasional start-up abort (`failed to mmap meta memory`).

### Configuration

| Variable | Default | Meaning |
|----------|---------|---------|
| `MMTK_PLAN` | `Immix` | MMTk plan: `Immix`, `StickyImmix`, `GenImmix`, `MarkSweep`, `NoGC`. |
| `MMTK_HEAP_SIZE_MB` | `1024` | Fixed heap size, in MiB. |
| `MMTK_VERBOSE` | unset | Print MMTk init and a GC summary at exit. |

mmtk-core's own `MMTK_*` options (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, …) also work.

**Choosing a plan.** Native code requires an Immix-family plan
(`Immix`/`StickyImmix`/`GenImmix`), since MMTk owns the nursery via TLAB aliasing;
bytecode works with any plan. `NoGC` never reclaims memory, so use it only for
short programs. To compare against the stock GC, build a separate vanilla OCaml 5.5
opam switch.

## Repository layout

```
gc/mmtk/            in-tree MMTk binding (a self-contained Cargo workspace)
├── common/         OCaml value layout: header, slot, scanning, object model
├── binding/        VMBinding impl + C ABI  ->  libmmtk_ocaml.a
└── include/        mmtk_ocaml.h, the C ABI consumed by the runtime
runtime/mmtk.c      C glue between the runtime and the binding
Makefile.mmtk       build glue (cargo invocation + link flags)
```

Runtime changes are concentrated in `runtime/` (allocation, the write barrier, root
scanning, and domain init), all routed unconditionally to MMTk.

## License

Same as OCaml — see [`LICENSE`](LICENSE). MMTk is licensed under its own terms.
