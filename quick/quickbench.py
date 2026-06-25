# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib", "numpy"]
# ///
"""quickbench.py — the quick-decision GC benchmark panel for the MMTk OCaml fork.

A SMALL, FAST panel that gives quick perf signal for GC changes. It does NOT
replace the heavyweight ocaml-bench / macro-bench authoritative suite — it is the
"did this GC change help / stay neutral / scale?" eyeball test you run while
iterating.

ONE self-contained script (PEP 723 — its matplotlib/numpy deps are declared in
the header, so `uv` fetches them per-run; no venv, no global install) that:
  * builds nothing (point it at prebuilt bench dirs — see --bin-a/--vanilla),
  * runs each (bench x variant [x domains]) cell, best-of-N, timing directly,
  * guards every cell with a per-cell wall TIMEOUT that kills the whole process
    group (some plans, e.g. ConcurrentImmix, deadlock on some benches),
  * prints a table (+ optional ASCII bar chart),
  * writes NDJSON results, and
  * renders PNG graphs (seq ratio bars + speedup-vs-domains lines).

USAGE
  uv run quick/quickbench.py [seq|par|all] [options]

    seq   sequential benches (binarytrees, nbody, fannkuchredux, spectralnorm,
          mandelbrot, matrix_multiplication, LU_decomposition)
    par   parallel benches (par_spectralnorm, par_matmul, par_binarytrees,
          chameneos_redux), domain sweep
    all   both (default)

  OPTIONS
    --plans P1,P2,...      MMTk plans to run            (default GenImmix)
                           Both "A,B" and "A B" (quoted) are accepted.
    --vanilla DIR          also run a vanilla baseline (a dir of native binaries
                           built with stock ocamlopt) — the ratio baseline.
    --bin-a DIR            the MMTk-built bench dir (x plans). With --vanilla this
    --label-a S            gives the vanilla + N-plans comparison. label default 'mmtk'.
    --bin-b DIR / --label-b S   optional second binary set (A/B feature axis).
    --domains 1,2,4,8      (par) domain counts to sweep   (default 1,2,4,8)
    --heap MB|dynamic      MMTK_HEAP_SIZE_MB; "dynamic" = don't pin (default),
                           so RSS tracks the live set (memory parity w/ vanilla).
    --threads N            pin MMTK_THREADS=N (GC workers). DEFAULT: unset — MMTk
                           uses its own default = nproc (num_cpus::get()). We do
                           NOT force a value; each record logs the effective count
                           ("nproc(<cpus>)" when unset) so the panel self-documents.
                           Pin =1 for a low-overhead single-domain run, or a small
                           N to cap oversubscription at domains >= cores.
    --reps N               measured reps per cell        (default 3, median)
    --warmup N             warmup runs per cell          (default 1)
    --quick                reps=1 warmup=0 + tiny/CI sizes — smoke only.
    --ci                   CI/tiny sizes at reps/warmup.
    --timeout SECS         per-cell wall cap (0 = off). A cell exceeding SECS is
                           killed (whole process group) and recorded HANG.
    --bytecode             use *.byte via ocamlrun (default: native *.native).
    --no-pin / --no-setarch   skip taskset / setarch wrappers (e.g. macOS).
    --cores LIST           taskset core base list (Linux).
    --gc                   add GC count / STW-ms columns (MMTK_VERBOSE; seq only).
    --chart                print a per-bench ASCII bar chart after the seq table.
    --json FILE            write NDJSON results to FILE (default: results.ndjson
                           next to this script). One record per measured cell.
    --graphs DIR           PNG output dir (default quick/graphs/).
    --no-plot              don't render PNGs (headless / table-only).
    -h|--help              this help.

WHAT EACH BENCH PROBES (GC axis)
  binarytrees           mixed lifetime -> generational promotion (CLBG)
  nbody                 compute-bound control, ~0 alloc (codegen/mutator) (CLBG)
  fannkuchredux         small fixed arrays, compute-bound, ~0 alloc (CLBG)
  spectralnorm          float arrays, compute-bound, light alloc (CLBG)
  mandelbrot            compute-bound escape-time, ~0 alloc (CLBG, checksummed)
  matrix_multiplication boxed int matrices -> mature live set (sandmark)
  LU_decomposition      large flat float array, in-place (sandmark)
  kb                    knuth-bendix completion: term-rewriting / symbolic
                        (Rocq/Coq-like), many small short-lived terms (testsuite)
  par_spectralnorm      parallel float compute, GC-worker scaling (sandmark)
  par_matmul            parallel boxed-matrix alloc + live set scaling (sandmark)
  par_binarytrees       parallel alloc + live set + cross-domain STW (sandmark)
  chameneos_redux       effect-handler green threads: heavy effect/continuation
                        (fiber) alloc + resume traffic (effects-examples)

All twelve are dependency-free (stdlib only). The four par_* / effect benches use
raw Domain.spawn (no Domainslib) and split a FIXED total work across the domain
count (STRONG scaling: ideal speedup = #domains).

GOTCHAS encoded here (don't rediscover):
  * ASLR mmap flake — MMTk can abort at startup ("failed to mmap meta memory:
    File exists"); wrap each run in `setarch <arch> -R` (Linux). macOS has no
    setarch; pass --no-setarch.
  * Per-cell timeout MUST kill the whole process group — a hung bench keeps its GC
    worker threads alive; killing just the parent orphans them. We use
    start_new_session=True + os.killpg(..., SIGKILL).
  * We time the whole process wall (warmup excluded), take the median of reps —
    no hyperfine, so no "JSON median is seconds not ms" wart.
"""
import argparse
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FORKROOT = os.environ.get("FORKROOT", os.path.abspath(os.path.join(HERE, "..", "..", "..")))
STDLIB = os.environ.get("STDLIB", os.path.join(FORKROOT, "stdlib"))
DEFAULT_OCAMLRUN = os.environ.get("OCAMLRUN", os.path.join(FORKROOT, "runtime", "ocamlrun"))

SEQ_BENCHES = ["binarytrees", "nbody", "fannkuchredux", "spectralnorm",
               "mandelbrot", "matrix_multiplication", "LU_decomposition", "kb"]
PAR_BENCHES = ["par_spectralnorm", "par_matmul", "par_binarytrees", "chameneos_redux"]

# perf sizes: each run ~0.5-1.5s AND triggers real GC volume. ci sizes: tiny smoke.
PERF = {
    "binarytrees": "20", "nbody": "20000000", "fannkuchredux": "11",
    "spectralnorm": "3000", "mandelbrot": "4000", "matrix_multiplication": "768",
    "LU_decomposition": "900", "par_spectralnorm": "4000", "par_matmul": "768",
    "par_binarytrees": "20", "chameneos_redux": "500000", "kb": "50",
}
CI = {
    "binarytrees": "10", "nbody": "10000", "fannkuchredux": "8",
    "spectralnorm": "200", "mandelbrot": "200", "matrix_multiplication": "64",
    "LU_decomposition": "64", "par_spectralnorm": "200", "par_matmul": "64",
    "par_binarytrees": "12", "chameneos_redux": "2000", "kb": "5",
}

COLORS = {
    "vanilla": "#444444", "mmtk:GenImmix": "#4C72B0",
    "mmtk:Immix": "#DD8452", "mmtk:ConcurrentImmix": "#55A868",
}


# ----------------------------------------------------------------------------
def parse_args(argv):
    mode = "all"
    if argv and argv[0] in ("seq", "par", "all"):
        mode = argv.pop(0)
    p = argparse.ArgumentParser(add_help=True, description="quick GC panel")
    p.add_argument("--plans", default="GenImmix")
    p.add_argument("--vanilla", default="")
    p.add_argument("--bin-a", dest="bin_a", default="")
    p.add_argument("--bin-b", dest="bin_b", default="")
    p.add_argument("--label-a", dest="label_a", default="mmtk")
    p.add_argument("--label-b", dest="label_b", default="b")
    p.add_argument("--domains", default="1,2,4,8")
    p.add_argument("--heap", default="dynamic")
    p.add_argument("--threads", default="")          # "" => leave unset (=nproc)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--warmup", type=int, default=1)
    p.add_argument("--quick", action="store_true")
    p.add_argument("--ci", action="store_true")
    p.add_argument("--timeout", type=float, default=0.0)
    p.add_argument("--bytecode", action="store_true")
    p.add_argument("--no-pin", dest="no_pin", action="store_true")
    p.add_argument("--no-setarch", dest="no_setarch", action="store_true")
    p.add_argument("--cores", default="")
    p.add_argument("--gc", action="store_true")
    p.add_argument("--chart", action="store_true")
    p.add_argument("--json", dest="json_path",
                   default=os.path.join(HERE, "results.ndjson"))
    p.add_argument("--graphs", default=os.path.join(HERE, "graphs"))
    p.add_argument("--no-plot", dest="no_plot", action="store_true")
    a = p.parse_args(argv)
    a.mode = mode
    if a.quick:
        a.reps, a.warmup, a.ci = 1, 0, True
    a.plans = [x for x in re.split(r"[,\s]+", a.plans.strip()) if x]
    a.domains = [int(x) for x in re.split(r"[,\s]+", a.domains.strip()) if x]
    return a


def build_variants(a):
    """list of dicts: {label, dir, ocamlrun, plan}  (plan "" = vanilla)."""
    def ocamlrun_for(d):
        c = os.path.join(d, "ocamlrun")
        return c if os.path.isfile(c) and os.access(c, os.X_OK) else DEFAULT_OCAMLRUN
    out = []
    if a.vanilla:
        out.append(dict(label="vanilla", dir=a.vanilla,
                        ocamlrun=ocamlrun_for(a.vanilla), plan=""))
    if a.bin_a or a.bin_b:
        for pl in a.plans:
            if a.bin_a:
                out.append(dict(label=f"{a.label_a}:{pl}", dir=a.bin_a,
                                ocamlrun=ocamlrun_for(a.bin_a), plan=pl))
            if a.bin_b:
                out.append(dict(label=f"{a.label_b}:{pl}", dir=a.bin_b,
                                ocamlrun=ocamlrun_for(a.bin_b), plan=pl))
    elif not a.vanilla:
        for pl in a.plans:
            out.append(dict(label=pl, dir=os.path.join(HERE, "build"),
                            ocamlrun=DEFAULT_OCAMLRUN, plan=pl))
    return out


# ---- launch plumbing -------------------------------------------------------
def have(cmd):
    return shutil.which(cmd) is not None


def setarch_prefix(a):
    if not a.no_setarch and have("setarch"):
        return ["setarch", os.uname().machine, "-R"]
    return []


def pin_prefix(a, k):
    if a.no_pin or not have("taskset"):
        return []
    if a.cores:
        lst = a.cores.split(",")[:k]
        return ["taskset", "-c", ",".join(lst)]
    return ["taskset", "-c", f"0-{k-1}"]


def effective_threads(a):
    """The MMTK_THREADS value we set (or None => MMTk default = nproc)."""
    return a.threads if a.threads else None


def threads_label(a):
    t = effective_threads(a)
    return str(t) if t is not None else f"nproc({os.cpu_count()})"


def cell_env(a, plan, dom):
    e = dict(os.environ)
    e["OCAMLLIB"] = STDLIB
    if plan:
        e["MMTK_PLAN"] = plan
        if a.heap != "dynamic":
            e["MMTK_HEAP_SIZE_MB"] = str(a.heap)
        # GC-worker count: leave UNSET by default so the cell reflects MMTk's
        # out-of-the-box default (= nproc). Only set it if --threads pins a value.
        t = effective_threads(a)
        if t is not None:
            e["MMTK_THREADS"] = str(t)
    if dom is not None:
        e["DOMAINS"] = str(dom)
    return e


def run_once(cmd, env, timeout):
    """Run cmd; return (elapsed_ms, status). status: 'ok'|'hang'|'err'.
    Kills the whole process group on timeout (hung GC workers included)."""
    t0 = time.monotonic()
    try:
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
    except FileNotFoundError:
        return None, "err"
    try:
        rc = p.wait(timeout=timeout if timeout and timeout > 0 else None)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.wait()
        return None, "hang"
    ms = (time.monotonic() - t0) * 1000.0
    return (ms, "ok" if rc == 0 else "err")


def cell_median(a, variant, bench, args, dom):
    """Best-of-reps median wall ms for one cell, or (None,'hang')."""
    exe = os.path.join(variant["dir"], f"{bench}." + ("byte" if a.bytecode else "native"))
    if not os.path.exists(exe):
        return None, "missing"
    launcher = [variant["ocamlrun"]] if a.bytecode else []
    argv = [str(args)] + ([str(dom)] if dom is not None else [])
    k = dom if dom else 1
    cmd = pin_prefix(a, k) + setarch_prefix(a) + launcher + [exe] + argv
    env = cell_env(a, variant["plan"], dom)
    for _ in range(a.warmup):
        _, st = run_once(cmd, env, a.timeout)
        if st == "hang":
            return None, "hang"
    times = []
    for _ in range(a.reps):
        ms, st = run_once(cmd, env, a.timeout)
        if st == "hang":
            return None, "hang"
        if ms is not None:
            times.append(ms)
    if not times:
        return None, "err"
    return statistics.median(times), "ok"


# ---- formatting ------------------------------------------------------------
def fmt_ms(v):
    if v is None:
        return "-"
    if v >= 100:
        return f"{v:.0f}"
    if v >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def emit(records, **rec):
    records.append(rec)


def write_json(path, records):
    with open(path, "w") as f:
        for r in records:
            f.write(json.dumps(r) + "\n")


# ---- run modes -------------------------------------------------------------
def run_seq(a, variants, sizes, records):
    print("\n## sequential   (cells: median-ms | ratio-vs-baseline)")
    colw = 22
    hdr = f"{'bench':22s}" + "".join(f"{v['label']:<{colw}}" for v in variants)
    print(hdr)
    print(f"{'-'*10:22s}" + "".join(f"{'-'*20:<{colw}}" for _ in variants))
    seq_med = {}      # bench -> label -> ms|None
    for b in SEQ_BENCHES:
        row = f"{b:22s}"
        base = None
        seq_med[b] = {}
        for v in variants:
            med, st = cell_median(a, v, b, sizes[b], None)
            seq_med[b][v["label"]] = med if st == "ok" else None
            emit(records, mode="seq", bench=b, variant=v["label"],
                 plan=v["plan"] or "vanilla", domains=1, threads=threads_label(a),
                 median_ms=med if st == "ok" else None,
                 status=("ok" if st == "ok" else "hang" if st == "hang" else st))
            if st == "missing":
                row += f"{'n/a':<{colw}}"; continue
            if st == "hang":
                row += f"{'HANG (>'+str(int(a.timeout))+'s)':<{colw}}"; continue
            if base is None:
                base = med
            ratio = f"{med/base:.2f}x" if base else "-"
            row += f"{fmt_ms(med)+' | '+ratio:<{colw}}"
        print(row)
    print(f"(ratio is vs the first variant: {variants[0]['label'] if variants else '-'} = 1.00x)")
    return seq_med


def run_par(a, variants, sizes, records):
    print(f"\n## parallel (domain sweep: {','.join(map(str,a.domains))})")
    lw = max([16] + [len(v["label"]) + 2 for v in variants])
    for b in PAR_BENCHES:
        print(f"\n### {b}  (args: {sizes[b]})")
        print(f"{'variant':<{lw}}" + "".join(f"{'d='+str(d)+' (ms | spd)':<18}" for d in a.domains))
        for v in variants:
            row = f"{v['label']:<{lw}}"
            t1 = None
            for d in a.domains:
                med, st = cell_median(a, v, b, sizes[b], d)
                emit(records, mode="par", bench=b, variant=v["label"],
                     plan=v["plan"] or "vanilla", domains=d, threads=threads_label(a),
                     median_ms=med if st == "ok" else None,
                     status=("ok" if st == "ok" else "hang" if st == "hang" else st))
                if st == "missing":
                    row += f"{'n/a':<18}"; continue
                if st == "hang":
                    row += f"{'HANG (>'+str(int(a.timeout))+'s)':<18}"; continue
                if t1 is None:
                    t1 = med
                spd = f"{t1/med:.2f}x" if (t1 and med) else "-"
                row += f"{fmt_ms(med)+' | '+spd:<18}"
            print(row)
        print(f"(spd = T(first domains)/T(N); ideal ~ linear. "
              f"MMTk GC workers = {threads_label(a)} per cell.)")


def print_chart(a, variants, seq_med):
    print("\n## sequential — ASCII bar chart (longer = slower; scaled per bench)")
    maxlabel = max((len(v["label"]) for v in variants), default=0)
    for b in SEQ_BENCHES:
        meds = seq_med.get(b, {})
        nums = [m for m in meds.values() if isinstance(m, (int, float))]
        print(f"\n{b}:")
        if not nums:
            print("  (no numeric data)"); continue
        rowmax = max(nums)
        for v in variants:
            m = meds.get(v["label"])
            if not isinstance(m, (int, float)):
                print(f"  {v['label']:<{maxlabel}} {'HANG' if m is None else 'n/a'}")
                continue
            n = max(1, round(m / rowmax * 40))
            print(f"  {v['label']:<{maxlabel}} {'█'*n} {fmt_ms(m)}ms")


# ---- plotting (matplotlib) -------------------------------------------------
def plot_all(records, outdir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    os.makedirs(outdir, exist_ok=True)

    seq = [r for r in records if r["mode"] == "seq"]
    par = [r for r in records if r["mode"] == "par"]

    if seq:
        by = {}
        variants = []
        for r in seq:
            by.setdefault(r["bench"], {})[r["variant"]] = r["median_ms"]
            if r["variant"] not in variants:
                variants.append(r["variant"])
        benches = [b for b in SEQ_BENCHES if b in by]
        mmtk = [v for v in variants if v != "vanilla"]
        if mmtk and "vanilla" in variants:
            x = np.arange(len(benches)); w = 0.8 / len(mmtk)
            fig, ax = plt.subplots(figsize=(11, 5))
            for i, v in enumerate(mmtk):
                ratios = [(by[b].get(v) / by[b]["vanilla"])
                          if (by[b].get("vanilla") and by[b].get(v)) else np.nan
                          for b in benches]
                off = (i - (len(mmtk)-1)/2) * w
                bars = ax.bar(x+off, ratios, w, label=v.replace("mmtk:", "MMTk "),
                              color=COLORS.get(v))
                for r in bars:
                    h = r.get_height()
                    if np.isfinite(h):
                        ax.annotate(f"{h:.2f}×", (r.get_x()+r.get_width()/2, h),
                                    textcoords="offset points", xytext=(0, 2),
                                    ha="center", va="bottom", fontsize=7.5)
            ax.axhline(1.0, color="#444", lw=1.2, ls="--", zorder=0)
            ax.text(len(benches)-0.5, 1.02, "vanilla 5.5.0 = 1.0", ha="right",
                    va="bottom", fontsize=9, color="#444")
            ax.set_ylabel("time / vanilla 5.5.0  (lower is better)")
            ax.set_title("Quick panel — sequential: MMTk vs vanilla OCaml 5.5.0\n"
                         "(native, dynamic heap = memory parity, best-of-N)")
            ax.set_xticks(x); ax.set_xticklabels(benches, rotation=20, ha="right", fontsize=9)
            ax.legend(loc="upper left"); ax.grid(axis="y", ls=":", alpha=0.4)
            fig.tight_layout()
            out = os.path.join(outdir, "seq_ratio.png")
            fig.savefig(out, dpi=120); print("wrote", out)

    if par:
        by = {}; doms = set(); workers = set()
        for r in par:
            by.setdefault(r["bench"], {}).setdefault(r["variant"], {})[r["domains"]] = r["median_ms"]
            doms.add(r["domains"])
            if r.get("plan", "vanilla") != "vanilla":
                workers.add(str(r.get("threads")))
        doms = sorted(doms)
        benches = [b for b in PAR_BENCHES if b in by]
        ncol = 2; nrow = (len(benches)+ncol-1)//ncol
        fig, axes = plt.subplots(nrow, ncol, figsize=(11, 4.3*nrow), squeeze=False)
        for i, b in enumerate(benches):
            ax = axes[i//ncol][i % ncol]
            ax.plot(doms, doms, color="#bbbbbb", ls="--", lw=1.2, label="ideal (linear)")
            for v, dv in by[b].items():
                t1 = dv.get(doms[0])
                xs = [d for d in doms if dv.get(d) and t1]
                ys = [t1/dv[d] for d in xs]
                if xs:
                    ax.plot(xs, ys, marker="o", lw=1.8, color=COLORS.get(v),
                            label=v.replace("mmtk:", "MMTk "))
                for d in doms:
                    if dv.get(d) is None:
                        ax.scatter([d], [0.15], marker="x", s=45, color="red", zorder=5)
            ax.set_title(b, fontsize=11); ax.set_xlabel("domains")
            ax.set_ylabel("speedup  T(1)/T(N)"); ax.set_xticks(doms)
            ax.grid(ls=":", alpha=0.4); ax.legend(fontsize=7.5, loc="upper left")
        for j in range(len(benches), nrow*ncol):
            axes[j//ncol][j % ncol].axis("off")
        wnote = ("MMTk GC workers = " + "/".join(sorted(workers))) if workers else ""
        fig.suptitle("Quick panel — parallel scalability: speedup vs domains\n"
                     f"(native, dynamic heap, best-of-N; red × = hang/crash; {wnote})",
                     fontsize=12)
        fig.tight_layout(rect=[0, 0, 1, 0.95])
        out = os.path.join(outdir, "speedup_domains.png")
        fig.savefig(out, dpi=120); print("wrote", out)


# ----------------------------------------------------------------------------
def main():
    # Line-buffer stdout so the table prints progressively (and survives an
    # interrupt) instead of sitting in a block buffer until exit when piped.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass
    a = parse_args(sys.argv[1:])
    sizes = CI if a.ci else PERF
    variants = build_variants(a)

    print("=" * 60)
    print(f"quick GC panel  —  mode={a.mode}  link={'bytecode' if a.bytecode else 'native'}"
          f"  sizes={'ci' if a.ci else 'perf'}")
    print(f"plans={' '.join(a.plans)}  heap={a.heap}  reps={a.reps}  warmup={a.warmup}"
          f"  gc-workers={threads_label(a)}")
    if a.vanilla:
        print(f"vanilla={a.vanilla}")
    if a.bin_a or a.bin_b:
        print(f"A={a.label_a} ({a.bin_a})   B={a.label_b} ({a.bin_b})")
    print("variants: " + " ".join(v["label"] for v in variants))
    print("=" * 60)

    records = []
    seq_med = {}
    if a.mode in ("seq", "all"):
        seq_med = run_seq(a, variants, sizes, records)
    if a.mode in ("par", "all"):
        run_par(a, variants, sizes, records)
    if a.chart and a.mode != "par":
        print_chart(a, variants, seq_med)

    write_json(a.json_path, records)
    print(f"\nwrote results JSON: {a.json_path}", file=sys.stderr)

    if not a.no_plot:
        try:
            plot_all(records, a.graphs)
        except Exception as e:  # plotting is best-effort; never fail the run
            print(f"(plotting skipped: {e})", file=sys.stderr)


if __name__ == "__main__":
    main()
