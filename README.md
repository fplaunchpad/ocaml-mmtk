# OCaml + MMTk (`ocaml-mmtk`)

A fork of [OCaml](https://github.com/ocaml/ocaml) 5.5 whose garbage collector is
[MMTk](https://www.mmtk.io), the Memory Management Toolkit. MMTk is the **only**
collector and is **always on** — there is no opt-out and no stock OCaml GC left in
the tree. A normal `./configure && make` builds the full compiler, both **bytecode**
and **native**, on an MMTk-managed heap; it self-hosts (the compiler bootstraps and
its documentation builds).

This is also a **GC-research platform**: one functional, immutable-by-default,
multicore, effect-handler language on which MMTk's many collectors can be compared
on a common substrate. The research agenda lives in
[`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md).

- **Base:** OCaml `5.5.0-rc1`. **Collector:** MMTk, always on, default plan **Immix**.
- **Binding:** in-tree at [`gc/mmtk/`](gc/mmtk), built against
  [`mmtk-core`](https://github.com/mmtk/mmtk-core) `0.32` from crates.io.
- Collection is multi-domain, parallel, and stop-the-world; moving plans relocate
  objects and generational plans use a write barrier. Native code allocates from a
  TLAB aliased to an MMTk Immix block, so it needs no special code generation. Both
  single- and multi-domain (`Domain.spawn`) programs run.

> Supported on **x86-64 Linux**; native code on macOS is untested. On GC-heavy
> workloads MMTk currently runs at ~1.4–1.8× the stock GC and uses more memory —
> performance tuning is the open milestone (M8); the measurement methodology is
> [`PERFORMANCE.md`](PERFORMANCE.md). Benchmark against a vanilla OCaml 5.5 opam switch.

## Status at a glance

- **Done:** the full bring-up (build, NoGC → MarkSweep → Immix, generational plans,
  multi-domain, native code, weak/ephemeron/finaliser support, the testsuite) and
  **excising the stock GC** — no stock minor/major collector, shared heap, or
  per-domain minor-heap arena remains. (Two tails remain: weak-clear semantics under
  generational plans, and a flagged memprof colour read.)
- **In progress:** performance (benchmark + optimise), and a from-first-principles,
  MMTk-native rework of the multi-domain stop-the-world handshake (see bug #3b in
  [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md)).

The milestone-by-milestone plan and current status are in
[`ROADMAP.md`](ROADMAP.md).

## Building

Exactly like upstream OCaml — the MMTk static library is compiled with `cargo` and
linked in automatically:

```sh
./configure
make            # builds the world; gc/mmtk is built and linked for you
make world.opt  # also build the native compiler
```

Beyond the usual OCaml build prerequisites (see [`INSTALL.adoc`](INSTALL.adoc)) you
need **Rust + Cargo** (stable) — the build runs `cargo` to produce the binding.

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
| `MMTK_PLAN` | `Immix` | GC plan — see **GC plans** below. |
| `MMTK_HEAP_SIZE_MB` | `1024` | Fixed heap size, in MiB. |
| `MMTK_VERBOSE` | unset | Print MMTk init and a GC summary at exit. |

mmtk-core's own `MMTK_*` options (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, …) also work.

### GC plans

`MMTK_PLAN` selects the collector at startup. Nine of mmtk-core 0.32's eleven plans are
wired and validated in **bytecode**; **native** runs the four that have a bump-pointer
nursery the TLAB can alias — `Immix`/`StickyImmix` (in-place) and `GenImmix`/`GenCopy`
(copy-nursery). `GenImmix` is the stock-faithful generational native default candidate.

| Plan | Description | Runtimes |
|------|-------------|----------|
| `Immix` *(default)* | mark-region, moving (defragments) | bytecode + native |
| `StickyImmix` | generational, in-place nursery | bytecode + native |
| `GenImmix` | generational, copying nursery | bytecode + native |
| `MarkSweep` | non-moving free-list | bytecode |
| `NoGC` | bump-only; never reclaims (short programs only) | bytecode |
| `SemiSpace` | classic two-space copying | bytecode |
| `GenCopy` | generational, copying nursery + SemiSpace mature | bytecode + native |
| `MarkCompact` | sliding compaction (Lisp-2) | bytecode |
| `PageProtect` | one page per object (debugging) | bytecode |

Two plans remain unwired: **`Compressor`** needs a unified object-reference model
(incompatible with OCaml's value/header layout) and **`ConcurrentImmix`** needs an SATB
write barrier — both need deeper changes (see [`ROADMAP.md`](ROADMAP.md)). Extending native
beyond the Immix family to the other bump-pointer plans (`SemiSpace`/`GenCopy`/`MarkCompact`)
is performance-milestone (M8) work.

## Repository layout

```
gc/mmtk/            in-tree MMTk binding (a self-contained Cargo workspace)
├── common/         OCaml value layout: header, slot, scanning, object model
├── binding/        VMBinding impl + C ABI  ->  libmmtk_ocaml.a
└── include/        mmtk_ocaml.h, the C ABI consumed by the runtime
runtime/mmtk.c      C glue between the runtime and the binding
Makefile.mmtk       build glue (cargo invocation + link flags)
```

In `runtime/`, allocation, the write barrier, root scanning, and domain
initialization all go through MMTk; the C glue lives in `runtime/mmtk.c`.

## Learn more

- [`ROADMAP.md`](ROADMAP.md) — the live plan and milestone status.
- [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) — dated design notes and investigations.
- [`gc/mmtk/FAQ.md`](gc/mmtk/FAQ.md) — correctness & concurrency hazards (mechanism-level Q&A).
- [`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) — the GC-research agenda.
- [`PERFORMANCE.md`](PERFORMANCE.md) — the GC-performance measurement method of record (M8).
- [`fork-handoff.md`](fork-handoff.md) — original cold-start brief.
- [`README.upstream.adoc`](README.upstream.adoc) — the upstream OCaml README.

## License

Same as OCaml — see [`LICENSE`](LICENSE). MMTk is licensed under its own terms.
