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

## The panel (ten dependency-free sandmark/CLBG benches)

All ten are **real, stdlib-only** programs adapted verbatim (or with a minimal,
documented checksum tweak) from the sandmark suite under `benchmarks/` — no opam,
no Domainslib, no Core. The three parallel benches port Domainslib's
`Task.parallel_for` to **raw `Domain.spawn`** via a tiny in-file helper.

| Bench | Par? | GC axis it probes | Source |
|---|---|---|---|
| `binarytrees`           | seq | **mixed lifetime → generational promotion** — short-lived trees + a long-lived tree. | `benchmarksgame/binarytrees5.ml` (verbatim) |
| `nbody`                 | seq | **compute-bound control, ≈0 allocation** — catches codegen/mutator regressions. | `benchmarksgame/nbody.ml` (verbatim) |
| `fannkuchredux`         | seq | **small fixed arrays, compute-bound, ≈0 alloc** — permutation enumeration. | `benchmarksgame/fannkuchredux.ml` (one-line bounds fix — see note) |
| `spectralnorm`          | seq | **float vectors, compute-bound, light alloc**. | `benchmarksgame/spectralnorm2.ml` (verbatim) |
| `mandelbrot`            | seq | **escape-time compute, ≈0 alloc** — emits an integer checksum over the P4 byte stream instead of a binary bitmap. | `benchmarksgame/mandelbrot6.ml` (checksummed) |
| `matrix_multiplication` | seq | **boxed-int matrices → mature live set**. | `multicore-numerical/matrix_multiplication.ml` (seeded + checksummed) |
| `LU_decomposition`      | seq | **large flat float array, in-place updates**. | `multicore-numerical/LU_decomposition.ml` (seeded + bit-checksummed) |
| `par_spectralnorm`      | **par** | **parallel float compute + GC-worker scaling**. | `multicore-numerical/spectralnorm2_multicore.ml` → raw `Domain.spawn` |
| `par_matmul`            | **par** | **parallel boxed-matrix alloc + live set scaling**. | `multicore-numerical/matrix_multiplication_multicore.ml` → raw `Domain.spawn` |
| `par_binarytrees`       | **par** | **parallel alloc + live set + cross-domain STW coordination**. | `multicore-numerical/binarytrees5_multicore.ml` → raw `Domain.spawn` |

Every bench is **deterministic and self-checking**: it prints a stable result /
checksum line, compared byte-for-byte against `golden/`. Each `Random`-using bench
seeds `Random.init 42` for reproducibility. The three parallel benches are
**domain-count-independent by construction** — the same output at 1, 2, 4, 8
domains (verified at 1 vs 4), which is itself a correctness check that the
parallel split is sound. Additionally, `par_matmul`'s checksum **equals**
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

Perf sizes (the panel sizes — what `quickbench.sh` and the goldens use; each ~0.5–1.5s
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
| `par_spectralnorm` | `4000` | ~1.3s |
| `par_matmul` | `768` | ~0.7s |
| `par_binarytrees` | `20` | ~0.9s |

CI/tiny sizes (fast smoke; `--ci` / `--quick` use these):

| Bench | CI args |  | Bench | CI args |
|---|---|---|---|---|
| `binarytrees` | `10` | | `LU_decomposition` | `64` |
| `nbody` | `10000` | | `par_spectralnorm` | `200` |
| `fannkuchredux` | `8` | | `par_matmul` | `64` |
| `spectralnorm` | `200` | | `par_binarytrees` | `12` |
| `mandelbrot` | `200` | | | |
| `matrix_multiplication` | `64` | | | |

Override any size by editing `quickbench.sh`'s `perf_args` / `ci_args`; the
goldens (regenerate with `make golden`) are tied to the perf sizes.

## Building

The benches build with the in-tree fork compiler (same convention as the CLBG
suite). The Makefile's `ROOT` defaults to the in-tree layout
(`<fork>/benchmarks/clbg/quick/` → fork is three levels up). For any other
layout (e.g. a standalone `git worktree` of the `benchmarks` branch) pass `ROOT`:

```sh
# bytecode (any plan) — correctness smoke + goldens
make -C quick bytecode ROOT=/path/to/fork

# native (Immix-family plans only) — what quickbench.sh times
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

> **Always run under ASLR-off.** `quickbench.sh` wraps every run in
> `setarch <arch> -R` (and `mkgolden.sh` too). MMTk maps side metadata at fixed
> addresses; an ASLR collision aborts startup (`failed to mmap meta memory`) — a
> known mmtk-core issue, not a benchmark bug. Pass `--no-setarch` on macOS.

## The harness — `quickbench.sh`

```
quickbench.sh [seq|par|all] [options]
```

| Option | Meaning |
|---|---|
| `--plans P1,P2,...` | MMTk plans to run (default `GenImmix`) |
| `--vanilla DIR` | also run a vanilla baseline (a dir of binaries) for ratios |
| `--bin-a DIR --bin-b DIR` | A/B two prebuilt binary sets, interleaved (the **feature axis**) |
| `--label-a S --label-b S` | labels for the two sets (default `a` / `b`) |
| `--feature S` | cosmetic header label (e.g. `no_zero`) |
| `--domains 1,2,4,8` | (par) domain counts to sweep (default `1,2,4,8`) |
| `--heap MB` | `MMTK_HEAP_SIZE_MB` (default `512`) |
| `--reps N --warmup N` | reps / warmups per cell (default `3` / `1`) |
| `--quick` | reps=1, warmup=0, CI/tiny sizes — smoke only |
| `--ci` | CI/tiny sizes at the configured reps/warmup |
| `--gc` | add GC-count / STW-ms columns (`MMTK_VERBOSE`, seq) |
| `--bytecode` | use `*.byte` via `ocamlrun` (default: native `*.native`) |
| `--cores LIST` | base core list for `taskset`; par uses the first K for K domains |
| `--no-pin` / `--no-setarch` | skip `taskset` / `setarch` (e.g. macOS) |

- **Sequential** output: per (bench × variant) **median wall time + ratio vs the
  first variant**. Uses `hyperfine` if installed (warmup + reps), else a built-in
  median timer. With `--gc`, appends GC count / total STW ms per cell.
- **Parallel** output: per bench a **scalability table** — wall time per domain
  count and **speedup T(1 domain)/T(N)**.
- The **feature axis** (`--bin-a`/`--bin-b`) A/Bs two prebuilt binary sets
  interleaved under every plan — it works uniformly for no-zero ON-vs-OFF,
  plan-vs-plan, and fork-vs-vanilla. The harness can't toggle a *compile-time*
  feature itself, so you build each variant's benches into its own directory and
  point `-a`/`-b` at them.

### Examples

```sh
# default: GenImmix, native, full panel, ~5 min
./quick/quickbench.sh all

# sequential only, two plans, with GC columns
./quick/quickbench.sh seq --plans GenImmix,Immix --gc

# parallel scalability sweep on StickyImmix
./quick/quickbench.sh par --plans StickyImmix --domains 1,2,4,8

# quick smoke (tiny sizes, 1 rep) — just confirm it runs
./quick/quickbench.sh all --quick --bytecode --no-pin --no-setarch
```

## Deciding on the no-zero allocation change

The no-zero change (skip zero-fill on fresh allocation) is a **compile-time**
binding feature, so build the benches twice — once against a fork built with the
feature OFF, once ON — into two directories, then A/B them:

```sh
# 1. build OFF and ON binary sets (native, on Linux)
make -C quick native ROOT=/path/to/fork-OFF BUILD=$PWD/bins/off
make -C quick native ROOT=/path/to/fork-ON  BUILD=$PWD/bins/on

# 2. A/B them across the panel + domain sweep
./quick/quickbench.sh all \
    --bin-a $PWD/bins/off --bin-b $PWD/bins/on \
    --label-a off --label-b on --feature no_zero \
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
