# godel re-baseline, 2026-09-30

The quick panel re-run on **godel**, a cross-check host, against released
vanilla OCaml 5.5.0. Raw records: [`godel-2026-09-30-dynamic.ndjson`](godel-2026-09-30-dynamic.ndjson)
(one record per cell, fields as described in the README's "Results JSON +
graphs"). These are *verified* on that host; every observation below carries
the host caveats at the end.

## Configuration

| | |
|---|---|
| host | godel: 56-core Intel Xeon Gold 5120 @ 2.2 GHz, 2 NUMA nodes, 62 GB; `powersave` governor (no sudo to change it) |
| fork | `6865b559ed` (mmtk-core `44d02b65a9`), i.e. before ocaml-mmtk PR 41 (synchronous explicit `Gc` requests, GenCopy copy-buffer fix) |
| vanilla | opam switch 5.5.0 (released, non-flambda) |
| plans | GenImmix (default), Bactrian, Immix. LXR excluded: it needs a pinned heap, and its results are provisional (ROADMAP item 21) |
| heap | dynamic (no `MMTK_HEAP_SIZE_MB`) |
| pinning | benches pinned to 14 physical cores of NUMA node 0 |
| reps | 3 measured (median) + 1 warmup per cell |
| sizes | the panel's perf sizes (the harness default; the records do not store sizes) |
| GC workers | `--threads domains`, as recorded in every record: by the harness source, 1 worker per sequential cell and workers = domains in the parallel sweep |

On the GC workers: an earlier description of this run gave the MMTk default
(nproc = 56) for the sequential cells. The records say `domains`, which the
harness maps to 1 worker there; which of the two actually ran is not
established. A separate worker-count check (`MMTK_THREADS` 4, 14 and 56 on
`binarytrees` and `kb` under GenImmix) showed no improvement, so worker
provisioning does not explain the sequential ratios either way.

## Sequential

Wall ratio vs vanilla / max RSS (MiB); the vanilla column is median wall (ms) /
max RSS (MiB).

| bench | vanilla (ms / MiB) | GenImmix | Bactrian | Immix |
|---|--:|--:|--:|--:|
| `binarytrees` | 10317 / 92 | 1.66x / 187 | 1.50x / 404 | 2.87x / 136 |
| `nbody` | 3134 / 15 | 1.01x / 15 | 1.03x / 15 | 0.99x / 15 |
| `fannkuchredux` | 5473 / 15 | 1.00x / 15 | 1.01x / 15 | 1.00x / 15 |
| `spectralnorm` | 3696 / 15 | 1.07x / 21 | 1.06x / 21 | 1.42x / 43 |
| `mandelbrot` | 2751 / 15 | 0.97x / 15 | 0.96x / 15 | 0.97x / 15 |
| `matrix_multiplication` | 2313 / 17 | 1.13x / 48 | 1.10x / 69 | 1.01x / 32 |
| `LU_decomposition` | 5789 / 17 | 1.13x / 34 | 1.13x / 34 | 1.63x / 49 |
| `kb` | 2972 / 15 | 1.47x / 29 | 1.46x / 25 | 1.28x / 58 |

## Parallel

Median wall (ms) / max RSS (MiB) per domain count, and speedup T(1)/T(8).

| bench | plan | d=1 | d=2 | d=4 | d=8 | T(1)/T(8) |
|---|---|--:|--:|--:|--:|--:|
| `par_spectralnorm` | vanilla | 6381 / 15 | 5006 / 15 | 3739 / 15 | 2670 / 19 | 2.39x |
| `par_spectralnorm` | GenImmix | 7206 / 22 | 8630 / 24 | 4823 / 26 | 2960 / 35 | 2.43x |
| `par_spectralnorm` | Bactrian | 7220 / 20 | 8845 / 24 | 4759 / 26 | 2936 / 36 | 2.46x |
| `par_spectralnorm` | Immix | 9842 / 43 | 6837 / 44 | 5482 / 44 | 3640 / 46 | 2.70x |
| `par_matmul` | vanilla | 2511 / 17 | 1397 / 17 | 847 / 17 | 529 / 17 | 4.74x |
| `par_matmul` | GenImmix | 2603 / 48 | 1609 / 72 | 969 / 54 | 807 / 49 | 3.23x |
| `par_matmul` | Bactrian | 2572 / 68 | 1910 / 50 | 1223 / 39 | 938 / 48 | 2.74x |
| `par_matmul` | Immix | 2467 / 32 | 1442 / 67 | 1012 / 62 | 763 / 53 | 3.23x |
| `par_binarytrees` | vanilla | 10323 / 92 | 9195 / 148 | 7105 / 271 | 4345 / 414 | 2.38x |
| `par_binarytrees` | GenImmix | 16099 / 170 | 9936 / 204 | 7485 / 277 | 5358 / 524 | 3.00x |
| `par_binarytrees` | Bactrian | 16142 / 389 | 9557 / 250 | 7592 / 433 | 5415 / 525 | 2.98x |
| `par_binarytrees` | Immix | 30693 / 137 | 24143 / 187 | 18714 / 258 | 19096 / 333 | 1.61x |
| `chameneos_redux` | vanilla | 12444 / 22 | 9857 / 38 | 8708 / 69 | 4338 / 145 | 2.87x |
| `chameneos_redux` | GenImmix | 21792 / 48 | 26001 / 61 | 24932 / 84 | 17228 / 151 | 1.26x |
| `chameneos_redux` | Bactrian | 29924 / 49 | 34774 / 58 | 31585 / 90 | 27045 / 166 | 1.11x |
| `chameneos_redux` | Immix | 5439 / 57 | 7267 / 70 | 8625 / 95 | 9337 / 137 | 0.58x |

## Observations (all host-caveated)

- **Compute-bound controls are flat**: `nbody`, `fannkuchredux`, `mandelbrot`
  within 0.96-1.03x of vanilla on every plan.
- **GC-heavy benches are worse relative to vanilla than in the July M4 panel**
  (the fork's README at `358ea7958c`, "Performance (quick panel)"):
  `binarytrees` GenImmix 1.40x -> 1.66x, `kb` 1.20x -> 1.47x,
  `matrix_multiplication` 0.87x -> 1.13x. The 16 MiB nursery default (since
  2026-08-12) is the suspect; unverified.
- **Bactrian `binarytrees` RSS is 404 MiB**: 4.4x vanilla and 2x GenImmix
  (257 MiB in July). Unexplained; candidates are the compaction law, sliced
  marking and the new pacing.
- **`chameneos_redux` is the worst result**: GenImmix is 1.75x vanilla at 1
  domain and 4x at 8. Immix is fastest at 1 domain (0.44x vanilla) but
  anti-scales (T(1)/T(8) = 0.58x).
- Parallel speedups: `par_spectralnorm` scales the same on every plan
  (2.4-2.7x); `par_matmul` scales worse on the MMTk plans (2.7-3.2x vs 4.7x);
  `par_binarytrees` scales better under GenImmix and Bactrian than vanilla
  (3.0x vs 2.4x) from a slower 1-domain start, and ends 1.2x slower at 8
  domains; Immix scales poorly (1.6x).

## Caveats

- **Cross-check host only.** The Apple M4 Pro is the representative bench
  host; the like-for-like run there is pending. Absolute times here are 3-13x
  slower than the July M4 numbers.
- The `powersave` governor could not be changed.
- **Unexplained anomaly:** a manual rerun of `kb` under GenImmix took 10.3 s
  vs the panel's 4.4 s with the same binary and settings (host variance, not
  explained).
- The fork predates PR 41: explicit `Gc` requests were still asynchronous and
  domain termination did not wait for its collection. None of the panel's
  benches call `Gc` explicitly, but the `par_*` benches and `chameneos_redux`
  terminate domains.
- **Method note:** a first attempt's numbers were discarded because a runaway
  process on the host was swapping. Check the host is idle before trusting a
  run.
