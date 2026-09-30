# Performance results: MMTk plans vs vanilla OCaml 5.5.0

GC performance is a space-time curve, so every figure here pairs wall time with
peak RSS, and a plan is better than vanilla only where its front lies below and
to the left of vanilla's. A single dynamic-heap cell (the quick panel) is a
screening result; the fronts are the verdict. Method: [`PERFORMANCE.md`](PERFORMANCE.md).

**Verdict (2026-09-30, Apple M4 Pro).** No MMTk plan dominates vanilla on any
GC-heavy bench. On `binarytrees`, `kb` and `LU_decomposition` every plan is
dominated: its lowest RSS sits 60–90 MiB above vanilla's, and at that RSS
vanilla is at least as fast. Where MMTk is faster (`matrix_multiplication`,
and Immix on single-domain `chameneos_redux`), it is faster only at 2–5× the
memory, in a region vanilla's knobs never reach. The gap is a memory floor, not
collector speed. The generational plans (GenImmix, Bactrian) take 2.6–3.1× the
time of plain Immix on the effects-heavy `chameneos_redux`, and at too-small
pinned heaps they segfault instead of raising `Out_of_memory` (GH issue 49).

## Space-time fronts (2026-09-30, M4 Pro)

![Space-time fronts, five benches](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4/summary.png)

x = max RSS (MiB), y = wall (s); lower-left is better. Markers are all grid
points, the solid line is each configuration's lower-left front, the hollow
ring is its default configuration, and the dashed curve is a
`t = a + b/(H - c)` fit (a visual aid only). One run per point, 440 points.
Classes are from the sweep's `SUMMARY.md`: *dominated* = vanilla's front is at
least as good over the overlapping RSS range; *faster only at higher RSS* =
the fronts do not overlap and MMTk is faster but further right.

### binarytrees (depth 20)

![binarytrees](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4/spacetime_binarytrees.png)

Vanilla's front runs from 64 MiB / 2.39 s to 162 MiB / 0.89 s (default:
91 MiB / 1.64 s). The MMTk fronts start where vanilla's ends: GenImmix at
156 MiB / 1.79 s, reaching 1.26 s at 188 MiB and 1.04 s at 233 MiB; Bactrian at
159 MiB / 1.86 s; Immix at 132 MiB / 2.58 s. At ~160 MiB GenImmix takes 1.79 s
against vanilla's 0.89 s, about 2×. All three plans are dominated. Six GenImmix
points (32 and 48 MiB heaps) and eight Bactrian points (32 and 48 MiB, plus 64
and 96 MiB with a 32 MiB nursery) segfaulted (GH issue 49); the six failed Immix
points (32 and 48 MiB) raised `Out_of_memory` cleanly.

**Takeaway: the allocation-heavy flagship loses by a full memory floor; the
panel's "0.78×" is a point at 238 MiB that vanilla beats at 116 MiB.**

### kb (Knuth–Bendix)

![kb](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4/spacetime_kb.png)

Vanilla's front lives at 7–39 MiB (0.61 → 0.38 s; default 8 MiB / 0.45 s).
The MMTk fronts start at 82–86 MiB, and their nearest points are 1.02–1.14×
vanilla's default time (Immix 86 MiB / 0.47 s, Bactrian 82 / 0.51, GenImmix
82 / 0.52). Every plan is dominated.

**Takeaway: time is near parity; the loss is memory alone, a 10× larger
footprint for a small-live-set program.**

### matrix_multiplication (768)

![matrix_multiplication](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4/spacetime_matrix_multiplication.png)

All 16 vanilla points sit at 19 MiB / 0.744 s: `o` and `s` do not move a
fixed-live-set compute bench. Every MMTk plan runs at 0.59–0.67 s, but its
lowest RSS is 41 MiB (GenImmix, with a 32 MiB nursery and a heap of 96 MiB or
more; its other points are 112–134 MiB), 77 MiB (Immix) and 97 MiB (Bactrian).
Class: faster only at higher RSS, 0.79× at the nearest point.

**Takeaway: the grids do not bracket each other here, so this is a floor
comparison, not a front comparison: MMTk is ~20% faster at 2–5× the RSS.**
Why the GenImmix floor drops from ~120 to 41 MiB with a 32 MiB nursery is open.

### LU_decomposition (900)

![LU_decomposition](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4/spacetime_LU_decomposition.png)

Vanilla: 17 MiB / 0.79 s (default 17 MiB / 0.80 s). GenImmix and Bactrian
start at 86 MiB and are 1.07–1.08× vanilla's time there, flattening to
0.80–0.81 s at 106–114 MiB; Immix sits at 98 MiB / 1.05 s (1.32×) and does not
improve with more memory. All dominated.

**Takeaway: same shape as `kb` (near-parity time, 5× the memory); Immix's
flat 1.3× is a separate, non-GC cost.**

### chameneos_redux (500000, 1 domain)

![chameneos_redux](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4/spacetime_chameneos_redux.png)

Vanilla: 37–46 MiB, 1.38 → 1.14 s (default 37 MiB / 1.37 s). Immix is flat at
1.03–1.05 s from 117 MiB up (default 118 MiB / 1.11 s): faster only at higher
RSS, 0.77× at the nearest point. GenImmix (118–174 MiB, 3.1 → 2.7 s; default
2.86 s) and Bactrian (120–140 MiB, 3.4 → 3.2 s; default 3.25 s) are dominated.
At their defaults, all within 118–127 MiB, the generational plans take 2.6×
(GenImmix, 2.86 s) and 2.9× (Bactrian, 3.25 s) Immix's time (1.11 s).

**Takeaway: on continuation- and effects-heavy code, generationality itself is
the cost; plain Immix beats vanilla by 23% at 3× the memory.**

## Quick panel (screening)

One point per plan, at the dynamic heap: where each default policy lands, not
which collector is better. Median wall ms (ratio to vanilla) / max RSS MiB.

![Sequential time ratio](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_m4/seq_ratio.png)
![Sequential max RSS](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_m4/seq_rss.png)

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

Reading: the compute-bound controls (`nbody`, `fannkuchredux`, `mandelbrot`)
are flat in time (0.99–1.01×) and their RSS is each plan's startup floor (26 MiB
GenImmix/Bactrian, 41–42 MiB Immix, 2 MiB vanilla). Every GC-heavy cell is at a
different RSS from vanilla's; the fronts above place each one, and none is a win
on both coordinates.

![Parallel speedup](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_m4/speedup_domains.png)

Median wall ms / max RSS MiB per domain count; `--threads domains` (N GC
workers for N domains).

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

Reading: `chameneos_redux` anti-scales on every MMTk plan (GenImmix 3016 ms at
1 domain to 13372 ms at 8, while vanilla goes 1421 → 419 ms; 369 vs 295 MiB at
8 domains). The other benches gain speed with domains but less than vanilla:
3.2–3.4× vs 5.2× on `par_spectralnorm`, 5.0–5.2× vs 6.1× on `par_matmul`, and
GenImmix 2.5× vs 4.35× on `par_binarytrees` (548 ms / 623 MiB vs 378 ms /
561 MiB at 8 domains); Immix anti-scales on `par_binarytrees`.

## What the curves say

1. **RSS ≫ heap, and the floor loses every front.** A 64 MiB pinned GenImmix
   heap gives 156–181 MiB RSS on `binarytrees`; a 32 MiB heap gives 82 MiB on
   `kb`. MMTk's lowest RSS is 60–90 MiB above vanilla's on `binarytrees`, `kb`,
   LU and `chameneos_redux`, and that offset, not collection speed, is what places each MMTk front to the
   right of vanilla's. *Open:* decompose the floor. Candidates to measure: side
   metadata, the nursery accounted outside the pinned heap, chunk-granularity
   mapping, the ~26 MiB startup floor, Immix block fragmentation. The
   `matrix_multiplication` GenImmix drop from ~120 to 41 MiB with a 32 MiB
   nursery says the floor is not static.
2. **Generational plans lose 2.6–3.1× to Immix on effects-heavy code**
   (`chameneos_redux`, 1 domain, in both the sweep and the panel), and every
   plan anti-scales with domains there. *Open:* is it continuation stacks
   (fiber scanning or remembered-set traffic on stack writes) or the promotion
   path? Needs a profile, not a guess.
3. **The generational plans segfault instead of raising `Out_of_memory` at
   too-small heaps** (GH issue 49; Immix fails cleanly at the 32 and 48 MiB heaps).
   The same signature was first recorded in August, near the true OOM point.
   *Open:* why the GC worker's root scan (`caml_scan_stack`) faults instead
   of the plan reporting exhaustion; a correctness bug, independent of the
   performance question.
4. **Compute-bound controls are flat** (0.99–1.01× time in the panel), so the
   MMTk mutator path costs nothing measurable on allocation-light code.
   *Open:* none for time; their RSS is still the startup floor.
5. **Host caveats bound all of the above.** macOS accounts RSS differently from
   Linux (GenImmix `kb`: 91 MiB on the M4, 29 MiB on godel); one run per sweep
   point and no dispersion; cores were not pinned, so the M4's efficiency cores
   can take a run. *Open:* repeat the sweep on godel with reps before quoting
   any front crossing.

## Configuration and raw data

- Host: Apple M4 Pro (8 performance + 4 efficiency cores, 24 GiB), macOS 26.6,
  kept quiet during the runs. No core pinning, no `setarch`.
- Fork: `b714bb86d3`, tree identical to mainline merge `3fbe544984` (PR 41);
  mmtk-core `95b425a27d`. Vanilla: opam `5.5.0` (released, non-flambda).
- Plans: GenImmix (default), Bactrian, Immix. LXR is excluded: it needs a
  pinned heap and its results are provisional.
- **Sweep** (`quick/spacetime.py`): benches `binarytrees`, `kb`,
  `matrix_multiplication`, `LU_decomposition`, `chameneos_redux` (1 domain) at
  perf sizes; vanilla `OCAMLRUNPARAM` `o` ∈ {40, 80, 120, 200, 320} × `s` ∈
  {256k, 1M, 4M} plus default; each MMTk plan `MMTK_HEAP_SIZE_MB` ∈ {32, 48, 64,
  96, 128, 192, 256} plus dynamic × `MMTK_NURSERY` ∈ {default,
  `Fixed:4194304`, `Fixed:33554432`}; `MMTK_THREADS=1`; one run per point; RSS
  = the child's `ru_maxrss`. Command:
  `uv run quick/spacetime.py --vanilla quick/build_m4_vanilla --mmtk quick/build_m4_mmtk --plans "GenImmix Bactrian Immix" --benches "binarytrees kb matrix_multiplication LU_decomposition chameneos_redux" --json quick/spacetime-m4.ndjson --graphs quick/graphs_spacetime_m4 --no-setarch --no-pin --resume`.
- **Panel** (`quick/quickbench.py`): dynamic heap, `--threads domains` (1 GC
  worker per sequential cell, N for N domains), perf sizes, 3 measured runs
  (median) after 1 warmup, RSS = peak across runs. Command (in `quick/`):
  `uv run quickbench.py all --vanilla build_m4_vanilla --bin-a build_m4_mmtk --plans "GenImmix Bactrian Immix" --heap dynamic --gc --chart --no-pin --no-setarch --json m4-2026-09-30-dynamic.ndjson --graphs graphs_m4`.
- Raw data, all on the [`benchmarks` branch](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks):
  sweep [`spacetime-m4.ndjson`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/spacetime-m4.ndjson),
  [`spacetime-m4.log`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/spacetime-m4.log),
  [`graphs_spacetime_m4/`](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks/quick/graphs_spacetime_m4)
  (with `SUMMARY.md`, the per-plan class table and the failed points);
  panel [`m4-2026-09-30-dynamic.ndjson`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/m4-2026-09-30-dynamic.ndjson),
  [`m4-2026-09-30.log`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/m4-2026-09-30.log),
  [`graphs_m4/`](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks/quick/graphs_m4);
  drivers [`spacetime.py`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/spacetime.py)
  and [`quickbench.py`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/quickbench.py),
  described in [`quick/README.md`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/README.md).

## History

- **2026-07-02, M4 Pro quick panel (superseded).** Measured before the 16 MiB
  nursery default, Bactrian's sliced marking and incremental sweep, and later
  runtime fixes; in `README.md` up to `358ea7958c`, "Performance (quick panel)".
  It is the only panel with LXR figures (`binarytrees` 0.76× vanilla's time at
  309 MiB), all of which predate the 2026-09-29 wrong-results fix and must be
  re-measured; its LXR `chameneos_redux` figures are invalid. The 2026-08-12
  [SHAPE round 28](gc/mmtk/SHAPE.md#round-28-d5-pareto--the-honest-frontier-front-to-front-2026-08-12)
  front comparison (vanilla dominated Bactrian on `binarytrees`, `kb`, LU and
  `spectralnorm`) was the first front-to-front result.
- **2026-09-30, godel cross-check.** The quick panel on the Linux host godel
  (Xeon Gold 5120, `powersave` governor), with the older fork `6865b559ed`
  (before PR 41): [`quick/RESULTS-godel-2026-09-30.md`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/RESULTS-godel-2026-09-30.md).
  Time ratios do not transfer across hosts (GenImmix `binarytrees` 1.66× on
  godel vs 0.78× on the M4); vanilla's RSS agrees where the live set dominates
  (`binarytrees` 92 MiB on both), MMTk's does not (`kb` 29 vs 91 MiB).
  Compare each host only with its own vanilla.
- **2026-09-30, M4 Pro panel and first space-time sweep (this page).**
