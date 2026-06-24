#!/usr/bin/env bash
# mkgolden.sh — (re)generate golden/ for the quick panel at the PANEL (perf) sizes.
#
# Goldens are the canonical correct output of each bench (a checksum / result
# line) at the exact perf sizes quickbench.sh times. They are plan-independent
# (the GC must not change a program's result) and, for the three par_* benches,
# domain-count-independent (verified at 1 vs 4 domains — see verify below). We
# generate under GenImmix (the default plan) at heap 512MB, using the native
# binaries by default (set LINK=bytecode to use *.byte via $OCAMLRUN instead).
#
# Keep the sizes here in sync with quickbench.sh perf_args.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
FORKROOT="${FORKROOT:-$(cd "$HERE/../../.." && pwd)}"
STDLIB="${STDLIB:-$FORKROOT/stdlib}"
OCAMLRUN="${OCAMLRUN:-$FORKROOT/runtime/ocamlrun}"
HEAP="${MMTK_HEAP_SIZE_MB:-512}"
PLAN="${PLAN:-GenImmix}"
LINK="${LINK:-native}"          # native | bytecode

mkdir -p "$HERE/golden"
sr(){ if command -v setarch >/dev/null 2>&1; then setarch "$(uname -m)" -R "$@"; else "$@"; fi; }

# g <bench> <argv...> : run bench at panel size under PLAN, write golden/<bench>.out
g(){ local b=$1; shift
     local exe launcher=""
     if [ "$LINK" = bytecode ]; then exe="$HERE/build/$b.byte"; launcher="$OCAMLRUN";
     else exe="$HERE/build/$b.native"; fi
     sr env MMTK_PLAN="$PLAN" MMTK_HEAP_SIZE_MB="$HEAP" OCAMLLIB="$STDLIB" \
       $launcher "$exe" "$@" > "$HERE/golden/$b.out"
     echo "golden/$b.out"; }

# --- sequential (7) ---
g binarytrees           20
g nbody                 20000000
g fannkuchredux         11
g spectralnorm          3000
g mandelbrot            4000
g matrix_multiplication 768
g LU_decomposition      900
# --- parallel (3) — golden generated at DOMAINS=1; domain-independent by
#     construction (quickbench.sh --ci re-checks 1 vs N) ---
g par_spectralnorm      4000 1
g par_matmul            768  1
g par_binarytrees       20   1

echo "regenerated quick/golden from $PLAN ($LINK, perf sizes, heap ${HEAP}MB)"
