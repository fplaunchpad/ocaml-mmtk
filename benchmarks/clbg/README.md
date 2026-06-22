# CLBG benchmark suite (MMTk OCaml)

The [Computer Language Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/)
programs, ported to run on this MMTk-backed OCaml fork. Two jobs:

- **Correctness across GC plans** — every program must produce byte-identical
  output under every MMTk plan. A divergence (or crash, or hang) is a GC bug.
  This is what CI runs (`.github/workflows/clbg.yml`).
- **Performance** — wall-time and max-RSS per plan, the basis for the M8
  comparison against stock OCaml (a separate vanilla 5.5 opam switch).

## Programs

| Program | Parallelism | Notes |
|---|---|---|
| `fasta` | sequential | LCG stream is stateful; also the stdin producer for the two below |
| `nbody` | sequential | time-stepped simulation |
| `spectralnorm` | sequential | (the parallel CLBG variants split the row loops — a future option) |
| `binarytrees` | **Domain.spawn** | one domain per depth class — heavy short-lived allocation |
| `mandelbrot` | **Domain.spawn** | one domain per row band; bands concatenated in order |
| `fannkuchredux` | **Domain.spawn** | one domain per permutation-index range |
| `knucleotide` | **Domain.spawn** | one domain per output task; **per-task local hashtables** (domains share a heap, unlike fork's copy-on-write) |
| `revcomp` | **Domain.spawn** | one domain per FASTA section |

The five parallel programs are CLBG entries that originally used `Unix.fork`.
Fork is unsafe under MMTk (it orphans the GC worker threads), so they are ported
to `Domain.spawn` — which also makes them genuine multi-domain GC stress tests.
`pidigits` and `regex-redux` are omitted (they need `zarith` / `Re`, not in the
compiler tree).

## Layout

```
src/        the 8 OCaml programs
golden/     reference stdout at CI sizes (committed; regenerate with `make golden`)
run.sh      validate / matrix / golden driver
Makefile    builds src/ with the in-tree compiler
build/      compiled programs + scratch (git-ignored)
results/    perf matrix CSV (git-ignored)
```

## Building & running

Build the compiler first (from the repo root: `./configure && make`), then:

```sh
# bytecode (any plan) — what correctness uses
make bytecode

# native (Immix-family plans only) — what perf uses
make native

# correctness: every program × every plan, byte-compared to golden/
./run.sh validate                 # all plans
./run.sh validate Immix StickyImmix

# performance: native, Immix-family, perf sizes -> results/matrix.csv
./run.sh matrix                   # needs /usr/bin/time (the `time` package)
```

> **Always run under ASLR-off.** `run.sh` already wraps every run in
> `setarch "$(uname -m)" -R`. MMTk maps side-metadata at fixed addresses and an
> ASLR collision aborts startup (`failed to mmap meta memory`) — a known
> mmtk-core issue, not a benchmark bug.

### Knobs

- `MMTK_HEAP_SIZE_MB` (default 1024) — fixed heap. `NoGC` needs it ≥ the live set.
- `CLBG_TIMEOUT` (default 120 s) — per-run cap; a timeout is reported as `HANG`.
- `PERF_<bench>=N` — override a perf size, e.g. `PERF_nbody=50000000`.
- CI builds with the bytecode `ocamlc` to avoid needing the native compiler:
  `make bytecode OCAMLC="$ROOT/runtime/ocamlrun $ROOT/ocamlc"`.

## Golden outputs

Goldens are committed and are the canonical correct output. They are
plan-independent (CLBG output does not depend on the GC) and compiler-version
independent for the integer/string programs; the float programs (`nbody`,
`spectralnorm`, `mandelbrot`) are byte-stable across plans on a given build.
Regenerate after a deliberate change with `make golden` (uses Immix).

## Caveats / known interactions

- **Native runs only under `Immix` and `StickyImmix`** — the TLAB nursery-aliasing
  path needs an *Immix* Default allocator. `GenImmix` has a copying-BumpPointer
  nursery, so native `GenImmix` aborts at startup with *"no Immix Default
  allocator"* (even though the runtime's error string and the top-level README
  list it as native-capable — a doc/code inconsistency worth reconciling).
  `NoGC`/`MarkSweep`/`GenImmix` are therefore bytecode-only here.
- **The parallel programs + StickyImmix at large sizes** may surface the open
  multi-domain moving-GC bug (`ROADMAP.md` bug #3) — a deterministic crash in
  the heavy `Domain.spawn` + `Gc` churn case. At CI sizes they pass on every
  plan; if a perf-matrix StickyImmix run crashes, that is bug #3, not the
  benchmark. The suite thus doubles as a regression test for that fix.
