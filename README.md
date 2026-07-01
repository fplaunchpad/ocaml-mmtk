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
memory is not a win. The tracing plans (vanilla 5.5.0, GenImmix, Immix, ConcurrentImmix) run at their
natural dynamic heap; `LXR` — which has no dynamic-heap trigger — is pinned per bench at that same
footprint (`--heap parity`). Run: `uv run quick/quickbench.py seq --plans "GenImmix Immix LXR" …`.

**Wall** — × vs vanilla 5.5.0 (lower is better):

![sequential wall ratio vs vanilla](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/seq_ratio.png)

**Max RSS** — MiB, the memory each plan actually uses (lower is better):

![sequential max RSS](https://raw.githubusercontent.com/fplaunchpad/ocaml-mmtk/benchmarks/quick/graphs/seq_rss.png)

Wall (× vs vanilla) / max RSS (MiB):

| bench | vanilla | GenImmix *(default)* | Immix | ConcurrentImmix | LXR |
|---|--:|--:|--:|--:|--:|
| binarytrees | 1.00× / 91 | 1.31× / 209 | 2.30× / 190 | 0.89× / 332 | **0.70× / 310** |
| nbody | 1.00× / 2 | 1.00× / 26 | 1.00× / 42 | 1.01× / 46 | 1.00× / 74 |
| fannkuchredux | 1.00× / 2 | 1.00× / 26 | 1.01× / 42 | 1.01× / 46 | 0.99× / 73 |
| spectralnorm | 1.00× / 5 | 1.00× / 82 | 1.17× / 94 | 1.18× / 90 | 1.11× / 218 |
| mandelbrot | 1.00× / 2 | 1.01× / 26 | 1.01× / 42 | 1.01× / 46 | 0.99× / 73 |
| matrix_multiplication | 1.00× / 19 | 0.87× / 38 | 0.87× / 70 | 0.88× / 78 | 0.89× / 122 |
| LU_decomposition | 1.00× / 17 | 1.06× / 99 | 1.30× / 98 | 1.33× / 106 | 1.16× / 258 |
| kb † | — | 1.00× / 95 | 0.86× / 103 | 0.76× / 143 | 1.31× / 199 |

† `kb` has no vanilla baseline; its ratios are vs GenImmix.

**Wall.** `LXR` (reference counting) is **fastest on allocation-heavy `binarytrees`** (0.70× — its in-place
RC avoids GenImmix's 1.31× copying-nursery cost and Immix's 2.30× re-marking), at **parity on the
compute-bound benches**, competitive on `spectralnorm`/`LU`/`matmul`, and **slower on `kb`** (1.31× — the
cyclic garbage its backup trace must sweep). GenImmix (the default) is parity-or-better on the compute
benches (`matrix_multiplication` 0.87×); ConcurrentImmix leads `kb` (0.76×).

**Memory.** `LXR` carries the **highest RSS across the board** — a fixed ~48 MiB whole-heap RC-metadata tax
(`RC_TABLE`) plus proportional overhead: ~73 MiB on the tiny-live compute benches (vs GenImmix's 26, vanilla's
2) and ~1.5× the tracing plans on the alloc-heavy ones. This is the RQ1 trade-off — RC buys throughput on
acyclic churn at a real memory cost. LXR is single- **and** multi-domain validated but experimental; run it
with `uv run quick/quickbench.py seq --plans LXR --heap parity`.

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
