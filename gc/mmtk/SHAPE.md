# SHAPE.md — measuring the GC space-time shape, vanilla vs Bactrian

`BACTRIAN.md` records that `MMTK_PLAN=Bactrian` is **architecture-matched, not
implementation-matched** to stock OCaml 5: vanilla marks and sweeps in incremental
slices *on the mutator domains*, Bactrian marks on GC worker threads and sweeps STW
inside FinalMark.

Per KC, closing that mechanism gap is **not** the goal. Dedicated GC workers are fine.
What has to match is the **space-time shape** — and the quantity that carries the
argument is *aggregate CPU spent doing work versus aggregate CPU spent doing GC*, a
ratio that is invariant to whether GC work is interleaved into mutator domains or handed
to dedicated workers. That invariance is what makes "dedicated workers are fine" a claim
rather than a concession.

This document is the instrument record: what is measured, how, what each instrument was
validated against, and the traps found along the way. Acceptance tolerances are
deliberately **not** set here — they are KC's call once the shape is measured.

---

## Dimensions

| # | Dimension | Metric |
|---|---|---|
| **D1** | **CPU budget** *(headline)* | CPU split three ways — mutator `W`, GC `G`, coordination-spin `C` — plus the ratio. Both `G/(W+G)` and `G/(W+G+C)` are reported; which is claimed must be stated |
| **D2** | Pacing curve | cumulative GC events vs cumulative bytes allocated |
| **D3** | Mutator-utilization timeline | MMU vs window width; 1st-percentile utilization; u(t,w) over wall time |
| **D4** | Footprint over time | RSS vs wall time — sawtooth amplitude, period, floating-garbage integral |
| **D5** | Space-time curve | throughput vs *measured* peak RSS |

Measured across two modulation axes: **M1** domain count, **M2** workload.

D1 and D3 are complementary, not overlapping. Coordination cost has two forms: *spinning*
burns CPU and lands in D1's `C`; *blocked parking* (futex wait) burns **no CPU at all** and
appears only as wall time in which the mutator does not progress, which is D3's business.

---

## Instruments

| Instrument | Serves | Where |
|---|---|---|
| `quick/lib/probe.ml` | D2, D3 | in-mutator tick, identical source both runtimes |
| `MMTK_PAUSE_LOG` | D3 attribution | `gc/mmtk/binding/src/collection.rs` (#R1) |
| `quick/lib/rss_sampler.py` | D4 | external `/proc/<pid>/statm` poller |
| `quick/lib/gcsplit.py` | D1 | three-bucket `perf` symbol classifier |
| `quickbench.py --gc` | D1, D2 | wall, CPU, RSS, GC counts, per-cell durability |

### Why the probe is in-mutator, not a separate domain

`Gc.minor_words` reads `Caml_state->stat_minor_words` — the *calling domain's* odometer.
`Gc.quick_stat` aggregates across domains, but for every domain other than the caller it
reads `sampled_gc_stats[]`, refreshed only inside a GC stop-the-world section
(`gc_stats.c:131`, called from `minor_gc.c:708` / `major_gc.c:1899`; the fork keeps this
at `minor_gc.c:221`). A probe in its own domain would therefore see the benchmark's
allocation advance **only at collections** — a step function encoding GC events into the
very axis D2 plots them against.

The same choice fixes D3's cross-runtime semantics: a gap between ticks is time this
mutator did not progress, whether it was stopped by MMTk's `stop_all_mutators` or was
itself running a vanilla mark slice. Both are "the mutator is not progressing", which is
what MMU means — and it avoids the asymmetry a non-allocating spin domain would have,
where it might never take a vanilla mark slice and so miss vanilla's GC cost entirely.

---

## Validation

Every number below is from this host (Core Ultra 7 265H, 6 P-cores pinned). They validate
the *instruments*; they are not the shape result.

**Probe allocates nothing.** D2's denominator *is* the allocation counter, so probe
allocation is measurement error, not overhead. 3M ticks on vanilla:

| backend | disabled | enabled |
|---|---|---|
| native | 0.000 | **0.000** words/tick |
| bytecode | 0.000 | **10.000** words/tick |

The first cut cost 2 words/tick — not from `gettimeofday`, but from `mutable float` fields
of a *mixed* record, which OCaml boxes (only all-float records unbox). Moving them to a
flat `float array` removed it. Bytecode has no float unboxing at all, so the probe
**refuses to arm** there rather than produce a quietly corrupted curve.

**The buffers, not the tick path, were the real hazard.** Vanilla binarytrees is
byte-deterministic bare: 458228545 minor words, 1778 minor collections, 61 major
collections, identical over five runs. With the probe armed at its original `1<<20`
capacity, major collections fell to **42** — a 31% shift in exactly what D2 plots. Cause:
16 MiB of gap arrays are *live for the whole run*, and both collectors size the heap from
the live set, so the probe was buying the program a bigger heap. At `1<<14` (~370 KiB)
vanilla reads 60 against bare's 61, reproducibly. Overflow now reports a `dropped` count.

**Output is byte-identical** bare vs instrumented, on both runtimes.

**Wall overhead is within noise**: 3.51 / 3.47 / 3.57 s for bare / probe-linked-off /
probe-on (7 reps). Note the 2.94–3.77 s spread — this laptop has ~28% run-to-run variance,
so medians of 3 are not enough here.

**D2's denominator is equivalent across runtimes.** `Gc.minor_words` deltas match exactly:

| | small (10 w) | large (1000 w) |
|---|---|---|
| vanilla | 2,950,022 | 750,022 |
| fork | **2,950,022** | **750,022** |

Caveat: both correctly *exclude* large allocations (they bypass the minor heap), and
`Gc.major_words` is **broken under MMTk** — it read 0 against vanilla's 200,188,187. So
D2's denominator is minor-path allocation, not total allocation. Equal on both sides, so
the comparison holds; but for a large-object-heavy workload the absolute "per GB" is
wrong and there is no MMTk-side counter to correct it.

**Pause records cross-validate to the millisecond.** Logged pauses sum to the
independently accumulated total: GenImmix 259 pauses / 2280 ms vs `MMTK_VERBOSE`'s 2279 ms;
Bactrian 233 / 1554 vs 1554.

**The probe's D3 output has a resolution limit — it is scoped to D2.** A gap means "the
mutator did not progress" only if the work between ticks is far smaller than a pause, and
that is a property of the *call site*. With the marker in binarytrees' outer loop, all
three runs — vanilla included — read 67–72% "stalled" with MMU(10ms) = 0.000, because one
depth-20 iteration is tens of milliseconds of ordinary work. Moving the marker into the
`check` recursion resolves it but costs 44% wall (1.92 → 2.77 s); a stride sweep
(1024/16384/262144 → 2.77/2.81/2.77 s) shows the cost is the per-call `Domain.DLS.get`,
not the clock, and even the disabled call costs 11%. On a workload whose unit of work is
~7 ns, no per-unit call is affordable.

So: **probe → D2**, where coarse placement is correct (the pacing curve only changes at
collections) and the probe is free (1.90 vs 1.89 s bare). **D3 pause distribution →
`MMTK_PAUSE_LOG` on the fork and `runtime_events` on vanilla**, both zero-overhead and
authoritative. Residual, recorded rather than hidden: coarse placement also under-samples
D2 during binarytrees' deep-tree phase (last sample at 2344 of ~3660 MiB, 10 of 61 major
collections). Benches with uniform iteration granularity, such as kb, do not have this.

**Probe gaps vs GC pause records** — the mutator-side and GC-side views agree where they
should. The probe is a strict superset (2.28× total stall, 24.6× event count: it also sees
scheduling, page faults, allocation slow paths). But the **largest** stalls track closely:

| rank | GC pause | probe gap | ratio |
|---|---|---|---|
| 1 | 82.27 ms | 91.62 | 1.11 |
| 2 | 77.73 | 88.23 | 1.14 |
| 3 | 65.41 | 74.38 | 1.14 |
| 5 | 54.57 | 53.37 | 0.98 |
| 9 | 18.77 | 24.91 | 1.33 |

Below ~rank 10 they diverge, as non-GC stalls exceed the small GC pauses. So: probe → MMU
(all stalls, cross-runtime); pause log → GC attribution (MMTk side only).

**Checkpointing works.** A killed campaign left 4 cells, all lines parsing; `--resume`
skipped exactly those.

---

## Protocol

- **Pin the heap.** Under a dynamic heap MMTk's GC count is bistable: five identical runs
  gave 259–265, but a perturbed run latched a different heap trajectory and gave **163**.
  At a pinned 512 MiB the same config is deterministic *to the object* — 57 GCs (7 full),
  6,082,529 objects copied, every run. D2 is not reproducible without this.
- **Pin to one core class.** This host is a three-tier hybrid — P 0–5 @ 5.3 GHz, E 6–13 @
  4.6, LP-E 14–15 @ 2.5, a 2.12× spread. `--cpu-set pcores` is the default; a whole-machine
  sweep is a legitimate figure but must be reported separately, never pooled.
- **Same core budget both sides.** The old harness pinned sequential cells to `taskset -c
  0-0` *and* set `MMTK_THREADS=1`, co-scheduling mutator and GC worker on one core — which
  specifically destroys plans whose premise is marking off the critical path.
- `setarch -R` on both sides; governor `performance`; ≥10 invocations given the variance
  above; timing and profiling as separate passes.
- **Hosts.** This laptop iterates; **church** is the authoritative campaign. Results are
  tagged by `host` and never pooled. The `church` numbers in `SCALABILITY.md` §11 are
  retracted for a contaminated *build*, not a bad host.

  church, confirmed: **2× Xeon Gold 5120**, 56 threads = 2 sockets × 14 cores × 2 SMT,
  NUMA `node0 = 0-13,28-41` and `node1 = 14-27,42-55`, 61 GB RAM, Ubuntu 26.04 with
  gcc 15.2.0 / glibc 2.43 — an exact toolchain match with the dev laptop. Cores are
  **uniform**: no hybrid split, which is the whole reason it is the reference host.

  This is the machine `PERFORMANCE.md` §5 was written against, and its `taskset -c 0-13`
  is not an arbitrary 14 CPUs: `cpu0`'s `thread_siblings_list` is `0,28`, so `0-13` is
  exactly one thread per *physical* core of socket 0. `quickbench --cpu-set auto` now
  derives that rather than copying it — uniform class, one thread per physical core,
  inside one NUMA node — and resolves to `0-13` on church, `0-5` on the laptop. Getting
  this wrong is not cosmetic: the previous default would have pinned `0-55`, spanning
  both sockets, and made every run a NUMA experiment.
- **`perf` needs the host.** The VM cannot substitute: as root with `paranoid=-1` it still
  reports "No supported events found" — Multipass does not virtualize the PMU. The host has
  `cpu_core`+`cpu_atom` and only lacks permission
  (`sysctl kernel.perf_event_paranoid=-1`). On this hybrid part perf must be told
  `-e cpu_core/cycles/`, or the `cpu_atom` open fails and takes the run with it.

---

## Early observations (laptop; not the result)

Sequential binarytrees, 6 P-cores, medians of 3, dynamic heap:

| | wall s | CPU s | CPU/wall | STW s | STW % |
|---|---|---|---|---|---|
| vanilla | 2.08 | 2.07 | 1.00 | — | — |
| GenImmix | 3.97 | 3.92 | 0.99 | 2.78 | 70% |
| Bactrian | 3.75 | **5.14** | **1.37** | 1.23 | 33% |

Two things worth carrying forward:

1. **Wall time hides D1.** Bactrian is 1.81× vanilla by wall and **2.48× by CPU**. Its
   halved stop-the-world time is bought with 31% more aggregate CPU than GenImmix.
   `CPU/wall` of 1.37 shows the concurrent marking genuinely overlapping the mutator,
   where vanilla and GenImmix both sit at ~1.00.
2. **"Concurrent marking cuts pauses" is true of the median and false of the tail.**
   Bactrian cuts the *median full-GC* pause 11× (12.5 → 1.1 ms) but leaves the max
   unchanged (119 → 112 ms), because in both plans the worst pauses are **nursery**
   collections, not marking. A summed total could neither support nor refute this.

---

## Campaign results — church, 2026-08-07 (full profile, all dimensions × both axes)

Data + figures: `benchmarks` branch, `quick/campaigns/20260807/` (shape_summary.md
carries every number; RUNLOG.md the machine state, hitches, and one retraction).
Operating point: MMTk pinned 192 MiB vs vanilla `o=500` (iso-memory); 14 physical
cores of socket 0.

**D1 — the headline result.** Bactrian at `MMTK_THREADS=1` matches vanilla's
*absolute* collector CPU on binarytrees to a centisecond — G 1.99 s vs 1.98 s — and
the entire fraction gap (0.42 vs 0.55) is the denominator: W 2.76 s vs 1.64 s, a
+68% mutator tax that the mutator-side instrumentation proves is NOT misattributed
GC work (explicit mutator-side GC totals 0.10 s). On kb the gap is in G as well
(0.53 vs 0.19 s): at small heaps the per-collection floor dominates both buckets.
The W tax needs perf's cache counters to attribute; blocked on root.

**Retraction.** The 2026-08-06 finding "idle workers park; G invariant to
MMTK_THREADS" came from the pre-fix sampler losing worker CPU at thread exit. With
per-TID accounting, G *grows* with T (binarytrees: 1.74 → 3.34 for T=1→4). Report
D1 at T=1 and T=domains, both.

**D2 + the tweak experiment (`shape/tweaks`).** Stock-parity pacing is pure
configuration: `MMTK_NURSERY=Fixed:2097152` + `MMTK_FULL_GC_CADENCE` reproduce
vanilla's collection count (1867 vs 1839 on binarytrees). The price is 5.4–7.7×
wall: the per-minor floor is ~7.5 ms against vanilla's 0.63 ms (~12×). The
tweak_frontier figure quantifies the whole nursery×cadence surface; the shipped
default sits at the opposite end (58 collections, 32× fewer than vanilla, best
wall). **Matching vanilla's D2 shape is blocked on the per-collection floor, not on
trigger design** — the floor is the lever (NURSERY_TRACE.md S2/S3, STW rendezvous
cost).

**D3 — the shapes are opposite, and neither dominates.** binarytrees: vanilla
stalls 3553 times for 1.98 s total — at fine grain it is nearly always briefly
stalled (MMU@100ms = 0.063); MMTk stalls 57–59 times for 1.0–1.25 s, concentrated
(MMU@100ms = 0 — clustered full-GC windows). kb separates them cleanly: vanilla
MMU@10ms = 0.77 vs MMTk 0.00. Which shape is "better" depends on the window a
consumer cares about; this is the argument for reporting MMU curves rather than
pause percentiles.

**M1.** MMTk beats vanilla on wall AND total CPU at d=2–4 (e.g. d=4: 1.90–1.98 s /
4.8–6.5 CPU vs vanilla 2.08 s / 5.6). At d=8 it collapses — wall 2.6 s, CPU 15–17 s
vs vanilla's 1.79 s / 7.1 — because 8 domains + 8 workers oversubscribe 14 cores.
Policy fix, not fundamental: cap workers so domains + workers ≤ physical cores.

**Instrument caveats current as of this campaign**: vanilla's own write-barrier
cost sits outside its spans (small, uncorrected, favours vanilla's W); gcpauses
fails with "corrupt stream" from a detached parent (unresolved — run it from an
interactive context); threadcpu undercounts by at most one sampling interval per
thread.

---

## perf attribution — church, 2026-08-07 (paranoid=-1, governor=performance)

**The W-tax is solved: it is L2 misses from allocation geometry, not extra work.**
matrix_multiplication, vanilla vs GenImmix T=1, whole process:

| | instructions | cycles | LLC-loads (=L2 misses) | LLC-load-misses |
|---|---|---|---|---|
| vanilla | 13.88 G | 4.71 G | 56.5 M | 51 k |
| MMTk | 14.06 G (+1.2%) | **8.70 G (+85%)** | **685.7 M (12.1×)** | 85 k |

Identical instruction stream, +85% cycles, and the entire difference is memory:
12× more L2 misses that all HIT the LLC (which is why plain "cache-misses"
looked innocent — nothing reaches DRAM). Vanilla's copying minor compacts the
live rows into L2-resident pools; MMTk's rows sit at 64-MiB-nursery allocation
pitch across Immix blocks, so the same traversal pays LLC latency continuously.
This also explains why the tax appears on every allocating bench and is
untouchable by GC-side accounting: it is program code executing against a worse
layout. Replicates SCALABILITY.md's par_matmul artifact note with the mechanism
now measured directly.

**D1 by symbols needs thread identity — flat classification undercounts G.**
Flat gcsplit on Bactrian/binarytrees read G/(W+G) = 0.25 against threadcpu's
0.42, because GC copying runs through libc memmove/memset, which a flat symbol
map files under W. The reconciled instrument is HYBRID: worker-thread samples
are G by thread identity, mutator samples classify by symbol. That yields 0.39
(bt) — agreeing with threadcpu's 0.42 — and 0.14 (kb, fresh run under the
performance governor).

**Coordination-spin is a non-issue at T=1 — and "spin" was the wrong reading.**
C = 0.0–0.2% in every profile. Worker CPU beyond the STW windows is real work,
not spinning: for Bactrian it is concurrent marking (by design), and the
worker symbols are trace/copy/metadata, with side_metadata_access appearing
three times independently — matching NOTES' 77% metadata/dispatch
decomposition of the per-object cost.

**kb correction.** The earlier remset hypothesis (value-unfiltered remset as
the specific culprit for kb's 2.8× G) is NOT supported by the flat worker
profile: no remset-processing symbol ranks, and the time sits in generic
per-object machinery (FieldSlot::from_address, forward_object, metadata
access). The mutation-rate contrast (barrier time 15 ms vs 0.035 ms) stands,
but the dominant kb cost is the per-object framework overhead atop the
per-collection floor. Remset share is unresolved without call graphs (-g);
the value-filtered-remset experiment on shape/tweaks remains worth running,
with reduced expectations.

---

## The W-tax, resolved into two distinct mechanisms (church, perf, 2026-08-07)

**Mechanism 1 — allocation-pitch set-aliasing (matmul-class, large regular objects).**
Sweeping the matrix size changes the row pitch and nothing else of substance;
MMTk's L2 misses collapse while vanilla's stay flat:

| size | vanilla LLC-loads | MMTk LLC-loads | ratio | cycle ratio |
|---|---|---|---|---|
| 768 | 56.5 M | 703.8 M | **12.5×** | 1.92× |
| 770 | 57.1 M | 317.5 M | 5.6× | 1.46× |
| 800 | 64.0 M | 159.4 M | 2.5× | 1.39× |

Bump allocation places 6.2-KB rows at a perfectly regular pitch; at size 768
(row = 6144 B payload, near a power of two) that stride cycles through few L2
sets — classic set-conflict aliasing. Vanilla's size-class pools place the same
rows at a different, benign pattern. The 12× headline is therefore a
size-768 pathology sitting on a real but smaller generic layout tax (~1.4×
cycles at non-pathological sizes, same instructions). NOT a nursery-size
effect: a 2 MiB nursery leaves the misses unchanged (672 M) while exploding
instructions 6× (GC storm).

**Mechanism 2 — nursery reuse-warmth (binarytrees-class, small-object churn).**
Per-thread counters, binarytrees, GenImmix T=1 vs vanilla:

| thread | cycles | instructions | LLC-loads | cache-misses | faults |
|---|---|---|---|---|---|
| vanilla (program+GC inline) | 10.91 G | 29.11 G | 7.9 M | 68.0 M | 18.0 k |
| MMTk mutator | 7.21 G | 13.40 G | 7.5 M | 66.9 M | **2** |
| MMTk worker | 4.19 G | 9.41 G | 2.8 M | 13.6 M | 49.1 k |

The mutator executes the same program instructions (13.40 G measured vs ~13.3 G
estimated for vanilla's program share) at IPC 1.86 vs vanilla's program-share
~2.4 (estimate: vanilla program cycles ≈ 10.91 G × its 50.3% symbol W-share).
Demand-LLC traffic is EQUAL — this is not mechanism 1. The difference is
allocation-region warmth: vanilla re-bumps the same 2 MiB arena (L2-warm every
lap, page faults on the mutator), MMTk streams through a 64 MiB nursery
(DRAM-class traffic on fresh lines; note the faults migrated wholesale to the
worker, which touches the fresh blocks during copying). Unlike matmul, this one
IS a nursery-size/reuse effect — the two mechanisms want opposite things,
which is the design tension a shape-matching nursery policy must resolve.

Bonus observation from the same table: MMTk's collector executes FEWER
instructions than vanilla's (9.4 G vs ~15.7 G estimated) — its 33× fewer
collections do save instruction-level work; vanilla's oldify simply runs at
extraordinary IPC (~2.9) while MMTk's metadata-heavy trace runs at 2.24.

## W-night round 1 (2026-08-08, church): placement fix found — jitter, not LOS

Goal reframed by the operating decision of 2026-08-08: **W parity** (mutator
cycle ratio → 1); G may differ in both seconds and count.

matmul-768, r1 single-rep peek (heap 192 MiB, T=1, cores 0-13, perf governor):

| config       | cycles | LLC-loads | wall |
|--------------|--------|-----------|------|
| vanilla      |  4.56G |    56.6M  | 1439ms |
| Bactrian     |  8.70G |   739.3M  | 2736ms |
| LOS ≥2056B   |  9.57G |   905.7M  | 3018ms |
| LOS ≥4096B   |  9.71G |   905.9M  | 3060ms |
| LOS ≥8192B (neg-ctl) | 9.16G | 686.1M | 2904ms |
| jitter       |  6.40G |    73.8M  | 2016ms |

- **MMTK_ALLOC_JITTER (1–16 word dead filler before ≥2KB bump allocs) removes
  10× of the LLC-load excess** — pitch-aliasing is confirmed causally, and the
  fix costs nothing (RSS unchanged; 2307 fillers ≈ one per row).
- **LOS routing is anti-productive**: page-aligned placement has zero low-bit
  entropy — a perfectly regular 8 KiB pitch, i.e. the pathology itself. The
  ≥8192 negative control (rows stay bump-allocated) ≈ stock, as predicted.
- Residual after jitter: 1.40× cycles at ~vanilla LLC-loads. The remaining
  stall source is below the LLC (L1/L2 conflicts or dTLB) — round 2 adds
  L1-dcache-load-misses/dTLB-load-misses to the event set.

### Round 1 complete: nursery curve + technique validation (wnight1)

**Technique is sound.** binarytrees ins_ratio (Bactrian/vanilla) = 0.824-0.825
under std pin (0-13), single core, no pin, and a drift re-pass; rep variance
0.15%. Only an SMT-sibling pair shifts cyc_ratio (1.14 -> 1.57, expected).
The sub-1 instruction ratio is real: Bactrian's collector executes fewer
total instructions (57 vs ~1800 collections' fixed work).

**Small nursery = catastrophe, warmth never materializes (binarytrees 20,
heap 192, T=1):**

| nursery | GCs | whole cyc | whole ins | wall | corrW |
|---------|-----|-----------|-----------|------|-------|
| default (64M scaled) | 57 | 13.1G | 25.1G | 3613ms | 2.46s |
| 32M | 115 | 19.0G | 36.0G | 4920ms | 2.88s |
| 16M | 229 | 29.1G | 55.9G | 7087ms | 3.71s |
| 8M  | 458 | 45.6G | 84.9G | 10231ms | 5.55s |
| 4M  | 924 | 78.6G | 138.7G | 16337ms | 9.65s |
| 2M  | 1864 | 140.9G | 239.6G | 27428ms | 17.98s |

kb is nursery-INSENSITIVE in W (corrW ~1.39-1.61 flat) — warmth is not kb's
mechanism at these sizes either.

**Open mystery (the night's quarry):** at 2M the mutator thread burns 20.36s
kernel-accounted CPU (wall 27.5s, STW spans 7.1s) — ~16s of real mutator-thread
CPU inside the park-call windows (parked TSC 23.3s), only 2.39s of it accounted
by the alloc/barrier wrappers. The Rust park is a proper condvar; the CPU is in
something the park windows enclose (become_running retry churn, domain-lock
ops, alloc-retry, or unaccounted slow paths). R2E perf-record profiles nur2
by comm+symbol to name it. Note vanilla-stock bt at THIS operating point is
already at wall parity (3613 vs 3630ms) — the bt problem is the 1.14x cycles
and what happens when GC count rises.

### Round 2: per-thread decomposition — the bt W-gap is IPC/latency, not misses

Per-thread perf attach (stock configs, heap 192, T=1):

| cell | mutator cyc | worker cyc | mutator ins | worker ins | mut IPC |
|------|------------|-----------|------------|-----------|---------|
| vanilla bt20 (1 thread, GC inline) | 11.30G | — | 29.91G | — | 2.65 blended |
| Bactrian bt20 | 7.92G | 5.00G | 14.17G | 10.64G | 1.79 |
| Bactrian bt20 nur8 | 19.01G | 26.75G | 22.97G | 61.83G | 1.21 |
| vanilla kb50 | 4.29G | — | 7.62G | — | 1.77 blended |
| Bactrian kb50 | 4.40G | 0.78G | 6.65G | 1.33G | 1.51 |

- Instruction closure: Bactrian program 14.2G + worker 10.6G = 24.8G vs vanilla
  program ~14G + inline GC ~16G = 29.9G. The 0.82x whole-process ins ratio is
  entirely collector-side instruction economy.
- bt W-gap = the same ~14G program instructions at IPC 1.79 vs ~2.65 -> mutator
  7.9G vs ~5.2G cycles (1.5x), matching corrW 2.46s/1.65s from /proc. Two
  independent instruments agree.
- Bactrian's mutator has FEWER L1 misses, LLC loads, and similar dTLB than
  vanilla on bt — stalls are latency, not miss counts. Hypothesis: PROMOTION
  SCATTER. bt allocates trees depth-first (nursery = parent-child adjacent);
  vanilla oldify copies survivors first-child-first, preserving spine
  adjacency; MMTk work-packet tracing copies in BFS batches, so promoted trees
  lose adjacency and every hop is an unprefetchable dependent load. To test:
  stall-cycle + hit-level counters (cycle_activity.stalls_mem_any,
  mem_load_retired.l1_hit/l2_hit/l3_hit) mutator-side; then, if confirmed, a
  scan-order experiment in the binding (process field 0 eagerly a la oldify).
- kb W ratio ~1.1x (4.4G-mutGC vs ~3.7G) — near done.

### Tiny-nursery mutator CPU: solved

strace: futex calls scale 56 -> 20,511 (bt16 stock vs 2MiB nursery); the perf
kernel self-time sits in the syscall ENTRY trampoline (5 buckets within 0x200
bytes) + Rust park-edge bookkeeping (is_mutator, RUNNING-set HashSet hash_one
visible at ~1.5%/1.2%). With 1864 GCs the park/wake machinery + full-GC storms
(186 fulls from premature promotion) explain the 8x mutator CPU inflation.
No single hot function; the fix is "don't collect 1864 times", not micro-opt.

### Round 3/3b: panel three-way, THP, and the stall verdict

Whole-process cycle ratios vs vanilla (r1, heap 192, T=1): bt 1.14, nbody 1.00,
fannkuch 1.03, spectral 1.17, mandelbrot 0.98, matmul 1.89 -> 1.36 (jitter),
LU 1.43 (jitter-neutral, as predicted), kb 1.20. Output gates passed on all 8;
par_binarytrees d=4 sane.

THP (MMTK_TRANSPARENT_HUGEPAGES=true, supported natively by mmtk-core's env
reader): uniform 2-3.5% win — bt 13.10G->12.81G, kb 5.43G->5.28G, LU
7.64G->7.38G; THP + 8MiB nursery on LU = 7.00G with cache-misses erased
(155M -> 1.0M). 6-bit jitter >= 5-bit everywhere (mm800 LLC 116M -> 72M).

**Stall verdict (round 3b) — scatter hypothesis REFUTED, store-frontier
confirmed.** Bactrian bt mutator (pt attach): loads hit L1 at 98.9%
(mem_load_retired: L1hit 3.69G, L2hit 0.04G, L3hit ~0) — pointer chasing is
FINE. Yet cycle_activity.stalls_mem_any = 1.42G (18% of mutator cycles;
stalls_total 2.31G = 29%). Memory stalls with a clean load side = STORE
stalls: bump-allocation stores into never-touched cold lines drain through
the store buffer at RFO latency, invisible to load counters. One mechanism
now covers LU (155M demand misses), bt (IPC 1.80 vs 2.65), kb (L2 hits 2x):
vanilla's 2MiB arena keeps the write frontier L2-resident; Bactrian's 64MiB
stream cannot. Whole-process stall shares: bt 16.4% vs 8.3% (stallMem/cyc),
kb 13.1% vs 10.8%.

**Defaults changed (commit 683521d7a):** pitch jitter ON at 6 bits
(MMTK_ALLOC_JITTER=0 disables), THP ON (explicit env still honoured).

**Engineering plan from here (ranked):**
1. Cheap collections -> affordable small nursery -> warm store frontier.
   The blockers measured tonight: park/wake futex churn (56 -> 20.5k calls),
   full-GC storms from premature promotion (6 -> 186 fulls at 2MiB), worker
   packet overhead (gcIns 10.6G -> 185.6G across the sweep).
2. TLAB/store-frontier experiments: prefetchW-ahead on refill, block reuse
   order (LIFO warm blocks first), non-temporal fills. Target: bt mutator
   stalls_mem_any 1.42G -> vanilla-like share.
3. Promotion scatter: deprioritized — loads are L1-clean; revisit only after
   the store side is fixed.

### Rounds 5-6: diagnosis locked — store-buffer stalls; prefetch refuted; worker cap

- **resource_stalls.sb (store-buffer full) is the unified residual mechanism**:
  LU 14.9% of cycles vs vanilla 1.4%; spectralnorm 7.6% vs 0.1%; bt 4.4% vs
  1.1%. LU's profile is >85% the same three OCaml functions on both runtimes —
  the same code, slower, stalled on stores. (stalls_mem_any missed this: it
  only counts stalls with pending LOADS; SB-full stalls show in stalls_total.)
- **TLAB prefetchw refuted**: warming the fresh 32KB block at refill HURTS
  everywhere (LU 7.38G -> 9.14G, kb 5.24 -> 5.62, bt 12.98 -> 13.46) — burst
  prefetch floods the fill buffers. Knob stays default-off (MMTK_TLAB_PREFETCH).
- **Worker cap for d=8** (par_binarytrees, 14 cores): T=4 -> 2.17s vs T=8 ->
  2.42s (vanilla 1.27s). Policy: domains + workers <= physical cores; T=4-6
  optimal at d=8. Bactrian already BEATS vanilla at d=1-4 (0.83-0.84x).
- Jitter entropy: 6 bits optimal (j7/j8 slightly worse on matmul).
- **Asymptote statement**: with placement + THP + worker-cap banked, the
  remaining 1.1-1.4x cycle ratios on allocating benches are the store-frontier
  cost of a 64 MiB streaming nursery vs vanilla's L2-resident 2 MiB arena.
  Erasing it requires a small nursery, which requires cheap collections
  (premature-promotion handling, park/futex churn, packet overhead) — the next
  engineering phase, in mmtk-core scheduling, not an env knob.

### Round 7 (post-pacer): the definitive WORK-ONLY table

Metric per the operating decision: W = work-only, symbol-classified
symmetrically (gcsplit; barriers/alloc-slow = G on both sides), hybrid
attribution on Bactrian (worker comm = G by identity, mutator by symbol).
Configs: bt/kb/mm stock defaults (jitter+THP), lu Fixed:4MiB, sp Fixed:8MiB —
small nurseries usable thanks to the allocation-denominated pacer (78ab9698a).

| bench | W-ins ratio | W-cyc ratio | vG-cyc | dG-cyc |
|-------|------------|-------------|--------|--------|
| binarytrees | **1.022** | 1.355 | 5.72G | 5.12G |
| kb | **1.022** | **1.039** | 0.57G | 0.82G |
| LU | **1.042** | 1.110 | 0.01G | 0.76G |
| spectralnorm | **1.028** | **1.076** | 0.00G | 0.32G |
| matmul | **1.005** | 1.311 | 0.02G | 0.04G |

- **W-instruction parity achieved: 1.005-1.042 across the panel.** The
  mutator executes vanilla's work instruction-for-instruction (jitter fillers
  and refill slow paths classified G, as on the vanilla side).
- W-cycles: kb and spectralnorm at ~parity; LU 1.11 (post-GC warmth loss over
  3282 minors); the stragglers are bt 1.355 (store frontier at the 64 MiB
  nursery — small nursery blocked by real survivors/premature promotion) and
  matmul 1.311 (residual conflict-miss stalls after jitter).
- Note bt's collector is now CHEAPER than vanilla's in cycles (5.12 vs 5.72G).
- The bt/mm W-cycle path forward is architectural: nursery AGING (survive N
  minors before promotion) to make a small warm nursery viable for
  survivor-heavy workloads — a Bactrian-plan-local change (keep out of
  LXR paths), multi-session scale.

### Round 8: adaptive marking lands — bt W-cyc 1.355 -> 1.239

Bactrian now STW-marks when mature < MMTK_CONC_MARK_MIN_MATURE_MB (default
256; plan-local, mmtk-core fork commit 6d7588c4c2). Re-certified splits
(hybrid attribution): bt W-ins 0.978 (SATB barrier instructions gone from the
marking windows), W-cyc 1.239, G-cyc 5.08 vs vanilla 5.72. kb reads 1.117
this round (1.04-1.12 = the sampled-split noise band). Remaining bt W-cyc
excess ~1.4G = store frontier (SB-full 0.45G) + post-GC warmth loss +
streaming L2/L3 latency at the 64 MiB nursery — the aging project.

W-cycle scoreboard after the two architectural changes (pacer + adaptive
marking): bt 1.24, kb ~1.04-1.12, LU 1.11, sp 1.08, mm 1.31.
W-instructions: 0.98-1.04 everywhere.

### Round 9: survivor aging — implemented, correct, negative for bt

MMTK_NURSERY_AGE=1 (experimental, default off; mmtk-core fork bdcd5356f9).
Church, bt-20: default nursery 12.10G->12.46G cycles; Fixed:4MiB
31.09G->33.12G (fulls 22->14 but copies 34.5M->50.9M). Outputs identical;
OFF-mode byte-identical. bt's survivor lifetimes exceed any nursery epoch —
age-1 double-copies. The W-floor insight sharpens: vanilla's tiny warm window
is affordable because its mature reclamation is incremental — premature
promotion is cheap there. Next structural lever: incremental mature sweep,
not aging. Full design + correctness map (finalizer fix shipped, remset hole
open) in NOTES 2026-08-08 (night).

### Round 10: bt and matmul endgames — mechanisms exhausted, fixes named

**binarytrees (W-cyc 1.239).** Mutator cycles are INVARIANT to: worker cache
domain (cross-socket L3: 7.05G -> 7.03G), GC count and nursery size (57 GCs /
61MiB vs 25 GCs / ~128MiB: 7.05G -> 7.09G). The gap is purely cold-frontier
allocation stores; no collector placement or pacing changes it. Bridging it
requires the warm-window architecture (small nursery affordable only with
incremental mature reclamation) — confirmed as the phase-2 target.

**matmul (W-cyc ~1.31-1.37).** Placement under bump allocation is chaotically
regime-dependent: the SAME binary measured LLC-loads regimes of 213M (current
default: nursery bump + random jitter), 739M (pre-aging layout, unjittered),
and 903-1021M (deterministic fixed pads 1..7 lines AND nonmoving-Immix
routing). Vanilla's 57M floor comes from size-class POOLS. Attempted the
faithful fix — marksweep_as_nonmoving + MMTK_MEDIUM_NONMOVING routing
(>=2056B, the stock Max_young_wosize boundary, straight to a swept free-list
space exactly like stock) — and it PANICS under Bactrian's fused marking
pauses: MarkSweepSpace::end_of_gc epilogue underflow (pending_release_packets
-1; InitialMark and FinalMark both drive common-space release). Fixing that
accounting = the matmul endgame. Routing to the DEFAULT nonmoving space
(Immix bump) instead is measured counterproductive (903M). Random jitter
remains the robust guard (protects the catastrophic regimes; cannot reach
the pool floor).

Build-system trap (recorded): runtime/libasmrun.a's bundle rule tracks
neither mmtk.n*.o nor the Rust staticlib — after ANY cargo change,
rm -f runtime/libasmrun.a runtime/mmtk.n*.o before rebuilding, else stale
GC code ships silently (cost us a phantom-panic chase tonight).

### Round 11: the bt W-G dial measured; warmer; jitter economics

**Jitter economics (user Q):** zero GC cost panel-wide (GC counts/copies/STW
byte-identical jitter on/off). Only matmul fires fillers (2307; band = the
2307 x 6160B rows; filler avg 2032B => ~33% overhead on that band, ~4.7MB
total, 0 GCs either way). LU fires ZERO fillers — float arrays take an
allocation entry that bypasses the alloc_shr hook (explains jitter's
measured no-op on LU). 16-position random (j=4) beats 6-bit at mm768
(5.79G/208M vs 5.98G/213M, ~8% waste) but REGRESSES mm800 (306M vs 253M):
default stays 6-bit (robustness across sizes). Rotating deterministic pads
(modes 24/25) measured worse than random (253M) — randomness is the point.

**bt: the W<->G dial, measured (pt-attach mutator cycles vs vanilla-W 5.79G):**
| config | mutCyc | W-ratio | gcCyc |
|--------|--------|---------|-------|
| default (61M) | 7.03G | 1.21 | 4.8G |
| default + frontier warmer | 6.89G | 1.19 | 5.5G (incl warmer) |
| Fixed:24M | 6.53G | 1.13 | 11.9G |
| Fixed:16M (L3-resident) | 6.15G | **1.06** | 13.1G |

An L3-resident (16M) frontier brings bt to near W-parity TODAY — at 2.7x G
(premature promotion). The frontier warmer (helper thread prefetching ahead
of the downward bump pointer, MMTK_FRONTIER_WARMER=1, default off) gives -2%
at the default nursery — its prefetchw lands M-state in the helper's cache,
so stores still pay a cross-core transfer — and adds NOTHING at 16M (already
warm): clean cross-validation of the store-frontier mechanism. The dial
confirms phase-2's shape: make G cheap at small nurseries (incremental
mature) and the W side is already parity-grade.

### Round 12 (phase-2 scouting): the decomposition, the existence proof, the design

**Decomposition of the n16 G-explosion (bt-20):** promotion COPIES dominate
(5.2M -> 18.9M objects; fulls only 7 -> 12). Classic incremental sweep would
fix ~nothing; the copies themselves are the cost. Aging at n16: marginal.
GenImmix-n16 == Bactrian-n16 (concurrent machinery ~1G extra).

**Existence proofs (StickyImmix + sticky_immix_non_moving_nursery feature —
in-place young marking, objects copied = 0 everywhere, outputs identical):**

| config | W (mutCyc) | G (gcCyc) | whole | vs vanilla whole 11.5G |
|--------|-----------|-----------|-------|------------------------|
| Bactrian default | 7.02G | 4.80G | 11.8G | 1.03 |
| sticky-nm default | 7.22G | 3.42G | 10.6G | 0.92 |
| **sticky-nm h384 cadence128** | 7.17G | **1.97G** | **9.1G** | **0.79** |
| Bactrian copy-nursery n16 | 6.15G | 13.2G | 19.3G | 1.68 |
| sticky-nm n16 c128 | 6.80G | 7.40G | 14.2G | 1.23 |

- In-place minors kill the promotion-copy term entirely; at default budget +
  headroom + relaxed cadence, bt runs at 79% of vanilla's total cycles
  (1 full GC; RSS ~172MB vs vanilla o500 ~140MB — the same space-for-time
  trade vanilla itself makes via space_overhead).
- The W dial persists under sticky (6.80 at n16) but survivors fragment
  recycled lines, so it warms less than the copying nursery (6.15).
- Panel spots: sticky-nm slightly WORSE than Bactrian on kb/LU/sp (in-place
  garbage -> more pressure fulls) — the win is specific to high-promotion
  workloads; a hybrid/policy choice, not a blanket replacement.

**Phase-2 design (Bactrian v2 = "concurrent sticky" mode):** in-place sticky
young marking (no copy nursery) fused with Bactrian's existing concurrent /
adaptive-STW full marking, byte-denominated cadence, and a mature-headroom
policy. Expected from the proofs above: bt-class whole-process BELOW vanilla
at W ~1.2, with the W dial (small young budget) available where W-parity
matters more than G. Implementation: a plan mode merging
plan/sticky/immix (minor path) with plan/concurrent/bactrian (full path) —
multi-session; the sticky_immix_non_moving_nursery build + knobs above are
the measurable stand-in until then.

### Round 13: the bump granule — W-parity reached with vanilla's own methodology

Root cause chain closed: mm's placement regimes (739/903/213M LLC-loads) were
the 32KB bump granule's TAIL-SKIP — ~5 rows fit, ~2KB skipped, every block
32KB-aligned => 5-position set phase-lock. Vanilla's flat 57M comes from
malloc-style CONTINUOUS size-pitch (its sizeclass pools cap below this band).
mi-malloc pools (marksweep_as_nonmoving, release-path bug fixed in cec95292be)
measured 904M — whole-line slot rounding resonates; pools are NOT vanilla's
mechanism here. The faithful fix: a larger bump granule => continuous pitch.

MMTK_BUMP_BLOCK_KB (bumpallocator, default now 512; upstream was 32): at 512KB
matmul lands EXACTLY on vanilla's floor (57.1M; mm800 64.5M vs vanilla 64M) —
and the rarer TLAB refills pay every allocating bench 2-10% of mutator cycles.

**FINAL W-cycle certification (new defaults; pt-attach, vanilla-W refs):**

| bench | mutCyc | vanilla-W | W-ratio |
|-------|--------|-----------|---------|
| kb | 3.86G | 3.95G | **0.98** |
| binarytrees @ Fixed:16M | 5.84G (3 reps ±0.01) | 5.79G | **1.009 — PARITY** |
| spectralnorm | 5.13G | 4.89G | 1.05 |
| matmul-768 | 5.35G | 4.66G | 1.15 |
| binarytrees (default nursery) | 6.73G | 5.79G | 1.16 |
| LU | 6.18G | 5.33G | 1.16 |

All with vanilla's methodology intact: copying nursery, allocation-paced
fulls, malloc-like continuous placement — no scheme departures (sticky data
retained as diagnostic only). Outputs identical everywhere.

Remaining phase-2 (G-side): bt@n16's gcCyc 14.2G (promotion copies at ~1.6k
ins/object vs vanilla's ~200-400 + full marks) — the per-slot trace-path slim
and lazy mature reclamation continue as the G work items; W is done pending
that (the n16 W-parity config costs G until then).

### Round 14: trace-path slim step 1 (phase-dynamic trusted loads) + D1 panel

Concurrent plans were statically excluded from the S1 trusted slot fast path,
paying TWO SFT lookups per traced slot (classify + load revalidation) even
inside STW pauses. Trusting now tracks the phase (on at running.is_empty(),
off before any wake; concurrent marking keeps full revalidation; gates incl.
forced-always-concurrent passed, outputs identical). Effect: bt-def G
5.18 -> 4.56G (-12%), bt-n16 G 14.2 -> 12.4G (-13%), gcIns -16%; and bt@n16
mutator now 5.74G vs vanilla-W 5.79G — W-ratio 0.99.

D1 panel at round-13 defaults (wnight8, symbol-split hybrid, pre-trusted-loads):

| bench | vanilla W/G (D1) | Bactrian W/G (D1) | W-ratio |
|-------|-----------------|-------------------|---------|
| binarytrees | 5.71/5.78 (0.503) | 6.87/5.26 (0.433) | 1.20 |
| kb | 3.96/0.56 (0.124) | 4.02/0.82 (0.170) | 1.02 |
| spectralnorm | 4.88/0.00 | 5.32/0.08 (0.015) | 1.09 |
| LU | 5.35/0.01 | 6.40/0.27 (0.041) | 1.20 |
| matmul | 4.67/0.00 | 5.66/0.03 | 1.21 |
| nbody / fannkuch / mandelbrot | ~0 G | ~0 G | 1.00 |

Bactrian's GC fraction is now BELOW vanilla's on bt (0.433 vs 0.503) with
whole-process cycles within 4%. Next slim steps: immediate pre-filter in
scan_ocaml_object (skip FieldSlot construction for tagged ints — vanilla's
oldify checks Is_block first), then the mark-bit load-before-CAS in the
concurrent trace.

### Round 15: matmul residual characterized to completion; pre-filter refuted

**matmul (W ~1.15-1.21): GC-side complete.** At current defaults every
countable resource is at vanilla parity: instructions 14.05 vs 13.88G, L1
hits 3.31 vs 3.23G, L2 hits 850 vs 849M, L3 57.0 vs 56.4M, L3-miss 0 both,
SB-full 0.2% both, TLB nil, startup floor 0.017 vs 0.007G, THP tested both
ways (ON wins everywhere: 6.08 vs 6.93G). MLP counters localize the residual:
memory-busy time is IDENTICAL in absolute cycles (4.38G vs 4.42G pending),
but vanilla overlaps compute inside it (94% pending coverage, stalls_mem
10.5%) while ours bunches loads worse (72% coverage, stalls_mem 18.5%) —
an instruction-scheduling/overlap artifact of layout below the counter
level. Further W-closure is program-structure territory (loop blocking),
out of scope: the benchmark is identical on both sides by methodology.

**Immediate pre-filter in scan_ocaml_object: refuted on bt** (G 4.56->4.70G;
tree nodes are pointer-only, so the filter pays a duplicate load with zero
skips) — reverted. classify already short-circuits immediates before any
SFT work, so the filter can only win on scans where slot-buffer traffic
dominates; none measured. Next G item: the per-object COPY path (alloc_copy
dispatch + post_copy metadata, ~1.6k ins/object vs vanilla oldify's
~200-400) — the dominant remaining term at n16.

### Round 16: copy-path economics + G-slim progress at n16

Worker profile at n16 (self): forwarding+metadata CAS pairs 11.6%, THREE
side-metadata ops per promoted object (unlog + object mark + line mark)
11.5%, forward_object 4.4%, slot machinery ~10%, header store 2.8%.

All-in per-object cost: Bactrian ~650 cy/object (11.9G / 18.9M) vs vanilla
~300 (5.78G total G over MORE promotions — its window is 8x smaller). The
delta is architectural: vanilla's domain-local minor uses ZERO atomics and
ZERO side metadata; MMTk pays them for parallel-tracer generality even at
MMTK_THREADS=1.

G-slim ledger at n16: 14.2G -> 12.4G (phase-dynamic trusted loads, -13%)
-> 11.9G (copy-telemetry counter gated behind MMTK_VERBOSE/PAUSE_LOG,
-0.45G). W held at 5.91G (ratio 1.02).

Identified next lever (fork design, not yet built): UP-trace mode — when
worker count == 1 AND the pause is STW AND concurrent marking is inactive,
exactly one thread traces, so forwarding-word installs and mark/unlog
side-metadata writes can use plain (non-atomic) ops, vanilla's regime.
Estimated from the profile: ~20-25% of the remaining per-object cost.
Plan-safety: gate at trace-context construction; LXR/parallel paths keep
atomics verbatim.

### Round 17: LU cleared, n16-for-all sweep, matmul annotated to the instruction

- LU: no regression (fresh def 6.23G == history; panel's 6.50 = instrument
  spread). Post-slim n4 config improved to 5.49G => W-ratio 1.03.
- Fixed:16M for all: bt 1.02, sp 0.96 (below vanilla, beats default), kb
  0.98, lu 1.07, mm CATASTROPHE 41.4G (8.9x): matmul's live matrices
  (~19MB with jitter) exceed the nursery — under Bactrian ALL medium
  objects transit the nursery, where vanilla pretenures >Max_young_wosize
  blocks straight to the major heap. A global 16M default is impossible;
  per-workload dial or medium-object pretenuring (the pools work) is the
  principled route.
- Per-bench W-optimal today: bt n16 1.02 | kb def 0.98 | lu n4 1.03 |
  sp n16 0.96 | mm def 1.15-1.24.
- matmul, annotated: 96.7%/96.4% of both sides' cycles in the SAME
  matrix_multiply function; the hot chain is the y.(k) bounds-check
  header load (movq -0x8(reg) -> shrq/sarq) + element load. The SAME
  sarq instruction absorbs 31.1% (vanilla) vs 57.9% (Bactrian) of the
  function — identical code, identical hit counts at every level,
  different effective latency/overlap on one dependent load chain.
  This is the whole 24%: layout-driven load-bunching on the row-header +
  element pair, below counter attribution (MLP data round 15).

### Round 18: matmul — the complete refutation ledger

New probes: TLB regime (vanilla eats 889M STLB hits ~8cy each IN the load
chain and still wins; our THP run has 0.3M and loses; our 4K run pays both
STLB 879M AND stays slow => physical-page entropy REFUTED); IP-stride
prefetch via mem_load_retired.fb_hit (7.6M vanilla / 7.2M jittered /
7.3M exact-pitch — equal => prefetch-difference REFUTED).

Refuted in total: LLC conflicts (fixed by granule), L1/L2/L3 hit counts
(equal), TLB (both regimes), physical entropy, store-buffer, MLP occupancy
(memory-busy time equal), fill-buffer/prefetch, instruction count, startup,
4K-aliasing (2.6M, negligible), THP direction. The ~20% (best config
noj+granule2048: 5.65G = 1.20) sits on one two-load dependent chain
(row header + element) as pure effective-latency/scheduling difference
below PMU attribution — L1D banking / replay territory.

Vanilla's established differences on this workload: (1) PRETENURING —
rows never transit the minor heap, are never copied by any GC (malloc'd
individually, >sizeclass-max); (2) exact-fit malloc placement at perfectly
periodic 6192B (96.75-line) pitch; (3) it tolerates the 4KB-page STLB cost
(889M hits) our THP policy avoids. The remaining actionable replication:
exact-fit pool pretenuring for the medium band (mi-bins failed on slot
rounding; an exact-size boundary-tag pool would replicate malloc), bundled
with the pools/lazy-sweep phase-2 work.

### Round 19: the matmul residual — my placement story REFUTED by experiment

Challenged (rightly) on whether "MMTk blocked allocation vs vanilla exact-fit
malloc" was established or hallucinated. Verification campaign:

CONFIRMED: vanilla mallocs this band individually (shared_heap.c:504/481,
SIZECLASS_MAX=128 words; ltrace: 914 mallocs >=2KB for matmul-300 vs OUR 0 —
MMTk never calls malloc for objects).

REFUTED — the causal test: MMTK_TEST_MALLOC_MEDIUM (measurement instrument:
>=2056B blocks via plain malloc as out-of-heap foreign objects — literal
glibc placement, checksums identical) changed NOTHING: 5.73-5.78G vs stock
5.82-5.88G, vanilla 4.65-4.71G. Exact-fit placement is NOT the mechanism;
the pools/malloc_ms quest would NOT close matmul.

Also refuted this round: DSB/MITE frontend (fork binary runs the loop from
the legacy decoder — real, but alignment-nudged pads restoring full DSB
service left cycles unchanged); DSB+malloc combined (5.59G best, gap
persists); branch misses (0.7M vs 0.9M); uop replays (issued==executed==
retired; ours retires FEWER uops); frequency (3.18 vs 3.12 GHz measured via
task-clock); address-layout lottery (10 ASLR draws per side: v 4.48-4.71,
d 5.52-5.88 — disjoint distributions, systematic).

VERDICT: MMTK_PLAN=NoGC = 5.80G — the residual is identical across NoGC/
Immix/GenImmix/Bactrian (5.6-6.1G). It is ambient to the FORK RUNTIME
PROCESS, independent of GC, barriers, plan, placement, alignment, frequency
and layout, concentrated on one dependent-load chain (identical 101-instr
function, byte-for-byte). Cause below PMU visibility; next tools would be
uncore PMU / VTune / pagetable-structure analysis — a separate
investigation, NOT a GC work item.

Consequences: (1) matmul's ~1.2 W-ratio should be reported as a fork-runtime
ambient effect, not a GC/placement cost — the GC-controlled dimensions are
at parity (LLC floor, counts equal); (2) medium-object pretenuring loses its
matmul justification; its remaining (real) value is liberating the nursery
dial (mm@16M nursery = 8.9x catastrophe stands); (3) 17 falsified hypotheses
are the strongest possible evidence the instruments and methodology work.

### Round 21: UP-trace lands — bt whole-process 0.90x vanilla

Single-tracer plain-op mode (util::up_trace; binding arms per pause when
worker==1 + quiesced + no marking window). Forwarding claim-CAS skipped,
SeqCst stores -> plain MOVs, side/header metadata primitives take non-atomic
twins. Locked RMWs per copied object 5.7 -> 1.1 (112.6M -> 21.3M); worker G
-22-25% (def 4.49->3.48G, n16 12.26->9.20G); D3 totSTW 1468->1169ms.
CERTIFIED: bt whole-process 10.30G +-0.01 vs vanilla 11.5 = 0.90x, outputs
byte-identical across all configs incl. forced-concurrent and T=4 exclusion
gates. bt@16M: W 5.73 = 0.99 ratio. Two stale-deploy traps caught mid-
session (visible fetch/reset now mandatory). Remaining locks (21M): packet/
queue ops — next slim; fulls' marks -> incremental mature (unchanged).

### Round 22: the vanilla-matching frontier (post-UP nursery curve)

Directive: match vanilla's shape, not beat aggregates. Post-UP curve (bt-20):

| nursery | W (ratio) | G | pauses | minors n/ms | fulls n/ms | p50 |
|---------|-----------|---|--------|-------------|------------|-----|
| 16M | 5.72 (0.99) | 9.6 | 233 | 220/1786 | 13/1298 | 0.35 |
| 8M | 5.46 (0.95) | 11.7 | 466 | 450/2217 | 16/1543 | 0.33 |
| 4M | 5.44 (0.95) | 17.8 | 998 | 975/3551 | 23/2131 | 0.33 |
| 2M | **5.38 (0.94)** | 19.9 | 2330 | 2303/3955 | **27**/2431 | 0.28 |
| vanilla | 5.74 | 5.76 | 3554 | 1763/1138 | 29 cycles/832ms slices | 0.04 |

MATCHED at 2M: W (0.94 — warmth confirmed: W improves monotonically as the
nursery shrinks now that the livelock is dead and UP is in), D2 pause count
(2330 vs 3554), and the MARK-CYCLE CADENCE (27 fulls vs vanilla's 29 major
cycles — the byte-pacer reproduces vanilla's law exactly).

REMAINING GAP (verified, single-axis): collection economics.
- Per-minor-object: ~340 cy vs vanilla 80 (post-UP; was 415). Left: 3x
  side-table addressing arithmetic, packet machinery, scan/SFT dispatch.
- Per-mark-cycle: 90ms STW fulls vs vanilla's ~29ms-equivalent sliced
  incrementally. Same volume, 3x machinery cost + un-sliced delivery.
Targets: per-object -> ~100-150cy (metadata fold + packet slim); mark
slicing (incremental mature) for the D3 tail (140ms max vs 15ms).

matmul@16M verified at 8.11G (1.75x): mutator-side nursery thrash of the
19MB init burst (vanilla pretenures the band; the pools gate). Unchanged
by UP as predicted. mm/LU/mm default-nursery ~20%: the fork-ambient
runtime effect (NoGC-reproducible), documented refutation ledger.

### Round 23: Max_young_wosize pretenuring — matmul joins the panel floor (2026-08-10)

The pools gate closed. The >=2056B band is now born in the mature Immix
space under Bactrian (default ON; MMTK_MEDIUM_NONMOVING=0 reverts), stock's
Max_young_wosize law. Three mechanisms had to land together:

1. **Placement** (mmtk-core ebb8f58e3c): NonMoving semantic -> reserved
   plan-local ImmixAllocator on the mature space; unlog-at-birth;
   allocate-as-live during marking windows. (Two worker panics en route:
   mismatched reserved sets between allocator/space mappings, and the
   common FreeList allocator's release-packet handshake — see NOTES.)
2. **Phase** (mmtk-core 2a390e0fc8): overflow blocks re-align every 32KB, so
   the medium stream's cache-set phase resets each block — object pads
   cannot fix that (904M unpadded / 205M padded / 57M floor LLC-loads).
   Rotating each fresh overflow block's start phase by one line (16
   positions, mutator-only) hits the floor: 58M.
3. **Remset economics** (runtime 9027ed6b9): initialising stores of
   IMMEDIATES no longer enter the remembered set (stock's own filter) —
   born-mature int arrays were buffering per-slot entries: 46MB retained
   modbuf on matmul, RSS 81 -> ~29MB.

wpret2 battery (church, 3-rep medians, T=1, 192MB), cycles vs vanilla:

    bench    v       d0(off)  d1(ON)   d1@16M  d1@2M
    matmul   1.00x   1.25x    1.08x    1.07x   1.05x
    LU       1.00x   1.24x    1.24x    1.16x   1.33x
    bt       1.00x   0.91x    0.91x    —       —
    kb       1.00x   1.02x    1.02x    —       —

matmul is now NURSERY-INDEPENDENT — the same shape as vanilla, whose
mediums never see the minor heap either. Knob-off is an alignment lottery
(the same build drew 118M- and 900M-class LLC regimes across batteries);
pretenure removes the die roll. bt/kb worker shares identical to 0.1pp; LU
neutral (its matrix is allocated once — LU's residual is the fork-ambient
item, not medium placement). At the vanilla-matched 2MB-nursery operating
point (round 22), matmul improves 1.80x -> 1.05x.

### Round 24: G economics — vanilla's oldify protocol lands in the worker (2026-08-11)

Item-1 campaign: bridge per-minor-object collection cost (~340 cy at round 23)
toward vanilla oldify's ~80. Fixed workload throughout: bt-20 @ Fixed:16M,
233 GCs / 13 fulls / 19,676,902 promoted objects, byte-identical output.

Landed, in order (each gated on the full battery + T=4 + forced-concurrent +
GenImmix/SemiSpace canaries):

1. **Thin LTO + codegen-units=1** — the binding's slot/scanning layer crossed
   a crate boundary outlined (FieldSlot::from_address alone 6% of worker).
   GC 3100 -> 2686ms. Matched-pair discovery: fat-vs-thin (same semantics,
   different layout) re-rolls the MUTATOR-side draw on matmul/kb/fannkuch —
   the fork-ambient mechanism is a code-layout lottery, now reproducible at
   will (item-4 handle).
2. **inline(always) on side-metadata paths** — neutral (cost re-homed to the
   dispatch layer): proof the expense was the dispatched work, not calls.
3. **Header-sentinel forwarding** — stock oldify's value-range protocol under
   UP: header word >= heap_start ⟺ forwarded; zero side-metadata traffic in
   the forwarding path (status load per visit + FORWARDED store + new-copy
   clear all gone). Broke kb first: the slot.rs Infix_tag disambiguator read
   the no-longer-written side bits — a forwarding pointer ending 0xf9 parsed
   as an infix header (garbage parent offset, worker SIGSEGV). Fixed by
   making that check value-range too (valid in all modes; kills its side
   load + is_mapped probe). GC 2686 -> 2544ms; bt whole-process 0.85x.
4. **UP direct-trace closure (nursery)** — the whole transitive closure in
   one packet with an explicit work list; per object the packet path's exact
   protocol. GC 2544 -> 2382ms. Copy order becomes parent-then-children.
5. **REFUTED x2 — the same drain for FULL traces**: layer-at-a-time
   materializes the full-heap BFS frontier (millions of objects, one
   cache-cold Vec): 2382 -> 2581ms. LIFO variant pays a pop_nodes Vec swap
   per object: 3426ms — worse than campaign start. Both reverted; the packet
   system's 4096-object interleaved units are the RIGHT structure at
   full-heap scale. Fulls stay on packets (item 2 restructures them anyway).

Net: **GC time -23%** (3100 -> 2381ms; ~340 -> ~260 cy/object), bt
whole-process **0.84x vanilla** (9.69-9.74G vs 11.48G). Panel: nbody/
fannkuch/mandelbrot ~1.0x; spectralnorm 1.08x (item 3); matmul 1.17x / kb
1.06x / LU 1.26x — mutator-side layout draws of this build (all counters
flat; the lottery, item 4). Remaining G buckets: the drained closure itself
(scan + trace dispatch + copy, now intrinsic MMTk-vs-hand-loop overhead) and
full-GC marking — both better addressed by item 2's mark-cycle slicing than
by further micro-folds.

### Round 25: sliced-STW marking — the D3 tail matched (2026-08-11)

Item-2 campaign. Design: stock's mutator mark slices, executed as bounded
STW quanta by the single worker inside nursery pauses (2ms budget,
MMTK_MARK_SLICE_MS), one per minor — allocation-paced exactly like stock.
Marking work parks in a plan-owned queue (never the Concurrent bucket);
FinalMark drains the remainder unbudgeted in its Closure stage. Default ON
(MMTK_MARK_SLICED=0 restores worker-concurrent/adaptive-STW). UP-trace
stays armed through the whole cycle: quanta run world-stopped on the one
worker, so the tracer is single — the item-1 economics apply to marking.

Why not worker-concurrent (measured, bt@2M): pauses drop 122 -> 14ms but
the mutator pays +7.9G cycles in cross-core LLC interference (marker
streams the live set through shared L3 while the mutator runs) — vanilla
avoids this by serializing slices with mutation, and so do we.

Two pacing bugs found by cadence tracing en route: (1) the scheduler's
parked-workers self-request raced the marking-state clear at FinalMark end
-> zero-allocation second collection -> emergency STW Full (15/run,
spuriously resetting the pacing); (2) FinalMark never reset the pacing
baseline (was_full only recognized Pause::Full) and the allocation budget
was measured from cycle END not START. Both fixed (self-request suppressed
under sliced; completion resets baseline, start resets budget — a STW Full
is both at once, that mode bit-identical).

FULL SHAPE COMPARISON (church, wshape battery, vanilla o=500 vs sliced
default, T=1, 192MB pinned):

D1 (cycles/ins ratios):
  bt 0.84x/0.64x | nbody 1.00 | fannkuch 1.03 | spectral 1.07 |
  mandelbrot 0.97 | matmul 1.16* | LU 1.26* | kb 1.05  (*layout draws)

D3 (pause streams):
  bt@2M:  2411 minors mean 1.66ms p95 6.7 MAX 8.0ms; 15 cycle pauses
          mean 3.6ms max 6.8ms.  VANILLA: 3554 pauses mean 0.55 MAX 15.2ms.
          The 78-122ms Full tail is GONE; whole-run max is BELOW vanilla's.
  kb def: max 8.8ms. bt def: minors to 67ms (64MB-nursery evacuations —
          the default-config minor size, not cycle work).

D2 (cycles + attribution): bt@2M 15 cycles, trigger 321x mature-pressure /
  6x cadence vs vanilla's 29 cycles at o=500. The pacing LAW is now
  structurally identical (pressure % over post-cycle baseline + allocation
  budget from cycle start); the 15-vs-29 period delta is the operating
  point: our post-cycle baseline includes the marking window's floating
  garbage and the 40% margin compounds over it. Calibration of
  MATURE_PRESSURE_OVERHEAD_PCT (the space_overhead analog) against
  vanilla's o is the remaining dial — a constant, not a mechanism.

W: bt@2M whole-process 18.0G vs 20.6G STW-fulls (-13%: UP armed through
IM/FM + quanta) vs 30.7G worker-concurrent. Total STW 4067ms vs vanilla
1970ms — the residual is mean minor cost (1.66 vs 0.55ms), i.e. item-1's
per-object economics, not the tail.

### Round 26: JCC mitigation on both toolchains — the lottery neutralized (2026-08-12)

Both compilers rebuilt with `-mbranches-within-32B-boundaries` (as) /
`-Wa,...` (cc): the assembler pads so no branch or macro-fused cmp+jcc
pair touches a 32-byte boundary — the complete fix for the Skylake JCC
erratum's DSB exclusion (NOTES 2026-08-12). Vanilla's SOURCE stays
pristine; this is a build-flag experiment, restore = reconfigure with
default flags (one command, documented here).

Verdict battery (church, 3-rep medians, outputs byte-identical):

  matmul DSB coverage: v 99.3% / B 99.2% — IDENTICAL (was 99.4% / 1.7%).
  Both hot functions land mod32=0.

  bench       before -> after     note
  bt          0.84x  -> 0.82x
  nbody       1.00x  -> 1.00x
  fannkuch    1.03x  -> 1.00x     BOTH sides -16% (v 10.59 -> 8.93G!)
  spectral    1.07x  -> 1.10x     unchanged-class; NOT a JCC bench
  mandelbrot  0.97x  -> 1.00x
  matmul      1.16x  -> 0.98x     RESOLVED (v 5.11G, B 5.00G)
  LU          1.26x  -> 1.22x     barely moved, as predicted (backend)
  kb          1.05x  -> 0.98x     resolved

Two findings beyond the fix:

1. **Vanilla was a lottery victim too.** fannkuch's vanilla dropped 16%
   (10.59 -> 8.93G) under mitigation — its stock build had its own JCC
   hit all campaign, invisible because we only ever compared ratios.
   "Vanilla always won its draws" is refuted: the mitigated build is the
   first FAIR W comparison this campaign has had. (matmul's vanilla went
   4.63 -> 5.11G: its lucky draw was real, and the padding NOPs cost the
   hot loop ~10% on both sides equally — the price of determinism.)
2. **The residual gap list is now short and mechanistic**: LU 1.22x
   (backend: mem 0.7->8.7 / core 3.3->11.5 — next specimen) and
   spectralnorm 1.10x (memory-bound rise, all placement hypotheses
   refuted). Everything else is at or below parity: bt 0.82, matmul 0.98,
   kb 0.98, fannkuch/nbody/mandelbrot 1.00.

Methodology note for the paper: single-build W comparisons at the <15%
granularity are unsound on JCC-era x86 — a 16-byte layout shift moves a
bench 16%. Either build with the mitigation on both sides (deterministic,
small uniform NOP tax) or report W across link draws.
