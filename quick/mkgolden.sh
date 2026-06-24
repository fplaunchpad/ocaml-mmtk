#!/usr/bin/env bash
# mkgolden.sh — (re)generate golden/ for the quick panel at CI/tiny sizes.
#
# Goldens are the canonical correct output (a checksum line per bench). They are
# plan-independent: the GC must not change a program's result. We generate under
# GenImmix (the default plan) in bytecode. Keep these sizes in sync with
# quickbench.sh CI_ARGS.
#
# The parallel benches' checksums are domain-COUNT-independent by construction
# (verified at 1 vs 4 domains), so the golden is generated at DOMAINS=1 and holds
# for any domain count — quickbench.sh re-checks this when run with --ci.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
FORKROOT="${FORKROOT:-$(cd "$HERE/../../.." && pwd)}"
STDLIB="${STDLIB:-$FORKROOT/stdlib}"
OCAMLRUN="${OCAMLRUN:-$FORKROOT/runtime/ocamlrun}"
HEAP="${MMTK_HEAP_SIZE_MB:-256}"
PLAN="${PLAN:-GenImmix}"

mkdir -p "$HERE/golden"
sr(){ if command -v setarch >/dev/null 2>&1; then setarch "$(uname -m)" -R "$@"; else "$@"; fi; }

# g <bench> <argv...> : run bench at CI size under PLAN, write golden/<bench>.out
g(){ local b=$1; shift
     sr env MMTK_PLAN="$PLAN" MMTK_HEAP_SIZE_MB="$HEAP" OCAMLLIB="$STDLIB" \
       "$OCAMLRUN" "$HERE/build/$b.byte" "$@" > "$HERE/golden/$b.out"
     echo "golden/$b.out"; }

g alloc           50000
g binarytrees     8
g mutate          2000 50000
g nbody           1000
g par_alloc       200000 1
g par_binarytrees 10 1

echo "regenerated quick/golden from $PLAN (CI sizes)"
