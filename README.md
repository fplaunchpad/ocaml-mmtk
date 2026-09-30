# OCaml + MMTk

`ocaml-mmtk` is a fork of **OCaml 5.5.0** with
[MMTk](https://www.mmtk.io), the Memory Management Toolkit, as its only garbage
collector. It builds the bytecode and native compilers, bootstraps itself, and
supports multicore programs using `Domain.spawn` and effect handlers. Choose a
collector at startup with `MMTK_PLAN`; the default is **GenImmix**.

**Research prototype (status 2026-09-30).** Several correctness bugs, including
silent data corruption under the default plan, were found and fixed on
2026-09-29 and 2026-09-30; see [known limits](#what-works-and-what-is-still-open).
Still open, and worth knowing before you try it: a rare collector abort when
domains are created and terminated rapidly
([GH issue 39](https://github.com/fplaunchpad/ocaml-mmtk/issues/39)); a `fork`ed
child cannot run a collection
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

**Known correctness limits (2026-09-30):**

- A GC worker occasionally aborts with `pointer being freed was not allocated`
  when domains are spawned and joined rapidly while another domain allocates
  ([GH issue 39](https://github.com/fplaunchpad/ocaml-mmtk/issues/39)): about 1–3%
  of runs of a stress program (default plan, debug runtime, bytecode, macOS).
  Whether it affects the release runtime, native code or Linux is unknown. It
  is a memory-safety bug in the collector; its cause is unknown. Linux CI also
  showed two one-off crashes in multi-domain tests on 2026-09-29 (a
  `double free` abort under ConcurrentImmix, a segfault under GenCopy); whether
  they are the same bug is unknown.
- A `fork`ed child has none of the collector's worker threads, so a collection
  in the child cannot run
  ([GH issue 33](https://github.com/fplaunchpad/ocaml-mmtk/issues/33)). Explicit
  `Gc` calls in the child are no-ops; a child that allocates enough to need a
  collection spins at 100% CPU.
- LXR results are provisional. With the wrong-results fix, LXR retains a
  whole block per survivor, so it needs much more memory than it appeared
  to (`chameneos_redux` needs a 256 MiB heap where it previously
  seemed to run in 64 MiB), and it still fails a set of tests locally and on
  Linux CI. The causes are diagnosed, with fixes not yet merged
  ([ROADMAP](ROADMAP.md) item 21). LXR does not clear weak references, so
  `Weak.get` can return freed memory (item 31).
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
| **LXR** | In-place reference counting on Immix, with stop-the-world backup tracing for cycles. **Provisional: high memory use and known test failures. Requires a pinned heap.** |
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
2026-09-29). With that fix, LXR needs much more memory than earlier runs
suggested: by source reading, each block holding a survivor stays whole, because
this port neither reuses partly free blocks nor evacuates survivors. This and
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

Current policy lives in the [Bactrian implementation](https://github.com/fplaunchpad/mmtk-core/blob/44d02b65a980743eb0e91078bf0bf8f3b54d9214/src/plan/concurrent/bactrian/global.rs)
and [binding collection code](gc/mmtk/binding/src/collection.rs). Further allocation,
major-pacing, and compaction experiments are recorded in
[`gc/mmtk/SHAPE.md`](gc/mmtk/SHAPE.md); consult the implementation for current
defaults rather than copying settings from historical runs.

</details>

## Performance evidence

GC performance is a space-time curve, so every figure below pairs wall time with
peak RSS. A single dynamic-heap cell is a screening result: it shows where each
collector's default policy lands, not which collector is better. The verdict
comes from a front-to-front sweep (heap and nursery for MMTk, `space_overhead`
and minor-heap size for vanilla, plotting max RSS against wall). That sweep is
pending; its curves will accompany this table and replace it as the reference result.

**Current panel: Apple M4 Pro, 2026-09-30.** This is the representative host. The quick
suite lives on the separate
[`benchmarks` branch](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks).
Raw records:
[`quick/m4-2026-09-30-dynamic.ndjson`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/m4-2026-09-30-dynamic.ndjson);
full log: [`quick/m4-2026-09-30.log`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/m4-2026-09-30.log);
charts: [`quick/graphs_m4/`](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks/quick/graphs_m4).
Configuration:

- host: Apple M4 Pro (8 performance and 4 efficiency cores, 24 GiB), macOS 26.6, kept quiet during the run;
- fork: `b714bb86d3`, whose tree is identical to mainline merge `3fbe544984`
  (PR 41), with mmtk-core `95b425a27d`;
- vanilla: opam `5.5.0` (released, non-flambda);
- plans: GenImmix (the default), Bactrian and Immix, each with the dynamic
  (space-overhead) heap;
- one GC worker per sequential cell and N workers for an N-domain cell
  (`--threads domains`);
- no core pinning and no `setarch`;
- the panel's perf sizes, 3 measured runs (median) after 1 warmup, with RSS as the peak across runs.

LXR is excluded because it needs a pinned heap and its results are provisional.

Each cell shows median wall time in ms, the ratio to vanilla in parentheses, and max RSS in MiB. Lower is better.

| bench | vanilla | GenImmix | Bactrian | Immix |
|---|--:|--:|--:|--:|
| `binarytrees` | 1653 / 92 | 1289 (0.78×) / 238 | 1390 (0.84×) / 249 | 2756 (1.67×) / 197 |
| `nbody` | 674 / 2 | 673 (1.00×) / 26 | 674 (1.00×) / 26 | 682 (1.01×) / 41 |
| `fannkuchredux` | 1550 / 2 | 1557 (1.00×) / 26 | 1559 (1.01×) / 26 | 1557 (1.00×) / 42 |
| `spectralnorm` | 671 / 5 | 668 (1.00×) / 74 | 672 (1.00×) / 74 | 794 (1.18×) / 94 |
| `mandelbrot` | 768 / 2 | 756 (0.99×) / 26 | 756 (0.99×) / 26 | 758 (0.99×) / 41 |
| `matrix_multiplication` | 751 / 19 | 654 (0.87×) / 112 | 611 (0.81×) / 98 | 618 (0.82×) / 78 |
| `LU_decomposition` | 803 / 17 | 825 (1.03×) / 91 | 825 (1.03×) / 91 | 1050 (1.31×) / 98 |
| `kb` | 426 / 8 | 490 (1.15×) / 91 | 477 (1.12×) / 87 | 464 (1.09×) / 103 |

The compute-bound controls, `nbody`, `fannkuchredux` and `mandelbrot`, stay within
0.99–1.01× of vanilla's time on every plan. Their RSS is each plan's startup floor:
26 MiB for GenImmix and Bactrian, 41–42 MiB for Immix, and 2 MiB for vanilla.

No plan wins on both coordinates for a GC-heavy bench. On `binarytrees`, GenImmix
takes 0.78× vanilla's time and uses 2.6× its memory. On `matrix_multiplication`,
it takes 0.87× the time and uses 5.9× the memory. On `kb`, it takes 1.15× the time
and uses 91 MiB against vanilla's 8 MiB.

<details>
<summary><b>Parallel domain sweep</b>: median wall (ms) / max RSS (MiB) for 1, 2, 4 and 8 domains</summary>

| bench | plan | d=1 | d=2 | d=4 | d=8 | T(1)/T(8) |
|---|---|--:|--:|--:|--:|--:|
| `par_spectralnorm` | vanilla | 1302 / 5 | 711 / 8 | 357 / 12 | 251 / 21 | 5.19× |
| `par_spectralnorm` | GenImmix | 1279 / 74 | 658 / 75 | 419 / 82 | 379 / 92 | 3.37× |
| `par_spectralnorm` | Bactrian | 1287 / 74 | 667 / 76 | 418 / 82 | 382 / 92 | 3.37× |
| `par_spectralnorm` | Immix | 1492 / 94 | 788 / 95 | 433 / 97 | 467 / 99 | 3.19× |
| `par_matmul` | vanilla | 770 / 19 | 456 / 19 | 265 / 19 | 127 / 20 | 6.08× |
| `par_matmul` | GenImmix | 709 / 112 | 488 / 135 | 308 / 106 | 137 / 111 | 5.17× |
| `par_matmul` | Bactrian | 651 / 99 | 365 / 99 | 209 / 99 | 130 / 104 | 5.01× |
| `par_matmul` | Immix | 641 / 78 | 362 / 111 | 194 / 99 | 129 / 100 | 4.97× |
| `par_binarytrees` | vanilla | 1644 / 92 | 819 / 149 | 570 / 293 | 378 / 561 | 4.35× |
| `par_binarytrees` | GenImmix | 1359 / 241 | 896 / 364 | 576 / 384 | 548 / 623 | 2.48× |
| `par_binarytrees` | Bactrian | 1459 / 247 | 958 / 483 | 641 / 490 | 515 / 609 | 2.83× |
| `par_binarytrees` | Immix | 2766 / 192 | 2984 / 249 | 3989 / 371 | 7537 / 398 | 0.37× |
| `chameneos_redux` | vanilla | 1421 / 37 | 697 / 74 | 542 / 146 | 419 / 295 | 3.39× |
| `chameneos_redux` | GenImmix | 3016 / 131 | 6688 / 161 | 9153 / 228 | 13372 / 369 | 0.23× |
| `chameneos_redux` | Bactrian | 3313 / 128 | 7305 / 160 | 11204 / 231 | 15694 / 370 | 0.21× |
| `chameneos_redux` | Immix | 1067 / 118 | 2070 / 149 | 2753 / 211 | 5423 / 335 | 0.20× |

</details>

**`chameneos_redux` anti-scales on every MMTk plan.** It is continuation-heavy and uses effect handlers.

| Domains | GenImmix | Vanilla |
|--:|---|---|
| 1 | 3016 ms, 131 MiB | 1421 ms, 37 MiB |
| 8 | 13372 ms, 369 MiB | 419 ms, 295 MiB |

At 1 domain, Immix is the fastest configuration: 1067 ms and 118 MiB. It also slows down as domains are added, reaching 5423 ms at 8 domains.

On the other parallel benches, all three MMTk plans still gain speed as domains are added, but less than vanilla does. Their speedup at 8 domains is 3.2–3.4× on `par_spectralnorm`, compared with vanilla's 5.2×. On `par_matmul` it is 5.0–5.2×, compared with 6.1×. On `par_binarytrees`, GenImmix reaches 2.5× and ends at 548 ms and 623 MiB. Vanilla ends at 378 ms and 561 MiB. Immix anti-scales on `par_binarytrees`.

**Caveats for this panel:**

- Each figure comes from one panel run on one host, and no dispersion is reported.
- The runs were not pinned to cores. Macs mix performance and efficiency cores, so 8-domain cells can
  land on efficiency cores.
- The RSS floors differ from Linux. The same program's peak RSS is accounted differently on macOS.

**Cross-check against godel.** The same panel was run on the Linux cross-check host
godel. That run is written up in
[`quick/RESULTS-godel-2026-09-30.md`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/RESULTS-godel-2026-09-30.md)
on the `benchmarks` branch. It used an older fork, from before PR 41, on a
Xeon running the `powersave` governor.

Time ratios do not carry across hosts:

| Bench (GenImmix) | godel | M4 |
|---|--:|--:|
| `binarytrees` | 1.66× | 0.78× |
| `kb` | 1.47× | 1.15× |
| `matrix_multiplication` | 1.13× | 0.87× |

Vanilla's peak RSS is the same on both hosts where the live set dominates. For `binarytrees`,
it is 92 MiB on each. MMTk's RSS is higher on the M4: GenImmix `binarytrees` uses 238 MiB there
against 187 MiB on godel, and `kb` uses 91 MiB against 29 MiB. Compare each host only against
its own vanilla. godel reported GC-heavy results worse than the July M4 panel, and a 404 MiB
Bactrian `binarytrees`. Neither appears on the M4.

**Superseded results.** The Apple M4 Pro quick panel of **2026-07-02** is now historical.
It was measured before the current nursery default, Bactrian's sliced marking
and incremental sweep, and later runtime fixes. It is still the only panel with LXR figures:
LXR took 0.76× vanilla's time on `binarytrees` with 309 MiB RSS. Every LXR
time and RSS figure predates the 2026-09-29 wrong-results fix and must be
re-measured. Its `chameneos_redux` measurements are invalid, because that run silently
dropped most reference-count increments.
The **2026-08-12** [SHAPE round 28](gc/mmtk/SHAPE.md#round-28-d5-pareto--the-honest-frontier-front-to-front-2026-08-12)
compared fronts after sweeping heap sizing and nursery settings: vanilla dominated
Bactrian on `binarytrees`, Knuth–Bendix (`kb`), LU and `spectralnorm`. It shows how the pending
sweep works, but the plans have changed since.

[`SCALABILITY.md`](SCALABILITY.md) preserves the scaling experiments and controls.
[`PERFORMANCE.md`](PERFORMANCE.md) describes the measurement methodology. Some setup
assumptions there are historical. For a comparison, record:

- compiler revisions;
- heap and nursery settings;
- domains and GC workers;
- both time and RSS.

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
