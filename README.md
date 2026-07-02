# OCaml + MMTk (`ocaml-mmtk`)

A fork of [OCaml](https://github.com/ocaml/ocaml) 5.5 whose garbage collector is
[MMTk](https://www.mmtk.io), the Memory Management Toolkit. MMTk is the **only**
collector and is **always on** — there is no opt-out and no stock OCaml GC left in
the tree. A normal `./configure && make` builds the full compiler, both **bytecode**
and **native**, on an MMTk-managed heap; it self-hosts (the compiler bootstraps and
its documentation builds).

This is also a **GC-research platform**: one functional, immutable-by-default,
multicore, effect-handler language on which MMTk's many collectors can be compared
on a common substrate. The research agenda lives in
[`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md).

- **Base:** OCaml `5.5.0` (final). **Collector:** MMTk, always on, default plan **GenImmix** (copying nursery + Immix mature — the generational, stock-OCaml-faithful plan).
- **Binding:** in-tree at [`gc/mmtk/`](gc/mmtk), built against
  [`mmtk-core`](https://github.com/mmtk/mmtk-core) `0.32` — a small in-tree fork
  (`gc/mmtk-core`) carrying OCaml-specific deltas (e.g. it skips redundant
  allocation-time zeroing).
- Collection is multi-domain, parallel, and stop-the-world; moving plans relocate
  objects and generational plans use a write barrier. Native code allocates from a
  TLAB aliased to an MMTk Immix block, so it needs no special code generation. Both
  single- and multi-domain (`Domain.spawn`) programs run.

> Runs on **x86-64 Linux** and **macOS (arm64)** — bytecode + native, both validated. Heap grows on
> demand (memory tracks the live set). Performance: see the quick panel below; tuning is the open
> milestone (M8, [`PERFORMANCE.md`](PERFORMANCE.md)), measured against a vanilla 5.5.0 opam switch.

## Status at a glance

- **Done:** the full bring-up (build, NoGC → MarkSweep → Immix, generational plans,
  multi-domain, native code, weak/ephemeron/finaliser support, the testsuite); **excising
  the stock GC** (no stock minor/major collector, shared heap, or minor-heap arena remains);
  advancing the base to **OCaml 5.5.0 final**; the MMTk-native multi-domain stop-the-world
  handshake; **retiring OCaml's own all-domains STW** so MMTk's `stop_all_mutators` is the
  *sole* all-domains rendezvous ("one STW" — the `caml_try_run_on_all_domains` family deleted,
  −478 lines; GH#15 fixed); and **`ConcurrentImmix`** (bytecode + native) — a concurrent marker
  with an SATB write barrier, proven clean on `lazy` values and on effect-handler continuations.
- **In progress:** the macro-benchmark performance campaign + analysis (M8); and a few
  rare-crash investigations tracked as GitHub issues (notably the pre-existing #31 `Domain.join`
  result use-after-free under heavy multi-domain join, which the STW excision unmasked — a
  global-rooted-promote fix has since roughly halved its rate, and instrumentation reframed the
  residual as a *separate* post-publish `term_sync` corruption, so GH#3 stays open).
- **Known tails:** a flagged memprof colour read and `runtime_events` emission under MMTk
  (broken — see ROADMAP / FAQ). (Weak-clear timing under the generational plans — GH#5 — is
  fixed: full GC under mature pressure + `Gc.major_collections` counts full GCs only.)
- **Testsuite triage** is complete (item #19): every failure is triaged with evidence — **0 non-flaky
  failures** under GenImmix/Immix, with 58 known-unsupported/timing tests disabled via a single greppable
  first-line marker, `(* MMTk DISABLED: <reason> [category] *)`, replacing the `(* TEST *)` block
  (`grep -rn 'MMTk DISABLED' testsuite/tests` enumerates them; per-test evidence in
  [`gc/mmtk/TESTSUITE_TRIAGE.md`](gc/mmtk/TESTSUITE_TRIAGE.md)). The only genuine MMTk semantic gaps are
  two deterministic signal-delivery poll-point diffs (a signal lands at a later safepoint, not lost).

The milestone-by-milestone plan and current status are in
[`ROADMAP.md`](ROADMAP.md).

### Performance (quick panel)

8 stdlib-only sequential CLBG/sandmark programs, native, median of 5 reps on an Apple M4 Pro. Compared
**at memory parity, reporting both wall time AND max RSS** — a plan that "wins" on wall by using more
memory is not a win. The tracing plans (vanilla 5.5.0, GenImmix, Immix, ConcurrentImmix, Bactrian) run at
their natural dynamic heap; `LXR` — which has no dynamic-heap trigger — is pinned per bench at that same
footprint (`--heap parity`). Run: `uv run quick/quickbench.py --vanilla <dir> --plans "GenImmix Immix
ConcurrentImmix Bactrian" --heap dynamic …` then a second pass `--plans LXR --heap parity`.

**Wall** — × vs vanilla 5.5.0 (lower is better):

![sequential wall ratio vs vanilla](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/seq_ratio.png)

**Max RSS** — MiB, the memory each plan actually uses (lower is better):

![sequential max RSS](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/seq_rss.png)

<details>
<summary><b>Sequential table</b> — wall (× vs vanilla) / max RSS (MiB)</summary>

| bench | vanilla | GenImmix *(default)* | Immix | ConcurrentImmix | Bactrian | LXR |
|---|--:|--:|--:|--:|--:|--:|
| binarytrees | 1.00× / 92 | 1.37× / 213 | 2.26× / 190 | 0.88× / 289 | 1.08× / 258 | 0.72× / 309 |
| nbody | 1.00× / 2 | 1.00× / 26 | 1.00× / 42 | 1.00× / 46 | 1.00× / 26 | 1.02× / 73 |
| fannkuchredux | 1.00× / 2 | 1.00× / 26 | 1.00× / 42 | 1.00× / 46 | 1.00× / 26 | 1.02× / 74 |
| spectralnorm | 1.00× / 5 | 1.01× / 82 | 1.14× / 94 | 1.16× / 90 | 1.01× / 82 | 1.11× / 218 |
| mandelbrot | 1.00× / 2 | 1.01× / 26 | 1.00× / 42 | 1.01× / 46 | 1.01× / 26 | 1.10× / 73 |
| matrix_multiplication | 1.00× / 19 | 0.86× / 38 | 0.87× / 70 | 0.88× / 78 | 0.86× / 38 | 0.88× / 122 |
| LU_decomposition | 1.00× / 17 | 1.04× / 99 | 1.26× / 99 | 1.31× / 107 | 1.04× / 99 | 1.16× / 259 |
| kb | 1.00× / 8 | 1.20× / 95 | 1.08× / 103 | 0.94× / 143 | 1.21× / 95 | 1.60× / 199 |

</details>

**Wall.** **`Bactrian` (RQ7) — the stock-*architecture* plan (copying nursery + concurrently-marked,
(near-)non-moving Immix mature + SATB deletion barrier — vanilla's collector *architecture*, though not
its implementation: vanilla marks AND sweeps in mutator slices with only tiny colour-flip STW sections,
while Bactrian marks on GC workers and still sweeps STW at FinalMark) —
tracks vanilla within ~8% on 7 of 8 benches** (`binarytrees` 1.08× where GenImmix is 1.37×; `LU` 1.04×;
`spectralnorm` 1.01×; `matmul` 0.86×), with `kb` the lone loss (1.21×, = GenImmix — the per-minor-GC
framework floor, see NOTES 2026-06-24/07-02). That is the RQ7 apples-to-apples readout: **most of the
MMTk-vs-stock gap measured on the other plans is collector-design difference, not MMTk framework
overhead.** `LXR` (reference counting) is **fastest on allocation-heavy `binarytrees`** (0.72× — in-place
RC avoids the copying-nursery and re-marking costs) but **slowest on `kb`** (1.60× — the cyclic garbage
its backup trace must sweep). On `kb`, ConcurrentImmix is now at 0.94× (marking off the critical path);
the generational plans pay the minor-GC pause floor.

**Memory.** `LXR` carries the **highest RSS across the board** — a fixed ~48 MiB whole-heap RC-metadata tax
(`RC_TABLE`) plus proportional overhead: ~73 MiB on the tiny-live compute benches (vs GenImmix's 26,
vanilla's 2) and ~1.5× the tracing plans on the alloc-heavy ones. This is the RQ1 trade-off — RC buys
throughput on acyclic churn at a real memory cost. `Bactrian`'s footprint matches GenImmix's (same nursery
+ mature spaces; its concurrent cycles add no measurable RSS on the sequential panel). Every MMTk plan
still carries a multiple of vanilla's RSS: the compute-bench numbers are exactly each plan's
**program-independent startup floor** (an empty program measures 26 MiB under GenImmix/Bactrian,
42 under Immix, 46 under ConcurrentImmix vs vanilla's ~2 — side-metadata tables mapped at init plus
initial chunk commits; the plan deltas are their extra metadata, e.g. LXR's ~48 MiB RC_TABLE), and the
alloc-heavy benches add the live×2.2 dynamic-heap headroom + Immix block slack on top — the open
memory-premium tail (M8).

**Parallel** — strong scaling (a fixed total work split across domains; ideal speedup = #domains) on 3
stdlib-only `Domain.spawn` benches, domains 1→8 on the M4 Pro (8 performance cores). Tracing plans run
their dynamic heap; `LXR` is pinned per bench at an adequate (non-thrashing) heap, with its peak RSS
reported alongside.

![speedup vs domains](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/speedup_domains.png)

**Why the parallel gap exists — measured, then fixed, then re-measured** (turing, 28 cores;
`SCALABILITY.md` UPDATEs 4–5): vanilla completes **zero major cycles** on every one of these runs, while
MMTk's old pacing *manufactured* domain-scaled major-GC work (one exhaustive full GC per `Domain`
termination, plus a fixed-cadence trigger) and paid for it stop-the-world. The **allocation-paced
trigger** (2026-07-02, with the lost-remembered-set fix it exposed, GH issue 3) de-manufactures it:

![GC work manufactured vs domains — before/after](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/gcwork_domains.png)

<details>
<summary><b>Parallel table</b> — speedup T(1)/T(8) / peak RSS at 8 domains (MiB)</summary>

| bench | vanilla | GenImmix *(default)* | Immix | ConcurrentImmix | Bactrian | LXR |
|---|--:|--:|--:|--:|--:|--:|
| par_matmul | 6.74 / 20 | 3.58 / 105 | 3.20 / 77 | 2.66 / 80 | 3.63 / 105 | 3.66 / 123 |
| par_spectralnorm | 5.13 / 21 | 2.42 / 87 | 3.08 / 98 | 2.10 / 97 | 2.40 / 87 | 2.88 / 225 |
| par_binarytrees | 3.91 / 521 | 1.02 / 554 | **3.20 / 359** | 0.29 / 533 | 0.67 / 504 | 0.29 / 573 |

</details>

**Stock OCaml still scales best.** Vanilla 5.5.0's purpose-built multicore GC (stop-the-world minor +
concurrent major) reaches 3.9–6.7× at 8 domains; **every MMTk plan scales worse**, and the gap widens with
allocation intensity. On the compute-bound benches the MMTk plans sit in the 2.1–3.7× band — on zero-GC
`par_matmul` the pacing fix removed the per-domain termination GCs entirely (fulls: one per spawned domain
→ **0**; turing d=24 wall 0.66→0.32 s, now within 1.6× of vanilla). On allocation-heavy `par_binarytrees`
the fix eliminated GenImmix's anti-scaling (0.89× → 1.02×; turing d=8/d=24 wall −22/−25% with fulls
83→20 and 160→22) — but flat is not scaling: plain `Immix` (non-generational, no per-minor STW cadence)
remains the only MMTk plan that genuinely scales there (3.20×), so the attribution stands — it is the
*STW-pause frequency*, not tracing itself, that blocks scaling. With the manufactured majors gone, the
measured residual is (a) legitimate domain-scaled trace work on the alloc-heavy benches and (b) the
**minor-pause rendezvous floor**: at 24 domains `par_spectralnorm` still spends ~1.3 s in ~1300 STW
minor pauses (~1 ms each, mostly 24-way synchronisation) — the next structural target.

**RQ1/RQ7 (parallel).** Reference counting does *not* rescue the multi-domain scaling: `LXR` wins
`binarytrees` sequentially (0.72×) but anti-scales in parallel (0.29× at 8 domains) — mature reclamation
is stop-the-world in this fork regardless of collector algorithm. Nor does matching stock's architecture:
`Bactrian` — whose *sequential* profile is at vanilla parity — still anti-scales there (0.67×; its
promotion pressure now trips the allocation-paced full-GC trigger, which caps its mid-cycle floating
garbage — peak RSS at 8 domains fell 1689 → 504 MiB — at the cost of paying those pauses on the wall).
The multi-domain STW coordination cost is now the clearest quantified framework gap (SCALABILITY.md has
the mechanism and the before/after; the macro-bench campaign in `PERFORMANCE.md` is authoritative). `chameneos_redux` (effect-handler/fiber
alloc): its single-domain LXR SIGSEGV is fixed (fiber-stack slot-unlog guard, `gc/mmtk/NOTES.md`), but a
racy *multidomain* LXR crash (continuation RC race) remains, so it is excluded from the LXR parallel
panel here.

## Building

Exactly like upstream OCaml — the MMTk static library is compiled with `cargo` and
linked in automatically:

```sh
./configure
make            # builds the world; gc/mmtk is built and linked for you
make world.opt  # also build the native compiler
```

Beyond the usual OCaml build prerequisites (see [`INSTALL.adoc`](INSTALL.adoc)) you
need **Rust + Cargo** (stable) — the build runs `cargo` to produce the binding.

## Running

MMTk is on by default — build and run as usual:

```sh
OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun ./ocamlc myprog.ml -o myprog.byte
OCAMLLIB=$PWD/stdlib ./runtime/ocamlrun myprog.byte
```

> **Linux:** run under `setarch "$(uname -m)" -R` (disables ASLR) to avoid an
> occasional start-up abort (`failed to mmap meta memory`).

### Configuration

| Variable | Default | Meaning |
|----------|---------|---------|
| `MMTK_PLAN` | `GenImmix` | GC plan — see **GC plans** below. |
| `MMTK_HEAP_SIZE_MB` | _dynamic_ | Pin a fixed heap (MiB). Unset: the heap grows on demand (`heap = live × 2.2`, clamped 32 MiB..RAM), like stock OCaml. |
| `MMTK_MIN_HEAP_MB` | `32` | Dynamic-heap floor (MiB). The smallest the `live × 2.2` target may shrink to; a too-low floor lets a nursery GC fire mid-build for a low-live/high-alloc program, promoting the half-built object → the generational write barrier then dominates (GH#6; matmul-768 was 19.6s at 16 MiB, 3.2s at 32 MiB). Only applies to the default dynamic heap. |
| `MMTK_NURSERY` | `Bounded:2097152,67108864` (2–64 MiB) **× live domain count** | Generational-plan nursery (GenImmix/GenCopy/StickyImmix/Bactrian), bounded/absolute and commit-on-demand (adapts down to fit small heaps). The default budget is scaled by the live domain count (N×2–N×64 MiB — stock parity: stock's minor arenas are 2 MiB *per domain*), latched at domain spawn/termination and applied lazily at the next trigger check; the dynamic heap gets matching headroom (`SCALABILITY.md` UPDATE 6: par_binarytrees d=8 5.7× faster). An explicit pin is never scaled; `MMTK_NURSERY_PER_DOMAIN=0` disables scaling the default. **Value must be raw BYTES** — e.g. `Fixed:33554432`, `Bounded:2097152,134217728`; the `2m,128m` suffix form does **not** parse (silently falls back to the default). |
| `MMTK_THREADS` | _nproc_ | GC worker threads. **Set `MMTK_THREADS=1` for single-/few-domain runs** — the `nproc` default oversubscribes and slows high-collection workloads (a domain-aware pool is the open fix). |
| `MMTK_VERBOSE` | unset | Print MMTk init and a GC summary at exit. |

mmtk-core's own `MMTK_*` options (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, …) also work.

### GC plans

`MMTK_PLAN` selects the collector at startup (default **`GenImmix`** — generational, copying nursery over
an Immix mature, the stock-OCaml-faithful fit for OCaml's short-lived allocation). **12** plans are wired
in bytecode — 10 of 0.32's 11 stock plans **plus our own `LXR`** (reference counting, see below) **and
`Bactrian`** (RQ7, the stock-OCaml-faithful generational+concurrent plan); **9** run native (those whose
allocator the inlined TLAB can alias, incl. LXR and Bactrian).

| Plan | Description | Runtimes |
|------|-------------|----------|
| `Immix` | mark-region, moving (defragments) | bytecode + native |
| `StickyImmix` | generational, in-place nursery | bytecode + native |
| `GenImmix` *(default)* | generational, copying nursery + Immix mature | bytecode + native |
| `GenCopy` | generational, copying nursery + SemiSpace mature | bytecode + native |
| `SemiSpace` | classic two-space copying | bytecode + native |
| `NoGC` | bump-only; never reclaims (short programs only) | bytecode + native |
| `MarkSweep` | non-moving free-list | bytecode (native infeasible — free-list) |
| `MarkCompact` | sliding compaction (Lisp-2) | bytecode (native infeasible — VO bit + header word) |
| `PageProtect` | one page per object (debugging) | bytecode |
| `ConcurrentImmix` | concurrent marking, SATB barrier | bytecode + native (low-latency research plan) |
| `Bactrian` | generational + concurrent: copying nursery, SATB-marked Immix mature | bytecode + native (RQ7 stock-*architecture* research plan; within ~6% of vanilla on 7/8 seq benches — residual deltas vs vanilla: STW sweep at FinalMark, GC-worker (not mutator-slice) marking; [`gc/mmtk/BACTRIAN.md`](gc/mmtk/BACTRIAN.md)) |
| `LXR` | reference counting (in-place, on Immix) + concurrent backup trace for cycles | bytecode + native — **experimental (research plan): single- and multi-domain validated (par_binarytrees D=1..32). Requires a pinned `MMTK_HEAP_SIZE_MB`.** |

`ConcurrentImmix` is the low-latency **research** plan: concurrent marking + SATB write barrier
(bytecode + native), clean on `lazy` and effect-handler continuations (subtle cases in
[`FAQ.md`](gc/mmtk/FAQ.md)); remaining work is performance-only. Its *allocate-black* marker (never scans
fresh objects) is also what lets the runtime skip zeroing new memory.

`LXR` is our **reference-counting** research plan (Zhao/Blackburn/McKinley, PLDI'22): a coalescing
field-logging write barrier feeds in-place reference counting on an Immix heap, with a periodic
stop-the-world backup mark/sweep to reclaim cycles (triggered only when RC under-reclaims, so it is
nearly free on acyclic code). It is **experimental** (a research plan) but now validated **both single-
and multi-domain**: correct + sanity-clean + at memory parity with Immix; the field barrier is essentially
free on OCaml's init-write-dominated code; and `par_binarytrees` runs correctly at D=1..32 domains (the
Domain.join result is kept alive across teardown by a synchronous recursive RC-pin). It **requires a pinned
`MMTK_HEAP_SIZE_MB`**. Extra knobs: `MMTK_RC_DEBUG`
(per-pause RC stats), `MMTK_RC_NO_CM` (disable the cycle-collecting backup trace), `MMTK_BARRIER_COUNT`
(count write-barrier fires). LXR is a **runnable sequential quick-panel plan** — `uv run
quick/quickbench.py seq --plans LXR --heap 512` (it needs a pinned heap, and is SEQ-only until
multidomain lands). On the sequential panel at memory parity (fixed 512 MiB heap) it is competitive
with the tracing plans — at parity with GenImmix on the compute benches, faster on binarytrees
(0.88×), and ahead of Immix on binarytrees/spectralnorm/LU. It is **not** in the CI cross-plan gate
yet (it is experimental — a different, non-byte-identical collector).

The one **unwired** stock plan is **`Compressor`** (needs a unified object-reference model incompatible
with OCaml's layout). `MarkSweep`/`MarkCompact`/`PageProtect` are bytecode-only (their allocators can't
back the inlined native TLAB).

## Repository layout

```
gc/mmtk/            in-tree MMTk binding (a self-contained Cargo workspace)
├── common/         OCaml value layout: header, slot, scanning, object model
├── binding/        VMBinding impl + C ABI  ->  libmmtk_ocaml.a
└── include/        mmtk_ocaml.h, the C ABI consumed by the runtime
runtime/mmtk.c      C glue between the runtime and the binding
Makefile.mmtk       build glue (cargo invocation + link flags)
```

In `runtime/`, allocation, the write barrier, root scanning, and domain
initialization all go through MMTk; the C glue lives in `runtime/mmtk.c`.

## Learn more

- [`ROADMAP.md`](ROADMAP.md) — the live plan and milestone status.
- [`gc/mmtk/NOTES.md`](gc/mmtk/NOTES.md) — dated design notes and investigations.
- [`gc/mmtk/FAQ.md`](gc/mmtk/FAQ.md) — correctness & concurrency hazards (mechanism-level Q&A).
- [`RESEARCH_QUESTIONS.md`](RESEARCH_QUESTIONS.md) — the GC-research agenda.
- [`PERFORMANCE.md`](PERFORMANCE.md) — the GC-performance measurement method of record (M8).
- [`fork-handoff.md`](fork-handoff.md) — original cold-start brief.
- [`README.upstream.adoc`](README.upstream.adoc) — the upstream OCaml README.

## License

Same as OCaml — see [`LICENSE`](LICENSE). MMTk is licensed under its own terms.
