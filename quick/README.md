# quick — quick-decision GC benchmark panel (MMTk OCaml)

A **small, fast, curated** panel for getting quick perf signal on a GC change to
the `ocaml-mmtk` fork (OCaml 5.5, always-on MMTk). It is the *"did this help /
stay neutral / scale?"* eyeball test you run while iterating — it **complements,
does not replace**, the heavyweight ocaml-bench / macro-bench authoritative
suite, and the CLBG cross-plan correctness suite at the branch root.

**Design budget:** a full run of the whole panel on **one variant** costs
**~5 minutes** at the default perf sizes and default reps (3 reps, 1 warmup). So
comparing e.g. vanilla + GenImmix-OFF + GenImmix-ON for a no-zero decision is
~15 min. Inputs are sized to stay inside that budget **while still actually
exercising the GC** (each bench's 1-domain wall is ~0.5–1.5s on an M4 Pro at
GenImmix / heap 512MB; the seven sequential + three parallel 1-domain runs sum to
under 10s, leaving plenty of headroom for reps + the parallel domain sweep).

## The panel (twelve dependency-free sandmark/CLBG/effects benches)

All twelve are **real, stdlib-only** programs adapted verbatim (or with a minimal,
documented checksum tweak) from the sandmark suite under `benchmarks/`, the CLBG
set, and the effects-examples repo — no opam, no Domainslib, no Core. The four
parallel benches port Domainslib's `Task.parallel_for` to **raw `Domain.spawn`**
via a tiny in-file helper.

| Bench | Par? | GC axis it probes | Source |
|---|---|---|---|
| `binarytrees`           | seq | **mixed lifetime → generational promotion** — short-lived trees + a long-lived tree. | `benchmarksgame/binarytrees5.ml` (verbatim) |
| `nbody`                 | seq | **compute-bound control, ≈0 allocation** — catches codegen/mutator regressions. | `benchmarksgame/nbody.ml` (verbatim) |
| `fannkuchredux`         | seq | **small fixed arrays, compute-bound, ≈0 alloc** — permutation enumeration. | `benchmarksgame/fannkuchredux.ml` (one-line bounds fix — see note) |
| `spectralnorm`          | seq | **float vectors, compute-bound, light alloc**. | `benchmarksgame/spectralnorm2.ml` (verbatim) |
| `mandelbrot`            | seq | **escape-time compute, ≈0 alloc** — emits an integer checksum over the P4 byte stream instead of a binary bitmap. | `benchmarksgame/mandelbrot6.ml` (checksummed) |
| `matrix_multiplication` | seq | **boxed-int matrices → mature live set**. | `multicore-numerical/matrix_multiplication.ml` (seeded + checksummed) |
| `LU_decomposition`      | seq | **large flat float array, in-place updates**. | `multicore-numerical/LU_decomposition.ml` (seeded + bit-checksummed) |
| `kb`                    | seq | **term-rewriting / symbolic (Rocq/Coq-like) — many small short-lived terms + a growing rule set**. | OCaml testsuite `tests/misc-kb/` (modules inlined; canonical-set output → checksum) |
| `par_spectralnorm`      | **par** | **parallel float compute + GC-worker scaling**. | `multicore-numerical/spectralnorm2_multicore.ml` → raw `Domain.spawn` |
| `par_matmul`            | **par** | **parallel boxed-matrix alloc + live set scaling**. | `multicore-numerical/matrix_multiplication_multicore.ml` → raw `Domain.spawn` |
| `par_binarytrees`       | **par** | **parallel alloc + live set + cross-domain STW coordination**. | `multicore-numerical/binarytrees5_multicore.ml` → raw `Domain.spawn` |
| `chameneos_redux`       | **par** | **effect-handler green threads — heavy effect/continuation (fiber) alloc + resume traffic** (the panel's effects workload). | `ocaml-multicore/effects-examples` (`mvar/chameneos.ml` + `MVar.ml` + `sched.ml`, inlined into one stdlib-only file) |

Every bench is **deterministic and self-checking**: it prints a stable result /
checksum line, compared byte-for-byte against `golden/`. Each `Random`-using bench
seeds `Random.init 42` for reproducibility. The four parallel benches are
**domain-count-independent by construction** — the same output at 1, 2, 4, 8
domains (verified at 1 vs 4), which is itself a correctness check that the
parallel split is sound. (`chameneos_redux` runs a **fixed** total of 8 games
split across the domains, so it is *strong*-scaling — total work constant in
domain count, ideal speedup = #domains — and its checksum is identical at any
domain count.) Additionally, `par_matmul`'s checksum **equals**
`matrix_multiplication`'s at the same size (same seeded operands, same fold) — a
cross-check that the parallel matmul computes the identical result.

> **`fannkuchredux` note.** The upstream sandmark `fannkuchredux.ml` crashes with
> `Invalid_argument("index out of bounds")` at *every* `n` (including its own
> default and the sandmark config size): the carry loop in `Perm.next` indexes
> `c.(!i)` / `p.(!i)` one past the length-`plen` permutation. The port adds the
> same `plen > !i` guard the adjacent `p.(!i)` write already has. The result is
> verified against the canonical CLBG values (`Pfannkuchen(11) = 51`).

> The `par_*` benches take **size from `argv[1]` and domain count from `argv[2]`
> OR the `DOMAINS` env var** (default 1) — note this is the *reverse* of the
> sandmark multicore programs, which take domains first.

## Input sizes

Perf sizes (the panel sizes — what `quickbench.py` and the goldens use; each ~0.5–1.5s
on an M4 Pro at GenImmix / heap 512MB, native):

| Bench | Perf args | ~1-dom wall |
|---|---|---|
| `binarytrees` | `20` | ~0.8s |
| `nbody` | `20000000` | ~0.6s |
| `fannkuchredux` | `11` | ~1.4s |
| `spectralnorm` | `3000` | ~0.7s |
| `mandelbrot` | `4000` | ~0.7s |
| `matrix_multiplication` | `768` | ~0.6s |
| `LU_decomposition` | `900` | ~0.9s |
| `kb` | `50` | ~0.7s |
| `par_spectralnorm` | `4000` | ~1.3s |
| `par_matmul` | `768` | ~0.7s |
| `par_binarytrees` | `20` | ~0.9s |
| `chameneos_redux` | `500000` | ~1.3s |

CI/tiny sizes (fast smoke; `--ci` / `--quick` use these):

| Bench | CI args |  | Bench | CI args |
|---|---|---|---|---|
| `binarytrees` | `10` | | `LU_decomposition` | `64` |
| `nbody` | `10000` | | `par_spectralnorm` | `200` |
| `fannkuchredux` | `8` | | `par_matmul` | `64` |
| `spectralnorm` | `200` | | `par_binarytrees` | `12` |
| `mandelbrot` | `200` | | | |
| `matrix_multiplication` | `64` | | | |

Override any size by editing `quickbench.py`'s `PERF` / `CI` dicts; the
goldens (regenerate with `make golden`) are tied to the perf sizes.

## Building

The benches build with the in-tree fork compiler (same convention as the CLBG
suite). The Makefile's `ROOT` defaults to the in-tree layout
(`<fork>/benchmarks/clbg/quick/` → fork is three levels up). For any other
layout (e.g. a standalone `git worktree` of the `benchmarks` branch) pass `ROOT`:

```sh
# bytecode (any plan) — correctness smoke + goldens
make -C quick bytecode ROOT=/path/to/fork

# native (Immix-family plans only) — what quickbench.py times
make -C quick native ROOT=/path/to/fork

# regenerate goldens (native, GenImmix, perf/panel sizes)
make -C quick golden ROOT=/path/to/fork
```

> **macOS (Apple Silicon)** builds + runs **native** fine (the MMTk Rust
> staticlib links into the native runtime locally), so the panel and its goldens
> are produced natively on macOS too. Bytecode also works for a quick smoke.

> **Native
> MMTk links only on Linux** (the Rust staticlib is linked into the native
> runtime there) — do native perf runs on a Linux box.

> **Always run under ASLR-off.** `quickbench.py` wraps every run in
> `setarch <arch> -R` (and `mkgolden.sh` too). MMTk maps side metadata at fixed
> addresses; an ASLR collision aborts startup (`failed to mmap meta memory`) — a
> known mmtk-core issue, not a benchmark bug. Pass `--no-setarch` on macOS.

## The harness — `quickbench.py`

ONE self-contained [PEP 723](https://peps.python.org/pep-0723/) script that runs
the benches, prints the table (+ optional ASCII chart), writes NDJSON results,
**and** renders the PNG graphs — all in one run. Its `matplotlib`/`numpy` deps
are declared inline, so [`uv`](https://docs.astral.sh/uv/) fetches them per-run;
no venv, no global install:

```
uv run quick/quickbench.py [seq|par|all] [options]
```

| Option | Meaning |
|---|---|
| `--plans P1,P2,...` | MMTk plans (default `GenImmix`); `"A,B"` or `"A B"` both work |
| `--vanilla DIR` | also run a vanilla baseline (a dir of native binaries) — the ratio baseline |
| `--bin-a DIR --label-a S` | the MMTk-built bench dir (× plans); label default `mmtk` |
| `--bin-b DIR --label-b S` | optional second binary set (A/B feature axis) |
| `--domains 1,2,4,8` | (par) domain counts to sweep (default `1,2,4,8`) |
| `--heap MB\|dynamic` | `MMTK_HEAP_SIZE_MB`; `dynamic` (default) = don't pin (memory parity) |
| `--threads N` | pin `MMTK_THREADS=N`. **Default: unset** — MMTk uses its own default = **nproc**; each record logs the effective count (`nproc(<cpus>)`) |
| `--reps N --warmup N` | reps / warmups per cell (default `3` / `1`); median of reps |
| `--quick` / `--ci` | tiny CI sizes (smoke); `--quick` also sets reps=1 warmup=0 |
| `--timeout SECS` | per-cell wall cap; an over-cap cell is killed (whole process group) and recorded `HANG` |
| `--gc` | add GC-count / STW-ms columns (`MMTK_VERBOSE`, seq) |
| `--bytecode` | use `*.byte` via `ocamlrun` (default: native `*.native`) |
| `--cores LIST` / `--no-pin` / `--no-setarch` | `taskset` / `setarch` controls (skip on macOS) |
| `--chart` | print a per-bench ASCII bar chart after the seq table |
| `--json FILE` | NDJSON results path (default `quick/results.ndjson`) |
| `--graphs DIR` / `--no-plot` | PNG output dir (default `quick/graphs/`) / skip plotting |

- **Sequential** output: per (bench × variant) **median wall time + ratio vs the
  first variant**. We time the whole process directly (median of reps; warmup
  excluded) — no `hyperfine`, so no seconds-vs-ms unit traps.
- **Parallel** output: per bench a **scalability table** — wall per domain count
  and **speedup T(1)/T(N)**. **GC workers default to nproc** (MMTk's own default —
  we do *not* force a value); the effective count is printed and logged so the
  curve's worker provisioning is explicit. Caveat: at `domains ≥ cores`, nproc GC
  workers + `d` mutator domains **oversubscribe** the pinned core set — but the
  multi-domain anti-scaling is STW-bound and shows at any worker count, so the
  out-of-the-box default is the honest measurement.
- A **per-cell timeout** is essential: some plans (ConcurrentImmix) **deadlock**
  on some benches (it panics — `GC request sent to WorkerMonitor while GC is still
  in progress` — then all GC workers die on the poisoned mutex). The cap kills the
  whole process group (`start_new_session` + `killpg`) so a deadlock is flagged
  `HANG` instead of wedging the run; those cells become `median_ms: null,
  status: "hang"` in the JSON.

### Examples

```sh
# fork-plans-vs-vanilla, full panel, graphs + JSON in ONE run:
uv run quick/quickbench.py all \
    --vanilla quick/build_vanilla --bin-a quick/build_mmtk --label-a mmtk \
    --plans "GenImmix Immix ConcurrentImmix" \
    --heap dynamic --timeout 30 --chart \
    --json quick/results.ndjson --graphs quick/graphs --no-pin --no-setarch

# quick smoke (tiny sizes, 1 rep), table only:
uv run quick/quickbench.py all --quick --no-pin --no-setarch --no-plot
```

## Results JSON + graphs

The same run writes machine-readable results as **NDJSON** (`--json`, default
`quick/results.ndjson`) — one record per measured cell: `mode, bench, variant,
plan, domains, threads, median_ms` (or `null`), `status` (`ok`/`hang`). The
`threads` field logs the effective MMTk GC-worker count (`nproc(<cpus>)` when
unset) so the panel self-documents its worker provisioning.

And the same run renders two PNGs into `quick/graphs/` (unless `--no-plot`):
`seq_ratio.png` (per-bench ratio-vs-vanilla bars, GenImmix/Immix) and
`speedup_domains.png` (speedup T(1)/T(N) vs domains per plan, with the
ideal-linear reference; red × marks a hung/crashed cell). Plotting is built into
`quickbench.py` — no separate step.

## Deciding on the no-zero allocation change

The no-zero change (skip zero-fill on fresh allocation) is a **compile-time**
binding feature, so build the benches twice — once against a fork built with the
feature OFF, once ON — into two directories, then A/B them:

```sh
# 1. build OFF and ON binary sets (native, on Linux)
make -C quick native ROOT=/path/to/fork-OFF BUILD=$PWD/bins/off
make -C quick native ROOT=/path/to/fork-ON  BUILD=$PWD/bins/on

# 2. A/B them across the panel + domain sweep
uv run quick/quickbench.py all \
    --bin-a $PWD/bins/off --bin-b $PWD/bins/on \
    --label-a off --label-b on \
    --plans GenImmix --domains 1,2,4,8 --gc
```

Read it as: **`binarytrees`/`matrix_multiplication` should improve** (ratio <
1.00x — the no-zero win shows up most strongly in the alloc-heavy, promotion-heavy
benches); **`nbody`/`fannkuchredux`/`mandelbrot` should stay neutral** (≈1.00x —
they barely allocate, so a regression there means the change leaked into
codegen/the mutator); **`spectralnorm`/`LU_decomposition`** sit in between; and
the **`par_*` speedup columns** confirm the change still scales (or reveal a
parallel-nursery regression). For an authoritative verdict, follow a positive
quick signal with the heavyweight suite.
