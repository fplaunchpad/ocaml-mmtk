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
- **Hosts.** This laptop iterates; **church** (homogeneous Xeon, 26.04, matching the host's
  gcc 15.2 / glibc 2.43) is the authoritative campaign. Results are tagged by `host` and
  never pooled. The `church` numbers in `SCALABILITY.md` §11 are retracted for a
  contaminated *build*, not a bad host.
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
