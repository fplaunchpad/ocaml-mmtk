# Performance results: MMTk plans vs vanilla OCaml 5.5.0

GC performance is a space-time curve, so every figure here pairs wall time with
peak RSS, and a plan is better than vanilla only where its front lies below and
to the left of vanilla's. A single dynamic-heap cell (the quick panel) is a
screening result; the fronts are the verdict. Method: [`PERFORMANCE.md`](PERFORMANCE.md).

**Verdict (2026-09-30, Apple M4 Pro, after the macOS memset fix).** No MMTk
plan dominates vanilla on any GC-heavy bench. Removing the macOS artefact
(mmtk-core `5454281016`, pinned by ocaml-mmtk PR 53, merged) moved every
MMTk front 40–66 MiB to the left, so the fronts now meet vanilla's and the
comparison is front against front. On `binarytrees` the gap is collector speed,
not memory: GenImmix's front starts at 94 MiB / 1.64 s, beside vanilla's
default (91 MiB / 1.51 s), but vanilla's curve falls faster (0.85 s at
116 MiB, where GenImmix needs 136 MiB for 1.31 s and 176 MiB for 0.98 s), so
at equal RSS vanilla is 1.15–1.54× faster. On `kb` and `LU_decomposition` the
gap is a residual memory floor of 9–16 MiB (GenImmix, Bactrian; Immix 30–37)
plus 3–22 % time. Where MMTk is faster (`matrix_multiplication`, 0.76×
vanilla's default time at 27–31 vs 19 MiB; Immix on single-domain
`chameneos_redux`, 0.77× at 74 vs 37 MiB), it is faster only at more memory.
The generational plans (GenImmix, Bactrian) take 2.25–2.51× vanilla's time on
the effects-heavy `chameneos_redux` (2.7–3.3× Immix's), and at too-small pinned
heaps they segfaulted instead of raising `Out_of_memory` in both sweeps (GH
issue 49; fixed since by PR 51, merged as `ce2dd86167`, which neither sweep's
build contains).

## Space-time fronts (2026-09-30, M4 Pro, after the memset fix)

![Space-time fronts, five benches](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4_nz/summary.png)

x = max RSS (MiB), y = wall (s); lower-left is better. Markers are all grid
points, the solid line is each configuration's lower-left front, the hollow
ring is its default configuration, and the dashed curve is a
`t = a + b/(H - c)` fit (a descriptive overlay, not a validated model). One
run per point, 440 points, one GC worker, no dispersion: these are observed
fronts over this grid. Classes are from the sweep's `SUMMARY.md`:
*dominated* = vanilla's front is at least as good over the common memory
budgets (each front's best point extended rightwards); *faster only at higher
RSS* = MMTk is faster but its front lies further right. The driver has no
symmetric "smaller but slower" class; no point of this dataset needs it. Each
section ends with the same bench in the first sweep, whose MMTk RSS carried the
macOS memset artefact (see "The macOS memset artefact" below). Vanilla's
binaries were the same in both sweeps; its default point moved by up to 12 %
between them (`kb` 0.45 → 0.40 s, `binarytrees` 1.64 → 1.51 s), which bounds
single-run noise.

### binarytrees (depth 20)

![binarytrees](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4_nz/spacetime_binarytrees.png)

Vanilla's front runs from 64 MiB / 2.17 s to 116 MiB / 0.85 s (default
91 MiB / 1.51 s). GenImmix's front starts at 94 MiB / 1.64 s and reaches
1.31 s at 136 MiB, 1.20 s at 142 MiB and 0.98 s at 176 MiB (default 167 MiB /
1.21 s); every point on it but one uses the 32 MiB nursery, and a 4 MiB
nursery costs 0.4–0.8 s at the same heap. Bactrian runs from 101 MiB / 1.74 s
to 186 MiB / 1.05 s, Immix from 90 MiB / 2.41 s to 272 MiB / 0.97 s. The
fronts now overlap: at 94.5 MiB vanilla (`o=120,s=1M`) takes 1.21 s and
GenImmix 1.64 s (1.36×); at 136 MiB, 0.85 s against 1.31 s (1.54×); vanilla
never needs more than 116 MiB for its best time, which GenImmix does not reach
anywhere on the grid. The SUMMARY's 1.09× compares GenImmix's first point
with vanilla's default, not with vanilla's front. All three plans are
dominated. Six GenImmix points (32 and 48 MiB heaps) and eight Bactrian points
(32 and 48 MiB, plus 64 and 96 MiB with a 32 MiB nursery) segfaulted (GH issue
49; fixed by PR 51, merged after this sweep and not in its build; with the
fix these points raise `Out_of_memory`); the six failed Immix points (32 and
48 MiB) raised `Out_of_memory` cleanly. Before the fix, the fronts started at
156 MiB / 1.79 s (GenImmix), 159 / 1.86 (Bactrian) and 132 / 2.58 (Immix) and
did not overlap vanilla's.

**Takeaway: with the artefact gone, the allocation-heavy flagship loses on
collector speed at a given heap, not on a memory floor: vanilla's curve falls
faster with memory than any MMTk plan's.**

### kb (Knuth–Bendix)

![kb](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4_nz/spacetime_kb.png)

Vanilla's front lives at 7–39 MiB (0.44 → 0.36 s; default 8 MiB / 0.40 s).
Bactrian starts at 22 MiB / 0.48 s and GenImmix at 23 MiB / 0.49 s, both
reaching 0.39–0.40 s at 63 MiB (defaults 27 MiB / 0.44 s and 31 MiB / 0.45 s);
Immix starts at 44 MiB / 0.43 s and reaches 0.38 s at 106 MiB. Against
vanilla's default the nearest points are 1.20× (Bactrian), 1.22× (GenImmix)
and 1.09× (Immix); against vanilla's best time (0.36 s at 39 MiB) MMTk's best
is 1.05–1.13× at 1.6–5× the RSS. Every plan is dominated. Before the fix, the
fronts started at 82 MiB (GenImmix, Bactrian) and 86 MiB (Immix), 75–79 MiB
above vanilla's.

**Takeaway: a small-live-set program pays a 15 MiB floor (37 MiB for Immix)
and 5–22 % time; the 10× footprint of the first sweep was the artefact.**

### matrix_multiplication (768)

![matrix_multiplication](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4_nz/spacetime_matrix_multiplication.png)

All 16 vanilla points sit at 19 MiB / 0.73–0.76 s: `o` and `s` do not move a
fixed-live-set compute bench. Every MMTk plan reaches 0.58 s, at 27 MiB
(GenImmix, only with a 32 MiB nursery and a heap of 96 MiB or more; its other
points are 48–71 MiB and 0.62–0.66 s, default 48 MiB / 0.64 s), 30 MiB
(Immix) and 31 MiB (Bactrian). Class: faster only at higher RSS, 0.76× at the
nearest point. Before the fix, the lowest RSS was 41 MiB (GenImmix, the same
32 MiB-nursery points; its other points 112–134 MiB), 77 MiB (Immix) and
97 MiB (Bactrian), with the same times.

**Takeaway: the grids still do not bracket each other, so this is a floor
comparison: MMTk is ~24 % faster at 8–12 MiB more RSS (1.4–1.7×).** Why
GenImmix drops from 48–71 to 27 MiB with a 32 MiB nursery and a large heap is
open.

### LU_decomposition (900)

![LU_decomposition](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4_nz/spacetime_LU_decomposition.png)

Vanilla: 17 MiB / 0.79 s (default 17 MiB / 0.82 s). GenImmix and Bactrian
start at 26 MiB / 0.84 s (1.03× vanilla's default, 1.07× its best) and flatten
to 0.79–0.80 s at 46 MiB (defaults 31 MiB / 0.81 s); Immix sits at
47–64 MiB / 1.02–1.03 s (1.25× vanilla's default) and does not improve with
more memory. All dominated. Before the fix, GenImmix and Bactrian started at
86 MiB / 0.85–0.86 s and Immix at 98 MiB / 1.05 s.

**Takeaway: same shape as `kb` (a 9 MiB floor for the generational plans,
near-parity time); Immix's flat 1.25× is a separate, non-GC cost.**

### chameneos_redux (500000, 1 domain)

![chameneos_redux](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs_spacetime_m4_nz/spacetime_chameneos_redux.png)

Vanilla: 37–46 MiB, 1.33 → 1.09 s (default 37 MiB / 1.33 s). Immix is flat
at 0.97–1.02 s from 74 MiB up (default 75 MiB / 1.01 s): faster only at higher
RSS, 0.77× vanilla's default at 2× its RSS (0.94× vanilla's best, 1.09 s at
46 MiB). GenImmix (58–139 MiB, 2.99 → 2.63 s; default 65 MiB / 2.77 s) and
Bactrian (54–82 MiB, 3.33 → 3.09 s; default 64 MiB / 3.28 s) are dominated,
2.25× and 2.51× vanilla's default time at their nearest points. At their
defaults, all within 64–75 MiB, the generational plans take 2.7× (GenImmix)
and 3.3× (Bactrian) Immix's time. Before the fix, all three fronts started at
117–120 MiB, with the same times.

**Takeaway: on continuation- and effects-heavy code, the generational plans'
nursery is the cost; plain Immix beats vanilla's default by 23 % at twice the
memory.** Diagnosed on 2026-09-30 (night): not the continuation stacks but
MMTk's per-object nursery cost, ≈ 1.36 µs per copied object at the same
copied volume as stock, dominated by one `malloc` per copied object in the
work-packet pipeline (item 2 of "What the curves say"; ROADMAP item 35).

## Quick panel (screening)

One point per plan, at the dynamic heap: where each default policy lands, not
which collector is better. Median wall ms (ratio to vanilla) / max RSS MiB.
The panel was measured before the macOS memset fix, so every MMTk RSS
figure below carries that artefact (spot re-measure after the fix: `nbody`
GenImmix 25 → 9 MiB; see "The macOS memset artefact"); its times stand.

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
8 domains). Most of that anti-scaling was the E1 barrier-study
counters, bumped from every domain on every write barrier (removed by PR 58,
merged `ee744db495`; measured on godel, not re-run on the M4); see "What the curves
say", item 2. The other benches gain speed with domains but less than vanilla:
3.2–3.4× vs 5.2× on `par_spectralnorm`, 5.0–5.2× vs 6.1× on `par_matmul`, and
GenImmix 2.5× vs 4.35× on `par_binarytrees` (548 ms / 623 MiB vs 378 ms /
561 MiB at 8 domains); Immix anti-scales on `par_binarytrees`.

## What the curves say

1. **The residual RSS floor is 10–20 MiB; on `binarytrees` the gap is
   collector speed.** With the memset artefact removed, the generational
   plans' fronts start 8–21 MiB above vanilla's on `kb`, LU,
   `matrix_multiplication` and `chameneos_redux` (Immix 11–37 MiB), and a
   compute-bound program's startup floor is 9 MiB (`nbody`, GenImmix; vanilla
   2). On `binarytrees` GenImmix's first point (94 MiB) sits beside vanilla's
   default (91 MiB), and what places the front to the right is time: at equal
   RSS vanilla is 1.36× (94.5 MiB) to 1.54× (136 MiB) faster, and GenImmix
   never reaches vanilla's 0.85 s. *Open, candidates for the floor:* the
   nursery (bounded up to 16 MiB by default, outside the pinned heap and
   counted twice in the heap budget); GC work-packet vectors (29–36 MiB of
   malloc on `binarytrees` at a 64 MiB heap, measured before the fix and
   independent of it); one mapped chunk per space and metadata spec; no page
   return on macOS (every return path is Linux-only). *Open, for speed:*
   MMTk's per-collection cost at a given heap (a nursery collection goes
   through the full stop-the-world and work-packet machinery) and the
   nursery size (a 4 MiB nursery costs 0.4–0.8 s on `binarytrees`). The
   `matrix_multiplication` GenImmix drop from 48–71 to 27 MiB with a 32 MiB
   nursery and a large heap says the floor is not static.
2. **Generational plans lose 2.7–3.3× to Immix on effects-heavy code**
   (`chameneos_redux`, 1 domain, at the defaults of this sweep; 2.6–3.1× in
   the first sweep and the panel), and every plan anti-scales with domains
   there. **Diagnosed (2026-09-30 night, godel; NOTES entry of that
   date, ROADMAP item 35):** neither continuation stacks nor a larger
   promoted volume. MMTk copies the same volume as stock (20.2 M objects at
   1 domain), but its nursery pause costs ≈ 1.36 µs per copied object,
   much of it in narrow work packets (2–4 objects each, one `malloc` per
   copied object in all), plus side-metadata traffic; full GCs are cheap (68 pauses,
   0.7 s of 27.5 s of pause). Plain Immix has no nursery and pays none of
   it. The anti-scaling at 8 domains was mostly the E1 barrier-study
   counters contending across domains (removed by PR 58, merged `ee744db495`:
   godel d=8 21.8 → 10.3 s, speedup 1.9× → 4.0×); the stop-the-world
   rendezvous is not a factor (26 of 674 ms of pause at 8 domains on
   `par_binarytrees`, M4). The numbers above predate both findings.
   **First lever (2026-09-30 late, opt-in, godel):** a worker-local
   nursery closure (`MMTK_LOCAL_NURSERY_TRACE=1`, GenImmix) cuts
   `chameneos_redux` nursery pause time 29 % at 1 domain (27.8 → 19.6 s)
   and 12 % at 8; it is not in these sweeps (NOTES 2026-09-30 late).
3. **The generational plans segfault instead of raising `Out_of_memory` at
   too-small heaps** (GH issue 49; Immix fails cleanly at the 32 and 48 MiB
   heaps). The same 20 points fail in both sweeps. The same signature was
   first recorded in August, near the true OOM point. **Root-caused:**
   raising `Out_of_memory` from inside `caml_call_gc` left the saved-register
   pointer NULL, and a collection started by the raise scanned that frame
   through it. Every plan is exposed; Immix rarely collects on the retry.
   Fixed by PR 51 (merged, `ce2dd86167`; not in either sweep's build); a correctness bug,
   independent of the performance question.
4. **Compute-bound controls are flat** (0.99–1.01× time in the panel), so the
   MMTk mutator path costs nothing measurable on allocation-light code.
   *Open:* none for time; their RSS is the startup floor (9 MiB after the fix).
5. **Host caveats bound all of the above.** The macOS memset artefact is fixed
   (below), so the M4 RSS coordinate no longer overstates MMTk's memory by
   ~50 MiB, but macOS still returns no page to the OS. One run per sweep
   point and no dispersion; vanilla's default point moved by up to 12 %
   between the two sweeps with the same binaries; cores were not pinned, so
   the M4's efficiency cores can take a run. *Open:* repeat the sweep on
   godel with reps before quoting any front crossing, and to check the
   residual floor under Linux RSS accounting.

### The macOS memset artefact

Off Linux, mmtk-core's `dzmmap`/`dzmmap_noreplace` zero-filled every heap and
side-metadata chunk as it mapped it (4 MiB each), so on macOS every mapped
chunk was fully resident from the start, whether or not it was used. A fresh
anonymous mapping is already zero, so the memset only made pages resident.
Measured with `vmmap`, `kb` at a 32 MiB heap was 60 MiB metadata + 12 mature +
8 nursery + 2.5 malloc = 82.6 MiB, of which 1.4 MiB of metadata was needed.
The fix is mmtk-core `5454281016` (fplaunchpad/mmtk-core PR 7, "memory: do not
memset fresh mappings on macOS"; mmtk-core PR 7, merged as `69b5ddf663`),
pinned by ocaml-mmtk PR 53 (merged, `3846019997`).
Checked with a `sanity` build (`binarytrees 18` at 64 MiB and `kb 30` at
32 MiB on GenImmix, Bactrian and Immix: no invalid reference), 12/12
quick-panel goldens, and wall time unchanged; spot RSS fell from 25 to 9 MiB
(`nbody`), 90 to 31 (`kb` at 32 MiB), 164 to 119 (`binarytrees` at 64 MiB,
GenImmix) and 86 to 43 (`kb` at 32 MiB, Immix). Across the sweep each front's
first point moved 40–66 MiB left (about 60 MiB for GenImmix and Bactrian,
42–51 for Immix; 14 for GenImmix's 32 MiB-nursery points on
`matrix_multiplication`), with the same times:

| bench | vanilla | GenImmix | Bactrian | Immix |
|---|--:|--:|--:|--:|
| `binarytrees` | 64 → 64 | 156 → 94 | 159 → 101 | 132 → 90 |
| `kb` | 7 → 7 | 82 → 23 | 82 → 22 | 86 → 44 |
| `matrix_multiplication` | 19 → 19 | 41 → 27 | 97 → 31 | 77 → 30 |
| `LU_decomposition` | 17 → 17 | 86 → 26 | 86 → 26 | 98 → 47 |
| `chameneos_redux` | 37 → 37 | 118 → 58 | 120 → 54 | 117 → 74 |

(Front-start RSS in MiB, first sweep → this sweep, recomputed from the two
NDJSON files.) The first sweep's files stay on the `benchmarks` branch as the
record of the artefact:
[`spacetime-m4.ndjson`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/spacetime-m4.ndjson),
[`spacetime-m4.log`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/spacetime-m4.log),
[`graphs_spacetime_m4/`](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks/quick/graphs_spacetime_m4).
The Linux cross-check that first pointed at it (`kb` GenImmix 29 MiB on godel
against 91 on the M4) is in
[`quick/RESULTS-godel-2026-09-30.md`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/RESULTS-godel-2026-09-30.md);
the decomposition and the fix are in `gc/mmtk/NOTES.md` 2026-09-30 (evening),
"macOS RSS floor".

## Configuration and raw data

- Host: Apple M4 Pro (8 performance + 4 efficiency cores, 24 GiB), macOS 26.6,
  kept quiet during the runs. No core pinning, no `setarch`.
- Fork, this sweep: `d6933f65da` (PR 53's head: mainline `6bb7a1be36` plus the
  mmtk-core pin; merged as `3846019997`), mmtk-core `5454281016`. Fork, first sweep and panel:
  `b714bb86d3`, tree identical to mainline merge `3fbe544984` (PR 41);
  mmtk-core `95b425a27d`. Vanilla, both: opam `5.5.0` (released,
  non-flambda), the same binaries.
- Plans: GenImmix (default), Bactrian, Immix. LXR is excluded: it needs a
  pinned heap and its results are provisional.
- **Sweep** (`quick/spacetime.py`): benches `binarytrees`, `kb`,
  `matrix_multiplication`, `LU_decomposition`, `chameneos_redux` (1 domain) at
  perf sizes; vanilla `OCAMLRUNPARAM` `o` ∈ {40, 80, 120, 200, 320} × `s` ∈
  {256k, 1M, 4M} plus default; each MMTk plan `MMTK_HEAP_SIZE_MB` ∈ {32, 48, 64,
  96, 128, 192, 256} plus dynamic × `MMTK_NURSERY` ∈ {default,
  `Fixed:4194304`, `Fixed:33554432`}; `MMTK_THREADS=1`; one run per point; RSS
  = the child's `ru_maxrss`. Command (the first sweep used `quick/build_m4_mmtk`,
  `spacetime-m4.ndjson` and `graphs_spacetime_m4`):
  `uv run quick/spacetime.py --vanilla quick/build_m4_vanilla --mmtk quick/build_m4_mmtk_nz --plans "GenImmix Bactrian Immix" --benches "binarytrees kb matrix_multiplication LU_decomposition chameneos_redux" --json quick/spacetime-m4-nz.ndjson --graphs quick/graphs_spacetime_m4_nz --no-setarch --no-pin --resume`.
- **Panel** (`quick/quickbench.py`): dynamic heap, `--threads domains` (1 GC
  worker per sequential cell, N for N domains), perf sizes, 3 measured runs
  (median) after 1 warmup, RSS = peak across runs. Command (in `quick/`):
  `uv run quickbench.py all --vanilla build_m4_vanilla --bin-a build_m4_mmtk --plans "GenImmix Bactrian Immix" --heap dynamic --gc --chart --no-pin --no-setarch --json m4-2026-09-30-dynamic.ndjson --graphs graphs_m4`.
- Raw data, all on the [`benchmarks` branch](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks)
  (this sweep at `3e150aa0dc`):
  sweep [`spacetime-m4-nz.ndjson`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/spacetime-m4-nz.ndjson),
  [`spacetime-m4-nz.log`](https://github.com/fplaunchpad/ocaml-mmtk/blob/benchmarks/quick/spacetime-m4-nz.log),
  [`graphs_spacetime_m4_nz/`](https://github.com/fplaunchpad/ocaml-mmtk/tree/benchmarks/quick/graphs_spacetime_m4_nz)
  (with `SUMMARY.md`, the per-plan class table and the failed points);
  first sweep as listed in "The macOS memset artefact";
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
  (`binarytrees` 92 MiB on both), MMTk's did not (`kb` 29 vs 91 MiB, the macOS
  memset artefact). Compare each host only with its own vanilla.
- **2026-09-30, M4 Pro panel and first space-time sweep (superseded for RSS).**
  The first sweep (`spacetime-m4.ndjson`, `graphs_spacetime_m4/`) found every
  MMTk front starting 60–90 MiB right of vanilla's and read the gap as a
  memory floor; about 40–66 MiB of each front start was the macOS memset
  artefact. Its times and verdict classes stand. The panel on this page is
  from the same build and still carries the artefact in its RSS column.
- **2026-09-30, M4 Pro space-time sweep after the memset fix (this page).**
  Same grid and vanilla binaries, MMTk from mmtk-core `5454281016`.
