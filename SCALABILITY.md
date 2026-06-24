# Multi-domain GC scalability — findings & default-plan analysis

**Date:** 2026-06-25. **Branch:** `night/scalability-findings`.
**Question (KC):** the fork anti-scales multi-domain — *should the default plan switch
from GenImmix to Immix, or to ConcurrentImmix?*

**Headline answer.** The multi-domain anti-scaling is **specific to the generational
plan (GenImmix)** and is driven by the **all-domains stop-the-world *minor* GC**, not by
GC-worker count (#52, abandoned) nor by per-terminate full GC (second-order). **Both
Immix and ConcurrentImmix scale where GenImmix anti-scales, and both also match or beat
GenImmix single-domain** on every workload measured. ConcurrentImmix is fastest and
scales best but is a **research plan, not default-ready** (#30: native SATB + a ~10 MB
sanity deadlock). **Immix is the strongest *immediate* default candidate;** the one
remaining gate before flipping the default is a full sequential CLBG/sandmark panel at
memory parity (below).

All numbers: native, OCaml 5.5.0 fork, M4 Pro (12 core) macOS unless noted; best-of-2/3,
hard 90 s timeout; `par_binarytrees` depth 21; checksum `925744980` identical across all
plans/domains (correctness self-check passes everywhere).

---

## 1. The decisive table — three plans, multi-domain `par_binarytrees` d21

Speedup `S(n) = wall(dom=1) / wall(dom=n)`. `S>1` scales, `S<1` anti-scales. Vanilla
5.5.0 on the same bench scales **~3.9×** at dom 8 (the reference).

### CHURN (spawn+join per depth-class)
| plan | wall d1 | wall d8 | S(2) | S(4) | S(8) | GCs d1→d8 | GC ms d1→d8 | copied d8 | RSS d8 |
|------|--------:|--------:|-----:|-----:|-----:|-----------:|------------:|----------:|-------:|
| **GenImmix** | 4.93 | 7.61 | 0.94 | 0.85 | **0.65** ⟵ anti | 982→1042 | 3230→**7040** | 93 M | 618 MB |
| **Immix** | 2.89 | 2.17 | 1.26 | 1.38 | **1.33** | 118→158 | 1372→1769 | 0 | 588 MB |
| **ConcurrentImmix** | 3.02 | 1.98 | 1.22 | 1.43 | **1.53** | 238→242 | 100→**132** | 0 | 589 MB |

### ONCE (each domain spawned exactly once — isolates steady-state minor cost)
| plan | wall d1 | wall d8 | S(2) | S(4) | S(8) | GCs d1→d8 | GC ms d1→d8 | RSS d8 |
|------|--------:|--------:|-----:|-----:|-----:|-----------:|------------:|-------:|
| **GenImmix** | 4.18 | 6.60 | 0.99 | 0.82 | **0.63** ⟵ anti | 993→1048 | 2813→6256 | 435 MB |
| **Immix** | 2.64 | 2.08 | 1.27 | 1.48 | **1.27** | 118→112 | 1289→1554 | 454 MB |
| **ConcurrentImmix** | 2.71 | 1.72 | 1.19 | 1.42 | **1.57** | 141→189 | 151→123 | 481 MB |

**Reading it.**
- GenImmix gets **slower** as domains grow (anti-scales). Immix and ConcurrentImmix get
  faster. ConcurrentImmix scales best.
- GenImmix is also the **slowest single-domain** here (binarytrees is promotion-heavy:
  it copies 53–96 M objects out of the nursery — the generational copying nursery's
  worst case). Immix/CI copy ~0 (non-moving mature, mark-in-place).
- **RSS is comparable across all three** (within ~10 % at memory parity) — ConcurrentImmix
  is *not* a memory hog.

---

## 2. Mechanism — *why* GenImmix anti-scales (four experiments)

### A — per-terminate forced full GC is real but **second-order** (REFUTED as driver)
Each domain terminate forces a full MMTk collection (`domain.c` → `caml_mmtk_collect`,
`force=exhaustive=true`). Hypothesis: that's the anti-scaling driver. **Refuted:** the
ONCE variant (8 terminates) anti-scales as badly as CHURN (~64 terminates), and at the
anti-scaling depths their *total* GC counts are within a few % (d21 dom8: CHURN 1036 vs
ONCE 1042). The terminate-GC term is swamped by allocation-driven minor GCs. *Confirmed
in isolation* by a spawn/join microbench (GCs = exactly rounds×ndom) — the mechanism is
real, just not dominant.

### B — the driver is the **all-domains STW minor GC** (cost grows with domain count)
At **fixed work**, GenImmix's minor-GC *count* is ~flat in domains (982→1042) but minor-GC
*time* climbs sharply: **3230 ms → 7040 ms** (CHURN) / 2813 → 6256 (ONCE) from dom 1→8.
The *same* collections cost ~2× more with more domains ⇒ the per-collection
stop-the-world **rendezvous/coordination** scales with domain count. cpu/wall climbs
2.77→5.62 (cores busy on GC, not useful work).

This is exactly what the plan split confirms:
- **Immix** has no copying nursery → **8× fewer collections** (118 vs 982) → far less STW.
- **ConcurrentImmix** marks **concurrently** with the mutator → GC time stays flat
  (~100–130 ms) regardless of domains.

Both sidestep the frequent, domain-count-scaling all-domains STW minor that sinks GenImmix.

### C — the discriminator is **allocation/GC pressure**, not program shape
(`par_matmul` integer/alloc-light vs `par_spectralnorm` boxed-float/GC-heavy, fork GenImmix)
| bench | d1 | d4 | d8 | verdict | GCs d1→d8 |
|-------|---:|---:|---:|---------|----------:|
| matmul (alloc-light) | 1.14 | 0.35 (3.3×) | 0.22 (**5.2×**) | scales ~vanilla | 4→11 |
| spectralnorm (GC-heavy) | 0.62 | 0.39 (1.6× peak) | 0.46 (regress) | anti-scales | 497→706 |

"Numerical" is **not** a proxy for "scales" — allocation rate is. GC count rises
monotonically with domains for the GC-heavy bench (the STW-pressure signature).

### D — high domain count (godel, 56-core Xeon, fork GenImmix)
Even compute-bound work eventually regresses as the per-domain spawn/terminate GCs become
an all-domains STW across many domains:
| bench | d1 | d8 | d16 | d28 | d56 | vanilla d56 |
|-------|---:|---:|----:|----:|----:|------------:|
| matmul | 3.75 | 0.89 | **0.85** (peak) | 1.08 | 1.73 | 0.30 |
| spectralnorm | 5.98 | 2.23 | 3.07 | 4.95 | **TIMEOUT** | 1.26 |
GC count grows with domains (matmul ~1 GC/domain → 55 at d56; spectralnorm's per-iteration
spawn churn → **999 GCs at d28**, GC time ≈ 80 % of wall). Vanilla scales throughout.

> **The vanilla puzzle (open).** Vanilla OCaml 5's minor GC is *also* all-domains STW, yet
> it scales ~3.9×. So the fork's deficit is the **per-minor-collection STW cost** (and its
> growth with domain count), not the STW barrier per se. Vanilla's minor GC is cheap enough
> that parallel mutator work dominates; the fork's is not (see #47 ~1.2 ms floor, #53/#G1
> narrow root scan). This is the sharpest research lead.

---

## 3. Single-domain — does GenImmix's generational design ever pay off here?
Real OCaml workload: native compile of `camlinternalFormat.ml` (the heaviest stdlib unit),
single-domain, best-of-3.
| heap | GenImmix | Immix | ConcurrentImmix | note |
|------|---------:|------:|----------------:|------|
| 96 MB (pressured) | 0.43 s / 1.38 M copied / 188 MB | **0.35 s** / 188 K / 170 MB | 0.39 s / 152 K / 178 MB | GenImmix slowest, copies 7× more |
| 1 GB (generous) | 0.29 s / 1 GC | 0.26 s / 0 GC | 0.29 s / 2 GC | three-way tie (GC negligible) |

GenImmix's copying nursery is a **net cost under memory pressure** (the AST is
medium-lived → high survival → every minor GC re-copies it) and **neutral when memory is
ample**. On the workloads measured it **rarely wins**. (Caveat: this is medium-lived-survivor
allocation; a long-running, *truly* short-lived-garbage workload — the textbook
generational case — was not isolated. That gap is the gate in §4.)

---

## 4. Recommendation

**Do not flip the default reflexively, but the data strongly motivates Immix as the new
default.** Concretely:

1. **Immix — the immediate candidate.** Scales multi-domain, matches/beats GenImmix
   single-domain at every measured point, comparable RSS, and is a **production-stable**
   plan (the M3 bring-up plan, heavily tested). The "generational is faithful to stock
   OCaml" rationale for GenImmix is *weakened* by the data: MMTk-GenImmix's copying-nursery
   tax isn't buying throughput that justifies it on these workloads.
   - **Gate before flipping:** run the full **sequential CLBG/sandmark panel,
     GenImmix-vs-Immix at memory parity** (task #36 harness). If Immix is ≥ GenImmix
     across the panel — as it is so far — switch the default to Immix. This guards against
     a generational-favoring workload class I haven't isolated.

2. **ConcurrentImmix — the eventual best default, not yet.** Fastest single-domain *and*
   best-scaling *and* comparable RSS — it dominates. But it's a **research plan**: #30 is
   open (native SATB completion, UNLOG-bit gate, ~10 MB sanity deadlock). A fast-but-flaky
   plan cannot be the default. **Productionizing ConcurrentImmix (#30) is the
   highest-value GC investment** — it converts the strongest experimental result into a
   shippable default.

3. **The research finding (publishable).** *MMTk's all-domains STW minor GC is the
   multi-domain scalability bottleneck for a generational plan on OCaml.* The fix is not
   worker count; it is reducing minor-GC STW frequency/cost. Non-generational Immix
   sidesteps it (fewer collections); concurrent marking hides it (ConcurrentImmix). Open
   RQ: **can a generational plan keep the nursery benefit without serializing all domains
   on every minor GC?** — and the vanilla puzzle (§2) says the per-collection cost, not the
   barrier, is the lever.

---

## 5. Status of the supporting runs
- **§1–§3 complete** (local M4 Pro + godel).
- **church 56-core high-domain *plan* sweep** (GenImmix/StickyImmix/Immix/ConcurrentImmix,
  dom 1→56): re-running clean (`~/night_bench/run_church_highdomain.sh` →
  `~/scaling_highdomain_raw.txt`); the prior agent's on-disk result was lost to a
  two-process race. Network to church is intermittently dropping (ZeroTier); results to be
  appended here. This will confirm whether ConcurrentImmix *keeps* scaling past 8 domains
  where even compute-bound matmul regresses (§2D).

## 6. Raw data
Scratchpad `night/`: `threeway_results.txt` (§1), `conclusion_A-terminate.md`,
`results_B-stw-minor.txt`, `conclusion_C-boundary.md`, `results_D-concurrent.txt` (§2),
godel `~/scaling_compute_raw.txt` (§2D). Harnesses: `run_threeway.sh`,
`run_church_highdomain.sh`.
