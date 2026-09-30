# OCaml + MMTk

`ocaml-mmtk` is a fork of **OCaml 5.5.0** with
[MMTk](https://www.mmtk.io), the Memory Management Toolkit, as its only garbage
collector. It builds the bytecode and native compilers, bootstraps itself, and
supports multicore programs using `Domain.spawn` and effect handlers. Choose a
collector at startup with `MMTK_PLAN`; the default is **GenImmix**.

**Research prototype (status 2026-09-30).** Several correctness bugs, including
silent data corruption under the default plan, were found and fixed on
2026-09-29 and 2026-09-30; see [known limits](#what-works-and-what-is-still-open).
Among them, fixed on mainline on 2026-09-30: a rare collector abort when
domains are created and terminated rapidly
([GH issue 39](https://github.com/fplaunchpad/ocaml-mmtk/issues/39), PR 52;
the process-exit path that force-cancels peer domains still deregisters them
without the fix's guard) and a segfault instead of `Out_of_memory` when a
pinned heap is too small
([GH issue 49](https://github.com/fplaunchpad/ocaml-mmtk/issues/49), PR 51).
Still open, and worth knowing before you try it: a `fork`ed child cannot run a collection
([GH issue 33](https://github.com/fplaunchpad/ocaml-mmtk/issues/33)); and results
under the LXR plan are provisional.

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

Bytecode and native code run on **x86-64 Linux** and **Apple Silicon macOS**,
including moving and generational collectors, multi-domain collection, weak
references, ephemerons, and finalisers. Native allocation uses the existing
compiler fast path, backed by MMTk allocation regions. The stock OCaml collector
has been removed; compare against a separate vanilla OCaml 5.5.0 build.

The evidence is OCaml's own testsuite, bytecode and native variants, run per
plan. On **2026-09-29**, with all of that day's fixes merged, the full testsuite
passed with no failures (1446 tests) under GenImmix, Immix and Bactrian on macOS
arm64 with the default dynamic heap. On
[Linux CI](https://github.com/fplaunchpad/ocaml-mmtk/actions/runs/36583101914)
(4 GiB pinned heap) it passed under GenImmix, Immix, StickyImmix, SemiSpace and
ConcurrentImmix, and the debug-runtime jobs passed. In the same run, Bactrian
failed one compile with an intermittent out-of-memory
([GH issue 36](https://github.com/fplaunchpad/ocaml-mmtk/issues/36)), GenCopy
failed `misc/darkening_work.ml`, an intermittent collection-count test that
depends on pacing (fixed later that day, below), and LXR failed its known
set of tests (below). On the
[CI run](https://github.com/fplaunchpad/ocaml-mmtk/actions/runs/36668846658) of
the explicit-`Gc` fix (below), every gating plan passed, GenCopy and Bactrian
included, except LXR on that known set. The suite includes disabled cases for unsupported features
and GC timing differences. Memprof is unsupported;
[`runtime_events` GC-event emission](https://github.com/fplaunchpad/ocaml-mmtk/issues/20)
is unimplemented, and [signal-delivery poll points](https://github.com/fplaunchpad/ocaml-mmtk/issues/19)
differ from stock OCaml in two tests.

**Fixed on 2026-09-29 and 2026-09-30** (on mainline; each verified with a
reproducer and full testsuite runs on macOS arm64, then by Linux CI, which pins
the heap and so does not exercise the heap-growth fix):

- Native `Array.fill` and unmarshalling of data marshalled with `No_sharing`
  skipped the generational write barrier, silently corrupting data under
  GenImmix (the default since 2026-06-24), StickyImmix, GenCopy and Bactrian
  ([GH issue 28](https://github.com/fplaunchpad/ocaml-mmtk/issues/28)).
- Programs using `Thread` with blocking sections could hang in a collection at
  thread exit or corrupt the heap
  ([GH issue 24](https://github.com/fplaunchpad/ocaml-mmtk/issues/24)). The debug
  runtime now aborts on any violation of the invariant involved. Not covered:
  Windows, and `fork` (below).
- LXR computed wrong results when a field holding an integer or a static value
  was overwritten ([GH issue 26](https://github.com/fplaunchpad/ocaml-mmtk/issues/26)).
- The dynamic heap grew on garbage under plans whose collections are
  triggered by the heap filling (Immix, SemiSpace). The cause was an MMTk
  change of 2026-08-30 that reached mainline on 2026-09-29; RSS figures for
  those plans measured with the dynamic heap on trees containing it are
  inflated.
- Creating domains while a collection was running could delay signal delivery
  to some domains, and tripped an assertion in the debug runtime
  ([GH issue 37](https://github.com/fplaunchpad/ocaml-mmtk/issues/37)).
- `Gc.minor`, `Gc.major`, `Gc.full_major` and `Gc.compact` returned before
  their collection ran, and `Gc.minor` requested no collection from MMTk. They
  now return after a completed pause: a full-heap pause for the major calls on
  the stop-the-world plans; on ConcurrentImmix and LXR one pause, not a whole
  marking cycle or backup trace. Unlike stock OCaml, a major call is one
  collection, not three, and each call is a real stop-the-world pause (about
  0.5 ms).
- GenCopy's GC workers abandoned part of a copy block at every nursery
  collection, which inflated its major-collection pacing
  ([ROADMAP](ROADMAP.md) item 15).
- LXR kept memory it should have freed: its sweeps refused dead blocks,
  resumed continuations never released what their stacks referenced, and
  2-bit counts saturated. `chameneos_redux` now runs at a 64 MiB heap with no
  backup traces (it needed 256 MiB); the cost is 5–10% wall time under LXR.
  Verified with the effects tests and pinned-heap probes, not a full LXR
  testsuite run ([ROADMAP](ROADMAP.md) item 21).

**Known correctness limits (2026-09-30):**

- **Fixed 2026-09-30.** A GC worker occasionally aborted with `pointer being freed was not allocated`
  (or segfaulted) when domains were spawned and joined rapidly while another
  domain allocated
  ([GH issue 39](https://github.com/fplaunchpad/ocaml-mmtk/issues/39)): a few
  percent of runs of a stress program, on the release and debug runtimes,
  native and bytecode, macOS and Linux. Cause (found with `rr`): a terminating
  domain's buffer flush raced a collection's flush of the same buffers, so one
  buffer was freed twice. Fixed by
  [PR 52](https://github.com/fplaunchpad/ocaml-mmtk/pull/52) (merged,
  `0015fbf179`); GH issue 39 is closed. Residual: when the main domain
  force-cancels peer domains at process exit, their deregistration still
  flushes without the fix's guard.
  Two one-off Linux CI crashes in multi-domain tests (a `double free` under
  ConcurrentImmix, a segfault under GenCopy) are plausibly the same bug.
- **Fixed 2026-09-30.** At a pinned heap too small for the program, a program could segfault in the
  collector's root scan instead of raising `Out_of_memory`, under every plan
  that collects on the retry
  ([GH issue 49](https://github.com/fplaunchpad/ocaml-mmtk/issues/49)). Fixed by
  [PR 51](https://github.com/fplaunchpad/ocaml-mmtk/pull/51) (merged,
  `ce2dd86167`, with the test `gc-roots/oom_in_call_gc.ml`); GH issue 49 is closed.
- A `fork`ed child has none of the collector's worker threads, so a collection
  in the child cannot run
  ([GH issue 33](https://github.com/fplaunchpad/ocaml-mmtk/issues/33)). Explicit
  `Gc` calls in the child are no-ops; a child that allocates enough to need a
  collection spins at 100% CPU.
- LXR results are provisional. LXR still retains a whole block per surviving
  object (it neither reuses partly free blocks nor evacuates survivors), so a
  small live set can still exhaust a small heap, and it still fails a set of
  tests locally and on Linux CI ([ROADMAP](ROADMAP.md) item 21). LXR does not
  count or clear weak references, so `Weak.get` can return freed memory: do
  not use LXR for programs that use `Weak` or `Ephemeron`
  ([GH issue 44](https://github.com/fplaunchpad/ocaml-mmtk/issues/44)). Its
  backup trace also reaches some objects whose reference count is zero
  ([GH issue 45](https://github.com/fplaunchpad/ocaml-mmtk/issues/45)).
- Under Bactrian, the native compiler (and, once, a test program)
  intermittently fails with `Out of memory` on Linux CI at a 4 GiB heap
  ([GH issue 36](https://github.com/fplaunchpad/ocaml-mmtk/issues/36)); cause
  unknown, not reproduced on macOS.

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
| **LXR** | In-place reference counting on Immix, with stop-the-world backup tracing for cycles. **Provisional: block-granularity memory retention, unsound weak references, and known test failures. Requires a pinned heap.** |
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
the pattern that gave wrong results
([GH issue 26](https://github.com/fplaunchpad/ocaml-mmtk/issues/26), fixed on
2026-09-29). With that fix, LXR needed much more memory than earlier runs
suggested. The retention fixes merged on 2026-09-30 (sweeps no longer refuse
dead blocks, resumed continuations release their stack referents, 4-bit
reference counts by default) removed most of it, but each block holding a
survivor still stays whole, because this port neither reuses partly free
blocks nor evacuates survivors. Line reuse waits on weak-reference handling
([GH issue 44](https://github.com/fplaunchpad/ocaml-mmtk/issues/44)) and a
zero-count anomaly
([GH issue 45](https://github.com/fplaunchpad/ocaml-mmtk/issues/45)). This and
the remaining test failures are tracked in [`ROADMAP.md`](ROADMAP.md) (open
item 21). The
[release-counter abort](https://github.com/fplaunchpad/ocaml-mmtk/issues/25) is
also fixed. LXR participates in the
[cross-plan testsuite workflow](.github/workflows/testsuite-plans.yml), while
remaining an experimental research plan.

## Configure a run

Environment variables apply at process startup. Sizes ending in `_MB` are MiB;
`MMTK_NURSERY` uses raw bytes.

| Variable | Default | Meaning |
|---|---|---|
| `MMTK_PLAN` | `GenImmix` | Select the collector. |
| `MMTK_HEAP_SIZE_MB` | Unset | Pin a fixed heap. Otherwise the intended target is about 2.2× the live heap, estimated from collector accounting, with nursery headroom and a physical-RAM ceiling. A heap budget is not an RSS limit. |
| `MMTK_MIN_HEAP_MB` | `32` | Lower bound for the default dynamic heap target. |
| `MMTK_NURSERY` | `Bounded:2097152,16777216` | Generational nursery budget: 2–16 MiB per live domain by default. Explicit values are not scaled; for example, `Fixed:8388608`. Unit suffixes such as `2m` do not parse. |
| `MMTK_NURSERY_PER_DOMAIN` | Enabled | Set to `0` to disable domain scaling of the default nursery. When the heap cannot grow, the scaled minimum is constrained to preserve mature-heap space. |
| `MMTK_THREADS` | Logical CPU count | GC workers. Try `1` for single-domain experiments; record the worker count when comparing results. |
| `MMTK_VERBOSE` | Unset | Any value, including `0`, enables initialization details and an exit summary; unset it to disable. |
| `MMTK_PAUSE_LOG` | Unset | `1`: one `[mmtk-pause] n= kind= stw_us= ttsp_us= gc_us= domains= epoch=` line per stop-the-world pause on stderr and a `[mmtk-pause-summary]` line at exit (count, full count, total, mean, p50/p95/p99, max in µs). Any other value: a path; the pauses are written there as NDJSON at exit. Useful while GC-event emission through `runtime_events` is unimplemented. Off costs one flag test per pause. |
| `MMTK_TRANSPARENT_HUGEPAGES` | `true` on Linux; off elsewhere | Request transparent hugepages on Linux. Record this setting in memory comparisons. |
| `MMTK_RC_RETAIN` | Unset | LXR only. Any value prints one `[RC-RETAIN]` line per pause (held vs live memory, continuation-resume decrements, dead-cycle counts) and `[RC-SANITY]` lines for traced objects with a zero count. Diagnostic. |
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

Current policy lives in the [Bactrian implementation](https://github.com/fplaunchpad/mmtk-core/blob/44d02b65a980743eb0e91078bf0bf8f3b54d9214/src/plan/concurrent/bactrian/global.rs)
and [binding collection code](gc/mmtk/binding/src/collection.rs). Further allocation,
major-pacing, and compaction experiments are recorded in
[`gc/mmtk/SHAPE.md`](gc/mmtk/SHAPE.md); consult the implementation for current
defaults rather than copying settings from historical runs.

</details>

## Performance evidence

GC performance is a space-time curve, so every result pairs wall time with peak
RSS, and a collector wins only where its front (max RSS against wall, over the
heap and nursery knobs for MMTk and `space_overhead` × minor-heap size for
vanilla) lies below and to the left of vanilla's. The current sweep
(2026-09-30, Apple M4 Pro, after removing a macOS artefact in which mmtk-core
memset every fresh mapping and so made it resident; mmtk-core `5454281016`,
pinned by PR 53, merged) finds that no MMTk plan dominates vanilla OCaml 5.5.0 on
any GC-heavy bench today. On `binarytrees` the gap is collector speed:
GenImmix's front starts at 94 MiB / 1.64 s beside vanilla's default
(91 MiB / 1.51 s), but vanilla's curve falls faster, and at equal RSS vanilla
is 1.15–1.54× faster. On `kb` and LU what remains is a 9–16 MiB memory floor
and 3–22 % time. Where MMTk is faster, it is faster only at more memory:
`matrix_multiplication` (0.58 s vs 0.76 s at 27–31 vs 19 MiB) and Immix on
single-domain `chameneos_redux` (1.02 s vs 1.33 s at 74 vs 37 MiB), where the
generational plans take 2.7–3.3× Immix's time.

![Space-time fronts, vanilla vs MMTk plans](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4_nz/summary.png)

Every graph with its reading, the quick-panel tables (sequential and parallel),
configuration, raw data and history: [`RESULTS.md`](RESULTS.md). Measurement
method: [`PERFORMANCE.md`](PERFORMANCE.md); scaling experiments:
[`SCALABILITY.md`](SCALABILITY.md).

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
[MIT](https://github.com/fplaunchpad/mmtk-core/blob/44d02b65a980743eb0e91078bf0bf8f3b54d9214/LICENSE-MIT)
and [Apache 2.0](https://github.com/fplaunchpad/mmtk-core/blob/44d02b65a980743eb0e91078bf0bf8f3b54d9214/LICENSE-APACHE).
