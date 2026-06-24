# Multi-domain GC scalability of `mmtk-ocaml` — findings

**TL;DR.** On allocation/GC-heavy parallel workloads the MMTk fork's default plan
(GenImmix) does not just fail to scale across domains — it *anti-scales*: adding domains
makes a fixed amount of work **slower**, while stock OCaml 5.5.0 speeds up ~3.9×. The
cause is **not** the GC worker-thread count, and **not** the forced full GC we do on every
domain terminate. It is the **all-domains stop-the-world (STW) collection pause itself**:
every MMTk collection stops *all* domains, and for the copying-nursery plans the in-pause
work (root scan + copying the survivors) **grows with domain count**, so more domains means
a longer global pause repeated on every collection. The plans whose pause work does *not*
grow with domains — non-generational **Immix** (marks in place, far fewer collections) and
the concurrent-mark **ConcurrentImmix** (trace off the critical path) — hold flat or scale
up. The clean, already-implemented win is to move marking off the STW path: ConcurrentImmix
turns S(8) ≈ 0.64 (anti-scaling) into S(8) ≈ 1.65–1.9 (scaling) on the same benchmark with
identical checksums; Immix is the production-ready intermediate.

Host: Apple M4 Pro / macOS, 12 cores (and godel, 56-core Xeon, for the high-domain sweep).
Primary benchmark: `par_binarytrees` (stdlib-only `Domain.spawn` port of the
Benchmarks-Game program), depth 21, a fixed iteration count **divided** across domains (so
it *should* scale; stock does). Wall = best-of-2/3, hard 90 s per-run timeout. Speedup
S(n) = wall(1 domain) / wall(n domains); ideal = n, anti-scaling = S < 1. The checksum is
domain-count-independent and was identical (`925744980`) in every cell that completed, so
the comparisons are correctness-valid and the scaling differences are not a miscollection
artefact.

---

## 1. Headline finding

The fork's multi-domain anti-scaling is **STW-pause-bound, not worker-count-bound.**

A fixed workload run on more domains does the *same* allocation and produces the *same*
survivors, so a collector that parallelised cleanly would keep total GC cost roughly flat
and let the extra cores cut wall time. Instead, on the copying-nursery MMTk plans the
measured GC time **rises monotonically with domain count at fixed work** — GenImmix
`par_binarytrees` d21 spends ~2.6 s in GC at 1 domain and ~5.9 s at 8 domains for the
*same* trees — and that rising GC pause is a global stop-the-world barrier, so it serialises
the mutators and wall time goes *up* with more domains. Stock OCaml, whose minor collection
is also STW but whose per-collection pause does not blow up with domains here, scales ~3.9×.

Two "obvious" candidate causes were tested and **rejected**:

- **It is not the GC worker thread count.** An earlier attempt to scale GC workers with
  running domains was abandoned precisely because it moved nothing: the signal is the pause
  *content*, not the number of GC helpers (Experiment 3 shows scaling recover when the
  pause-trace is moved off the critical path with worker count unchanged).
- **It is not the per-domain-terminate forced full GC.** We force one full collection on
  each domain terminate (a correctness fix). Removing ~7/8 of those terminates changes wall
  time by <20 % and leaves the anti-scaling fully intact (Experiment 1).

The discriminator across plans is mechanical and consistent: **any plan that does
copying/tracing work *inside* the STW pause anti-scales; any plan that does not, scales.**

---

## 2. Plan matrix — `par_binarytrees` depth 21, all native plans

Best-of-2 wall (s); S(8) = wall(d1)/wall(d8); GC stats are `gcs / gc_time_ms` (from
`MMTK_VERBOSE`). `gc_time_ms` is aggregate CPU-in-GC summed across workers and overlapping
the mutator, so use it for cross-config *ratios*, not as a literal wall fraction. Native
plans only (GenImmix/Immix/StickyImmix/GenCopy/SemiSpace/ConcurrentImmix all alias the
TLAB nursery; MarkSweep/NoGC are not native-capable here).

| plan | d1 | d2 | d4 | d8 | **S(8)** | verdict |
|---|---:|---:|---:|---:|:---:|---|
| **stock** OCaml 5.5.0 | 3.160 | 1.595 | 1.073 | 0.803 | **3.93** | scales (reference) |
| **GenImmix** (fork default) | 3.937 | 4.185 | 4.869 | 6.189 | **0.64** | anti-scales |
| **StickyImmix** | 4.865 | 5.208 | 6.479 | 8.297 | **0.59** | anti-scales (worst) |
| **GenCopy** | TIMEOUT | TIMEOUT | TIMEOUT | TIMEOUT | — | off the chart (>90 s even at d1) |
| **SemiSpace** | 17.319 | 18.663 | 20.624 | 22.990 | **0.75** | anti-scales atop a vast GC cost |
| **Immix** (non-generational) | 2.729 | 2.389 | 2.205 | 2.417 | **1.13** | roughly flat / mild scale |
| **ConcurrentImmix** | 2.919 | 2.440 | 1.980 | 1.773 | **1.65** | **scales** |

GC stats (gcs / gc_time_ms):

| plan | d1 | d2 | d4 | d8 | gc_time trend with domains |
|---|---|---|---|---|---|
| GenImmix | 985 / 2582 | 984 / 3449 | 1004 / 4444 | 1048 / 5880 | **climbs ~2.3×** |
| StickyImmix | 843 / 3408 | 899 / 4689 | 870 / 5818 | 850 / 7268 | **climbs ~2.1×** |
| SemiSpace | 119 / 16190 | 131 / 18433 | 141 / 20166 | 154 / 22869 | climbs 1.4× on a vast baseline |
| Immix | 117 / 1318 | 130 / 1532 | 138 / 1682 | 159 / 1942 | nearly flat (small absolute) |
| ConcurrentImmix | 240 / 97 | 258 / 111 | 252 / 113 | 243 / 134 | **flat (~97→134 ms)**, copying = 0 |

**Reading the matrix.** The plans sort cleanly by *what they do inside the STW pause*:

- **Copying-nursery / copying plans (GenImmix, StickyImmix, GenCopy, SemiSpace)** copy or
  trace surviving objects *inside the pause*. That work grows with domain count (more
  domains ⇒ more roots and more surviving nursery copied per cycle), so `gc_time_ms` climbs
  with domains and they all anti-scale. GenCopy (copy-only mature) and SemiSpace (whole-heap
  copy of a large live set) are so copy-dominated they are off the practical chart — GenCopy
  cannot even finish d21 inside 90 s, SemiSpace runs 17–23 s vs ~3 s for the Immix family.
- **Non-generational Immix** marks in place (no nursery copy) and collects **~8× less often**
  (117–159 GCs vs ~1000 for GenImmix); its pause is a near-constant in-place mark, so
  `gc_time_ms` barely moves and wall stays roughly flat (S(8) ≈ 1.13).
- **ConcurrentImmix** moves the heap trace *off* the STW critical path (concurrent marking,
  strictly non-moving here: `objects_copied = 0` everywhere). Its pause work is small and
  domain-independent (`gc_time_ms` flat at ~97–134 ms across d1→d8), so it is the only fork
  plan that scales *up* with domains — the right direction.

---

## 3. Experiments

Each experiment names the in-tree mechanism by file:line so the claim is checkable against
the source.

### Experiment 1 — Is the per-domain-terminate forced full GC the cause? → **REFUTED**

**Hypothesis.** The fork forces a full-heap MMTk collection on *every* domain terminate, so
a benchmark that churns domains (spawn+join many times) should anti-scale because of those
terminate-GCs; spawning each domain only once should scale (or anti-scale far less), and the
churning variant should have far more GCs.

**Mechanism.** On domain teardown we call `caml_mmtk_collect()`
(`runtime/domain.c:1182`), which routes to a *forced, exhaustive* (full-heap) collection
(`gc/mmtk/binding/src/api.rs:495` — `handle_user_collection_request(tls, /*force=*/true,
/*exhaustive=*/true)`). This is deliberate and a **correctness fix**: under always-on MMTk
the stock domain-terminate minor collection is neutered to a bare nursery discard rather
than a promotion (`runtime/minor_gc.c:237`, `young_ptr = young_start`), so the domain's
result would otherwise be published to the joiner while still living in a young block that
another domain's collection could relocate out from under it (intermittent `Domain.join`
SIGSEGV). The forced full GC traces/promotes the result into stable space before teardown.

**Result. REFUTED** (with one sub-claim confirmed):

- A spawn-once variant (only `domains` terminates, vs `domains × depth-classes` for the
  churning variant) **anti-scales almost identically**. d21 spawn-once: 3.75 → 3.97 → 4.65 →
  **5.98 s** (d1→d8, +59 %); churn: 3.89 → 4.05 → … → 6.19 s. Both rise monotonically;
  vanilla on the same workload falls 3.16 → 0.81 s (~3.9×). The terminate-restructuring does
  not rescue scaling.
- GC counts are **equal** at the anti-scaling depths: at d21/d8, churn ≈ 1052 GCs vs once ≈
  1057 GCs — both ~1000, within a few %, dwarfing the 8× difference in *terminate* count. The
  terminate-GCs are lost in the noise of allocation-driven collections.
- **Confirmed sub-claim (mechanism is real in isolation):** a pure spawn/join microbench
  (trivial work, terminates = rounds × domains) shows GCs == exactly rounds × domains and
  wall growing ~linearly with domain count (1→8 domains: 0→400 GCs, 8→188 ms). Each
  terminate *does* force one full GC whose cost grows with domains — it is simply
  **second-order** at the depths that anti-scale, where allocation-driven collections
  dominate.

The leftover signal — *same number of GCs, but each costs more with more domains* (d21
spawn-once gc_time 2575 → 5631 ms d1→d8) — points straight at the STW pause (Experiment 2).

### Experiment 2 — Is the all-domains STW collection pause the cause? → **SUPPORTED**

**Hypothesis.** The dominant cost is the global stop-the-world collection pause, whose
in-pause work (rendezvous + root scan + nursery copy) grows with domain count, serialising
the mutators.

**Mechanism.** Every MMTk collection is a global STW: the GC worker's `stop_all_mutators`
(`gc/mmtk/binding/src/collection.rs:224`) marks the collection active, **poisons every
domain** to trap it to a safepoint (`for domain in domain_addrs() { caml_mmtk_interrupt(
domain) }`, collection.rs:243), and **spins until the RUNNING set is empty**
(collection.rs:256) before any collection work starts; it then root-scans every mutator and
(for the copying-nursery plans) copies the survivors — all inside the pause, all domains
stopped. Both the mutator count to round up and root-scan, and the surviving nursery to
copy, scale with the number of domains.

**Result. SUPPORTED.** At fixed work (d21, spawn-once, GenImmix) the GC *count* is
essentially constant across domain counts (983 → 1044) but the GC *time* climbs steeply:
**2844 → 4181 → 4730 → 6305 ms** (d1→d8), and so does CPU/wall (2.77 → 5.62) and max RSS
(259 → 456 MB). Same collections, repeatedly more expensive as domains rise — the signature
of an STW pause whose content grows with domain count. The cross-plan matrix (Section 2) is
the corroborating contrast: `gc_time_ms` climbs with domains for exactly the plans that copy
in-pause and stays flat for the plans that don't.

### Experiment 3 — Does moving the trace off the critical path fix it? → **SUPPORTED (strongly)**

**Hypothesis.** If the anti-scaling is caused by in-pause trace/copy work, then a plan that
marks *concurrently* with the mutators (so the pause stops growing with domains) should scale
where GenImmix anti-scales.

**Mechanism.** ConcurrentImmix performs heap-trace marking off the STW critical path and is
strictly non-moving in this configuration (concurrent-mark, no in-pause copy —
`objects_copied = 0` in every cell). The STW pause shrinks to roots + handshake, which does
not blow up with domains.

**Result. SUPPORTED (strongly).** Same benchmark, same depth, identical checksums:

| variant | plan | S(1) | S(2) | S(4) | S(8) |
|---|---|---:|---:|---:|---:|
| churn | GenImmix | 1.00 | 1.11 | 0.92 | **0.66** (anti) |
| churn | ConcurrentImmix | 1.00 | 1.42 | 1.69 | **1.87** (scales) |
| once | GenImmix | 1.00 | 0.93 | 0.83 | **0.65** (anti) |
| once | ConcurrentImmix | 1.00 | 1.32 | 1.57 | **1.80** (scales) |

ConcurrentImmix's `gc_time_ms` is **flat** with domain count (churn 97/114/116/120 ms at
d1/d2/d4/d8), exactly as the mechanism predicts, while GenImmix's climbs 3855 → 7108 ms and
its `objects_copied` grows 53M → 96M — the in-pause work that serialises mutators. At d8
ConcurrentImmix beats GenImmix's scaling by ~2.8× and, more importantly, scales in the
*right direction* (S > 1) where GenImmix goes backwards (S < 1). This is the direct, in-tree
demonstration that the bottleneck is in-pause work, not worker count.

### Experiment 4 — Which workloads anti-scale, and what is the discriminator? → boundary mapped

**Hypothesis.** Only allocation/GC-heavy parallel work anti-scales; compute-bound work
scales. The naive proxy "numerical kernels scale, tree-shaped kernels don't" is the seed.

**Mechanism.** Anti-scaling tracks STW pause frequency × cost, i.e. **allocation / GC
pressure**, not the surface shape of the program.

**Result. Refined hypothesis SUPPORTED; the "numerical vs tree" proxy REFUTED.**

| benchmark | character | S(8) fork | S(8) vanilla | GCs d1→d8 |
|---|---|---:|---:|---|
| `par_matmul` (size 900) | integer accumulator, alloc-light | **5.21×** | 5.27× | 4 → 11 |
| `par_spectralnorm` (size 2400) | boxed-float inner loop, GC-heavy | **~1.6× peak, regresses past d4** | 3.83× | 497 → 706 |
| `par_binarytrees` (d21) | tree builds, alloc-heavy | **0.64×** | 3.93× | ~985 → ~1048 |

`par_matmul` is alloc-light (single digits of GCs) and tracks vanilla within ~1 % to 8
domains. `par_spectralnorm` is *nominally* "numerical" but its boxed-float inner loop churns
the minor heap (hundreds of GCs, count rising with domains) and anti-scales just like
binarytrees — peaking at ~1.6× by d4 and regressing at d8. So "numerical" is **not** a safe
proxy for "scales"; **allocation/GC pressure is the discriminator.** (One d8 spectralnorm
run timed out in a back-to-back batch but did not reproduce in isolation — treat it as
transient contention, not a deterministic cliff. The robust claim is "anti-scales, peaks
~1.6× then regresses," not "falls off a cliff at d8.")

### Experiment 5 — Does the effect get worse at high domain count? → **YES** (godel, 56-core Xeon)

Even compute-bound work eventually regresses as the per-domain spawn/terminate GCs become an
all-domains STW across many domains. Fork GenImmix, wall (s):

| benchmark | d1 | d8 | d16 | d28 | d56 | vanilla d56 |
|---|---:|---:|---:|---:|---:|---:|
| `par_matmul` | 3.75 | 0.89 | **0.85** (peak) | 1.08 | 1.73 | 0.30 |
| `par_spectralnorm` | 5.98 | 2.23 | 3.07 | 4.95 | **TIMEOUT** | 1.26 |

GC count grows with domains (matmul ~1 GC/domain → 55 at d56; spectralnorm's per-iteration
spawn churn → ~999 GCs at d28, with GC time ≈ 80 % of wall). Vanilla scales throughout. So
the more cores, the worse the fork's anti-scaling — consistent with a pause whose cost rises
with domain count.

---

## 4. Attribution — where the multi-domain slowdown actually goes

For the anti-scaling workloads at d21 (GenImmix default), the extra wall time as domains rise
breaks down as:

1. **All-domains STW collection pause that grows with domain count — DOMINANT.** Same
   collection count, but per-collection cost rises ~2.3× from d1→d8 (GenImmix gc_time 2582 →
   5880 ms; spawn-once measured 2844 → 6305 ms at fixed work). This is the in-pause root-scan
   + nursery-copy whose size scales with the number of stopped domains, plus the rendezvous
   to round all domains up. It accounts for essentially all of the anti-scaling: the plans
   that remove it (Immix in-place mark, ConcurrentImmix concurrent mark) stop anti-scaling.
2. **Per-domain-terminate forced full GC — MINOR / second-order.** Real (each terminate
   forces one full GC; cost ∝ domain count, confirmed in isolation) but contributes <20 % of
   wall and is invisible against the steady-state collection cost at the depths that
   anti-scale. It dominates only at tiny depths where there is almost no allocation.
3. **GC-worker / mutator core contention — present but not the cause.** With default workers
   = nproc on a 12-core box, 8 mutator domains plus GC workers oversubscribe the cores; this
   adds variance (the non-reproducible d8 timeouts) but is not the mechanism — ConcurrentImmix
   scales *through* d8 under the same worker default, and the anti-scaling appears with worker
   count held fixed.

Net: **the multi-domain anti-scaling is STW-pause-bound.** The headline number is the rising
per-collection GC time at fixed work, and the fix space is everything that shrinks or removes
that pause.

---

## 5. Boundary — what scales and what doesn't (default GenImmix plan)

**Scales (≈ vanilla, ~3.6–5.3× at 8 domains):** compute-bound, allocation-light kernels —
work that touches few heap objects per unit of compute and therefore triggers few
collections. Example: `par_matmul` (integer accumulator), single-digit GC counts, 5.21× at
d8.

**Anti-scales (S(8) < 1, slower with more domains):** allocation/GC-heavy parallel work —
any workload that drives frequent collections, because each collection is a global STW pause
that gets *more expensive* as domains rise. Examples: `par_binarytrees` (tree builds), and
the boxed-float `par_spectralnorm` despite being "numerical."

**The discriminator is allocation / minor-GC pressure, not program shape.** A "numerical"
benchmark that boxes floats in its inner loop is on the anti-scaling side of the boundary; a
"tree" benchmark would be too. RSS also rises with domains on the anti-scaling side (d21
spawn-once: 259 → 456 MB d1→d8), so memory-parity comparisons must report RSS alongside wall.
At very high domain count (Experiment 5) even compute-bound work eventually crosses the
boundary, because per-domain spawn/terminate GCs accumulate into a many-domain STW.

---

## 6. Does the generational copying nursery ever pay off here? (single-domain)

Real OCaml workload — native compile of `camlinternalFormat.ml` (the heaviest stdlib unit),
single-domain, best-of-3:

| heap | GenImmix | Immix | ConcurrentImmix | note |
|---|---|---|---|---|
| 96 MB (pressured) | 0.43 s / 1.38 M copied / 188 MB | **0.35 s** / 188 K / 170 MB | 0.39 s / 152 K / 178 MB | GenImmix slowest, copies ~7× more |
| 1 GB (generous) | 0.29 s / 1 GC | 0.26 s / 0 GC | 0.29 s / 2 GC | three-way tie (GC negligible) |

GenImmix's copying nursery is a **net cost under memory pressure** (the AST is medium-lived
→ high survival → every minor GC re-copies it) and **neutral when memory is ample**. On the
workloads measured it rarely wins single-domain. (Caveat: this is medium-lived-survivor
allocation; a long-running, truly short-lived-garbage workload — the textbook generational
case — was not isolated.)

---

## 7. Candidate fixes, ranked

Ranked by impact-per-effort, with the reasoning for each. (1)/(2) are already implemented and
validated on this benchmark; the rest are the follow-on space.

### 1. Move marking off the STW path: make ConcurrentImmix the eventual default. **HIGHEST impact.**
**Why.** It directly removes the dominant cost (Attribution #1) by tracing concurrently with
the mutators, and it is **already implemented and measured**: it turns S(8) ≈ 0.64 (anti)
into S(8) ≈ 1.65–1.9 (scales) on `par_binarytrees` d21, with flat per-collection GC time
(~100–130 ms) and identical checksums, at comparable RSS. It is the strongest experimental
result.
**Why not yet:** it is still a research plan with production-completion work outstanding
(native SATB write barrier, the UNLOG-bit gate, and a small-heap ~10 MB sanity deadlock). A
fast-but-flaky plan cannot be the default. **Finishing ConcurrentImmix is the highest-value
GC investment** — it converts the best result into a shippable default.

### 2. Switch the default to non-generational Immix now (the production-ready intermediate). **HIGH, lowest risk.**
**Why.** Immix sidesteps the bottleneck by collecting ~8× less often and marking in place
(no in-pause copy), so its pause does not grow with domains: it scales (S(8) ≈ 1.13–1.33)
and **matches or beats GenImmix single-domain** at every point measured, at comparable RSS,
and it is a production-stable, heavily-tested plan. The "generational is faithful to stock
OCaml" rationale for GenImmix is weakened by the data — MMTk-GenImmix's copying-nursery tax
is not buying throughput that justifies it on these workloads.
**Gate before flipping:** run the full sequential CLBG/sandmark panel, GenImmix-vs-Immix at
memory parity. If Immix is ≥ GenImmix across the panel — as it is so far — switch the default
to Immix. This guards against a generational-favouring workload class not yet isolated.

### 3. Shrink the per-collection STW pause for the generational plans: narrow the root scan + in-pause copy. **MEDIUM-HIGH.**
**Why.** If GenImmix stays the default for single-/few-domain throughput, the lever is the
*content* of its pause. The root scan and survivor copy scale with stopped domains; cutting
them attacks Attribution #1 directly — e.g. narrow the minor-GC root scan to young-only
globals and prune stack frames that cannot hold young pointers, so per-collection work scales
with live young data, not domain count × full root set.
**Reasoning.** Lower-risk than changing the plan and keeps the generational fast path, but it
is incremental — it reduces the *slope* of the rising pause rather than removing the global
STW.

### 4. Reduce STW collection *frequency*: decouple per-domain nursery triggers / size the nursery to amortise the global pause. **MEDIUM.**
**Why.** Anti-scaling = pause cost × pause frequency. If every domain's nursery fill triggers
a *global* STW, more domains ⇒ more frequent global pauses on top of more expensive ones.
Making nursery exhaustion trigger only local/cheaper work, or sizing nurseries so collections
are rarer, cuts the frequency term. The matrix supports this: Immix already collects far less
often (117–159 vs ~1000 GCs) and does not anti-scale.
**Reasoning.** Attacks the orthogonal (frequency) axis and composes with #1–#3, but touches
trigger/heuristic machinery and must not balloon RSS.

### 5. Make domain terminate cheap: per-domain promotion instead of a forced full GC. **LOW-MEDIUM.**
**Why.** The forced full GC on terminate is a genuine per-terminate cost (∝ domain count)
that hurts domain-churning and tiny-depth programs. The cleaner fix is the one vanilla uses:
a per-domain *promotion* of the terminating domain's young survivors into stable space (mirror
stock's `caml_empty_minor_heap_promote`) rather than a whole-heap collection — preserving the
join-safety guarantee without a global STW per terminate.
**Reasoning.** Correctly scoped and low-risk, removes a real (if second-order) cost; but
Experiment 1 shows it is **not** the driver of the headline anti-scaling, so it is a polish /
edge-case win, not the main lever.

### 6. Right-size GC worker count vs domains. **LOW (variance control only).**
**Why.** Default workers = nproc oversubscribe the cores once mutator domains are added,
adding variance (the non-reproducible d8 timeouts). Workers ≈ cores − running domains would
cut park/wake contention.
**Reasoning.** This is tail/variance control, not the mechanism — Experiment 3 shows scaling
is governed by pause *content*, not worker count — so it is the lowest-priority lever, useful
mainly to stabilise measurements. (An explicit "scale workers with running domains" attempt
was tried and abandoned for exactly this reason.)

---

## 8. The open research question

Vanilla OCaml 5's minor GC is *also* an all-domains stop-the-world barrier, yet it scales
~3.9×. So the fork's deficit is the **per-collection STW cost** (and its growth with domain
count), not the existence of the barrier. Vanilla's minor GC is cheap enough that parallel
mutator work dominates; the fork's is not. The sharpest research question this investigation
opens:

> **Can a generational plan keep the nursery benefit without serialising all domains on every
> minor GC — i.e. drive the per-collection STW cost (and its growth with domain count) down to
> vanilla's level, or remove the global barrier for the minor collection entirely?**

ConcurrentImmix answers a version of it by hiding the trace; the harder, more general version
is whether the *generational* copying path can be made domain-scalable. That is the lever the
data most strongly motivates.

---

## 9. Caveats and follow-ups

- **Matrix is one host, one benchmark.** All Section-2 cells are `par_binarytrees` d21 on one
  12-core macOS box. The mechanism predicts anti-scaling worsens with core count — Experiment
  5 confirms it on a 56-core Xeon; per-pause instrumentation (`perf`/`bpftrace`) on Linux
  would attribute rendezvous vs root-scan vs copy directly.
- **`gc_time_ms` is aggregate CPU-in-GC** across workers and overlaps the mutator — use it for
  cross-config ratios, not as a literal wall fraction.
- **Two transient timeouts** (one d21/d4 churn run, one d8 spectralnorm run) did not reproduce
  in isolation; treated as contention, not data points.
- **Memory parity.** The anti-scaling plans grow RSS with domains; ConcurrentImmix is
  non-moving and may sit at a different RSS. Any "ConcurrentImmix/Immix wins" claim should be
  re-stated at pinned, equal heap size with RSS reported (RSS was within ~10 % across the three
  Immix-family plans at d8 in the runs measured).
- **GenCopy could not complete d21 in 90 s at any domain count** and SemiSpace runs 17–23 s
  (vs ~3 s for the Immix family) — both are copy-dominated at this live-set size and are not
  viable defaults for allocation-heavy parallel work regardless of scaling.
