#!/usr/bin/env bash
# CLBG benchmark harness for the MMTk OCaml fork.
#
#   ./run.sh validate [PLAN...]   correctness: run each bench (bytecode) under
#                                 each plan at CI size, byte-compare to golden/.
#                                 Prints a matrix; exits nonzero on any failure.
#   ./run.sh matrix   [PLAN...]   performance: run each bench (native) under each
#                                 native-capable plan (Immix/StickyImmix) at perf
#                                 size; record wall-ms + max-RSS to results/matrix.csv.
#   ./run.sh golden   [PLAN]      regenerate golden/ from one plan (default Immix).
#
# Env: MMTK_HEAP_SIZE_MB (default 1024), CLBG_TIMEOUT (default 120 s),
#      OCAMLRUN (default ../../runtime/ocamlrun), and PERF_<bench>=N overrides.
#
# Runs under `setarch -R` (ASLR off): MMTk maps side metadata at fixed
# addresses and an ASLR collision aborts startup (a known mmtk-core issue).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
STDLIB="${STDLIB:-$ROOT/stdlib}"
OCAMLRUN="${OCAMLRUN:-$ROOT/runtime/ocamlrun}"
HEAP="${MMTK_HEAP_SIZE_MB:-1024}"
TIMEOUT="${CLBG_TIMEOUT:-120}"

# Benches split by how they take input; BENCHES is the two concatenated.
ARG_BENCHES="fasta nbody spectralnorm binarytrees mandelbrot fannkuchredux"  # take N on argv
STDIN_BENCHES="knucleotide revcomp"           # consume a FASTA stream on stdin
BENCHES="$ARG_BENCHES $STDIN_BENCHES"

# All plans run in bytecode (allocation goes through real C calls). ALL_PLANS is
# the two groups concatenated.
IMMIX_FAMILY="Immix StickyImmix GenImmix"     # Immix-based plans
# Non-Immix plans (validated bytecode-only): the free-list / semi-space / mark-compact
# families wired in addition to the defaults. PageProtect is correct too but is a
# page-per-object debug plan that exceeds the CI time cap (e.g. mandelbrot > TIMEOUT),
# so it is intentionally NOT in the CI set — run it manually with a larger TIMEOUT/heap.
BYTECODE_ONLY_PLANS="NoGC MarkSweep SemiSpace GenCopy MarkCompact"
ALL_PLANS="$BYTECODE_ONLY_PLANS $IMMIX_FAMILY"

# Native uses TLAB nursery-aliasing over a bump/Immix-Default allocator. SEVEN plans
# qualify — Immix/StickyImmix/GenImmix/GenCopy/SemiSpace/NoGC/ConcurrentImmix — including
# GenImmix, whose copy-nursery BumpPointer TLAB has worked natively since the native-
# GenImmix work; it is the DEFAULT plan, so the perf matrix must cover it. The matrix
# runs the practical fast subset: the Immix family plus the GenImmix default. (GenCopy and
# SemiSpace are native-capable but copy-dominated — GenCopy times out on binarytrees and
# SemiSpace is several× slower — so they are left out of the timed matrix; NoGC never
# reclaims; ConcurrentImmix is the research plan, run separately.)
NATIVE_PLANS="Immix StickyImmix GenImmix"

# CI (correctness) sizes — small, fast, deterministic, GC-exercising.
declare -A CI_N=( [fasta]=1000 [nbody]=10000 [spectralnorm]=100 \
  [binarytrees]=10 [mandelbrot]=200 [fannkuchredux]=7 )
# Perf sizes — moderate defaults (override with PERF_<bench>=N). Far below the
# official CLBG sizes so a full matrix finishes in minutes, not hours.
declare -A PERF_N=( [fasta]=2500000 [nbody]=5000000 [spectralnorm]=3000 \
  [binarytrees]=18 [mandelbrot]=4000 [fannkuchredux]=11 )

sr(){ setarch "$(uname -m)" -R "$@"; }
# Run a command under ASLR-off + timeout + the MMTk env for plan $1.
mmtk_run(){ local pl=$1; shift
  sr timeout "$TIMEOUT" env MMTK_PLAN="$pl" MMTK_HEAP_SIZE_MB="$HEAP" OCAMLLIB="$STDLIB" "$@"; }
is_stdin(){ [[ " $STDIN_BENCHES " == *" $1 "* ]]; }
TIME_BIN="${TIME_BIN:-/usr/bin/time}"   # GNU time, for the perf matrix

# Input fed to the stdin benches (knucleotide/revcomp) during validate. The
# CI-size stream is just fasta's CI output, which is fasta's committed golden.
# (`make golden` keeps it in sync with CI_N[fasta]; matrix mode makes its own.)
STDIN_INPUT="${STDIN_INPUT:-$HERE/golden/fasta.out}"

# Run one benchmark under one plan and byte-compare to its golden.
#   run_one <bench> <plan> <exe> <launcher> <tag>
# <launcher> is "$OCAMLRUN" for bytecode and "" for native (run directly).
# Echoes PASS / DIFF / HANG / CRASH<rc>; returns nonzero unless PASS.
run_one(){
  local b=$1 pl=$2 exe=$3 launcher=$4 tag=$5
  local out="build/val/$b.$pl.$tag" in=/dev/null arg="" rc
  # stdin benches read a FASTA stream; arg benches take N and ignore stdin.
  if is_stdin "$b"; then in="$STDIN_INPUT"; else arg="${CI_N[$b]}"; fi
  mmtk_run "$pl" $launcher "$exe" $arg < "$in" > "$out" 2>/dev/null; rc=$?
  if   [ $rc -eq 124 ]; then echo "HANG";     return 1
  elif [ $rc -ne 0 ];   then echo "CRASH$rc"; return 1
  elif cmp -s "$out" "golden/$b.out"; then echo "PASS"; return 0
  else echo "DIFF"; return 1; fi
}

# Print a (benchmark × plan) matrix for one build kind, byte-comparing to golden.
#   matrix_pass <tag> <suffix> <launcher> <plan...>
# Sets global FAIL=1 on any non-PASS. Returns nonzero if no such binaries exist.
matrix_pass(){
  local tag=$1 suffix=$2 launcher=$3; shift 3
  local plans="$*"
  ls "$HERE"/build/*."$suffix" >/dev/null 2>&1 || return 1
  printf "%-16s" "benchmark"; for pl in $plans; do printf "%-13s" "$pl"; done; echo
  for b in $BENCHES; do
    printf "%-16s" "$b"
    for pl in $plans; do
      local r
      if [ -x "$HERE/build/$b.$suffix" ]; then
        r=$(run_one "$b" "$pl" "$HERE/build/$b.$suffix" "$launcher" "$tag") || FAIL=1
      else r="n/a"; fi
      printf "%-13s" "$r"
    done
    echo
  done
}

cmd_validate(){
  local plans="${*:-$ALL_PLANS}"
  FAIL=0
  mkdir -p build/val

  echo "bytecode (all plans):"
  matrix_pass byte byte "$OCAMLRUN" $plans

  # Native: the Immix-family subset of the requested plans, if native binaries
  # were built (`make native`). Skipped gracefully otherwise (e.g. a
  # bytecode-only CI build), so native correctness is checked wherever possible.
  local nplans=""
  for pl in $plans; do [[ " $NATIVE_PLANS " == *" $pl "* ]] && nplans="$nplans $pl"; done
  echo
  if [ -n "$nplans" ] && ls "$HERE"/build/*.native >/dev/null 2>&1; then
    echo "native (Immix/StickyImmix):"
    matrix_pass native native "" $nplans
  else
    echo "native: skipped (no build/*.native — run 'make native')"
  fi

  echo
  [ "$FAIL" -eq 0 ] && echo "ALL PASS" || echo "FAILURES PRESENT"
  return "$FAIL"
}

cmd_matrix(){
  local plans="${*:-$NATIVE_PLANS}"
  mkdir -p results build/perf
  # FASTA input for the stdin benches at perf size (PERF_fasta overrides).
  local fn="${PERF_fasta:-${PERF_N[fasta]}}"
  mmtk_run Immix "$HERE/build/fasta.native" "$fn" > build/perf/fasta.input 2>/dev/null
  local csv="results/matrix.csv"
  echo "plan,benchmark,n,wall_ms,max_rss_kb,status" > "$csv"
  for pl in $plans; do
    for b in $BENCHES; do
      local n stdin="" arg ev="PERF_$b"
      if is_stdin "$b"; then n="$fn"; stdin="build/perf/fasta.input"; arg="";
      else n="${!ev:-${PERF_N[$b]}}"; arg="$n"; fi
      local t="build/perf/$b.$pl.time" out="build/perf/$b.$pl.out" in=/dev/null
      [ -n "$stdin" ] && in="$stdin"
      # (/usr/bin/time is external and can't wrap the mmtk_run function, so the
      #  setarch+timeout+env prefix is spelled out here — just once.)
      sr $TIME_BIN -v -o "$t" timeout "$TIMEOUT" env MMTK_PLAN="$pl" MMTK_HEAP_SIZE_MB="$HEAP" \
        "$HERE/build/$b.native" $arg < "$in" > "$out" 2>/dev/null
      local rc=$? status="ok"
      [ $rc -eq 124 ] && status="timeout"; [ $rc -gt 0 ] && [ $rc -ne 124 ] && status="crash$rc"
      # perf output is not byte-compared to CI golden (different N); just flag empties
      [ -s "$out" ] || [ "$status" != ok ] || status="empty"
      local wall rss
      wall=$(awk -F'): ' '/Elapsed \(wall/{print $2}' "$t" 2>/dev/null)
      rss=$(awk -F': ' '/Maximum resident/{print $2}' "$t" 2>/dev/null)
      echo "$pl,$b,$n,${wall:-NA},${rss:-NA},$status" >> "$csv"
      printf "%-12s %-16s %-10s %s\n" "$pl" "$b" "$n" "$status"
    done
  done
  echo "wrote $csv"
}

cmd_golden(){
  local pl="${1:-Immix}"
  mkdir -p golden
  mmtk_run "$pl" "$OCAMLRUN" build/fasta.byte 1000 > golden/fasta.out 2>/dev/null
  for b in nbody spectralnorm binarytrees mandelbrot fannkuchredux; do
    mmtk_run "$pl" "$OCAMLRUN" build/$b.byte "${CI_N[$b]}" > golden/$b.out 2>/dev/null
  done
  for b in knucleotide revcomp; do
    mmtk_run "$pl" "$OCAMLRUN" build/$b.byte < golden/fasta.out > golden/$b.out 2>/dev/null
  done
  echo "regenerated golden/ from $pl"
}

case "${1:-}" in
  validate) shift; cmd_validate "$@";;
  matrix)   shift; cmd_matrix "$@";;
  golden)   shift; cmd_golden "$@";;
  *) echo "usage: $0 {validate|matrix|golden} [plans...]" >&2; exit 2;;
esac
