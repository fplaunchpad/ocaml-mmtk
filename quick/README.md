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
exercising the GC** (several collections / real allocation volume / real
promotion / real barrier traffic per bench — not toy sizes).

## The panel (six benches, one per GC axis)

| Bench | Par? | GC axis it probes | Source |
|---|---|---|---|
| `alloc`           | seq | **nursery / minor-alloc throughput** — tight loop allocating short-lived 3-word cons cells, ~all dead-on-arrival. The **no-zero / nursery probe**. | new (`src/alloc.ml`) |
| `binarytrees`     | seq | **mixed lifetime → generational promotion** — short-lived trees + a long-lived tree. | CLBG (`src/binarytrees.ml`, reused) |
| `mutate`          | seq | **write barrier / remembered-set** — large long-lived array of boxed refs, then many `a.(i) <- fresh_alloc` old→young overwrites, firing the generational barrier on every store. CLBG has no barrier bench; this fills that gap. | new (`src/mutate.ml`) |
| `nbody`           | seq | **compute-bound control, ≈0 allocation** — catches codegen/mutator regressions and confirms a GC change is free where it should be. | CLBG (`src/nbody.ml`, reused) |
| `par_alloc`       | **par** | **parallel nursery + GC-worker scaling** — N domains each churning the `alloc` loop; fixed total work split across domains. | new (`src/par_alloc.ml`) |
| `par_binarytrees` | **par** | **parallel alloc + live set + cross-domain STW coordination** — N domains each building/checking trees; fixed task set split across domains. | new (`src/par_binarytrees.ml`) |

Every bench is **deterministic and self-checking**: it prints a checksum line,
compared byte-for-byte against `golden/`. The two parallel benches are
**domain-count-independent by construction** — the same answer at 1, 2, 4, 8
domains (only the `domains=` field changes), which is itself a correctness check
that the parallel split is sound.

## Input sizes

CI/tiny sizes (define the goldens; ~tens of ms each in bytecode):

| Bench | CI args |
|---|---|
| `alloc` | `50000` |
| `binarytrees` | `8` |
| `mutate` | `2000 50000` |
| `nbody` | `1000` |
| `par_alloc` | `200000` |
| `par_binarytrees` | `10` |

Perf sizes (native target: each run a few seconds and triggers real GC activity —
counts below are indicative, measure with `--gc` / `MMTK_VERBOSE=1`):

| Bench | Perf args | Roughly exercises |
|---|---|---|
| `alloc` | `40000000` | ~40M cons cells, ~all dead-on-arrival → many minor GCs |
| `binarytrees` | `18` | CLBG depth 18 — sustained alloc + promotion of the long-lived tree |
| `mutate` | `500000 20000000` | 500k-box old array (force-promoted), 20M old→young barrier stores |
| `nbody` | `20000000` | 20M steps, ≈0 allocation (compute/codegen control) |
| `par_alloc` | `64000000` | 64M cells total, split across the domain sweep |
| `par_binarytrees` | `18` | depth-18 task set, split across the domain sweep |

Override any perf size by editing `quickbench.sh`'s `perf_args`; tune for your
heap and core count if needed.

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

# regenerate goldens (bytecode, GenImmix, CI sizes)
make -C quick golden ROOT=/path/to/fork
```

> **macOS** builds + runs **bytecode** fine (the correctness smoke). **Native
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

Read it as: **`alloc` should improve** (ratio < 1.00x — the no-zero win shows up
most strongly where allocation is high and dead-on-arrival); **`nbody` should
stay neutral** (≈1.00x — it barely allocates, so a regression there means the
change leaked into codegen/the mutator); **`binarytrees`/`mutate`** show the
effect on promotion- and barrier-heavy mixes; and the **`par_*` speedup columns**
confirm the change still scales (or reveal a parallel-nursery regression). For an
authoritative verdict, follow a positive quick signal with the heavyweight suite.
