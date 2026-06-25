#!/usr/bin/env bash
# quickbench.sh — quick-decision GC benchmark panel for the MMTk OCaml fork.
#
# A SMALL, FAST panel that gives quick perf signal for GC changes. It does NOT
# replace the heavyweight ocaml-bench / macro-bench authoritative suite — it is
# the "did this GC change help / stay neutral / scale?" eyeball test you run
# while iterating. Design budget: a full run of the whole panel on ONE variant
# costs ~5 minutes at the default perf sizes and default reps.
#
# USAGE
#   quickbench.sh [seq|par|all] [options]
#
#     seq   sequential benches only (binarytrees, nbody, fannkuchredux,
#           spectralnorm, mandelbrot, matrix_multiplication, LU_decomposition)
#     par   parallel benches only  (par_spectralnorm, par_matmul,
#           par_binarytrees), domain sweep
#     all   both (default)
#
#   OPTIONS
#     --plans  P1,P2,...        MMTk plans to run            (default GenImmix)
#     --vanilla DIR             also run a vanilla baseline (a dir holding
#                               ocamlrun/native binaries — see --bin-* below) for
#                               ratios. Sugar for adding it as an extra variant.
#     --bin-a DIR --bin-b DIR   A/B two prebuilt binary sets, interleaved. This is
#                               the feature axis (e.g. no-zero OFF vs ON): build
#                               each variant's benches into its own dir, point -a
#                               and -b at them, and they're compared head-to-head
#                               under every plan. Works identically for
#                               plan-vs-plan and fork-vs-vanilla.
#     --label-a S --label-b S   labels for the two binary sets (default a / b).
#     --feature S               cosmetic label for the run header (e.g. no_zero).
#     --domains 1,2,4,8         (par) domain counts to sweep   (default 1,2,4,8)
#     --heap MB                 MMTK_HEAP_SIZE_MB              (default 512)
#     --reps N                  measured reps per cell        (default 3)
#     --warmup N                warmup runs per cell          (default 1)
#     --quick                   reps=1 warmup=0 + tiny/CI sizes — smoke only.
#     --ci                      CI/tiny sizes (correctness sizes) at reps/warmup.
#     --bytecode                use *.byte via ocamlrun (default: native *.native).
#     --cores LIST              taskset core list base (default 0-...). par uses
#                               the first K of these for K domains.
#     --no-pin                  don't taskset (e.g. macOS / no util-linux).
#     --no-setarch              don't wrap in `setarch -R` (e.g. macOS).
#     --gc                      add GC count / STW-ms columns (MMTK_VERBOSE;
#                               an extra untimed run per cell, seq only).
#     --chart                   after the seq table, print a per-bench ASCII bar
#                               chart (bars scaled to the slowest variant in each
#                               bench row) for a paste-into-README eyeball view.
#     --timeout SECS            per-cell wall cap (default 0 = off). A cell whose
#                               measurement exceeds SECS is killed and rendered
#                               HANG instead of wedging the whole table — needed
#                               because some plans (e.g. ConcurrentImmix) can hang
#                               on a given bench. SECS is the cap for the WHOLE
#                               warmup+reps measurement of one cell.
#     -h|--help                 this help.
#
# WHAT EACH BENCH PROBES (GC axis)
#   binarytrees           mixed lifetime -> generational promotion (CLBG)
#   nbody                 compute-bound control, ~0 alloc (codegen/mutator) (CLBG)
#   fannkuchredux         small fixed arrays, compute-bound, ~0 alloc (CLBG)
#   spectralnorm          float arrays, compute-bound, light alloc (CLBG)
#   mandelbrot            compute-bound escape-time, ~0 alloc (CLBG, checksummed)
#   matrix_multiplication boxed int matrices -> mature live set (sandmark)
#   LU_decomposition      large flat float array, in-place (sandmark)
#   par_spectralnorm      parallel float compute, GC-worker scaling (sandmark)
#   par_matmul            parallel boxed-matrix alloc + live set scaling (sandmark)
#   par_binarytrees       parallel alloc + live set + cross-domain STW (sandmark)
#
# All ten are dependency-free (stdlib only). The three par_* benches use raw
# Domain.spawn (no Domainslib) and are domain-count-INDEPENDENT by construction.
#
# OUTPUT
#   seq: per (bench × variant) median±σ wall time + ratio vs baseline (the
#        vanilla/bin-a/first-plan, in that order of preference). Optional GC
#        columns (count / STW ms) from MMTK_VERBOSE.
#   par: per bench a scalability table — wall per domain count + speedup T1/TN.
#
# Runs each measurement under `setarch -R` (ASLR off; MMTk maps side metadata at
# fixed addresses and an ASLR collision aborts startup) and, on Linux, pins cores
# with taskset for stable numbers.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
BRANCHROOT="$(cd "$HERE/.." && pwd)"          # the benchmarks-branch root
FORKROOT="${FORKROOT:-$(cd "$HERE/../../.." && pwd)}"  # the OCaml fork tree
STDLIB="${STDLIB:-$FORKROOT/stdlib}"
DEFAULT_OCAMLRUN="${OCAMLRUN:-$FORKROOT/runtime/ocamlrun}"

# ---- defaults -------------------------------------------------------------
MODE="all"
PLANS="GenImmix"
DOMAINS="1,2,4,8"
HEAP="512"
REPS=3
WARMUP=1
SIZESET="perf"          # perf | ci
LINK="native"           # native | bytecode
BIN_A=""; BIN_B=""; LABEL_A="a"; LABEL_B="b"
VANILLA=""; FEATURE=""
CORES=""; PIN=1; USE_SETARCH=1; SHOW_GC=0; CHART=0; CELL_TIMEOUT=0

# ---- input sizes ----------------------------------------------------------
# Kept as case-functions (not associative arrays) so the harness runs on stock
# macOS bash 3.2 as well as Linux bash 4/5.
#
# PERF sizes (native target): each run ~2-5s AND triggers several collections /
# real allocation volume — see README "Input sizes". CI sizes: fast, tiny,
# deterministic, still GC-touching; they define the goldens.
perf_args(){ case "$1" in
  binarytrees)           echo "20";;        # depth 20 — mixed lifetime, promotion
  nbody)                 echo "20000000";;  # 20M steps, ~0 alloc, compute control
  fannkuchredux)         echo "11";;        # n=11 permutation enumeration
  spectralnorm)          echo "3000";;      # 3000 float vector, 10 AtA iterations
  mandelbrot)            echo "4000";;      # 4000x4000 escape-time, byte checksum
  matrix_multiplication) echo "768";;       # 768x768 boxed-int matmul
  LU_decomposition)      echo "900";;       # 900x900 in-place LU
  par_spectralnorm)      echo "4000";;      # 4000 vector, parallel A/At passes
  par_matmul)            echo "768";;       # 768x768 matmul, row loop split
  par_binarytrees)       echo "20";;        # depth-20 class set, split across domains
esac; }
ci_args(){ case "$1" in
  binarytrees)           echo "10";;
  nbody)                 echo "10000";;
  fannkuchredux)         echo "8";;
  spectralnorm)          echo "200";;
  mandelbrot)            echo "200";;
  matrix_multiplication) echo "64";;
  LU_decomposition)      echo "64";;
  par_spectralnorm)      echo "200";;
  par_matmul)            echo "64";;
  par_binarytrees)       echo "12";;
esac; }

SEQ_BENCHES="binarytrees nbody fannkuchredux spectralnorm mandelbrot matrix_multiplication LU_decomposition"
PAR_BENCHES="par_spectralnorm par_matmul par_binarytrees"

usage(){ sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//; s/^#$//' | sed '$d'; exit "${1:-0}"; }

# ---- arg parse ------------------------------------------------------------
case "${1:-}" in seq|par|all) MODE="$1"; shift;; -h|--help) usage 0;; esac
while [ $# -gt 0 ]; do
  case "$1" in
    --plans)    PLANS="$2"; shift 2;;
    --vanilla)  VANILLA="$2"; shift 2;;
    --bin-a)    BIN_A="$2"; shift 2;;
    --bin-b)    BIN_B="$2"; shift 2;;
    --label-a)  LABEL_A="$2"; shift 2;;
    --label-b)  LABEL_B="$2"; shift 2;;
    --feature)  FEATURE="$2"; shift 2;;
    --domains)  DOMAINS="$2"; shift 2;;
    --heap)     HEAP="$2"; shift 2;;
    --reps)     REPS="$2"; shift 2;;
    --warmup)   WARMUP="$2"; shift 2;;
    --quick)    REPS=1; WARMUP=0; SIZESET="ci"; shift;;
    --ci)       SIZESET="ci"; shift;;
    --bytecode) LINK="bytecode"; shift;;
    --native)   LINK="native"; shift;;
    --cores)    CORES="$2"; shift 2;;
    --no-pin)   PIN=0; shift;;
    --no-setarch) USE_SETARCH=0; shift;;
    --gc)       SHOW_GC=1; shift;;
    --chart)    CHART=1; shift;;
    --timeout)  CELL_TIMEOUT="$2"; shift 2;;
    -h|--help)  usage 0;;
    *) echo "unknown option: $1" >&2; usage 2;;
  esac
done

# size lookup
arg_for(){ local b=$1; if [ "$SIZESET" = ci ]; then ci_args "$b"; else perf_args "$b"; fi; }
suffix(){ [ "$LINK" = bytecode ] && echo byte || echo native; }

# ---- variants -------------------------------------------------------------
# A "variant" = a label + a binary directory + an ocamlrun (for bytecode) + a
# fixed MMTk plan (or "" = vanilla, no MMTk env). We build the variant list from
# --bin-a/--bin-b (× plans) and/or --vanilla, falling back to "this tree's
# build/ dir × plans" if no --bin-* given.
VAR_LABEL=(); VAR_DIR=(); VAR_RUN=(); VAR_PLAN=()
add_variant(){ VAR_LABEL+=("$1"); VAR_DIR+=("$2"); VAR_RUN+=("$3"); VAR_PLAN+=("$4"); }

ocamlrun_for(){ # binary dir -> ocamlrun to use for bytecode (dir/ocamlrun if present)
  local d=$1; if [ -x "$d/ocamlrun" ]; then echo "$d/ocamlrun"; else echo "$DEFAULT_OCAMLRUN"; fi; }

# Split plans on EITHER commas or whitespace, so both `--plans GenImmix,Immix`
# and `--plans "GenImmix Immix"` work. (A space-separated string used to collapse
# into a single bogus plan label "GenImmix Immix ...", which both rendered one
# joined column AND produced a malformed `env MMTK_PLAN=GenImmix Immix …` whose
# extra words were taken as the command to exec — so the bench binary + its size
# argv never ran, giving the ~3 ms "instant" non-runs.)
read -r -a PLAN_ARR <<< "$(echo "$PLANS" | tr ',' ' ')"
build_variants(){
  local pl
  if [ -n "$VANILLA" ]; then
    # vanilla = no MMTk; binaries live in $VANILLA (a build dir)
    add_variant "vanilla" "$VANILLA" "$(ocamlrun_for "$VANILLA")" ""
  fi
  if [ -n "$BIN_A" ] || [ -n "$BIN_B" ]; then
    # A/B feature axis × plans, interleaved (A then B per plan) for fair pairing
    for pl in "${PLAN_ARR[@]}"; do
      [ -n "$BIN_A" ] && add_variant "$LABEL_A:$pl" "$BIN_A" "$(ocamlrun_for "$BIN_A")" "$pl"
      [ -n "$BIN_B" ] && add_variant "$LABEL_B:$pl" "$BIN_B" "$(ocamlrun_for "$BIN_B")" "$pl"
    done
  elif [ -z "$VANILLA" ]; then
    # default: this tree's own build/ dir × plans
    for pl in "${PLAN_ARR[@]}"; do
      add_variant "$pl" "$HERE/build" "$DEFAULT_OCAMLRUN" "$pl"
    done
  fi
}

# ---- launch plumbing ------------------------------------------------------
# setarch prefix as a STRING (embedded into command strings so they're
# self-contained — works under both `eval` and hyperfine's own shell).
sr_prefix(){ if [ "$USE_SETARCH" -eq 1 ] && command -v setarch >/dev/null 2>&1; then
               echo "setarch $(uname -m) -R"; else echo ""; fi; }
SR="$(sr_prefix)"

# core-pin prefix for K cores (par) or 1 core (seq).
pin_prefix(){ local k=$1
  [ "$PIN" -eq 0 ] && { echo ""; return; }
  command -v taskset >/dev/null 2>&1 || { echo ""; return; }
  local list="${CORES:-0-$((k-1))}"
  # if a base list was given, take its first K entries; else use 0..k-1
  if [ -n "$CORES" ]; then
    echo "taskset -c $(echo "$CORES" | tr ',' '\n' | head -n "$k" | paste -sd, -)"
  else
    echo "taskset -c 0-$((k-1))"
  fi
}

# Build the full command (no measurement) for one (variant, bench, args, domains).
#   emits the argv as a string suitable for `eval`/hyperfine -- after the prefix.
exe_path(){ local dir=$1 b=$2; echo "$dir/$b.$(suffix)"; }

# env+launcher for a variant; $1=plan ("" vanilla) $2=ocamlrun $3=domains
launch_env(){ local plan=$1 dom=$2
  local e="OCAMLLIB=$STDLIB"
  if [ -n "$plan" ]; then
    e="$e MMTK_PLAN=$plan"
    # --heap dynamic => don't pin a fixed heap; use the fork's dynamic (MemBalancer)
    # default so RSS tracks the live set, for memory parity with vanilla.
    [ "$HEAP" != "dynamic" ] && e="$e MMTK_HEAP_SIZE_MB=$HEAP"
  fi
  [ -n "$dom" ]  && e="$e DOMAINS=$dom"
  echo "$e"
}

have_hyperfine(){ command -v hyperfine >/dev/null 2>&1; }

# Run a shell command string under a wall-clock cap (seconds). Returns the
# command's exit status, or 124 if it was killed for exceeding the cap (like GNU
# `timeout`). Portable (no coreutils `timeout` on macOS): a perl parent sets an
# alarm, runs the command in its OWN process group, and on timeout kills that
# whole group (so the bench AND its GC worker threads die, not just the shell).
# CELL_TIMEOUT=0 disables (runs the command directly).
cap_run(){ local cmd=$1
  if [ "${CELL_TIMEOUT:-0}" = 0 ]; then eval "$cmd"; return $?; fi
  CAP="$CELL_TIMEOUT" CMD="$cmd" perl -e '
    use POSIX qw(setsid);
    my $secs = $ENV{CAP}; my $cmd = $ENV{CMD};
    my $pid = fork();
    if ($pid == 0) { setsid(); exec("/bin/sh","-c",$cmd) or exit 127; }
    local $SIG{ALRM} = sub { kill("KILL", -$pid); waitpid($pid,0); exit 124; };
    alarm($secs);
    waitpid($pid, 0);
    alarm(0);
    exit($? >> 8);
  '
}

# Median wall (ms) of N reps via /usr/bin/time, fallback when no hyperfine.
# args: <full command string>. Honours CELL_TIMEOUT (a capped, killed run is
# recorded as the cap so the cell's median reflects "at least this slow / hung").
time_median(){ local cmd=$1 reps=$2 warmup=$3 i t times=()
  for ((i=0;i<warmup;i++)); do cap_run "$cmd" >/dev/null 2>&1; done
  for ((i=0;i<reps;i++)); do
    local s e rc
    s=$(date +%s%N); cap_run "$cmd" >/dev/null 2>&1; rc=$?; e=$(date +%s%N)
    if [ "$rc" = 124 ]; then times+=( $(( CELL_TIMEOUT * 1000 )) )
    else times+=( $(( (e - s) / 1000000 )) ); fi
  done
  printf '%s\n' "${times[@]}" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'
}

# Pull "GCs STWms" from one MMTK_VERBOSE run. arg: <full command string>.
# (MMTk prints "[mmtk] GCs: 14, GC time: 3322 ms, objects copied: ..." on stderr
#  at exit; vanilla has no such line, so returns "- -".)
gc_stats(){ local cmd=$1
  local line; line=$(eval "MMTK_VERBOSE=1 $cmd" 2>&1 >/dev/null | grep -m1 'GCs:')
  local gcs stw
  gcs=$(echo "$line" | sed -n 's/.*GCs: \([0-9]*\).*/\1/p')
  stw=$(echo "$line" | sed -n 's/.*GC time: \([0-9]*\) ms.*/\1/p')
  echo "${gcs:--} ${stw:--}"
}

# ---------------------------------------------------------------------------
print_header(){
  echo "============================================================"
  echo "quick GC panel  —  mode=$MODE  link=$LINK  sizes=$SIZESET"
  local heapdesc; [ "$HEAP" = dynamic ] && heapdesc="dynamic" || heapdesc="${HEAP}MB"
  echo "plans=${PLAN_ARR[*]}  heap=${heapdesc}  reps=$REPS  warmup=$WARMUP${FEATURE:+  feature=$FEATURE}"
  [ -n "$BIN_A" ] && echo "A=$LABEL_A ($BIN_A)   B=$LABEL_B ($BIN_B)"
  [ -n "$VANILLA" ] && echo "vanilla=$VANILLA"
  echo "variants: ${VAR_LABEL[*]}"
  echo "============================================================"
}

# ---- sequential -----------------------------------------------------------
# hyperfine median (ms) for a full shell command string, or "" if unavailable.
# NB: hyperfine's --export-json reports every time in SECONDS regardless of
# --time-unit (that flag only affects the human-readable stdout). The manual
# time_median fallback returns integer milliseconds, so we convert here (×1000)
# to keep ONE unit — milliseconds — across both code paths and the "ms" labels.
hf_median(){ local cmd=$1
  have_hyperfine || { echo ""; return; }
  # Cap the WHOLE hyperfine run (it does warmup+reps internally; one hung run
  # would otherwise wedge it forever). Budget = per-cell cap × invocations + a
  # little slack. A capped/killed hyperfine -> sentinel HANG (propagated up).
  local out rc total
  if [ "${CELL_TIMEOUT:-0}" != 0 ]; then
    total=$(( CELL_TIMEOUT * (WARMUP + REPS) + CELL_TIMEOUT ))
    out="$(CELL_TIMEOUT="$total" cap_run "hyperfine -w $WARMUP -r $REPS --export-json /dev/stdout \"$cmd\" 2>/dev/null")"; rc=$?
    [ "$rc" = 124 ] && { echo "HANG"; return; }
  else
    # NOT -N: the command has `env VAR=… …`, which needs a shell to interpret.
    out="$(hyperfine -w "$WARMUP" -r "$REPS" --export-json /dev/stdout "$cmd" 2>/dev/null)"
  fi
  local sec
  sec="$(echo "$out" | sed -n 's/.*"median":[ ]*\([0-9.]*\).*/\1/p' | head -1)"
  [ -z "$sec" ] && { echo ""; return; }
  awk -v s="$sec" 'BEGIN{printf "%.4f", s*1000}'
}

# one (variant, bench) median in ms — hyperfine if present, else manual timer.
# Returns the literal HANG if a cap fired (so the table shows it, not a number).
cell_median(){ local cmd=$1
  local m; m="$(hf_median "$cmd")"
  [ "$m" = HANG ] && { echo "HANG"; return; }
  [ -z "$m" ] && m="$(time_median "$cmd" "$REPS" "$WARMUP")"
  echo "$m"
}

# Format a raw millisecond value (possibly a long hyperfine float like
# 0.005633024679999999 or a manual-timer integer) into a short, column-friendly
# string: >=100ms -> integer; >=10 -> 1dp; else 2dp. Keeps the table aligned.
fmt_ms(){ awk -v v="$1" 'BEGIN{
    if (v=="" ) { print "-"; exit }
    if (v+0 >= 100)      printf "%.0f", v;
    else if (v+0 >= 10)  printf "%.1f", v;
    else                 printf "%.2f", v;
  }'
}

# Medians recorded by run_seq for the optional ASCII chart, flat-indexed
# bench_index*nvar + variant_index (plain indexed arrays => bash 3.2 safe).
MED=(); SEQ_DONE=()
run_seq(){
  local COLW=22; [ "$SHOW_GC" -eq 1 ] && COLW=34
  local nvar=${#VAR_LABEL[@]}
  echo; echo "## sequential   (cells: median-ms | ratio-vs-baseline)"
  printf "%-22s" "bench"
  for v in "${VAR_LABEL[@]}"; do printf "%-${COLW}s" "$v"; done
  echo
  # rule line under the header
  printf "%-22s" "----------"
  for v in "${VAR_LABEL[@]}"; do printf "%-${COLW}s" "--------------------"; done
  echo
  local prefix; prefix="$(pin_prefix 1)"
  local bi=0
  for b in $SEQ_BENCHES; do
    SEQ_DONE[$bi]="$b"
    local args; args="$(arg_for "$b")"
    printf "%-22s" "$b"
    local base=""   # baseline median for ratios (first variant)
    local vi
    for vi in "${!VAR_LABEL[@]}"; do
      local dir="${VAR_DIR[$vi]}" run="${VAR_RUN[$vi]}" plan="${VAR_PLAN[$vi]}"
      local exe; exe="$(exe_path "$dir" "$b")"
      local launcher=""; [ "$LINK" = bytecode ] && launcher="$run"
      if [ ! -e "$exe" ]; then MED[$((bi*nvar+vi))]=""; printf "%-${COLW}s" "n/a"; continue; fi
      local env; env="$(launch_env "$plan" "")"
      local cmd="$prefix $SR env $env $launcher $exe $args"
      local med; med="$(cell_median "$cmd")"
      MED[$((bi*nvar+vi))]="$med"
      if [ "$med" = HANG ]; then printf "%-${COLW}s" "HANG (>${CELL_TIMEOUT}s)"; continue; fi
      # baseline = first NON-hung variant (normally vanilla, which never hangs)
      [ -z "$base" ] && base="$med"
      local ratio="-"
      if [ -n "$base" ] && [ -n "$med" ] && [ "$base" != 0 ]; then
        ratio=$(awk -v m="$med" -v b="$base" 'BEGIN{printf "%.2fx", m/b}')
      fi
      local cell="$(fmt_ms "$med") | ${ratio}"
      if [ "$SHOW_GC" -eq 1 ] && [ -n "$plan" ]; then
        local g; g="$(gc_stats "$cmd")"   # "GCs STWms"
        cell="$cell | gc ${g% *}/${g#* }ms"
      fi
      printf "%-${COLW}s" "$cell"
    done
    echo
    bi=$((bi+1))
  done
  echo "(ratio is vs the first variant: ${VAR_LABEL[0]:-} = baseline 1.00x)"
}

# Optional ASCII bar chart from the medians run_seq recorded. Per bench, bars are
# scaled to the SLOWEST variant in that bench row (so the longest bar = slowest;
# shorter = faster). Reads MED[]/SEQ_DONE[]/VAR_LABEL[].
print_chart(){
  [ "${#SEQ_DONE[@]}" -eq 0 ] && return
  local nvar=${#VAR_LABEL[@]} width=40
  echo; echo "## sequential — ASCII bar chart (longer = slower; scaled per bench)"
  local maxlabel=0 v
  for v in "${VAR_LABEL[@]}"; do [ ${#v} -gt $maxlabel ] && maxlabel=${#v}; done
  local bi
  for bi in "${!SEQ_DONE[@]}"; do
    local b="${SEQ_DONE[$bi]}"
    # row max (only over numeric medians; HANG / "" / n/a are skipped)
    local rowmax=0 vi med
    for ((vi=0; vi<nvar; vi++)); do
      med="${MED[$((bi*nvar+vi))]:-}"
      case "$med" in ''|*[!0-9.]*) ;; *) rowmax=$(awk -v a="$rowmax" -v b="$med" 'BEGIN{print (b>a)?b:a}');; esac
    done
    echo; echo "$b:"
    [ "$rowmax" = 0 ] && { echo "  (no numeric data)"; continue; }
    for ((vi=0; vi<nvar; vi++)); do
      med="${MED[$((bi*nvar+vi))]:-}"
      case "$med" in
        '') printf "  %-*s n/a\n" "$maxlabel" "${VAR_LABEL[$vi]}"; continue;;
        *[!0-9.]*) printf "  %-*s %s\n" "$maxlabel" "${VAR_LABEL[$vi]}" "$med"; continue;;  # e.g. HANG
      esac
      local nblk; nblk=$(awk -v m="$med" -v mx="$rowmax" -v w="$width" \
        'BEGIN{n=int(m/mx*w+0.5); if(n<1)n=1; print n}')
      local bar; bar=$(awk -v n="$nblk" 'BEGIN{s="";for(i=0;i<n;i++)s=s"\xe2\x96\x88";print s}')
      printf "  %-*s %s %sms\n" "$maxlabel" "${VAR_LABEL[$vi]}" "$bar" "$(fmt_ms "$med")"
    done
  done
}

# ---- parallel -------------------------------------------------------------
run_par(){
  echo; echo "## parallel (domain sweep: $DOMAINS)"
  IFS=',' read -r -a DOM_ARR <<< "$DOMAINS"
  local b
  for b in $PAR_BENCHES; do
    local args; args="$(arg_for "$b")"
    echo; echo "### $b  (args: $args)"
    printf "%-16s" "variant"
    for d in "${DOM_ARR[@]}"; do printf "%-18s" "d=$d (ms | spd)"; done
    echo
    local vi
    for vi in "${!VAR_LABEL[@]}"; do
      local dir="${VAR_DIR[$vi]}" run="${VAR_RUN[$vi]}" plan="${VAR_PLAN[$vi]}"
      local exe; exe="$(exe_path "$dir" "$b")"
      local launcher=""; [ "$LINK" = bytecode ] && launcher="$run"
      printf "%-16s" "${VAR_LABEL[$vi]}"
      if [ ! -e "$exe" ]; then echo "n/a"; continue; fi
      local t1=""   # first-domain-count wall for speedup
      local d
      for d in "${DOM_ARR[@]}"; do
        local prefix; prefix="$(pin_prefix "$d")"
        local env; env="$(launch_env "$plan" "$d")"
        local med; med="$(cell_median "$prefix $SR env $env $launcher $exe $args $d")"
        [ "$d" = "${DOM_ARR[0]}" ] && t1="$med"
        local spd="-"
        if [ -n "$t1" ] && [ -n "$med" ] && [ "$med" != 0 ]; then
          spd=$(awk -v t1="$t1" -v tn="$med" 'BEGIN{printf "%.2fx", t1/tn}')
        fi
        printf "%-18s" "$(fmt_ms "$med") | ${spd}"
      done
      echo
    done
    echo "(spd = T(first domain count) / T(N); ideal ~ linear in domains)"
  done
}

# ---------------------------------------------------------------------------
build_variants
print_header
case "$MODE" in
  seq) run_seq;;
  par) run_par;;
  all) run_seq; run_par;;
esac
[ "$CHART" -eq 1 ] && [ "$MODE" != par ] && print_chart
