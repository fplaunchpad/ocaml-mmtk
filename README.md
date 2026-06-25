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

> Runs on **x86-64 Linux** and **macOS (arm64)** — bytecode + native, both validated. Heap grows on
> demand (memory tracks the live set). Performance: see the quick panel below; tuning is the open
> milestone (M8, [`PERFORMANCE.md`](PERFORMANCE.md)), measured against a vanilla 5.5.0 opam switch.

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
- **Testsuite triage** is ongoing: each failing test is either fixed or disabled with a single
  greppable marker as its first line, `(* MMTk DISABLED: <reason> *)`, replacing the `(* TEST *)`
  block. `grep -rn 'MMTk DISABLED' testsuite/tests` lists every intentionally-disabled test and why
  (see ROADMAP item #19).

The milestone-by-milestone plan and current status are in
[`ROADMAP.md`](ROADMAP.md).

### Performance (quick panel)

12 stdlib-only CLBG/sandmark/effects programs, native, dynamic heap (memory parity with vanilla),
best-of-5 on an Apple M4 Pro. Single-domain uses `MMTK_THREADS=1`; the parallel sweep uses
`MMTK_THREADS=domains`, core-pinned. Run: `uv run quick/quickbench.py …`.

**Sequential** — wall vs vanilla 5.5.0 (lower is better):

![sequential ratio vs vanilla](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/seq_ratio.png)

| bench | GenImmix *(default)* | Immix | ConcurrentImmix |
|---|--:|--:|--:|
| binarytrees | 1.07× | 2.14× | **0.55×** |
| nbody | 1.01× | 1.01× | 1.02× |
| fannkuchredux | 0.99× | 1.00× | 1.00× |
| spectralnorm | **0.96×** | 1.20× | 1.19× |
| mandelbrot | 1.00× | 1.00× | 1.01× |
| matrix_multiplication | **0.90×** | 0.87× | 0.88× |
| LU_decomposition | **1.05×** | 1.25× | 1.31× |
| kb | 1.30× | 1.10× | **0.96×** |

GenImmix is parity-or-better on 7/8 benches; `kb` is the lone outlier, where ConcurrentImmix wins.

**Parallel** — speedup at 8 domains, controlled (pinned, `MMTK_THREADS=domains`; ideal = 8):

![speedup vs domains](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/speedup_domains.png)

| bench | vanilla | GenImmix | Immix | ConcurrentImmix |
|---|--:|--:|--:|--:|
| par_matmul | 5.72 | 4.71 | 5.31 | **5.78** |
| par_binarytrees | 3.49 | 1.36 | hang† | 0.91 |
| par_spectralnorm | 2.44‡ | hang† | hang† | 2.90 |

The fork scales ≈ vanilla on `par_matmul`; the "anti-scaling" earlier reported was mostly an
`nproc`-worker oversubscription artifact (now controlled). †intermittent multidomain hang; ‡vanilla also
regresses (bandwidth). Mechanism + RQ10: `SCALABILITY.md`.

The scheduler-assert deadlock is fixed for all plans (GH#6/#14, mmtk-core `ec2f5079f8`). Open residuals:
the intermittent multidomain hang above, and `chameneos_redux` under ConcurrentImmix. *Eyeball panel —
the M8 macro-bench campaign (`PERFORMANCE.md`) is authoritative.*

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
| `MMTK_NURSERY` | `Bounded:2097152,67108864` (2–64 MiB) | Generational-plan nursery (GenImmix/GenCopy/StickyImmix), bounded/absolute and commit-on-demand (adapts down to fit small heaps). The 64 MiB max keeps GenImmix competitive single-domain on allocation-heavy workloads. **Value must be raw BYTES** — e.g. `Fixed:33554432`, `Bounded:2097152,134217728`; the `2m,128m` suffix form does **not** parse (silently falls back to the default). |
| `MMTK_THREADS` | _nproc_ | GC worker threads. **Set `MMTK_THREADS=1` for single-/few-domain runs** — the `nproc` default oversubscribes and slows high-collection workloads (a domain-aware pool is the open fix). |
| `MMTK_VERBOSE` | unset | Print MMTk init and a GC summary at exit. |

mmtk-core's own `MMTK_*` options (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, …) also work.

### GC plans

`MMTK_PLAN` selects the collector at startup (default **`GenImmix`** — generational, copying nursery over
an Immix mature, the stock-OCaml-faithful fit for OCaml's short-lived allocation). **10** of 0.32's 11
plans are wired in bytecode; **7** run native (those whose allocator the inlined TLAB can alias).

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

`ConcurrentImmix` is the low-latency **research** plan: concurrent marking + SATB write barrier
(bytecode + native), clean on `lazy` and effect-handler continuations (subtle cases in
[`FAQ.md`](gc/mmtk/FAQ.md)); remaining work is performance-only. Its *allocate-black* marker (never scans
fresh objects) is also what lets the runtime skip zeroing new memory.

The one **unwired** plan is **`Compressor`** (needs a unified object-reference model incompatible with
OCaml's layout). `MarkSweep`/`MarkCompact`/`PageProtect` are bytecode-only (their allocators can't back
the inlined native TLAB).

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
