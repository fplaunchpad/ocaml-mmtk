# PERFORMANCE.md — GC & runtime performance-measurement methodology for `ocaml-mmtk`

**Status:** best-practices of record. Adopt this before publishing *any* throughput, memory, or pause-time number that compares plans against each other or against stock OCaml 5.5. It operationalizes the "Methodology" bullets and RQ2 of [`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md): characterize the workload first, normalize for the time–space tradeoff, then rank collectors with statistical rigor.

The one-sentence rule: **a GC result that is a single number is wrong.** Every comparison is a curve over heap size, every benchmark carries its workload fingerprint, and every claim carries a dispersion estimate.

The standing order from the maintainer: **find and remove obvious overhead from the allocation + collection fast paths FIRST, then measure with this harness, then iterate.** Section 10 (the ranked backlog, kept in ROADMAP) is the "what to do"; this document is the "how to know it worked."

---

## 0. Why this document exists (the central methodological hazard)

A garbage collector trades *space* for *time*: give it more heap and it collects less often, so it runs faster; squeeze the heap and it thrashes. Any two collectors therefore have *crossing* time-vs-heap curves, and you can make either one "win" by picking the heap size. Reporting throughput at a single heap size is the canonical way to lie with GC benchmarks — usually unintentionally. The whole-program design of this methodology exists to remove that degree of freedom.

This bites `ocaml-mmtk` especially hard because it is **fixed-heap by default** (`MMTK_HEAP_SIZE_MB`, default 1024; `FixedHeapSize` in `gc/mmtk/binding/src/api.rs:39`) whereas stock OCaml grows its heap adaptively. A naive head-to-head at "the default heap" compares two different points on two different curves and tells you nothing. The first M8 baseline already proves the magnitude: `gcbench` under Immix is **14.1 s @512 MB → 7.0 s @1024 MB → 5.9 s @2048 MB** (NOTES 2026-06-20). A single heap point is meaningless here.

The literature this rests on:

- **MemBalancer** — Kirisame, Shenoy & Panchekha, *Optimal Heap Limits for Reducing Browser Memory Use*, PACMPL 6(OOPSLA2) Art. 160, 2022 (DOI 10.1145/3563323; arXiv:2204.10455). Formalizes the heap-limit/GC-frequency tradeoff and gives the √-rule sizing that makes "how much heap should I give it" a principled, not arbitrary, choice. We use it both as a sizing policy to *test* and as the justification for sweeping heap size.
- **Distilling the Real Cost of Production Garbage Collectors** — Cai, Blackburn, Bond & Maas, ISPASS'22 (DOI 10.1109/ISPASS55109.2022.00005; arXiv:2112.07880). Establishes the *lower-bound overhead* method: measure each collector against the idealized floor (mutator running with a perfect/zero-cost collector), and report the gap, so absolute throughput differences are attributed to GC and not to noise.
- **Myths and Realities: The Performance Impact of Garbage Collection** — Blackburn, Cheng & McKinley, SIGMETRICS'04 (DOI 10.1145/1005686.1005693). The ancestor: implement the canonical collectors in one framework so only *policy* differs, then compare across a *range of heap sizes*. This is exactly our setup (one language, one runtime, N MMTk plans) and dictates the time-vs-heap-multiple presentation.

These three give the same instruction from three angles: **present results as time-vs-heap-size curves, never single numbers.**

---

## 1. Workload characterization comes FIRST (RQ2 precondition)

**Do not rank collectors until each benchmark has a published workload fingerprint.** RQ2 is explicitly a *characterization* paper: "quantify the workload first … then show which plan wins where — so the comparison is explanatory, not a leaderboard." A plan ranking with no workload axis is uninterpretable and unpublishable at ISMM.

For every benchmark, report these four covariates **before** any collector comparison:

| Metric | Definition | How to obtain in this tree | Why it matters |
|---|---|---|---|
| **Allocation rate** | bytes allocated per unit mutator time (and total bytes allocated) | `stat_minor_words` odometer (`runtime/mmtk.c:231,287,670`); cross-check MMTk's own counters | Sets tracing-independent pressure; the axis OCaml is expected to be extreme on (high rate, small short-lived objects) |
| **Survival rate** | fraction of allocated bytes that survive a collection (volume copied / volume allocated) | `MMTK_VERBOSE=1` emits `objects copied` at exit (`runtime/mmtk.c:151-158`, via `mmtk_ocaml_objects_copied()`); per-GC survival needs the GC-trace hook (§7) | Drives copy cost; separates the generational hypothesis (low survival) from mature-heap churn |
| **Mutation rate** | heap stores that hit the write barrier per unit mutator time | Count `caml_modify`/`caml_initialize` barrier hits (native barrier self-gates on `caml_mmtk_generational`, `runtime/mmtk.c:563-568`). Instrument or bpftrace the barrier slow path | **The load-bearing covariate for RQ1.** The immutability thesis predicts residual GC cost scales with mutation, not allocation |
| **Lifetime dispersion** | Gini-coefficient concentration of object lifetimes — Dolan, *Lifetime Dispersion and Generational GC*, ISMM'25 (DOI 10.1145/3735950.3735958) | Build a per-benchmark lifetime profiler (age-at-death histogram → Gini). **Platform deliverable; does not exist yet** | Turns "how generational is this workload" into a measured, composable predictor of how much generational collection should help |

Presentation: a small per-benchmark table or radar chart of {alloc rate, survival, mutation rate, dispersion}. This is the independent-variable space; collector rankings are then read *as a function of position in it*. The headline RQ2/RQ1 result is a **regression of the collector-choice outcome (or the residual throughput gap) against these covariates** — e.g. RQ1 lives or dies on the slope of (throughput gap) vs (mutation rate / dispersion).

**Benchmark suites** (PRIMARY first, per the maintainer's instruction):
- **`ocaml-bench/macro-benches` (PRIMARY — headline throughput/RSS).** opam-monorepo, ~19 tools / ~30 real programs (menhir, cpdf, alt-ergo, coq, eio, irmin, ocamlformat…), committed `macro-benches.opam.locked`, `make setup` (~10 min, no run-time solve). Mostly single-domain. Real programs give the "Distilling"/"Myths & Realities" framing RQ2 wants.
- **`ocaml-bench/benches` (microbench complement — GC microstructure + multicore scaling).** `simple/`, `with_deps/`, `with_packages/`, and crucially **`multicore/`** (`multicore-numerical` via `Domainslib.Task`, `multicore-effects`, lock-free `Atomic` structures, `alloc_multicore`, `graph500par`). `markbench` reports **seconds-per-GC-cycle** (directly GC-relevant). These are the GC-under-parallelism workloads that also exercise the bug #3b/#3c STW paths.
- **In-tree `gcbench`** — the M8 baseline workhorse (NOTES 2026-06-20); keep as the *fast-iteration* probe. The two suites are the *campaign*.
- Also span the **mutability spectrum** deliberately: pure-functional ↔ `ref`/`Bytes`/mutable-array-heavy, so the mutation-rate axis is actually exercised (CLBG suite on the `benchmarks` orphan branch helps here).

Both suites share the **`RUNNING_OCAML_*` build-script contract** (`RUNNING_OCAML_SWITCH`, `_RUNTIME_NAME`, `_OUTPUT`, `_BENCH_DIR`) driven by **`running-ng`** — the single integration seam for pointing them at the fork (see §3.3).

---

## 2. Heap-size normalization — the time–space tradeoff (the core protocol)

**Never compare collectors (or compare against stock) at a single heap size.** Sweep it.

### 2.1 Define the heap-size axis as a multiple of live size, not absolute MiB

1. Measure each benchmark's **minimum live size `L`** (high-water mark of reachable data): smallest `MMTK_HEAP_SIZE_MB` at which the most space-efficient plan (MarkCompact, when native; else Immix) completes without `Out_of_memory`. Cross-read live words from `Gc.stat`/`MMTK_VERBOSE`.
2. Sweep heap at **multiples of `L`**: e.g. `m ∈ {1.2, 1.5, 2, 3, 4, 6}`, i.e. `MMTK_HEAP_SIZE_MB ≈ m·L`. Absolute MiB is not comparable across benchmarks; heap *multiple* is. (Myths and Realities convention.)
3. Plot **wall-clock time (and pause-time percentiles) on Y, heap multiple on X**, one curve per plan, plus the stock-OCaml curve. The crossing points *are* the finding.

### 2.2 What to hold constant vs sweep

| Hold constant | Sweep / report as a curve |
|---|---|
| Source, opam deps, compiler flags, machine, `setarch -R`, CPU governor, core count, GC worker count (per run) | **Heap size** (X-axis, as a multiple of `L`) |
| Benchmark inputs, golden outputs (correctness gate) | Plan (`MMTK_PLAN`) — one curve each |
| Warmup/invocation counts (§4) | (Separately) GC worker thread count `MMTK_THREADS` — see §5 |

### 2.3 Lower-bound / floor (Distilling)

Where feasible, also report each plan's gap to the **lower-bound overhead**: mutator time with collection driven toward zero (largest heap so GC is rare; or `NoGC`, bytecode, for the alloc-only floor). The publishable quantity is then "plan X costs Y% over the floor at heap multiple Z," robust to machine noise in a way raw throughput is not.

### 2.4 Test MemBalancer as a policy, separately

Dynamic sizing is itself a variable. A small-min `DynamicHeapSize` experiment *regressed* on large live sets and was reverted (>190 s thrash; NOTES). So: (a) do the fixed-heap sweep above for the apples-to-apples science; (b) *separately*, evaluate MemBalancer-style adaptive sizing as a candidate default, reporting where its chosen point lands on the fixed-heap curve. Do not conflate the two.

---

## 3. Fair comparison against vanilla OCaml 5.5.0

Vanilla OCaml has a generational hybrid (per-domain bump-pointer **minor heap** copying into a non-moving mostly-concurrent **major heap**; *Retrofitting Parallelism onto OCaml*, ICFP'20). `ocaml-mmtk` has **no separate minor heap** — young objects live in MMTk's own heap; the only STW is MMTk's. The heap models genuinely differ, so apples-to-apples requires care.

### 3.0 Two-stage comparison strategy (the headline metric is MMTk-vs-vanilla)

The thing we ultimately care about is **MMTk vs the vanilla OCaml GC**, not an N×N plan tournament re-run against vanilla every time. So split it:

1. **Stage 1 — intra-MMTk bake-off.** Run the MMTk plans against *each other* (full §2 heap-multiple sweeps, §1 workload fingerprints) across the suite to determine the **champion plan** — the one that performs best for MMTk *generally* (expected contenders: Immix, StickyImmix, GenImmix; the champion may differ bytecode vs native). MMTk plans are **one opam switch** differing only by `MMTK_PLAN`, so this stage is cheap. Pick the champion (and note where a runner-up wins a workload class).
2. **Stage 2 — champion vs vanilla.** Carry *only* the champion forward into the rigorous head-to-head against vanilla OCaml 5.5.0 (equal-RSS + iso-headroom curves, §3.4). This is the headline result and the one we maintain over time; re-run the full Stage-1 bake-off only when the binding changes materially (a new plan, an alloc/barrier/scan change).

Day-to-day regression tracking watches the **champion vs vanilla** number; the full bake-off is periodic, not per-commit.

### 3.1 Subjects

| Runtime id | What | Switch source |
|---|---|---|
| `vanilla-5.5.0` | **released** OCaml 5.5.0 (auto-sizing 2-gen GC) — the baseline | normal opam switch (`ocaml-base-compiler.5.5.0`) |
| `mmtk-immix` | fork, `MMTK_PLAN=Immix` (default, native) | pin the fork |
| `mmtk-stickyimmix` | fork, `StickyImmix` (gen, in-place nursery) | same switch, env only |
| `mmtk-genimmix` | fork, `GenImmix` (stock-faithful gen, copy nursery) | same switch, env only |

**Version-match the baseline.** The fork is being advanced to **5.5.0 final** (from 5.5.0-rc1; the rc1→5.5.0 delta is 6 release-plumbing commits — see ROADMAP), so the vanilla baseline must be **released 5.5.0**, not rc1 and not trunk — otherwise an OCaml-version delta confounds the GC comparison. Do not start the Stage-2 campaign until both sides are 5.5.0. Build the *same* program with both the `vanilla-5.5.0` switch and the fork switch.

Efficiency: the MMTk plans are **one opam switch** differing only by `MMTK_PLAN` — no rebuild between plans (this is what makes Stage 1 cheap). **olly** consumes `runtime_events` from a built program, so each measured subject needs a real compiler switch (`vanilla-5.5.0` for the baseline; the fork switch for MMTk); install `runtime_events_tools` into a *separate* tooling switch — but note olly-on-MMTk is broken until backlog #R1–#R4 (§7).

### 3.2 Setup invariants (hold constant)

- **Same source, same opam dependency set, same compiler flags.** Build the *same* program with two compilers: a vanilla OCaml 5.5 switch and the fork. MMTk is always-on with no opt-out, so the stock baseline *must* be a separate switch — there is no in-tree A/B.
- Same machine, same `setarch x86_64 -R` (apply to *both* — it is an environment control, not an MMTk-only crutch), same governor/affinity, same core count.
- Same inputs and the golden-output correctness gate (CLBG `run.sh validate` is byte-identical cross-plan; use it to prove the two builds compute the same thing before timing them).

### 3.3 Pinning the fork as an opam switch (the `RUNNING_OCAML_SWITCH` target)

```
opam switch create mmtk-fork --empty
opam pin add -n ocaml-variants file:///path/to/ocaml-mmtk    # uses ocaml-variants.opam
opam install ocaml-variants dune
# drive a suite at it:
RUNNING_OCAML_SWITCH=mmtk-fork RUNNING_OCAML_RUNTIME_NAME=mmtk-immix \
  bash benchmarks/<tool>/<tool>.build.sh
MMTK_PLAN=Immix MMTK_HEAP_SIZE_MB=<H> setarch x86_64 -R taskset -c 0-13 ./<tool>-mmtk-immix
```
The fork carries `libmmtk_ocaml.a` linked via configure global-link + opam relocatability, so a pinned switch is self-contained. `running-ng` is the intended orchestrator for both suites (manages per-runtime switches + the `RUNNING_OCAML_*` contract + can shell out to olly/perf); recommended over hand-rolling for the full campaign. For fast single-bench iteration, the manual `*.build.sh` + hyperfine path is enough.

### 3.4 Footprint comparison (heap models differ)

Account **total footprint**, not "major heap size." For stock, total RSS = minor heaps (`Minor_heap_size` × domains) + major heap; for MMTk it is the single `MMTK_HEAP_SIZE_MB`. **MMTk reserves the whole fixed heap → `/usr/bin/time -v` max-RSS ≈ heap, not live set.** This is correct to report but must be framed against stock's auto-sized peak, else MMTk looks falsely memory-hungry. Compare at **equal total peak RSS** *and* present the full time-vs-total-footprint curve; drive stock's footprint via `OCAMLRUNPARAM`/`Gc.set` and MMTk's via `MMTK_HEAP_SIZE_MB`. The two headline points: "MMTk-Immix vs stock at equal peak RSS" (iso-memory) and "…at 2× live-set heap" (iso-headroom) — both, never one.

### 3.5 Generational and moving-vs-non-moving fairness

For the **generational** question ("did we regress the generational design"), compare **GenImmix**/**StickyImmix** (the structural analogs of stock's hybrid), not plain Immix. For **moving vs non-moving**: stock's major heap is non-moving; MMTk Immix moves. A non-moving collector is "fundamentally exposed to fragmentation and reduced locality" (de Souza Amorim et al., ISMM'25) — so at *tight* heaps the moving plans should win on space and non-moving ones may win on mutator-locality cost avoided. Report both regimes; never pick the heap size that flatters your side.

---

## 4. Statistical rigor

GC introduces run-to-run nondeterminism (collection timing depends on allocation interleaving, worker scheduling, and — multicore — domain races). Treat every number as a distribution.

**Checklist (every reported measurement):**
- [ ] **Warmup.** `hyperfine --warmup 3` (macro-benches are short; warmup fills page cache — OCaml is AOT so there's no JIT). For long GC-heavy runs `--warmup 1`. Whole-program/compiler runs have no steady state → each full run is one sample; lean on invocation count.
- [ ] **Multiple invocations.** ≥10 fresh-process invocations per (plan, benchmark, heap) cell (`--runs 15` headline; `--runs 30` for noisy/tail-latency). Fresh process each time (heap/metadata mmap layout varies).
- [ ] **Median + dispersion.** Report **median** (robust to GC-pause outliers) with explicit dispersion: IQR/MAD or 95% bootstrap CI. **Never a bare mean** — GC pauses right-skew the distribution. hyperfine gives mean±σ; also extract median from `--export-json`. Geomean across benchmarks for an aggregate, with per-benchmark spread shown.
- [ ] **Confidence / overlap.** State whether CIs overlap before claiming "X beats Y." Small single-digit-percent differences inside overlapping CIs are not results.
- [ ] **Counter normalization.** Normalize hardware counters **per instruction retired**, **per byte allocated**, and per unit mutator time — not per wall-second (which folds in the GC time you are isolating). L1/LLC misses *per byte allocated* isolates allocator/locality from collection frequency.
- [ ] **GC-time vs mutator-time split reported separately** (§7). A regression with no split is not a diagnosis.
- [ ] **Outliers.** Flag runs >3 MAD; investigate (GC at an unlucky point vs noise) rather than silently drop.

---

## 5. Pitfalls specific to this project (control these or your numbers are noise)

- **ASLR mmap flake → always `setarch x86_64 -R`.** MMTk can abort at startup with `failed to mmap meta memory: File exists`; *not* a correctness bug. Run every measured invocation (and the stock baseline) under `setarch x86_64 -R`. (rr disables ASLR itself.)
- **`MMTK_PLAN` and `MMTK_HEAP_SIZE_MB` are the primary independent variables.** Pin them explicitly per run; never rely on defaults in a results table. **Native code requires an Immix-family plan** (TLAB nursery-aliasing): native runs `Immix`/`StickyImmix`/`GenImmix`/`GenCopy`; `MarkSweep`/`PageProtect`/Compressor are **bytecode-only** (abort at startup on native — `runtime/mmtk.c:195`). A *native* cross-plan study is restricted to the four bump-pointer-aliasable plans; full 9-plan sweeps are bytecode-only — and **bytecode vs native is itself a confound you must never cross** (the bytecode alloc path goes per-object through `mmtk_ocaml_alloc`; native uses an inlined TLAB bump — see backlog #A1).
- **GC worker thread count (`MMTK_THREADS`) vs cores.** Defaults to `nproc` *per process* (oversized for short runs — NOTES lever #4). Both a confound and a knob: (a) **hold it constant** within a comparison; (b) when studied, sweep it ({1,2,4,8}) as its own axis and account that GC workers and mutator domains contend for the same cores (on an N-core box, workers = domains = N oversubscribes). Pin affinity; report (mutator domains, GC workers, physical cores) for every run.
- **Host settings (verified on `turing`).** `perf_event_paranoid = -1` (perf/bpftrace work without sudo — good). `intel_pstate/no_turbo = 1` (turbo already off — leave it). **`scaling_governor = powersave` — MUST change to `performance`** before any timing run (`sudo cpupower frequency-set -g performance`), else clock scaling adds variance. CPU is 2-socket Xeon Gold 5120 (28 cores) — NUMA matters; **pin to one socket** (`taskset -c 0-13`).
- **`sanity` feature is a correctness tool, not a measured config.** Full-heap re-trace after each GC is far too slow to leave on for timing. Verify a moving plan with `sanity` at a small heap in a *separate* run; time with `sanity` off.
- **Known hangs distort aggregates.** Several tests/benches hang under MMTk (statmemprof, some finaliser cases, the bug #3c burn-pattern multidomain hang). Use a **per-run timeout** and *report* timed-out cells as failures — do not silently drop them. **Never wrap a run in an outer `timeout N …`**: if it fires before the inner timeout it orphans the `setpgid`'d process group, which then runs forever (we leaked ~17 runaway `ocamlrun` this way). Use the harness's own timeout / `setsid` + kill the group. A benchmark that intermittently hangs is a workload-characterization finding, not a data point to average in.
- **Kill stray runners before measuring.** `pgrep -x ocamlrun` and clean by PID — **never `pkill -f`** (it can match your own ssh command line and kill your session).

---

## 6. Presentation standard (what a result looks like)

A complete result for one benchmark is:
1. A **workload fingerprint** table (§1): alloc rate, survival, mutation rate, lifetime-dispersion Gini.
2. A **time-vs-heap-multiple plot**: one curve per plan + stock OCaml, X = heap as a multiple of live size `L`, Y = median wall time with dispersion band.
3. A **footprint-vs-time plot** for the stock comparison (equal-total-RSS, §3.4).
4. A **pause-time distribution** (median / p99 / p99.9 / max) at each heap multiple — required for any latency claim (RQ1).
5. The **GC-time vs mutator-time split** (§7) and counter-normalized deltas (§4) for any regression.

Aggregate across benchmarks with geomean **and** show the per-benchmark spread. One number for the whole suite, with no per-benchmark or per-heap-size breakdown, is not acceptable.

---

## 7. Observability: `runtime_events` / `olly`, and the gap to close

OCaml ships zero-overhead per-domain `runtime_events` ring buffers; `olly` (from `runtime_events_tools`, opam) consumes them for GC latency histograms, GC time / overhead %, total/minor/promoted words, and collection counts.

**Critical caveat — `runtime_events` is broken under MMTk today** (a known M7 non-pass area). The audit established:
- **The real MMTk pause emits no event.** The actual STW window is `stop_all_mutators`→`resume_mutators` in `gc/mmtk/binding/src/collection.rs:224,278` (pause-start `:230`, elapsed folded `:287`); **neither `mmtk.c` nor any file in `gc/mmtk/` emits a single `caml_ev_*`.** MMTk tracks `GC_COUNT`/`GC_NANOS` (`collection.rs:88-89`) but that never reaches the ring.
- **The events that DO fire wrap neutered stock code.** `EV_MINOR` (`minor_gc.c:218,290`), `EV_EMPTY_MINOR`, and `EV_MAJOR` (`domain.c:1951`) bracket no-ops / the empty-heap spawn-terminate rendezvous (`caml_major_collection_slice` is inert — `major_gc.c:1012`). So **olly's latency histogram is built from phantom spans and is meaningless**; pause times read tiny and fictional.
- **Words counters read wrong/zero.** `EV_C_MINOR_ALLOCATED_WORDS` reads stock `young_end-young_ptr` (= **0 in bytecode**); `EV_C_MINOR_PROMOTED_WORDS` is structurally **always 0** (no nursery→major promotion; `gc_ctrl.c:153`). The true figures sit in `stat_minor_words` and `caml_mmtk_gc_stats` (`mmtk.c:530`) and are not emitted.

**Therefore, until the runtime_events wiring is fixed (backlog #R1–#R4):**
- For pause-time distributions, **fall back to `bpftrace` uprobes** on the STW enter/leave hooks (`caml_mmtk_park`/the stop/resume boundary, `mmtk.c:588-629`) and on the alloc entrypoint for alloc-size histograms.
- For words/counts/GC-time, use `MMTK_VERBOSE=1` (`GCs: N, GC time: T ms, objects copied: C`).
- Treat any olly-on-MMTk GC number as **to-be-validated**, not assumed. olly works against the **stock** baseline immediately.

The planned fix (RQ5(c) plan-agnostic observability; Huang, Blackburn & Cai, MPLR'23) is to emit a real GC-STW span around the MMTk pause and source the words counters from the MMTk odometer — see backlog #R1–#R4.

---

## 8. Tool stack — what each yields & how it plugs in

| Tool | Measures | Invocation sketch | Use for |
|---|---|---|---|
| **hyperfine** | wall-clock w/ warmup, median/stddev/min/max, JSON export | `hyperfine --warmup 3 --runs 15 --export-json out.json -L plan Immix,StickyImmix,GenImmix 'MMTK_PLAN={plan} MMTK_HEAP_SIZE_MB=H setarch x86_64 -R taskset -c 0-13 ./bench-mmtk'` | primary throughput; cross-plan `-L` matrix |
| **`/usr/bin/time -v`** | **max RSS** (peak), page faults, ctx switches | `/usr/bin/time -v ./bench 2> rss.txt` (grep "Maximum resident") | footprint (the other axis of §2 curves) |
| **perf stat** | cycles, instructions, IPC, LLC misses, branch-misses, dTLB-load-misses, page-faults | `perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses,dTLB-load-misses,page-faults ./bench` | *why* a plan is slow — alloc-bound (IPC, branch-miss) vs GC-bound (cache/TLB on tracing) |
| **perf record/report** | hot functions (call-graph) | `perf record -g --call-graph dwarf ./bench; perf report` | expect `mmtk_ocaml_alloc`/TLAB-refill and `scan_object`/marking to dominate — validates the #A1 / collection-trace levers |
| **bpftrace / bcc** | alloc-size dist, GC pause dist, mmap/madvise churn | uprobe on alloc entry + STW enter/leave; hist of stop→resume intervals | pause-time tails (RQ1), mmap churn (fixed-heap), **the runtime_events fallback (§7)** |
| **olly + runtime_events** | GC latency histograms, words, custom spans | `olly latency ./bench` / `olly trace trace.json ./bench` | OCaml-native GC latency — **works on stock now; MMTk emission is the open gap (§7)** |

**Ordering:** never run perf/bpftrace and hyperfine simultaneously — instrumentation perturbs timing. Run **timing (hyperfine + /usr/bin/time)** and **profiling (perf/bpftrace/olly)** as separate passes, same pinning/config. `olly` install: `opam install runtime_events_tools` into a dedicated `olly-tools` switch (NOT the measured subject switch).

---

## 9. "How to attribute a regression" playbook

A staged funnel: confirm it is real, find which counter moved, find the function, split GC vs mutator, then characterize the distribution. Run all of this under `setarch x86_64 -R` with `MMTK_PLAN`/`MMTK_HEAP_SIZE_MB`/`MMTK_THREADS` pinned.

1. **Confirm it is real — `hyperfine`.** `hyperfine --warmup 3 --runs 20 -L plan Immix,StickyImmix '…'`. Non-overlapping CIs before believing the regression exists. If CIs overlap, stop — it is noise (§4).
2. **Find the counter that moved — `perf stat`.** Normalize per byte allocated / per instruction (§4). The counter that moved names the mechanism class: cache-misses ↑ → locality/fragmentation (non-moving or post-defrag); instructions ↑ → more code on the path (barrier or alloc-slowpath); branch-misses ↑ → predictor effects in the fast path.
3. **Find the function — `perf record`/`report`.** Distinguish mutator symbols (`caml_*`) from collector symbols (`mmtk_*`, `scan_ocaml_object` in `gc/mmtk/common/src/scanning.rs`). A regression in `mmtk_ocaml_alloc` is the bytecode alloc-fast-path lever (#A1); one in scanning/copy is collector work (#C1–#C3).
4. **Split GC-time vs mutator-time.**
   - **Quick split:** `MMTK_VERBOSE=1` prints `GCs: N, GC time: T ms, objects copied: C` (`mmtk.c:151-158`). `T ÷ wall` = GC-time fraction; `C` trend = copy volume. First question: did total GC time rise (collect-more) or mutator time rise (path-cost) — completely different fixes.
   - **Per-phase/per-domain detail — `runtime_events`+`olly` (once #R1 lands; until then `bpftrace`, §7).**
5. **Characterize distributions — `bpftrace`.** Histograms on the alloc entrypoint (alloc-size → confirms "small short-lived objects" or finds a large-object regression) and on STW enter/leave for pause-length distribution. Where tail-latency regressions (RQ1) that medians hide become visible.

**Output of the playbook** — a one-line attribution: *"+8% wall at 1.5×L on benchmark B is GC-time (fraction 12%→19%), driven by survival ↑ (copy volume +40% per `MMTK_VERBOSE`), localized to evacuation in `mmtk_*`; mutator path unchanged (instructions/byte flat)."* A regression report without this shape is not actionable.

---

## 10. The optimization backlog

The full ranked backlog is **Appendix A** of this document (canonical); ROADMAP open-work #17 carries the summary. The standing order: **obvious removals first, then measure with §2–§9, then the deeper structural levers.** The single dominant lever is #A1 (give bytecode a TLAB / inline its alloc fast path); the dominant collection-side levers are #C1 (per-slot SFT lookup + double slot-load) and #B1 (inline the no-op native write barrier for the default Immix plan). Both marquee static findings (#A1 bytecode no-TLAB at `memory.h:263`; #C1 double slot-load at `slot.rs:92`/`:179`) were spot-verified against the tree.

---

## 11. Quick checklist (paste into every experiment log)

- [ ] Workload fingerprint published first: alloc rate, survival, mutation rate, lifetime-dispersion Gini (§1)
- [ ] Heap swept as a **multiple of live size**, not absolute MiB; results are **curves**, not points (§2)
- [ ] PRIMARY = macro-benches; benches/multicore for scaling; gcbench for fast iteration (§1)
- [ ] Stage 1 (intra-MMTk bake-off → champion) then Stage 2 (champion vs **vanilla 5.5.0**); baseline = separate released-5.5.0 switch, same source/deps/flags, compared at equal total RSS (§3)
- [ ] `setarch x86_64 -R` on **both** sides; governor=performance; `MMTK_PLAN`+`MMTK_HEAP_SIZE_MB`+`MMTK_THREADS`+(domains,workers,cores) recorded (§5)
- [ ] ≥10 fresh-process invocations; **median + dispersion + CI**; counters normalized per byte/instruction (§4)
- [ ] GC-time vs mutator-time split reported; pause distribution (p99/p99.9/max) for any latency claim (§4,§7)
- [ ] Pause times via bpftrace until runtime_events fixed; olly-on-MMTk treated as to-be-validated (§7)
- [ ] Bytecode vs native not mixed; `sanity` off for timing (correctness checked separately) (§5)
- [ ] Timed-out/hung cells reported as failures, never dropped; no outer `timeout` wrapper (§5)

---

## 12. Citations

- Kirisame, Shenoy & Panchekha, *Optimal Heap Limits for Reducing Browser Memory Use* (MemBalancer), PACMPL 6(OOPSLA2) Art. 160, 2022. DOI 10.1145/3563323. arXiv:2204.10455.
- Cai, Blackburn, Bond & Maas, *Distilling the Real Cost of Production Garbage Collectors*, ISPASS'22. DOI 10.1109/ISPASS55109.2022.00005. arXiv:2112.07880.
- Blackburn, Cheng & McKinley, *Myths and Realities: The Performance Impact of Garbage Collection*, SIGMETRICS'04. DOI 10.1145/1005686.1005693.
- Dolan, *Lifetime Dispersion and Generational GC: An Intellectual Abstract*, ISMM'25. DOI 10.1145/3735950.3735958.
- Sivaramakrishnan et al., *Retrofitting Parallelism onto OCaml*, PACMPL 4(ICFP) Art. 113, 2020. DOI 10.1145/3408995.
- de Souza Amorim et al., *Reconsidering Garbage Collection in Julia: A Practitioner Report*, ISMM'25. DOI 10.1145/3735950.3735957.
- Wang et al., *Evaluating GC Performance Across Managed Language Runtimes* (GEAR), ICSE'25. DOI 10.1109/ICSE55347.2025.00218.
- Huang, Blackburn & Cai, *Improving Garbage Collection Observability with Performance Tracing*, MPLR'23. DOI 10.1145/3617651.3622986.

---

**Prerequisites flagged for ROADMAP #17:** two platform deliverables that do not yet exist and that RQ1/RQ2 require: (1) a per-benchmark **lifetime-dispersion (Gini) profiler** (§1); (2) a **per-GC survival / mutation-rate instrument** beyond the at-exit `MMTK_VERBOSE` totals (§1, §7). Plus the **runtime_events emission fix** (§7, backlog #R1–#R4) before olly-on-MMTk is usable.

---

# Appendix A — Ranked optimization backlog (canonical)

*This is the canonical full backlog; ROADMAP open-work #17 carries the summary. Every "impact" tag is a static hypothesis until measured per §2–§9.*

> **Measured — 2026-06-24 (the obvious-removal pass).** Seven obvious-removal levers were implemented and
> correctness-gated (build / mmtk `sanity` 0-invalid-ref / CLBG byte-identical / native compile-repro); six
> passed and were pushed as `perf-lever-*`, one was rejected. **Only #C1-sftbound is load-bearing** (cached
> `[heap_start,heap_end)` pre-check before the per-edge SFT lookup in `FieldSlot::classify`: **+1.26% on
> fannkuchredux, outside noise**; halves the SFT-lookup self% cluster). **#C2/#C4/#B2/#B3/#A3 are correct +
> safe but perf-neutral** (≤ noise on fft/binarytrees/nbody/fannkuchredux). **#C1-double-load was REJECTED —
> the "double load" is NOT removable:** MMTk's sanity GC clones root slots and re-`load()`s them *after* the
> real GC writes forwarded refs, so a cached slot word returns stale pre-GC pointers (dangling edge). Update
> this backlog accordingly — #C1's removable part is the SFT-bounds pre-check, not the second load.
> **Takeaway: micro-levers buy ~1.5%; the gap is structural** — STW root-scan (`caml_call_gc` /
> `caml_garbage_collection` / `caml_find_frame_descr` ≈21% of fft@128, ≈32% of fannkuchredux), Immix
> sweep/metadata (spectralnorm), young-object throughput vs vanilla's minor collector, and #A1 (bytecode TLAB).
> The write barrier is hot in **zero** profiles (why #B2/#B3 are neutral). **Definitive vanilla-vs-MMTk-Immix
> baseline (post-fft-fix `c2560f1596`, native):** nbody 1.005×, fft@128 **1.115×**, fft@default 1.079×,
> spectralnorm **1.738×**, fannkuchredux **0.985×**, binarytrees **0.661×**. The landed fft fix (`46cb3253f2`)
> closed BOTH fft@128 (was 1.69×) and fannkuchredux (was 1.65×) — both were the post-GC poll storm (fannkuchredux
> perf: ~37% of cycles in `caml_call_gc`/`caml_garbage_collection`/`caml_find_frame_descr` PRE → ~0% POST).
> **5 of 6 configs are now parity-or-better; spectralnorm (1.74×, Immix sweep/metadata) is the lone structural
> loss and the genuine M8 target. MMTk wins 1.5× on parallel alloc-heavy (binarytrees).** The one lever worth
> landing is isolated on **`perf-c1-sftbound`** @`b953a50e4` (C1-sftbound on current mainline, ~+1% binarytrees,
> correctness-gated); the neutral cleanups live on `perf-lever-*`. Detail: `gc/mmtk/NOTES.md` (2026-06-24);
> `~/postfix-baseline-findings.md`, `~/perf_opt_findings.md` on turing. **Nothing merged** to `5.5+mmtk`.

## #17 / M8 — ranked optimization backlog (perf work)

Adopt `PERFORMANCE.md` as the method of record FIRST; do obvious removals, then measure each
change with the §2–§9 protocol before believing it. Tags: **[obvious-removal]** = static dead/
redundant work removable with low risk; **[deeper]** = structural, correctness-gated, measure-driven.
Sequencing: cheapest high-impact first within each group.

### Group A — Allocation fast path (mutator)

- **#A1 — Give bytecode a TLAB; inline its alloc fast path. [deeper] — IMPACT: HI (the single dominant lever).**
  - Where: `runtime/caml/memory.h:263-277` (`Alloc_small_with_reserved` `#undef`'d → per-object extern
    `caml_mmtk_alloc_small`); `runtime/mmtk.c:215-233`; `gc/mmtk/binding/src/api.rs:127-163`; call sites
    `runtime/interp.c:642,657,687,785,802,810,820,832,861,1365`.
  - What: bytecode has **no TLAB** — every constructor/closure/float box is a C-call→FFI→Rust round-trip
    where stock and the MMTk *native* path do a 3-instruction inline bump. Replace with an inlined
    `young_ptr -= whsize; if (young_ptr < young_start) refill_slow(); else { Hd_hp=make_header; ...}`,
    reusing the native refill (`caml_mmtk_refill_tlab`, `mmtk.c:267-297`) for the slow path only.
  - Subsumes #A3–#A6 (they live inside the per-object FFI callee the bump eliminates).
  - Est. impact: HI. Risk: MED (interp keeps accu/env/unpublished sp in C locals — must publish before the
    *slow* refill, but no longer per object). This is ROADMAP #17's named "inline bytecode alloc fast-path."

- **#A2 — Move `Setup_for_gc`/`Restore_after_gc` root-publish off the per-object path. [deeper] — IMPACT: HI (coupled to #A1).**
  - Where: `runtime/caml/memory.h:264-277` (the `CAML_MMTK_SETUP_ROOTS`/`RESTORE_ROOTS` wrap + temp-var
    `caml_mmtk_blk` dance); `runtime/interp.c:76-100`.
  - What: today every bytecode alloc does 3 stack stores + sp-publish + 3 reloads unconditionally because
    the out-of-line call can STW. Once #A1's fast path cannot STW, move these into the refill slow path
    (stock only spills on the slow path); the temp-var dance then disappears. Risk: LOW once gated behind
    the bump check (proven on the native side).

- **#A3 — Dedicated `mmtk_ocaml_alloc_default` skipping the `match semantics`. [obvious-removal] — IMPACT: LO/MED.**
  - Where: `gc/mmtk/binding/src/api.rs:135-142`; small-alloc caller always passes `CAML_MMTK_SEM_DEFAULT`
    (`mmtk.c:218`). The 5-arm `match` is a dead choice on the small path. Mostly evaporates under #A1; do as a
    quick independent pass. Risk: trivial.

- **#A4 — Confirm `post_alloc` is elided with `vo_bit` off; gate it out if not. [obvious-removal] — IMPACT: LO.**
  - Where: `gc/mmtk/binding/src/api.rs:160`; `Cargo.toml:14` enables only `object_pinning` (not `vo_bit`),
    so `post_alloc` is documented no-op (`api.rs:195-197`). Verify via objdump it's fully inlined-away; if
    not, gate the call for the Default small path. Must stay if `vo_bit` is ever enabled. Risk: trivial.

- **#A5 — Per-object `stat_minor_words` add. [deeper] — IMPACT: LO (tied to #A1).**
  - Where: `runtime/mmtk.c:231`. Native accounts at block retirement (`mmtk.c:286-288,670-671`); bytecode
    bumps per object only because it has no TLAB. Disappears automatically under #A1.

- **#A-note — Native small-alloc fast path: DO NOT TOUCH.** `asmcomp/amd64/emit.mlp:607-636`,
  `runtime/amd64.S:641-682` are byte-for-byte stock (same `sub/cmp/jb` poison-safepoint, no extra
  branch/dead check, no zeroing/extra alignment — `header.rs:14-17`, `object_model.rs:24,60`). All MMTk cost
  is in the slow path. Flagged so the campaign wastes no effort here.

### Group B — Write barrier (mutator, collection-adjacent)

- **#B1 — Inline the native write barrier; for non-generational (default Immix) make it a plain store. [deeper] — IMPACT: HI.**
  - Where: `asmcomp/cmm_helpers.ml:2290-2296` (`caml_modify` extcall), array-set `:771`, init `:774`;
    `runtime/memory.c:183-200`; gate `runtime/mmtk.c:563-568`.
  - What: every pointer store is an unconditional out-of-line `call` into `caml_modify`→`caml_mmtk_region_barrier`,
    which returns immediately under Immix (`caml_mmtk_generational==0`) — i.e. a full call sequence to do
    nothing. Stock inlines the test and only calls on the slow path. Inline an early-out: load+branch on
    `caml_mmtk_generational` (plan is runtime-chosen so codegen can't hard-assume), or compile `Pointer`
    assignment to `Simple` store under a non-gen plan assumption with a gen fallback. Removes the call for the
    default plan. Risk: MED. The call-removal is **[obvious-removal]**; the generational inlining is deeper.

- **#B2 — `Is_long(val)` short-circuit in `caml_modify`/`caml_initialize`. [obvious-removal] — IMPACT: LO–MED (gen plans).**
  - Where: `runtime/memory.c:300-318` (`caml_initialize` unconditional `caml_mmtk_region_barrier(fp,1)`),
    `:183-200` (`caml_modify`). An immediate value can never be a young pointer; a cheap `Is_long(val)` test
    before the barrier elides most init/modify barriers. Safe/standard. Best landed once #B1's inlining exists.

- **#B3 — Scalar field barrier avoiding the two-slice + dyn-dispatch. [obvious-removal (the empty src slice) / deeper (dyn elim)] — IMPACT: MED (gen plans).**
  - Where: `gc/mmtk/binding/src/api.rs:282-293` builds **two** `OCamlMemorySlice` (dst + empty src) and calls
    through `mutator.barrier()` (a `Box<dyn Barrier>` virtual call) for the scalar count==1 `caml_modify` case
    (`slot.rs:200-204`). Add `mmtk_ocaml_field_barrier(mutator, slot)` calling the slot-remembering fast path
    directly; the empty-`src` construction is an obvious removal. Risk: MED (must match plan barrier semantics).

### Group C — Collection trace loop (collector)

- **#C1 — Per-slot SFT-map lookup + double slot-load in `FieldSlot`. [deeper core; double-load is obvious-removal] — IMPACT: HI.**
  - Where: `gc/mmtk/common/src/slot.rs:89-94` (`from_address` loads the word), `:108-152` (`classify` calls
    `memory_manager::is_in_mmtk_spaces` = chunk-indexed SFT lookup + virtual `is_in_space`), `:174-181`
    (`load` re-reads the word via `raw_value()` `:102-104` — **verified the word is loaded twice**). Runs once
    per pointer field of every live object (`common/src/scanning.rs:86-89,104-107`) + once per root slot
    (`binding/src/scanning.rs:150-161`).
  - Fix: (a) **obvious removal — cache the loaded word** so `load` doesn't re-read (safe for the load that
    immediately follows classify; care under moving plans where the word can change before trace). (b) cheaper
    first-cut heap-range bounds check before the full SFT lookup (most OCaml pointers are in-heap). (c) check
    whether MMTk's own `ProcessEdgesWork` already SFT-filters, making the binding-side filter partly redundant.
  - Est. impact: HI (core per-slot mark cost). Risk: MED (immediate/foreign filtering must stay exact;
    double-load removal interacts with moving plans).

- **#C2 — Single header read + single tag dispatch in `scan_object`. [obvious-removal] — IMPACT: MED.**
  - Where: `gc/mmtk/binding/src/scanning.rs:255-282` calls `continuation_stack(object)` (header read + tag
    compare, `common/src/scanning.rs:25-38`) for **every** object, then `scan_ocaml_object` reads the **same
    header again** (`common/src/scanning.rs:51`). Read the header once, dispatch on tag there, fold the
    continuation case into the existing `match tag` (`scanning.rs:61`). Pure read/dispatch-order refactor.
    Risk: LOW.

- **#C3 — Gate the infix/forwarding logic behind a "plan moves objects" flag. [deeper] — IMPACT: MED.**
  - Where: `slot.rs:146-151` (`classify` reads header at `addr-WORD_SIZE` unconditionally + `is_forwarded`
    side-metadata `:57-63`). For non-moving plans nothing is ever forwarded and infix offsets never need
    recompute — collapse `classify` to immediate/null/foreign→`NOT_TRACEABLE` else `0`, dropping the header
    load and side-metadata read from the common case. Flag must be "is this plan ever-moving" (Immix defrag
    moves, so Immix can't take the cheap path unless defrag is statically disabled). Risk: MED.

- **#C4 — Hoist `debug_check_enabled()` out of the per-root callback. [obvious-removal] — IMPACT: LO.**
  - Where: `gc/mmtk/binding/src/scanning.rs:150-161` calls `debug_check_enabled()` (`:104-107`) per root slot.
    Select between `collect_root_slot` and a debug variant once per `scan_roots_in_mutator_thread`, so the
    inner callback has no branch. Risk: LOW.

- **#C5 — Lighter slot type for the array-blit barrier path. [deeper] — IMPACT: LO (gen + array-blit).**
  - Where: `slot.rs:226-235` (`OCamlSliceIter::next` builds a full `FieldSlot::from_address` per slot,
    incl. #C1's SFT lookup) — but the remembering step only needs addresses, not infix classification.
    Shares the fix with #C1/#C3. Risk: MED.

### Group D — STW handshake (pause latency)

- **#D1 — Stop the 1 ms-quantized re-poison loop in `stop_all_mutators`. [deeper — correctness-critical] — IMPACT: MED (latency).**
  - Where: `gc/mmtk/binding/src/collection.rs:254-270` (loops: re-`caml_mmtk_interrupt` every domain `:258-259`,
    `wait_timeout(... 1 ms)`); domains were already poisoned at `:243-245`. The 1 ms timeout dominates stop
    latency when a straggler is briefly in C. Rely on the condvar notification (already done on every STOPPED
    edge) and re-poison only on a longer fallback. **Risk: HI** — this is the locus of the bug #3/#3b deadlock
    fixes (`collection.rs:1-37`, `runtime/mmtk.c:570-620`); validate against the spawn/terminate-burn tests.
    NOT an obvious removal — defer behind correctness gating.

### Group R — runtime_events / olly observability (blocks the measurement story)

- **#R1 — Emit a real GC-STW span around the MMTk pause. [deeper] — IMPACT: HI (makes olly's latency histogram correct).**
  - Where: hook begin at `gc/mmtk/binding/src/collection.rs:230` (`GC_PAUSE_START`), end at `:287` (elapsed
    folded). Caveat: `caml_ev_*` needs a domain with a valid ring slot — the GC worker is not a domain. Emit
    from the triggering mutator domain around `block_for_gc`/`caml_mmtk_park` (`mmtk.c:615`), or add a C entry
    the worker calls that writes a designated domain's buffer. Use `EV_MAJOR_GC_STW`. Risk: MED (ring thread-
    affinity).

- **#R2 — Source minor/major *words* counters from the MMTk odometer. [obvious-ish] — IMPACT: HI.**
  - Where: `EV_C_MINOR_ALLOCATED_WORDS` should emit `stat_minor_words` (real odometer in `mmtk.c`), not stock
    `young_end-young_ptr` (= 0 in bytecode); `EV_C_MAJOR_HEAP_WORDS` from `caml_mmtk_gc_stats` (`mmtk.c:530`).
    Emit at a real cadence (per MMTk GC, via #R1). Data already exists — just emit the right source. Risk: LOW.

- **#R3 — Stop/relabel the WRONG GC spans. [obvious-removal] — IMPACT: MED (stop polluting olly).**
  - Where: `EV_MINOR` (`minor_gc.c:218,290`), `EV_EMPTY_MINOR`, `EV_MAJOR` (`domain.c:1951`), `EV_C_MAJOR_*`
    (`major_gc.c:927-937`) wrap no-ops/empty heaps and actively mislead olly. The `EV_MAJOR` span around the
    inert `caml_major_collection_slice` (`major_gc.c:1012`) is the clearest removal. Removing phantom events
    can't break correctness. Risk: LOW.

- **#R4 — Add mark / sweep-release / evacuate phase spans. [deeper] — IMPACT: MED.**
  - Where: `EV_MAJOR_MARK`, `EV_MAJOR_SWEEP`, and especially `EV_COMPACT_EVACUATE` are MISSED; evacuation is
    *the* distinctive MMTk Immix phase (`mmtk_ocaml_objects_copied()` proves it runs, `mmtk.c:156`) and is
    currently invisible. Also `EV_MAJOR_EPHE_MARK/_SWEEP` around `caml_mmtk_ephe_mark_pass`/`_clean_pass`
    (`mmtk.c:474,488`). Needs the same worker→ring plumbing as #R1 plus scheduler hooks. Risk: MED.

### Group P — measurement prerequisites (platform deliverables RQ1/RQ2 require)

- **#P1 — Per-benchmark lifetime-dispersion (Gini) profiler.** Age-at-death histogram → Gini (Dolan ISMM'25).
  Does not exist; required for the §1 workload fingerprint. [deeper]
- **#P2 — Per-GC survival / mutation-rate instrument** beyond the at-exit `MMTK_VERBOSE` totals. [deeper]

### Suggested sequence
1. **Obvious removals, low risk, immediate (a quick pass before any campaign):** #A3, #A4, #C2, #C4, #R3,
   #C1-double-load, #B3-empty-slice. Verify each with `perf record` before/after.
2. **High-impact mutator levers (measure-gated):** #B1 (+#B2) then #A1 (+#A2, which subsumes #A5).
3. **High-impact collection lever:** #C1 (full), then #C3, #C5.
4. **Observability so the campaign is measurable:** #R2, #R1, then #R4; #P1/#P2 in parallel.
5. **Latency, last (correctness-critical):** #D1.
