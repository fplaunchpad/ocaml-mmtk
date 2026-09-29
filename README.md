# OCaml + MMTk

`ocaml-mmtk` is a fork of **OCaml 5.5.0** with
[MMTk](https://www.mmtk.io), the Memory Management Toolkit, as its only garbage
collector. It builds the bytecode and native compilers, bootstraps itself, and
supports multicore programs using `Domain.spawn` and effect handlers. Choose a
collector at startup with `MMTK_PLAN`; the default is **GenImmix**.

**Research prototype:** native code using the default plan has a reproduced
silent-corruption bug; see [known limits](#what-works-and-what-is-still-open).

## Why this fork exists

This is a platform for studying garbage collection in a functional language.
OCaml combines precise roots, mostly immutable data, frequent short-lived
allocation, multiple domains, and captured continuations. These properties let
us ask how collector trade-offs change across workloads and language designs.

The central questions are:

- Does low pointer mutation make low-latency collection cheap without adding work
  to every pointer read?
- When do copying, in-place collection, concurrent marking, or reference counting
  suit OCaml's allocation and mutation patterns?
- How much of a performance difference comes from collector policy, and how much
  comes from the runtime binding and MMTk's shared machinery?
- How do domain coordination and continuation scanning affect throughput and
  pause time as programs scale?

[`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) develops the agenda. Two research
plans make these comparisons concrete: **Bactrian** brings the collector
architecture closer to stock OCaml, while **LXR** explores reference counting.
Their differences from stock OCaml and the original LXR design are part of the
experiments.

## What works, and what is still open

Bytecode and native execution have been validated on **x86-64 Linux** and
**Apple Silicon macOS**, including moving and generational collectors,
multi-domain collection, weak references, ephemerons, and finalisers. Native
allocation uses the existing compiler fast path, backed by MMTk allocation
regions. The stock OCaml collector has been removed; compare against a separate
vanilla OCaml 5.5.0 build.

On **2026-09-29**, [post-merge Linux CI](https://github.com/fplaunchpad/ocaml-mmtk/actions/runs/36534216189),
covering bytecode and native variants, reported no failures among 1495 tests
considered per plan under GenImmix, StickyImmix, SemiSpace, and Bactrian.
GenCopy failed `misc/darkening_work.ml`; Immix and ConcurrentImmix hit the
`weaklifetime.ml` timeout, addressed by a pending
[test heap-pinning change](https://github.com/fplaunchpad/ocaml-mmtk/pull/27).
The suite includes disabled cases for unsupported features and GC timing
differences. Memprof is unsupported; [`runtime_events` GC-event emission](https://github.com/fplaunchpad/ocaml-mmtk/issues/20)
is unimplemented, and [signal-delivery poll points](https://github.com/fplaunchpad/ocaml-mmtk/issues/19)
differ from stock OCaml in two tests.

**Known correctness limits (2026-09-29):** LXR has a documented
[silent wrong-results bug](https://github.com/fplaunchpad/ocaml-mmtk/issues/26), so
its results are provisional. The [native `Array.fill` path](runtime/array.c) also
lacks the generational write barrier: silent corruption has been reproduced
under **GenImmix (the default)**, StickyImmix, and GenCopy. A fix is in progress.
Programs using `Thread` and blocking sections have a separate
[coordination audit](https://github.com/fplaunchpad/ocaml-mmtk/issues/24): source
reading identifies possible unsafe execution during GC and a thread-exit GC
hang, but neither has been reproduced.

Explicit `Gc` requests are also under investigation: `Gc.major`, `Gc.full_major`,
and `Gc.compact` can return before their collection runs, while `Gc.minor` does
not trigger MMTk in the tested configurations. Current dynamic heap sizing can
also grow the budget despite a stable live set, reproduced under Immix; its fix
has not landed. These gaps matter when interpreting tests and benchmark results.

[`ROADMAP.md`](ROADMAP.md) tracks the work;
[`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) contains dated fixes, validation, and known
failure cases. Consult the latest entries for current evidence.

## Build and run

You need the usual [OCaml build prerequisites](INSTALL.adoc), plus **stable Rust
and Cargo** on your `PATH`. The MMTk binding builds and links automatically.

```sh
git clone --branch 5.5+mmtk https://github.com/fplaunchpad/ocaml-mmtk.git
cd ocaml-mmtk
git submodule update --init gc/mmtk-core
./configure
make -j4 world.opt
```

`world.opt` builds both bytecode and native compilers and libraries. The
`gc/mmtk-core` submodule pins the collector implementation; `make` also initializes
it when missing. Initialize it explicitly before building the Rust binding with
Cargo directly.

From the build directory, compile and run a small program with both runtimes:

```sh
cat > hello.ml <<'ML'
let () = print_endline "Hello from OCaml + MMTk"
ML
export OCAMLLIB="$PWD/stdlib"
./ocamlc.opt hello.ml -o hello.byte
./runtime/ocamlrun hello.byte
./ocamlopt.opt hello.ml -o hello.native
./hello.native

# Select a different collector without rebuilding.
MMTK_PLAN=Immix ./hello.native
MMTK_PLAN=LXR MMTK_HEAP_SIZE_MB=512 ./hello.native
```

On Linux, an occasional startup abort (`failed to mmap meta memory`) can be
avoided by disabling ASLR for the command:

```sh
setarch "$(uname -m)" -R make -j4 world.opt
setarch "$(uname -m)" -R ./hello.native
```

## Choose a collector

These plans support **both bytecode and native code**:

| Plan | What it lets you compare |
|---|---|
| **GenImmix** (default) | Generational collection: a copying nursery for young objects, with an Immix mature heap. Collections stop all mutators. |
| **Immix** | Whole-heap mark-region collection with optional evacuation to reduce fragmentation; stop-the-world. |
| **StickyImmix** | Generational collection with an in-place nursery; stop-the-world. |
| **Bactrian** | Copying nursery and Immix mature heap; adaptive full or sliced stop-the-world major collection and incremental sweep by default. Optional worker-concurrent marking. |
| **ConcurrentImmix** | Concurrent marking with a deletion write barrier; mature reclamation remains stop-the-world. |
| **LXR** | In-place reference counting on Immix, with stop-the-world backup tracing for cycles. **Known wrong results; provisional. Requires a pinned heap.** |
| **GenCopy** | Copying nursery and copying mature heap; stop-the-world. |
| **SemiSpace** | Whole-heap copying between two spaces; stop-the-world. |
| **NoGC** | Allocation without reclamation, useful for bounded experiments. |

The remaining supported plans are **bytecode-only**:

| Plan | Purpose |
|---|---|
| **MarkSweep** | Non-moving collection using a free-list allocator. |
| **MarkCompact** | Sliding compaction, requiring per-object bookkeeping absent from the native allocation fast path. |
| **PageProtect** | Debugging collector with one page per object. |

The stock MMTk **Compressor** plan is not wired into this binding.

**Bactrian and stock OCaml.** Both use a copying nursery and a mature heap with a
barrier that preserves old references during marking. Bactrian executes marking
and sweeping on GC workers; its default policy spreads sufficiently costly
majors over stop-the-world nursery pauses, including pauses requested to advance
marking or sweeping when allocation goes directly to the mature heap. Short
predicted majors, explicit full collections, and allocation emergencies can
collect the whole heap in one stop-the-world pause (a **Full**).
Stock OCaml performs major work in mutator slices, and uses a different mature
allocator. Bactrian's Immix mature heap can also move objects during a Full.
These differences are part of the comparison, not eliminated framework costs.

**LXR and the paper.** This is a simplified, adapted port of the PLDI'22 LXR
design. Its reference-counting pauses and backup tracing are stop-the-world;
it does not reproduce the paper's concurrent backup collector. The backup is
requested when an RC pause reclaims too little at high occupancy, or leaves the
heap critically full. Earlier validation included single-domain runs and
multi-domain `par_binarytrees` at 1–32 domains, but those checks did not exercise
the pattern now known to give wrong results. The
[release-counter abort](https://github.com/fplaunchpad/ocaml-mmtk/issues/25) was
fixed in the current MMTk submodule; that fix does not resolve issue 26.
LXR participates in the
[cross-plan testsuite workflow](.github/workflows/testsuite-plans.yml), while
remaining an experimental research plan.

## Configure a run

Environment variables apply at process startup. Sizes ending in `_MB` are MiB;
`MMTK_NURSERY` uses raw bytes.

| Variable | Default | Meaning |
|---|---|---|
| `MMTK_PLAN` | `GenImmix` | Select the collector. |
| `MMTK_HEAP_SIZE_MB` | Unset | Pin a fixed heap. Otherwise the intended target is about 2.2× the live heap, estimated from collector accounting, with nursery headroom and a physical-RAM ceiling; see the current growth bug above. A heap budget is not an RSS limit. |
| `MMTK_MIN_HEAP_MB` | `32` | Lower bound for the default dynamic heap target. |
| `MMTK_NURSERY` | `Bounded:2097152,16777216` | Generational nursery budget: 2–16 MiB per live domain by default. Explicit values are not scaled; for example, `Fixed:8388608`. Unit suffixes such as `2m` do not parse. |
| `MMTK_NURSERY_PER_DOMAIN` | Enabled | Set to `0` to disable domain scaling of the default nursery. When the heap cannot grow, the scaled minimum is constrained to preserve mature-heap space. |
| `MMTK_THREADS` | Logical CPU count | GC workers. Try `1` for single-domain experiments; record the worker count when comparing results. |
| `MMTK_VERBOSE` | Unset | Any value, including `0`, enables initialization details and an exit summary; unset it to disable. |
| `MMTK_PAUSE_LOG` | Unset | Path for per-pause stop-the-world records, useful while GC-event emission through `runtime_events` is unimplemented. |
| `MMTK_TRANSPARENT_HUGEPAGES` | `true` on Linux; off elsewhere | Request transparent hugepages on Linux. Record this setting in memory comparisons. |
| `MMTK_ALLOC_JITTER` | `6` | Vary placement before bump allocations of at least 2 KiB, using up to 63 cache-line pads. `0` disables this padding. |

Both bounded nurseries and explicit fixed pins have a guard against a nursery
larger than one quarter of the current heap, including dynamic heaps. A bounded
nursery still respects its minimum, which can exceed that guard. With one domain
at the 32 MiB heap floor, the effective default nursery maximum is **8 MiB**.

MMTk's own options, including `MMTK_GC_TRIGGER` and `MMTK_STRESS_FACTOR`, are also
available. An explicit `MMTK_GC_TRIGGER` overrides the default dynamic policy;
a pinned `MMTK_HEAP_SIZE_MB` selects the fixed policy.

<details>
<summary>Advanced collector controls</summary>

Most controls here apply to Bactrian; the early-cycle and compaction thresholds
also apply to other concurrent plans, including ConcurrentImmix.
Slice budgets and targets are **not hard bounds on total pause time**: work
quotas, packet granularity, nursery collection, and emergency drains can exceed
them.

| Variable | Default | Meaning |
|---|---|---|
| `MMTK_MARK_SLICED` | `1` | Adaptive Full/sliced STW mode. `0` selects worker-concurrent marking with STW sweep at the mark-completion pause (FinalMark), and a Full fallback for small mature heaps. |
| `MMTK_SLICE_WORTH_MS` | `200` | Slice when collecting the whole heap in one pause is predicted to cost more than this. Mature-direct allocation cycles bypass this test. |
| `MMTK_SLICE_MAX_PAUSE_MS` | `100` | Pause target used for mark-slice sizing and early cycle starts. |
| `MMTK_MARK_SLICE_MS` | `2` | Time-budget top-up after accounting for incoming work and a share of the marking backlog. |
| `MMTK_SWEEP_SLICE_MS` | `2` | Sweep budget, supplemented by a share of remaining work. |
| `MMTK_SWEEP_SLICE_CAP_MS` | `20` | Soft time cap on that sweep-work share. |
| `MMTK_CONC_TRIGGER_PCT` | `80` | Concurrent plans' early-cycle threshold as a percentage of the heap limit, combined with mature-pressure and growth guards. `0` disables the clamp. |
| `MMTK_MEDIUM_NONMOVING` | `1` for Bactrian | Allocate blocks ≥2056 B directly in mature space on 64-bit hosts, following stock OCaml's young-allocation cutoff. This controls placement, not a guarantee against later evacuation. |
| `MMTK_MEDIUM_TO` | `immix` | Set to `freelist` to place the pretenured medium-object band in the common mark-sweep space. Experimental allocator comparison. |
| `MMTK_COMPACT_OVERHEAD_PCT` | `100` | Concurrent plans' threshold for requesting mature compaction after a Full when reserved space sufficiently exceeds traced live bytes. `0` disables this trigger. |
| `MMTK_UP_OLDIFY` | `0` | Opt-in stock-style nursery tracing for eligible single-worker Bactrian pauses. |

Current policy lives in the [Bactrian implementation](https://github.com/fplaunchpad/mmtk-core/blob/045f121143c5772fd2715d8bfaf5274238ced4b1/src/plan/concurrent/bactrian/global.rs)
and [binding collection code](gc/mmtk/binding/src/collection.rs). Further allocation,
major-pacing, and compaction experiments are recorded in
[`gc/mmtk/SHAPE.md`](gc/mmtk/SHAPE.md); consult the implementation for current
defaults rather than copying settings from historical runs.

</details>

## Performance evidence

The quick benchmark suite lives on the separate
[`benchmarks` branch](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks).
Historical Apple M4 Pro runs from **2026-07-02** found that collector choice changed
both throughput and memory use: LXR performed well on allocation-heavy,
acyclic `binarytrees`, taking 0.76× vanilla's time with **309 MiB RSS**, versus
**92 MiB** for vanilla and **213 MiB** for GenImmix. Per-domain nursery scaling
substantially improved parallel `binarytrees`; continuation-heavy
`chameneos_redux` exposed promotion and scanning costs.

Those results predate the current nursery default, Bactrian's sliced marking and
incremental sweep, and later runtime fixes. Dynamic heaps and pinned LXR heaps
also produced different RSS values. They motivate the research questions;
they are not current-default or equal-memory performance claims.
LXR results are additionally provisional because of its open wrong-results bug;
its earlier `chameneos_redux` measurements cannot be treated as valid evidence.

The **2026-08-12** [SHAPE round 28](gc/mmtk/SHAPE.md#round-28-d5-pareto--the-honest-frontier-front-to-front-2026-08-12)
compared memory/time frontiers after sweeping heap sizing and nursery settings
for both collectors. Vanilla dominated Bactrian on `binarytrees`, Knuth–Bendix
(`kb`), LU, and `spectralnorm`: it offered better time/memory trade-offs than
those Bactrian configurations.
This stronger comparison also predates the September merge and later policy
changes; it is historical evidence, not a current-default result.

[`SCALABILITY.md`](SCALABILITY.md) preserves the scaling experiments and controls.
[`PERFORMANCE.md`](PERFORMANCE.md) describes the broader measurement methodology,
including heap sweeps and reporting wall time alongside memory; some setup
assumptions there are historical. Use the configuration above for current
defaults and record compiler revisions, heap/nursery settings, domains, GC workers,
and both time and RSS for a comparison.

## Source and further reading

| Path | Contents |
|---|---|
| [`gc/mmtk/`](gc/mmtk) | Rust binding workspace: OCaml value layout, scanning, object model, and C API. |
| [`gc/mmtk-core/`](gc/mmtk-core) | Pinned fork of MMTk 0.32, including research plans and OCaml-specific changes. |
| [`runtime/mmtk.c`](runtime/mmtk.c) | Runtime glue for allocation, barriers, and domain coordination. |
| [`Makefile.mmtk`](Makefile.mmtk) | Cargo build and runtime linking. |
| [`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) | Research agenda and literature. |
| [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) | Dated design decisions, investigations, and validation. |
| [`gc/mmtk/TESTSUITE_TRIAGE.md`](gc/mmtk/TESTSUITE_TRIAGE.md) | Evidence behind testsuite exclusions and timing differences. |
| [`gc/mmtk/BACTRIAN.md`](gc/mmtk/BACTRIAN.md), [`gc/mmtk/FAQ.md`](gc/mmtk/FAQ.md) | Design background and integration hazards; historical descriptions may lag current code. |
| [`README.upstream.adoc`](README.upstream.adoc) | Upstream OCaml overview. |

## License

OCaml retains its [license](LICENSE). MMTk is dual-licensed under
[MIT](https://github.com/fplaunchpad/mmtk-core/blob/045f121143c5772fd2715d8bfaf5274238ced4b1/LICENSE-MIT)
and [Apache 2.0](https://github.com/fplaunchpad/mmtk-core/blob/045f121143c5772fd2715d8bfaf5274238ced4b1/LICENSE-APACHE).
