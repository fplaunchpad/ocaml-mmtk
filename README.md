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

12 stdlib-only CLBG/sandmark/effects programs, native, dynamic heap (memory parity with vanilla),
median of 5 reps on an Apple M4 Pro. GC workers = domains. Run: `uv run quick/quickbench.py …`.

**Sequential** — wall vs vanilla 5.5.0 (lower is better):

![sequential ratio vs vanilla](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/seq_ratio.png)

| bench | GenImmix *(default)* | Immix | ConcurrentImmix |
|---|--:|--:|--:|
| binarytrees | 1.05× | 2.11× | **0.53×** |
| nbody | 1.01× | **0.94×** | 1.01× |
| fannkuchredux | 1.00× | 1.00× | 1.01× |
| spectralnorm | **0.96×** | 1.24× | 1.23× |
| mandelbrot | 1.00× | 0.99× | 1.00× |
| matrix_multiplication | **0.86×** | 0.87× | **0.87×** |
| LU_decomposition | 1.09× | 1.32× | 1.33× |
| kb | 1.23× | 1.10× | **0.98×** |

GenImmix (the default) is parity-or-better on 6 of 8 benches — including `matrix_multiplication`
(**0.86×**); `kb` (1.23×) and `LU_decomposition` (1.09×) are the exceptions. ConcurrentImmix leads on the
allocation-heavy `binarytrees` (**0.53×**) and `kb` (0.98×); Immix trails on `binarytrees` (2.11×).

**`LXR`** (reference counting, RQ1) has **no dynamic heap**, so it is benched at a fixed heap **pinned per
bench to GenImmix's natural (dynamic) footprint** — the real memory-parity comparison — reporting **wall
(ratio vs GenImmix) and max RSS** (LXR carries a side-metadata cost, so RSS matters):

| bench | heap | GenImmix | Immix | LXR |
|---|--:|--:|--:|--:|
| binarytrees | 288 MiB | 11.4 s / 306 MB | 3.3 s / 383 MB (0.29×) | **1.3 s / 378 MB (0.12×)** |
| kb | 112 MiB | 0.52 s / 137 MB | 0.43 s / 161 MB (0.83×) | 0.70 s / 264 MB (1.35×) |
| spectralnorm | 96 MiB | 0.76 s / 114 MB | 0.78 s / 154 MB (1.03×) | 0.76 s / 218 MB (1.00×) |
| LU_decomposition | 112 MiB | 1.00 s / 134 MB | 0.96 s / 174 MB (0.96×) | 0.96 s / 258 MB (0.96×) |
| nbody · fannkuch · mandelbrot · matmul | 32–48 MiB | 1.00× | ≈1.00× | ≈1.00× (heap-insensitive) |

LXR's **in-place RC dominates on allocation-heavy `binarytrees` at GenImmix's own memory** (**0.12× — 8×
faster**: GenImmix's copying nursery thrashes there while LXR promotes in place — the RQ1 result), is at
**parity on compute-bound benches**, and is **slower on `kb`** (1.35× — cyclic garbage swept by the backup
trace). The trade-off is memory: LXR carries a fixed ~48 MiB whole-heap RC-metadata tax plus proportional
overhead, so its RSS runs ~20–90% above the tracing plans at the same heap. This is the RQ1 story — RC buys
throughput + memory-robustness on acyclic churn, at a side-metadata cost. LXR is single- and multi-domain
validated but experimental; run it via `uv run quick/quickbench.py seq --plans LXR --heap <MB>`.

**Parallel** — speedup at 8 domains (ideal = 8):

![speedup vs domains](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/speedup_domains.png)

| bench | vanilla | GenImmix | Immix | ConcurrentImmix |
|---|--:|--:|--:|--:|
| par_matmul | 5.72 | 2.89 | 0.56 | 3.03 |
| par_binarytrees | 3.76 | 0.72 | 3.15 | 0.86 |
| par_spectralnorm | 4.81 | 1.74 | 2.39 | 1.56 |

Multi-domain scaling is sublinear: the per-collection stop-the-world cost grows with domain count, so the
allocation-heavy `par_binarytrees` anti-scales under GenImmix (0.72) while Immix scales best (3.15).
`SCALABILITY.md` has the mechanism; the macro-bench campaign (`PERFORMANCE.md`) is authoritative.

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
| `MMTK_NURSERY` | `Bounded:2097152,67108864` (2–64 MiB) | Generational-plan nursery (GenImmix/GenCopy/StickyImmix), bounded/absolute and commit-on-demand (adapts down to fit small heaps). The 64 MiB max keeps GenImmix competitive single-domain on allocation-heavy workloads. **Value must be raw BYTES** — e.g. `Fixed:33554432`, `Bounded:2097152,134217728`; the `2m,128m` suffix form does **not** parse (silently falls back to the default). |
| `MMTK_THREADS` | _nproc_ | GC worker threads. **Set `MMTK_THREADS=1` for single-/few-domain runs** — the `nproc` default oversubscribes and slows high-collection workloads (a domain-aware pool is the open fix). |
| `MMTK_VERBOSE` | unset | Print MMTk init and a GC summary at exit. |

mmtk-core's own `MMTK_*` options (`MMTK_THREADS`, `MMTK_STRESS_FACTOR`, …) also work.

### GC plans

`MMTK_PLAN` selects the collector at startup (default **`GenImmix`** — generational, copying nursery over
an Immix mature, the stock-OCaml-faithful fit for OCaml's short-lived allocation). **10** of 0.32's 11
stock plans are wired in bytecode, **plus our own `LXR`** reference-counting research plan (see below);
**8** run native (those whose allocator the inlined TLAB can alias, now incl. LXR).

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
