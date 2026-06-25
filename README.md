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
- **Testsuite triage** is ongoing: each failing test is either fixed or disabled with a single
  greppable marker as its first line, `(* MMTk DISABLED: <reason> *)`, replacing the `(* TEST *)`
  block. `grep -rn 'MMTk DISABLED' testsuite/tests` lists every intentionally-disabled test and why
  (see ROADMAP item #19).

The milestone-by-milestone plan and current status are in
[`ROADMAP.md`](ROADMAP.md).

### Performance (quick panel)

A fast eyeball panel — 12 stdlib-only CLBG/sandmark/effects programs, native, **dynamic heap (memory
parity** with vanilla), best-of-5 on an Apple M4 Pro. **Single-domain runs use `MMTK_THREADS=1`** (the
correct config at ≤ a few domains — MMTk's default `nproc` workers oversubscribe and heavily tax
high-collection benches; a domain-aware worker pool is the open fix). The parallel sweep uses
**`MMTK_THREADS=domains`, core-pinned**. Reproduce: `uv run quick/quickbench.py …` (raw data in
`quick/results.ndjson`).

**Sequential** — ratio vs vanilla 5.5.0 at `MMTK_THREADS=1` (lower is better):

| bench | GenImmix *(default)* | Immix | ConcurrentImmix |
|---|--:|--:|--:|
| binarytrees | 1.07× | 2.14× | **0.55×** |
| nbody | 1.01× | 1.01× | 1.02× |
| fannkuchredux | 0.99× | 1.00× | 1.00× |
| spectralnorm | **0.96×** | 1.20× | 1.19× |
| mandelbrot | 1.00× | 1.00× | 1.01× |
| matrix_multiplication | **0.90×** | 0.87× | 0.88× |
| LU_decomposition | **1.05×** | 1.25× | 1.31× |
| kb *(symbolic, Rocq-like)* | 1.30× | 1.10× | **0.96×** |

**GenImmix (the default) is parity-or-better than vanilla on 7/8 sequential benches** (0.90–1.07×;
spectralnorm and matmul are *faster*, from no-zero allocation). The lone GenImmix outlier is `kb`
(1.30×, a symbolic term-rewriting churn) — where **ConcurrentImmix instead beats vanilla** (0.96×). The
earlier "LU/spectralnorm ~2× structural outlier" was **not** a GC-design cost but a measurement artifact
of the `nproc`-worker default: LU is 2.26× at `nproc` (it triggers ~2467 tiny collections from boxed
intermediates, and 12 idle workers park/wake on each) but **1.05× at `MMTK_THREADS=1`**.

**Parallel scalability** — speedup `S(N)=T(1)/T(N)` at 8 domains, **controlled** (core-pinned,
`MMTK_THREADS=domains`, so domains + GC workers ≤ cores; ideal `S=8`):

| bench | vanilla | GenImmix | Immix | ConcurrentImmix |
|---|--:|--:|--:|--:|
| par_matmul | 5.72 | 4.71 | 5.31 | **5.78** |
| par_binarytrees | 3.49 | 1.36 | hang† | 0.91 |
| par_spectralnorm | 2.44‡ | hang† | hang† | 2.90 |

† an *intermittent* multidomain-rendezvous hang remains after the GH#6 assert fix (a rarer residual —
see below). ‡ even *vanilla* regresses past d4 on par_spectralnorm (memory-bandwidth bound, not GC).

**The dramatic "anti-scaling" was substantially a measurement artifact.** The earlier panel ran `nproc`
GC workers unpinned, oversubscribing the cores; controlled (`MMTK_THREADS=domains`, pinned) and with the
GH#6 deadlock fixed, the fork **scales as well as vanilla on `par_matmul`** (GenImmix 4.71,
ConcurrentImmix 5.78 ≈ vanilla 5.72) and only **mildly sublinearly on the heaviest-alloc bench**
(par_binarytrees GenImmix `S(8)=1.36`, vs the uncontrolled `0.64`). The same `nproc`-oversubscription
inflated the sequential outliers above. The remaining real levers — a domain-aware GC-worker pool and
the per-minor-collection cost — and the RQ10 architecture question (per-domain minor vs MMTk-major-only)
are in `SCALABILITY.md` / `RESEARCH_QUESTIONS.md`.

**Scheduler-assert deadlock FIXED for ALL plans** (mmtk-core `ec2f5079f8`): the stop-the-world-era
`scheduler.rs` assert that forbade a GC request while a GC is in progress is **removed** — its premise
is false for OCaml's multi-domain model. It bit two ways: **ConcurrentImmix** (GH#14, a domain
re-requesting mid-concurrent-mark; spectralnorm / LU / par_spectralnorm now run clean, checksums match
golden) **and** any **STW plan (e.g. GenImmix) at ≥8 domains** (GH#6 — a 2nd domain's alloc poll, or a
domain being *created* refilling its TLAB; rr-confirmed, par_binarytrees d8 was a 100% hang → 5/5 OK
after the fix). **Two residuals remain, both open:** (1) a *rarer intermittent* multidomain-rendezvous
hang (the assert was the dominant, deterministic cause — removing it unmasked a slower bug#3c-class race
that still scatters some parallel cells, marked `hang†` above; needs more rr); (2) `chameneos_redux`
hangs under ConcurrentImmix via a *separate* deadlock (no assert — an effects/continuation ×
concurrent-mark issue). *This is a quick eyeball panel, not the system of record — the macro-benchmark
campaign (M8, `PERFORMANCE.md`) is authoritative.*

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
