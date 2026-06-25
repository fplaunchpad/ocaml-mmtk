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

- **Base:** OCaml `5.5.0` (final). **Collector:** MMTk, always on, default plan **GenImmix** (copying nursery + Immix mature — the generational, stock-OCaml-faithful plan).
- **Binding:** in-tree at [`gc/mmtk/`](gc/mmtk), built against
  [`mmtk-core`](https://github.com/mmtk/mmtk-core) `0.32` — a small in-tree fork
  (`gc/mmtk-core`) carrying OCaml-specific deltas (e.g. it skips redundant
  allocation-time zeroing).
- Collection is multi-domain, parallel, and stop-the-world; moving plans relocate
  objects and generational plans use a write barrier. Native code allocates from a
  TLAB aliased to an MMTk Immix block, so it needs no special code generation. Both
  single- and multi-domain (`Domain.spawn`) programs run.

> Supported on **x86-64 Linux**; on **macOS (arm64)** both the **bytecode build** and **native
> compile + link + run** are validated (native programs link the in-tree MMTk staticlib
> relocatably via `-lmmtk_ocaml`, the same mechanism as Linux).
> MMTk's overhead is workload-dependent — on a native Immix-vs-vanilla-5.5.0 sweep it now
> reaches **parity or better on 5 of 6 benchmarks** (and is **~1.5× faster** on parallel allocation-heavy
> work), with one structural outlier (~1.74× on a sweep-bound float kernel). The heap grows on demand,
> so memory tracks the live set. Performance tuning is the open milestone (M8); the methodology is
> [`PERFORMANCE.md`](PERFORMANCE.md). Benchmark against a vanilla OCaml 5.5.0 opam switch.

## Status at a glance

- **Done:** the full bring-up (build, NoGC → MarkSweep → Immix, generational plans,
  multi-domain, native code, weak/ephemeron/finaliser support, the testsuite); **excising
  the stock GC** (no stock minor/major collector, shared heap, or minor-heap arena remains);
  advancing the base to **OCaml 5.5.0 final**; the MMTk-native multi-domain stop-the-world
  handshake; and **`ConcurrentImmix`** (bytecode + native) — a concurrent marker with an SATB
  write barrier, proven clean on `lazy` values and on effect-handler continuations.
- **In progress:** the macro-benchmark performance campaign + analysis (M8); and three
  rare-crash investigations tracked as GitHub issues.
- **Known tails:** weak-clear semantics under generational plans, a flagged memprof colour
  read, and `runtime_events` emission under MMTk (broken — see ROADMAP / FAQ).

The milestone-by-milestone plan and current status are in
[`ROADMAP.md`](ROADMAP.md).

### Performance (quick panel)

A fast eyeball panel — 10 stdlib-only CLBG/sandmark programs, native, **dynamic heap (memory
parity** with vanilla), best-of-3 on an Apple M4 Pro. Cells are `median-ms | ratio-vs-vanilla-5.5.0`;
lower is better. Reproduce with `benchmarks/quick/quickbench.sh` (`--chart` for ASCII bars).

| bench | vanilla | GenImmix *(default)* | Immix |
|---|--:|--:|--:|
| binarytrees | 1497 ms | 1567 (1.05×) | 1328 (0.89×) |
| nbody | 654 ms | 654 (1.00×) | 660 (1.01×) |
| fannkuchredux | 1442 ms | 1449 (1.00×) | 1449 (1.00×) |
| spectralnorm | 645 ms | 874 (1.35×) | 794 (1.23×) |
| mandelbrot | 687 ms | 691 (1.01×) | 690 (1.01×) |
| matrix_multiplication | 712 ms | 651 (0.91×) | 635 (0.89×) |
| LU_decomposition | 787 ms | 1785 (2.27×) | 1296 (1.65×) |

Parity-or-better on the compute-bound and mature-live-set benches (and **faster** on
matrix_multiplication); the two boxed-float kernels (spectralnorm, LU_decomposition) are the known
structural outliers (MMTk Immix mature-space sweep/metadata cost). `ConcurrentImmix` is omitted —
it's the experimental low-latency plan and currently **hangs on spectralnorm + LU_decomposition**
(a distinct, post-`#4`-fix issue; see `gc/mmtk/NOTES.md`). *This is a quick eyeball panel, not the
system of record — the macro-benchmark campaign (M8, `PERFORMANCE.md`) is authoritative.*

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
| `MMTK_PLAN` | `GenImmix` | GC plan — see **GC plans** below. |
| `MMTK_HEAP_SIZE_MB` | _dynamic_ | Pin a fixed heap (MiB). Unset: the heap grows on demand, like stock OCaml. |
| `MMTK_NURSERY` | `Bounded:2m,64m` | Generational-plan nursery (GenImmix/GenCopy/StickyImmix), bounded/absolute and commit-on-demand (adapts down to fit small heaps). The 64 MiB max keeps GenImmix competitive single-domain on allocation-heavy workloads; e.g. `Fixed:33554432`, `Bounded:2m,128m`. |
| `MMTK_THREADS` | _nproc_ | GC worker threads (MMTk's own default = all cores). Worker count does **not** improve multi-domain throughput scaling — that is bound by the all-domains stop-the-world, not the thread pool — so it mainly trades CPU for shorter GC pauses. Set `MMTK_THREADS=1` for the lowest-overhead single-domain runs (avoids per-collection worker park/wake). See `gc/mmtk/NOTES.md` (2026-06-24). |
| `MMTK_VERBOSE` | unset | Print MMTk init and a GC summary at exit. |

mmtk-core's own `MMTK_*` options (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, …) also work.

### GC plans

`MMTK_PLAN` selects the collector at startup. **Ten** of mmtk-core 0.32's eleven plans are
wired in **bytecode**; **native** runs the **seven** whose Default allocator is a bump/Immix
region the inlined TLAB can alias — `Immix`/`StickyImmix` (in-place), `GenImmix`/`GenCopy`
(copy-nursery), `SemiSpace`/`NoGC`, and `ConcurrentImmix`. `GenImmix` (copying nursery over an Immix mature) is
the **default** plan — generational, stock-OCaml-faithful, and the right fit for OCaml's short-lived-allocation
profile (a cheap copying nursery; see [`PERFORMANCE.md`](PERFORMANCE.md)).

| Plan | Description | Runtimes |
|------|-------------|----------|
| `Immix` | mark-region, moving (defragments) | bytecode + native |
| `StickyImmix` | generational, in-place nursery | bytecode + native |
| `GenImmix` *(default)* | generational, copying nursery + Immix mature | bytecode + native |
| `GenCopy` | generational, copying nursery + SemiSpace mature | bytecode + native |
| `SemiSpace` | classic two-space copying | bytecode + native |
| `NoGC` | bump-only; never reclaims (short programs only) | bytecode + native |
| `MarkSweep` | non-moving free-list | bytecode (native infeasible — free-list) |
| `MarkCompact` | sliding compaction (Lisp-2) | bytecode (native infeasible — VO bit + header word) |
| `PageProtect` | one page per object (debugging) | bytecode |
| `ConcurrentImmix` | concurrent marking, SATB barrier | bytecode + native (low-latency research plan) |

`ConcurrentImmix` is the low-latency **research** plan: it marks concurrently with the mutator using
an SATB (snapshot-at-the-beginning) write barrier, wired in both bytecode and native. It runs cleanly on
`lazy` values and on effect-handler continuations — the subtle cases are written up in
[`gc/mmtk/FAQ.md`](gc/mmtk/FAQ.md). The remaining work on it is performance-only. Its marker is
*allocate-black* (it never scans freshly-allocated objects), which is also what lets the runtime safely skip
zeroing freshly-allocated memory.
The one **unwired** plan is **`Compressor`**, which needs a unified object-reference model
incompatible with OCaml's value/header layout (see [`ROADMAP.md`](ROADMAP.md)).
`MarkSweep`/`MarkCompact`/`PageProtect` are bytecode-only — their allocators can't back the
inlined native TLAB.

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
