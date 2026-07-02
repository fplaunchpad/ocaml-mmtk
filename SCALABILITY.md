# Multi-domain GC scalability of `mmtk-ocaml` — findings

> ## ⚠️ UPDATE (2026-06-25) — the anti-scaling headline below is SUBSTANTIALLY REVISED
>
> A **controlled re-run** (core-pinned, **`MMTK_THREADS=domains`** so domains + GC workers ≤ cores, and
> with the **GH#6 scheduler-assert deadlock fixed**, mmtk-core `ec2f5079f8`) shows the dramatic
> "anti-scaling" was **largely a measurement artifact**, not a structural STW-pause property:
>
> - **The GC-worker count WAS a dominant factor** — contradicting the old TL;DR's "*not* the worker
>   count". The old runs used **`nproc` workers, unpinned**, so at high domain count ~20 threads
>   contended for 12 cores and idle workers park/wake on every collection. The old experiments compared
>   *plans at fixed `nproc`* and never tested `workers=domains`, so they missed this confound. The same
>   `nproc` tax also inflated the *single-domain* sequential numbers (`LU_decomposition` is **2.26× at
>   `nproc` → 1.05× at `MMTK_THREADS=1`**; it triggers ~2467 tiny collections from boxed intermediates).
> - **Controlled scaling (turing, 28-core, pinned, `workers=domains`):** `par_matmul` scales **as well as
>   vanilla** (GenImmix `S(8)=4.71`, ConcurrentImmix `5.78` ≈ vanilla `5.72`); `par_binarytrees` GenImmix
>   is **`S(8)=1.36`** (mildly sublinear) — *not* the `0.64` cliff below; on `par_spectralnorm` even
>   vanilla regresses past d4 (memory-bandwidth bound) and StickyImmix scales *better* than vanilla.
> - **The old "GenImmix cliffs / times out past 16 domains" was the GH#6 deadlock** — the STW-only
>   `scheduler.rs` assert firing plan-independently at ≥8 domains (a 2nd domain's alloc poll, or a domain
>   being *created* refilling its TLAB, requests a GC mid-GC → worker panic → poisoned WorkerMonitor →
>   all domains deadlock). rr-confirmed; the assert is **removed for all plans** (`ec2f5079f8`). It was
>   **not** a structural pause blow-up.
> - **Real residual:** a *mild* sublinear gap on the heaviest-alloc bench (`par_binarytrees`), plus a
>   **rarer intermittent multidomain-rendezvous hang** that survives the assert fix (the bug#3c-class
>   residual — the experiments below partly describe it; needs more rr).
>
> **Net:** the flagship "anti-scaling" deflates to a *mild* residual once `nproc` oversubscription and the
> GH#6 deadlock are removed. The detailed experiments below are retained as historical record, but their
> headline conclusions — especially "**not** the worker count" and `S(8)=0.64` — are **superseded** by
> this update. The real open levers are a **domain-aware GC-worker pool** and the per-minor-collection
> cost (RQ10).

> ## ⚠️ UPDATE 2 (2026-06-25) — the residual is MILD (S(8)≈1.2–1.6); two levers (nursery size; off-STW marking); pole-B NO-GO. A first church run was a CONTAMINATED build — corrected here.
>
> The "mild residual" from UPDATE 1, measured **clean** (turing, 28-core, pinned, **`MMTK_THREADS=domains`**,
> fixed 4 GiB, `par_binarytrees d21`, 3 reps, min wall-time, verbose GC counts):
>
> | config | d1 | d2 | d4 | d8 | **S(8)** | GCs d1→d8 |
> |---|---|---|---|---|---|---|
> | GenImmix-default (64 MiB) | 9.32 | 7.44 | 6.45 | 7.60 | **1.23** | 114→165 |
> | GenImmix-256 MiB | 7.78 | 5.85 | 4.71 | 4.88 | **1.59** | 28→86 |
> | StickyImmix-default | 10.41 | 8.03 | 6.72 | 7.65 | **1.36** | 109→163 |
>
> - **The residual is mild and similar across plans** (~1.2–1.6), all regressing past d4 — *not* a cliff.
> - **Lever 1 — nursery SIZE:** GenImmix-256 MiB is ~20% faster at every domain count + S(8) 1.23→1.59
>   (commit-on-demand ⇒ ~free for small programs). The cheap, shippable win → **ROADMAP #21**.
> - **Lever 2 — off-STW marking:** even GenImmix-256 MiB (28–86 GCs) still regresses d4→d8, so the nursery is a
>   *level* shift, not a *slope* fix; the residual slope is the per-collection all-domains STW cost — **this
>   vindicates §10.2 / UPDATE 1** and is what ConcurrentImmix (trace off the STW), not pole-B, addresses.
> - **BUG B (mainline-confirmed):** `MMTK_NURSERY="Bounded:2m,64m"` (documented) silently parse-fails → mmtk-core
>   default; only raw bytes parse. → #21.
> - **RQ10 pole-B** (VM-owned ParMinor + MMTk major-only): **NO-GO** — it keeps the all-domains minor STW (so it
>   wouldn't fix Lever 2's slope) and Lever 1 is a config knob; no VM-ParMinor rebuild is justified.
>
> **⚠️ Correction.** A first pass ran on **church** (branch `fix/bug3c-cross-stw`), whose build silently ran an
> ~8 MiB default nursery (913 GCs) and produced a dramatic "GenImmix S(8)=1.13 vs StickyImmix 2.86" + a "BUG A:
> degenerate default install" claim. **Clean rebuilds on local + turing refute it** (default = 114 GCs = correct
> 64 MiB; turing even shares church's mmtk-core `0fe660bb9c`). BUG A is a church branch/build artifact, not
> mainline. The §11 tables marked *(church, contaminated)* are kept only as a record of the artifact.

> ## ⚠️ UPDATE 3 (2026-07-01) — the residual slope is the STW **mature/full-GC FREQUENCY**, and it scales ~linearly with domain count. LXR + ConcurrentImmix added.
>
> New parallel panel (M4 Pro, 8 P-cores; `quickbench.py par`; strong scaling; **speedup T(1)/T(8)**):
> `par_binarytrees` — vanilla **3.68** / GenImmix **0.90** / ConcurrentImmix **1.11** / LXR **1.26**;
> `par_matmul` 6.38 / 3.31 / 4.08 / 3.37; `par_spectralnorm` 4.77 / 2.22 / 1.83 / 2.62.
>
> **Root cause, sharpened + code-grounded.** Experiments 2/3 below already pinned it to *STW trace-in-pause*;
> the decomposition is now precise: **the amplifier is mature/full-GC FREQUENCY, not the per-minor copy volume.**
> GenImmix `par_binarytrees` d1→d8 at FIXED total allocation: **full (mature) GCs ×2.4–5.0**, total GCs only
> ×1.48, objects-copied ×2.1–2.7, **GC-time tracks the FULL-GC count** (d18 probe: full 9/19/31/45 ↔ GC-time
> 148/259/366/494 ms). Mechanism: with N domains ~N trees are concurrently live at each global STW minor GC →
> promotion scales ~N → the Immix **mature space fills ~N× faster** → the mature-pressure trigger
> (`binding/src/collection.rs` `MATURE_PRESSURE_OVERHEAD_PCT=120` / nursery-cadence, ~`:108–135`; `stop_all_mutators`
> `:322`) fires ~N× more → ~N× more **whole-mature-heap STW traces** on the critical path.
>
> **Why stock scales (3.7×) and we don't:** the fork **deleted stock OCaml's mostly-concurrent major GC in M9**
> (`dcb35ef00` "delete the stock shared heap"; `shared_heap.c` gone; stock's `caml_major_collection_slice` in
> `runtime/major_gc.c` is bypassed). Stock absorbs the extra promotion as *concurrent* mature work overlapping the
> mutators; MMTk GenImmix does it as *synchronous STW*, then scales that pause's frequency with N. **That design
> choice — where mature reclamation runs, on-STW vs concurrent — is the sole determinant of scaling here.**
> Corroboration: **ConcurrentImmix** (off-STW trace) keeps GC-time nearly flat & copied=0 and out-scales GenImmix.
> **LXR** (incremental RC) is fastest single-domain and beats GenImmix at every domain count, **but its RC-pause STW
> is NOT flat** — it climbs 9→74% with domains (see the STW-fraction table below; this corrects an earlier
> "LXR GC-time ~constant" claim), because RC increment/decrement *pause* processing is itself stop-the-world and its
> volume scales with mutation/allocation ∝ domains. So only ConcurrentImmix keeps STW flat. The **nursery-size
> control** refutes starvation (128 MiB @d8 cuts GC count but makes wall *worse*
> 2.66→3.21 s → it's per-collection STW *cost*, not thrash). `par_matmul` (GC-light) scales fine on every plan —
> the gap appears *only* under mature promotion, exactly as predicted.
>
> **`MMTK_THREADS` control (rules out GC-worker contention as the binarytrees cause).** GenImmix `par_binarytrees`
> S(8): **1 GC worker = 0.50×**, workers=domains = **0.88×** — i.e. *fewer* GC threads makes the anti-scaling
> WORSE, not better. At d8 the full-GC *count* is identical (80 vs 79) but GC-time is **3825 ms serial vs 2468 ms
> with 8 workers**: the parallel workers HELP because the STW mature trace is real parallelizable work, and its
> *frequency* (∝ mature pressure ∝ domains) is unaffected by worker count. So the alloc-heavy anti-scaling is the
> STW mature trace itself, NOT worker oversubscription. (Opposite on GC-LIGHT `par_spectralnorm`: 1 worker = 2.61×
> beats workers=domains = 2.28× — there the 8-worker park/wake overhead exceeds the tiny GC work. Two distinct
> effects; the mature-trace one dominates wherever promotion is high.)
>
> **Fixable, not fundamental.** Stock scales 3.93× on the identical bench, so the *anti-scaling* is a design
> artifact of on-STW mature reclamation (removable). What IS baked-in is OCaml 5's global STW-minor barrier, which
> caps even the best case at stock's ~0.49 efficiency — reaching stock parity, not linear speedup, is the target.
> Fix path (ranked): (1) **ConcurrentImmix + native SATB** — concurrent MARK is read-barrier-free in OCaml (RQ1:
> init-write-dominated), the cheap high-impact win; its continuation-scan hang (GH#4/#14) is **already fixed** and
> it is **correctness-ready as the parallel default** (only the deferred UNLOG-bit barrier-gate PERF item `#30`
> remains, not a blocker); (2) **domain-aware full-GC trigger**
> (`MATURE_PRESSURE_OVERHEAD_PCT` is domain-blind → fires ~N× more) to cut frequency; (3) **LXR** as the
> read-barrier-free *moving*-reclamation research vehicle (concurrent EVACUATION is the hard part — needs a read
> barrier or RC); (4) non-gen **Immix** as the pragmatic interim parallel default.
>
> **RQ10 sharpened:** *can a third-party moving generational GC be integrated behind OCaml 5's multicore runtime so
> that mature reclamation is concurrent/incremental (à la the deleted stock major, or LXR's RC) — no read barrier,
> keeping the copying nursery — recovering vanilla's ~N× minor scaling?* OCaml's init-write-dominated allocation
> (RQ1: field barrier near-free) is exactly what makes off-STW mature reclamation cheap here — the scalability fix
> and the low-latency-RC bet are the same.
>
> **STW-WALL DECOMPOSITION — caveat CLOSED, cross-host (2026-07-01).** `gc_time_ms` turns out to BE the STW
> **pause-wall**: a single `Instant` span from `stop_all_mutators` to `resume_mutators` (`collection.rs:328` sets
> `GC_PAUSE_START`, resume adds elapsed to `GC_NANOS`; doc `:84`), NOT aggregate worker CPU — my earlier caveat was
> wrong. So **STW-fraction = `gc_time_ms / wall`** is a direct wall measurement. par_binarytrees 20,
> MMTK_THREADS=domains, **STW-wall as % of total wall**:
>
> | domains | 1 | 2 | 4 | 8 | 16 | 28 |
> |---|--:|--:|--:|--:|--:|--:|
> | **GenImmix** (turing, 28c) | 73 | 76 | 83 | 91 | 93 | **94** |
> | **ConcurrentImmix** (turing, 28c) | 12 | 7 | 11 | 13 | 14 | **15** |
> | **LXR** (turing, 28c, 1 GiB) | 9 | 15 | 27 | 51 | 74 | **71** |
> | GenImmix (M4 Pro, 8c) | 60 | 80 | 90 | 94 | — | — |
>
> (turing rows: one clean build `a52e3a776`, MMTK_THREADS=domains, single rep — the monotone trend is robust.)
> GenImmix's STW fraction climbs and **plateaus ~94%** — at high domain count the program is almost entirely
> stopped-the-world, so there is no mutator parallelism left to gain (its wall bottoms at d4 then *rises* d8→d28) →
> anti-scaling. **Only off-STW ConcurrentImmix stays flat (~7–15%)** across d1→d28 → it is THE clean fix, and it
> holds at scale. **LXR does NOT stay flat** (this corrects an earlier over-optimistic "LXR GC-time ~constant"
> claim): its STW fraction climbs 9→**74%**, because LXR's **RC increment/decrement *pause* processing is itself
> stop-the-world and its volume scales with domains** (dec-buffer size ∝ mutation/alloc ∝ domains; par_binarytrees
> is acyclic so ~0 backup traces — this is pure RC pause work). LXR is still markedly better than GenImmix — lower
> STW-plateau (74% vs 94%) and ~2× lower absolute wall (d8 2.3 s vs 5.8 s) — but it does *not* escape the
> "on-STW work scales with domains" trap; **only making the reclamation concurrent (ConcurrentImmix) does.** The
> M4 and turing GenImmix curves agree (60→94 vs 73→94), so it is not a host artifact. Method note: the 6 structured lenses of the analysis
> workflow failed on an output-schema bug; synthesis was self-verified + code-checked and the STW-wall
> decomposition is now cross-host-confirmed — but the independent adversarial-verify layer did NOT run, so treat the
> mechanism as strong + measured and the ranked remedies as one-analyst.

> ## ⚠️ UPDATE 4 (2026-07-02) — quantified decomposition on turing (28c): vanilla does ZERO major work on every cell; MMTk's pacing MANUFACTURES domain-scaled full-GC work; Bactrian's concurrency doesn't rescue it; plus a zero-GC layout stall on matmul.
>
> Full three-phase campaign on turing (perf + GC-work accounting, mainline `fefb82c5b5`,
> `~/phase{1,2,3}-results.txt`; vanilla = `OCAMLRUNPARAM=v=0x400`, MMTk = `MMTK_VERBOSE=1`;
> par benches, domains 1/8/24, reps 3).
>
> **1. Vanilla completes ZERO major cycles on every single cell** (spectralnorm d8: 854 minors/0 major;
> binarytrees d8: 350 minors/0 major; matmul d24: 18/0) while MMTk runs **domain-scaled FULL-heap STW
> collections**: spectralnorm fulls 71 (d1) → 328 (d8) → **807 (d24)**; binarytrees fulls 32 → 83 → 160.
> The GH#5 mature-pressure(120%)+cadence(8-minor) trigger against a shared nursery whose fill rate scales
> with domains converts domain-scaled minor traffic into domain-scaled WHOLE-HEAP STW work that stock —
> allocated-words-paced, generous `space_overhead`, concurrent major — simply never does on these runs.
> STW-wall: binarytrees d8 GenImmix **5.9 s of 6.4 s wall (92%)**; d24 95%; spectralnorm d24 71%.
> The single-worker control magnifies it (binarytrees d8 `MMTK_THREADS=1`: **21.2 s STW of 21.7 s wall, 98%**).
>
> **2. perf-stat separation of work vs stalls vs churn (d8):**
> - binarytrees: MMTk executes **4.3× vanilla's instructions** (124e9 vs 29e9) at half the IPC
>   (1.37 vs 2.23) and 3.2× the LLC misses — the manufactured traces are real *work*, not just pauses.
>   Symbols: 28% mark-CAS (`atomic_compare_exchange_weak`) + 17% `copy_object` + ~8% side-metadata.
> - spectralnorm: instructions +3% only, **mutator wall ≈ vanilla** (0.99−0.27 GC ≈ 0.69 s vanilla @d8) —
>   "MMTk doesn't scale without GC work" is really "MMTk CREATES GC work". The pause machinery churns:
>   context-switches 3.9 k (vanilla) → **76 k** (GenImmix d8) → **634 k** (d24, 31 k/s, IPC 1.07); at d24
>   the profile is ~18% kernel futex/sched + **8.7% `Mutex::lock_contended`** (scheduler/work-bucket locks)
>   vs ~26% OCaml compute — the 807-pause × 24-worker rendezvous burns more than the computation.
> - **Bactrian does NOT rescue the parallel case** (new since UPDATE 3): binarytrees d8 wall 8.3 s (worse
>   than GenImmix's 6.4) — STW drops to 4.9 s but mutator-side time explodes 0.5→3.4 s with **752 k
>   context-switches (27× GenImmix)** and IPC 0.98: with the pressure trigger firing every ~3 minors it is
>   *permanently mid-cycle* (SATB always armed, `ConcurrentTraceObjects` 18% of samples, workers racing
>   mutators, 2.9% lock_contended), and RSS balloons on floating garbage. Concurrency relocates the
>   manufactured work off the pause; it does not remove it — **the trigger design is the root cause**, and
>   allocation-paced cycles (BACTRIAN.md closing-step #4) are now the top-ranked fix, ahead of concurrent
>   sweep.
>
> **3. A separate, zero-GC mutator-side finding (matmul, Xeon-specific):** par_matmul d1 runs **0 GCs**
> under GenImmix/Bactrian yet is **1.49×** vanilla (3.45 vs 2.32 s) with **identical instruction counts**
> (+1%) and *fewer* L1/LLC/dTLB misses — IPC 2.87→1.94. `perf annotate` pins **40% of kernel cycles on the
> row-header bounds-check load** (`mov -0x8(%rdi)`, 9% under vanilla, identical codegen); the gap vanishes
> when the matrices fit L2 (256×256: 0.07 vs 0.08 s) and is absent on the M4 (0.87×). Layout-induced
> memory-hierarchy stall — bump-packed 6208 B row stride vs malloc's ~8 KiB page-spread rows
> (4K-aliasing/prefetcher interaction suspected). Nursery-size control refutes the promoted-compaction
> theory (256 KiB nursery → 66 GCs → *slower*, 3.76 s). Open micro-item with a clean repro; sequential-panel
> matmul ratios on Xeon carry this and it is NOT GC-machinery cost.

> ## ✅ UPDATE 5 (2026-07-02) — allocation-paced trigger LANDED (`33ae0009f8`): manufactured full-GC work de-manufactured (spectralnorm d24 fulls 807→39); binarytrees wall −22/−25% at d8/d24; matmul termination-GCs → 0; the fix also exposed and closed GH issue 3 (lost remembered-set at domain termination). Residual = legit trace work + the STW *minor*-pause rendezvous floor.
>
> UPDATE 4's verdict ("the trigger design is the root cause") is now *acted on and re-measured*
> (same turing protocol, mainline `33ae0009f8`, `/tmp/turing-after.log`; medians of 3).
> Three landed changes:
>
> 1. **Domain-termination full GCs eliminated** (`runtime/domain.c` + binding `api.rs`): domain
>    termination now runs a *minor* collection (promote the `Domain.join` result out of the dying
>    domain's nursery, with a promotion-retry loop) instead of `Gc.full_major`-equivalent exhaustive
>    collections — the old path manufactured **one whole-heap STW GC per spawned domain** (fulls ==
>    spawn count: matmul d8/d24 had exactly 8/24… and short-lived-domain programs had hundreds).
> 2. **Allocation-paced mature-pressure trigger** (binding `collection.rs`): the GH#5 pressure floor
>    is now `max(32 MiB, nursery size)` of *newly promoted* pages (was a tiny fixed floor that fired
>    every ~3 minors under multi-domain promotion), and the fallback cadence scales per-domain
>    (8 × ndomains minors, was flat 8) — pacing full GCs by allocation, à la stock's
>    allocated-words/`space_overhead` pacing, not by domain-scaled minor frequency. (A flat cadence-64
>    first cut stretched `weaklifetime`'s finalization latency into a testsuite timeout — per-domain
>    scaling keeps d=1 behaviour identical to before.)
> 3. **GH issue 3 root-caused and fixed** (binding `active_plan.rs`, `aa60e04407`): the change from
>    exhaustive to minor termination-GCs *unmasked* a latent crash — a terminating domain's mutator was
>    deregistered **without flushing its thread-local remembered-set buffers**, dropping old→young edges;
>    the old exhaustive full GCs had been hiding it (a full trace needs no remset). 20/20 crash-free
>    (was ~50% crash at d=8 spawn churn); rr chaos trace `~/rr-gh3-1000-1` preserved.
>
> **Before → after (turing medians; fulls = whole-heap STW collections per run):**
>
> | cell | fulls before | fulls after | wall before | wall after |
> |---|--:|--:|--:|--:|
> | par_spectralnorm GenImmix d=8 | 328 | **37** | 0.99 s | 1.05 s |
> | par_spectralnorm GenImmix d=24 | **807** | **39** | 1.95 s | 1.90 s |
> | par_binarytrees GenImmix d=8 | 83 | **20** | 6.4 s | **5.0 s (−22%)** |
> | par_binarytrees GenImmix d=24 | 160 | **22** | 8.4 s | **6.3 s (−25%)** |
> | par_binarytrees Bactrian d=8 | ~165 | **19** | 8.3 s | **5.9 s (−28%)** |
> | par_matmul (all plans) d=24 | 24 (= spawns) | **0** | 0.66 s | **0.32 s** |
>
> M4 panel: `par_binarytrees` GenImmix S(8) 0.89 → **1.02** (anti-scaling eliminated); Bactrian's
> 8-domain peak RSS 1689 → **504 MiB** (the paced trigger caps its mid-cycle floating garbage).
>
> **What the residual is (the new sharp question).** With the manufactured majors gone, spectralnorm
> d24 wall barely moved (1.95→1.90 s) — its GC bill (~1.3 s of 1.9 s) is now **~1300 STW *minor*
> pauses × ~1 ms each**, and at 24 domains that ~1 ms is mostly the 24-domain + GC-worker rendezvous,
> not copying (d1 pays ~0.1 ms/minor for the same nursery). The manufactured-major problem was hiding
> a **minor-pause frequency/rendezvous floor** — attack it via nursery scaling per domain, or stock's
> trick: keep minors stop-the-world but make them *rare and cheap* (domain-local minor GCs are stock's
> actual answer; MMTk's shared-nursery design pays a global rendezvous per fill). `par_binarytrees`'s
> remaining 3–4× vs vanilla is legitimate domain-scaled trace work (fulls 20 × ~200 ms) — next levers
> are concurrent sweep + mutator-paced marking (BACTRIAN.md closing steps 2–3).
>
> ### Prioritised residual culprits (post-trigger-fix; ranked by measured cost)
>
> 1. **STW minor-pause frequency × rendezvous cost — the shared global nursery.** Dominates GC-light
>    workloads. Evidence: spectralnorm d24 = ~1300 STW minors × ~1 ms ≈ 1.3 s of 1.9 s wall; the ~1 ms
>    is mostly the 2N-thread rendezvous, not copying (d1: ~0.1 ms/minor, same nursery). One shared
>    nursery fills ~N× faster with N domains → minor *frequency* scales with N, and each fill stops
>    everyone. Stock's answer is domain-**local** minor heaps (no global rendezvous per fill).
>    Levers: nursery size × ndomains (cheap experiment, bounded win, more promotion per pause) →
>    domain-local nurseries (RQ10 pole-A — structural, the real fix).
> 2. **All mature reclamation is STW — stock's concurrent major was deleted in M9.** Dominates
>    alloc-heavy workloads: binarytrees' remaining 3–4× vs vanilla is legitimate domain-scaled trace
>    work paid on the wall (fulls ~20 × ~200 ms at d24; STW share still ~90%). Proof pause-frequency
>    is the poison, not tracing: plain Immix scales 3.20× where every generational plan sits ≤1.02.
>    Levers: concurrent/lazy sweep (design in BACTRIAN.md step 3) + mutator-paced mark slices (step 2).
> 3. **GC-worker scheduler churn per pause — the multiplier on 1 and 2.** Context switches 3.9 k
>    (vanilla) → 76 k (GenImmix d8) → 634 k (d24); 8.7% `Mutex::lock_contended` + ~18% kernel
>    futex/sched at d24 (UPDATE 4). Every pause wakes and parks the whole worker pool through the
>    work-bucket scheduler. Levers: `MMTK_THREADS=domains`, fewer worker wakes for small nursery
>    packets.
> 4. **Bactrian's paced-pauses-vs-floating-garbage trade.** The pacing fix capped its RSS
>    (1689→504 MiB at d8) but it now pays paced STW fulls on the wall (M4 binarytrees S(8) 0.67).
>    Fixed by the same levers as 2, plus value-filtered remset (BACTRIAN.md step 4) to cut
>    InitialMark/FinalMark seed traffic.
> 5. **Non-GC: the Xeon header-load stall** (matmul 1.49× at d1 with ZERO GCs; layout/4K-aliasing;
>    absent on M4, vanishes in L2 — UPDATE 4 item 3). Inflates turing parallel ratios; not GC
>    machinery; open micro-item.
>
> Recommended attack order: concurrent sweep first (design done; hits 2 and 4 at once), then the
> nursery-per-domain-scaling experiment (cheap; quantifies how much of 1 is size vs architecture —
> its result decides whether RQ10 pole-A deserves a full build), then scheduler-churn reduction.

> ## ✅ UPDATE 6 (2026-07-02) — per-domain nursery scaling LANDED: the culprit-1 experiment ran, and size is most of it. par_binarytrees d=8 wall 2.53→0.44 s (5.7×, GC time ÷9, copied objects ÷9 — premature promotion was the hidden half); par_spectralnorm 0.74→0.45 s (minors ÷3.4) at an RSS trade (88→320 MiB).
>
> KC's direction: don't build domain-local *collection* yet — first scale the minor-heap area with
> the domain count, lazily (stock parity: stock gives each domain its own 2 MiB arena, so total
> nursery capacity is N×2 MiB and sits OUTSIDE the major-heap budget). Landed (`ed02eafc6b` +
> binding): the default `Bounded:2MiB,64MiB` budget is now scaled by the **live domain count** —
> effectively `Bounded: N×2MiB, N×64MiB` — latched from the domain registry at spawn/termination
> and consumed **lazily at the next trigger check** (the budget is a pure accounting number: no
> eager mapping/copying; a termination that leaves usage above the shrunk budget simply triggers
> the next minor GC). An explicit `MMTK_NURSERY` pin is never scaled; opt out with
> `MMTK_NURSERY_PER_DOMAIN=0`. Single-domain (scale=1) behaviour is bit-identical.
>
> **The trap the first cut hit (and why stock never sees it):** MMTk's nursery lives INSIDE the
> heap budget, and the fork's space-overhead trigger sizes the heap to live×2.2 with no nursery
> term — so a scaled budget larger than a tiny-live heap made `virtual_memory_exhausted()` convert
> EVERY collection to full-heap (spectralnorm d=8: 28 → 671 fulls). Fix (both no-ops at scale=1):
> the heap target now adds the scaled-up portion of the budget × (1 + worst-case copy expansion);
> pinned heaps (no headroom mechanism) instead cap the scaled portion at heap/4.
>
> **Measured (M4, GenImmix, d=8, 3 reps; scale-on vs `MMTK_NURSERY_PER_DOMAIN=0`):**
>
> | bench | wall | GCs (full) | GC time | copied | maxRSS |
> |---|--|--|--|--|--|
> | par_binarytrees 20 | 2.53 → **0.44 s** | 337 (22) → **60 (5)** | 2350 → **250 ms** | 37 M → **4 M** | 534 → 546 MiB |
> | par_spectralnorm 4000 | 0.74 → **0.45 s** | 853 (33) → **253 (13)** | 405 → **116 ms** | ~2.8 k | 88 → **320 MiB** |
>
> Outputs byte-identical; d=1 GC counts identical; Bactrian sees the same win (psn d=8: 247 GCs,
> 111 ms). The binarytrees ÷9 in *copied objects* is the buried lede: the bigger per-domain budget
> lets short-lived allocation DIE YOUNG instead of being promoted at the next (too-early) shared
> fill — culprit 1 wasn't just pause *frequency*, it was **premature promotion feeding culprit 2's
> mature trace work**. The residual after this is the per-pause rendezvous cost itself (still ~1 ms ×
> fewer pauses) and genuinely-live trace work.
>
> **Full-panel confirmation (M4, 5 reps, same day):** `par_binarytrees` GenImmix S(8) 1.02 → **5.62**
> with absolute wall **395 ms vs vanilla's 406 ms** — the generational anti-scaling story is over on
> this panel; Bactrian 0.67 → **3.40** (RSS@8 561 MiB). Compute benches: GenImmix/Bactrian
> par_spectralnorm 2.4 → **3.4** (RSS 87 → 318 MiB, the capacity trade); the 2.8–3.8× band vs
> vanilla's 5–6.7× is the minor-rendezvous floor (culprit 1's *architecture* half). Sequential panel
> unchanged (scale=1 no-op). New coverage: `chameneos_redux` now runs on ALL plans (LXR fiber guards
> landed) and is pathological everywhere (GenImmix d=8 wall 12.3 s vs vanilla 0.32 s, RSS 1.7 GB) —
> the fiber/continuation scan path is the sharpest open workload class. Turing d=24 revalidation owed.

**TL;DR (SUPERSEDED — see the UPDATE above).** On allocation/GC-heavy parallel workloads the MMTk fork's default plan
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

**Refinement (§10).** A later round of runs splits the problem cleanly. GenImmix's
*single-domain* weakness on alloc-heavy code is a **mis-sized 8 MB default nursery**, not the
plan: enlarging it to 32–64 MB makes GenImmix 1.3–3× faster *and* lowers RSS, and at memory
parity GenImmix(64 MB) is competitive-to-best single-domain (fastest *and* leanest on
spectralnorm). But the nursery does **not** fix the *multi-domain* anti-scaling (S(8) stays
0.71 with 8× fewer collections — the per-collection STW cost grows with domains regardless of
frequency), and at high domain count the generational plans **cliff** (GenImmix times out
past 16 domains on a 56-core box). So the actionable conclusion is **not** a blanket switch to
Immix (whose single-domain edge largely evaporates once GenImmix's nursery is tuned, at 3–4×
the RSS): it is (1) **enlarge the default nursery** for the common single-/few-domain case,
and (2) **steer parallel-heavy workloads to Immix/ConcurrentImmix by plan selection**, with
**ConcurrentImmix the genuinely better default once its production work (#30) lands.**

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

---

## 10. Overnight expansion — high-domain matrix, nursery sizing, refined recommendation

Three follow-up runs sharpen Sections 6–7. **Net: GenImmix's single-domain weakness is a
mis-sized default nursery, not the plan — but its multi-domain anti-scaling is structural,
and nursery sizing does not fix it.**

### 10.1 High-domain plan matrix — `par_binarytrees` ONCE d21, church (56-core Xeon)

Per-plan S(n) = wall(d1)/wall(dn); default heap, default workers (= nproc = 56). The
*within-plan* S(n) shape is valid; cross-plan *absolute* walls are confounded by the
56-worker default (oversubscription at low domain counts), so read the shape, not the
seconds. `d1` column is absolute seconds; the rest are S(n).

| plan | d1 (s) | S(8) | S(16) | S(28) | S(56) | shape |
|---|--:|:--:|:--:|:--:|:--:|---|
| GenImmix | 14.6 | 0.65 | 0.50 | **TIMEOUT** | **TIMEOUT** | cliff past d16 |
| StickyImmix | 16.9 | 0.77 | 0.54 | 0.41 | 0.24 | monotonic collapse |
| Immix | 26.8 | 1.08 | 1.06 | 0.95 | 0.63 | flat to ~d28 |
| ConcurrentImmix | 9.3 | 1.14 | 1.06 | 0.99 | 0.74 | flat to ~d28, **fastest absolute everywhere** |

Extends Experiment 5: the generational plans don't merely anti-scale, they **fall off a
cliff** — GenImmix cannot finish d21 within 90 s once domains ≥ 28. ConcurrentImmix is the
fastest plan at *every* domain count and holds flat to 28 domains; Immix holds flat too but
is slow single-domain here (56-worker whole-heap mark). GC time climbs with domains for the
copying plans (GenImmix 10.5→28.6 s d1→d16; StickyImmix 12.6→67.2 s d1→d56) and stays
low/flat for ConcurrentImmix (3.2→5.5 s) — the Section-2 signature, confirmed to 56 cores.

### 10.2 Nursery sizing — fixes single-domain, NOT multi-domain (Experiment 6)

The 8 MB default nursery is too small for high-allocation-rate workloads: it forces
hundreds-to-thousands of near-empty minor collections. Enlarging it (single-domain,
isolated, @512 MB heap):

| bench | GenImmix (8 MB default) | GenImmix (64 MB) | Δ |
|---|---|---|---|
| spectralnorm 3000 | 0.879 s, 723 GCs, 75 MB | **0.692 s, 89 GCs, 131 MB** | 1.27× faster, 8× fewer GCs |
| binarytrees 18 | 0.423 s, 110 GCs, 262 MB | **0.142 s, 20 GCs, 185 MB** | 3.0× faster, *lower* RSS |

But a bigger nursery does **not** rescue multi-domain scaling. `par_binarytrees` ONCE d21,
GenImmix, @1 GB:

| nursery | d1 | d2 | d4 | d8 | S(8) | GCs d1→d8 | GC ms d1→d8 |
|---|--:|--:|--:|--:|:--:|---|---|
| default 2–8 MB | 3.58 | 3.76 | 4.33 | 5.63 | 0.64 | 923→959 | 2383→5318 |
| 64 MB | 2.12 | 1.70 | 2.38 | 2.97 | **0.71** | 114→118 | 822→2667 |

The 64 MB nursery is uniformly ~1.9× faster (8× fewer collections) and **best absolute at
every domain count**, yet it still **anti-scales** (S(8) = 0.71 < 1): even with 8× fewer
collections, GC time still climbs ~3.2× with domains (822→2667 ms). This is the clean
confirmation of Attribution #1 (§4): the anti-scaling is the **per-collection STW cost
growing with domain count**, independent of collection *frequency*. Reducing frequency
lowers the whole curve; it does not change its upward slope.

### 10.3 Clean single-domain GC-heavy panel at memory parity

Isolated (no concurrent runs — the Section-6 sequential panel was contention-polluted on
its absolute walls and is superseded here for the GC-heavy cells), @512 MB heap, best-of-2,
wall s / maxRSS:

| bench | GenImmix(def) | GenImmix(64 MB) | Immix | ConcurrentImmix |
|---|---|---|---|---|
| spectralnorm | 0.879 / 75 MB | **0.692 / 131 MB** | 0.765 / 556 MB | 0.758 / 320 MB |
| binarytrees | 0.423 / 262 MB | 0.142 / 185 MB | **0.095 / 554 MB** | 0.098 / 325 MB |

At memory parity the naive read inverts: **GenImmix(64 MB) is the fastest *and* most
memory-efficient plan on spectralnorm**, and on binarytrees it closes most of the gap (1.5×
of Immix) at **3× less RSS**. Immix/ConcurrentImmix win binarytrees outright (promotion-heavy:
the trees survive the nursery, so GenImmix copies them while Immix marks in place) but pay
3–4× the RSS for it. Compute-bound benches (fasta/nbody/mandelbrot/fannkuchredux) are a
three-way tie (GC negligible: 0–12 collections).

### 10.4 Refined recommendation (supersedes §7 for the single-domain case)

The §7 ranking holds for *parallel* workloads; the nursery finding refines it for the common
single-/few-domain case:

1. **Enlarge the default nursery (8 MB → 64 MB). ✅ IMPLEMENTED** (mainline `5.5+mmtk`
   @ `7b65a2b016`, 2026-06-25: `api.rs` default `Bounded:2m,8m` → `Bounded:2m,64m`). Highest
   impact-per-effort for the common case: it makes GenImmix 1.3–3× faster on alloc-heavy
   single-domain workloads *and lowers RSS*, and at memory parity makes the generational plan
   competitive-to-best single-domain. The 8 MB default — chosen for bounded RSS (#44) — was
   simply too small once allocation rate is high. Kept `Bounded` (not `Fixed`) so it stays
   commit-on-demand and adapts down to fit small heaps (16/24/32 MB pinned all validated rc=0).
   No plan change, low risk. (It does **not** fix multi-domain anti-scaling — confirmed:
   S(8) stays 0.71 with 8× fewer collections.)
2. **For parallel/multi-domain workloads, plan choice is the lever, not the nursery.**
   GenImmix and StickyImmix structurally anti-scale and cliff past ~16–28 domains;
   ConcurrentImmix (research, #30) is the only plan both fastest single-domain *and* scaling;
   Immix is the production-ready scaler (at a RSS premium). Steer parallel-heavy workloads to
   `MMTK_PLAN=Immix`/`ConcurrentImmix` until ConcurrentImmix is production-ready.
3. **On the default-plan question itself:** with a right-sized nursery, **GenImmix remains a
   defensible default** for the common case — memory-efficient, competitive single-domain,
   stock-faithful — *provided* the multi-domain limitation is documented and steerable by
   plan selection. A blanket switch to Immix is **not** warranted: Immix's single-domain edge
   largely evaporates once GenImmix's nursery is tuned, and it costs 3–4× RSS. The genuinely
   better default is **ConcurrentImmix once #30 lands** (fastest single-domain *and* the only
   plan that scales).

> **Method note.** Several absolute walls in §6's sequential panel were inflated by running
> the panel concurrently with other jobs (benchmarks contend for cores → invalid timing); the
> §10.3 numbers are the clean isolated re-measurement and should be cited in preference. GC
> *counts* in §6 are unaffected. A clean re-run of the full §2 matrix with `MMTK_THREADS`
> pinned (to remove the worker-oversubscription confound flagged in §10.1) is the remaining
> follow-up.

## 11. Nursery probe — pole-B NO-GO; nursery-size lever + BUG B (2026-06-25)

> **⚠️ The tables in §11.1–11.3 below are from the CONTAMINATED church run** (branch `fix/bug3c-cross-stw`,
> build silently on an ~8 MiB default nursery → 913 GCs). They are kept only as a record of the artifact. **For
> the real, clean mainline numbers see UPDATE 2 at the top** (turing: GenImmix-default S(8)=1.23, StickyImmix
> 1.36, GenImmix-256 MiB 1.59). What survives clean: the nursery-size lever (256 MiB ~20% faster + S(8)
> 1.23→1.59), the residual-slope = per-collection STW cost (§11.2/Lever 2), and **BUG B** (§11.4, mainline-
> confirmed). The "BUG A degenerate default install" (§11.3) does **NOT** reproduce on clean mainline — it was
> the church build artifact (local + turing both give 114 GCs at the default = correct 64 MiB).

Church (56-core Xeon), assert-fixed mmtk-core, core-pinned, `MMTK_THREADS=domains`, fixed 4 GiB heap,
`par_binarytrees d21`, 3 reps, **min** wall-time (s), RSS via `/usr/bin/time`.

### 11.1 Plan discriminator (all at the binding DEFAULT nursery)
In-place nurseries scale; the copying nursery does not — at the *same* all-domains STW.

| plan | nursery kind | d1 | d2 | d4 | d8 | **S(8)** | RSS |
|---|---|---|---|---|---|---|---|
| GenImmix | shared **copy** | 11.62 | 9.63 | 9.29 | 10.19 | **1.13** | ~1.2–1.8 GB |
| StickyImmix | generational **in-place** | 23.86 | 13.72 | 9.36 | 8.35 | **2.86** | ~1.3 GB |
| Immix | non-gen in-place | 7.94 | 4.91 | 3.20 | 2.61 | **3.04** | 4 GB (heap-filled) |

StickyImmix runs GenImmix's exact STW + per-domain root scan yet scales 2.86× ⇒ the STW rendezvous is **not**
the bottleneck.

### 11.2 Dose-response — GenImmix, EXPLICIT raw-byte nursery cap (`Bounded:2097152,<bytes>`)
Bigger nursery → better scaling + faster single-domain; plateau ~256 MiB; commit-on-demand keeps RSS low.

| cap | d1 | d8 | **S(8)** | RSS (d1 / d8) |
|---|---|---|---|---|
| 64 MiB | 6.72 | 4.21 | **1.60** | 356 MB / 784 MB |
| **256 MiB** | **5.64** | **2.19** | **2.58** | **~360 MB** |
| 1024 MiB | 5.81 | 2.23 | 2.60 | 1.1 GB |

### 11.3 "BUG A" (degenerate default-nursery install) — RETRACTED: a church build artifact, NOT mainline
On church the *unset*-`MMTK_NURSERY` default ran **913 minor GCs / d1=11.62 s / S(8)=1.13** while an *explicit
identical 64 MiB* env ran **113 GCs / 6.75 s** — which looked like the binding's default install (`api.rs:118`)
silently running ~8 MiB. **But this does NOT reproduce on clean mainline.** A fresh rebuild on **local macOS**
and on **turing** (clean `5.5+mmtk`, even sharing church's mmtk-core `0fe660bb9c`) both give **114 GCs** at the
default — i.e. the *correct* 64 MiB (= explicit-64 MiB; = §10.2's 64 MiB). The intervening mmtk-core commits
(`0fe660bb9c`→`ec2f5079f8`) are all scheduler/FinalMark — none touch nursery code — so it is the **church
`fix/bug3c-cross-stw` branch api.rs or a stale church build**, not mmtk-core and not mainline. **Lesson:** the
church campaign's *default*-nursery numbers (incl. §11.1's GenImmix S(8)=1.13) were contaminated by this; the
clean numbers are in UPDATE 2. Worth checking before merging `fix/bug3c-cross-stw` (it would be a real
regression if it carries this).

### 11.4 BUG B — `MMTK_NURSERY` suffix syntax silently parse-fails
`MMTK_NURSERY="Bounded:2m,64m"` (the form in CLAUDE.md/README) → *"unable to set MMTK_NURSERY… Can't parse
value. Default value will be used."* → silent fallback to mmtk-core's default. Only raw bytes
(`Bounded:2097152,67108864`) parse. Fix the parser to accept `k/m/g` suffixes, or correct the docs.

### 11.5 Reconciliation with §10.2 (which concluded nursery sizing does NOT help multi-domain)
§10.2 **did** get its nursery change to take effect (923→114 GCs at 64 MiB), so it did *not* hit BUG B — the
discrepancy is the **measurement conditions**, not a no-op override. §10.2 ran under the **pre-fix confounds
UPDATE 1 flags** (`nproc` workers, unpinned; pre-assert-fix) and at a different heap/workload (d21 *ONCE* @1 GB,
S(8)=0.71 at 64 MiB), whereas §11 is **pinned, `MMTK_THREADS=domains`, assert-fixed @4 GiB** and shows nursery
sizing *does* lift net scaling (S(8) 1.60→2.58 from 64→256 MiB). The most likely cause of the flip: removing the
worker-oversubscription tax (pinning + `workers=domains`) eliminates much of the "GC time climbs with domains"
that §10.2 measured (822→2667 ms). **Caveat — not fully closed:** §11 captured wall-clock only, so it does **not**
directly refute §10.2's narrower claim that *per-collection STW cost* still grows with domains; a clean
apples-to-apples re-run of §10.2's exact config (with per-domain GC-time, pinned + assert-fixed) is owed to
settle whether any residual STW-cost-growth remains under a large nursery. §10.2's *single-domain* nursery
effect stands unchanged.

### 11.6 Pole-B verdict
RQ10 pole-B (VM-owned ParMinor + MMTk major-only): **NO-GO.** The anti-scaling is fixed in-framework — fix/raise
the nursery (§11.2–11.3) and/or use an in-place plan (§11.1) — with no VM-ParMinor rebuild. Fixes → ROADMAP #21.

**Caveats:** one alloc-heavy bench at one depth on the `fix/bug3c-cross-stw` branch; relative comparisons are
robust, absolutes are heap-/branch-specific; BUG A root-cause + a mainline + second-workload confirmation are
still owed.
