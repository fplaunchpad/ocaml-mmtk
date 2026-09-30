# /// script
# requires-python = ">=3.11"
# dependencies = ["matplotlib", "numpy"]
# ///
"""spacetime.py — space-time curve sweep for the MMTk OCaml fork vs vanilla.

GC performance is a SPACE-TIME CURVE, not a single wall ratio at whatever heap a
plan's sizing policy happens to pick. This driver follows Jane Street's "memory
allocator showdown" method (blog.janestreet.com/memory-allocator-showdown/):
sweep the GC's space knobs, one run per grid point, plot x = peak memory (RSS),
y = wall time, one series per configuration, and compare the lower-left fronts.
Jane Street fitted  H = b/(t - a) + c  (H = memory, t = time; a = asymptotic
minimum time, c = asymptotic minimum space ~ live data + fragmentation); we
overlay the same model, rearranged  t = a + b/(H - c),  as a dashed bonus curve.

USAGE
  uv run quick/spacetime.py --vanilla DIR --mmtk DIR \\
      --plans "GenImmix Bactrian" \\
      --benches "binarytrees kb matrix_multiplication LU_decomposition chameneos_redux" \\
      --json OUT.ndjson --graphs DIR [--reps 1] [--no-setarch] [--no-pin] [--quick]
      [--resume] [--logy] [--timeout 600]
      [--vanilla-o "40 80 120 200 320"] [--vanilla-s "256k 1M 4M"]
      [--heaps "32 48 64 96 128 192 256 dynamic"]
      [--nurseries "default Fixed:4194304 Fixed:33554432"]
      [--plot-only] [--pauses]

GRIDS
  vanilla   OCAMLRUNPARAM=o=<O>,s=<S> over O x S, plus one extra `default` point
            (no OCAMLRUNPARAM at all).
  mmtk:P    MMTK_PLAN=P, MMTK_HEAP_SIZE_MB over --heaps (`dynamic` = unset, the
            space-overhead default) x MMTK_NURSERY over --nurseries (`default` =
            unset). The (dynamic, default) point is the plan's `default` point.
            MMTK_THREADS=1 always (single-domain cells, as the panel's
            `--threads domains`).
  Every inherited OCAMLRUNPARAM / MMTK_* variable is REMOVED from the child env,
  so the recorded `env` dict fully determines the GC configuration of a point.

MEASUREMENT (same as quickbench.py)
  wall = perf_counter around the child; peak RSS = the child's rusage ru_maxrss
  from os.wait4 (macOS bytes -> KiB normalised), reported in MiB. With --reps N
  the point keeps the MIN wall and the MAX RSS over the reps. A non-zero exit
  (e.g. Out_of_memory at a too-small pinned heap) or a timeout is recorded as a
  failed point (exit code + last stderr line) — never dropped. All points run.

PAUSES (--pauses)
  Adds MMTK_PAUSE_LOG=1 to MMTk points and v=0x400 to vanilla's OCAMLRUNPARAM
  (measurement only: not recorded in `env`, which stays the GC configuration).
  Each row gains `pauses` (quickbench.parse_pauses: count, full, total_ms,
  mean/p50/p95/p99/max_us, ttsp_ms, kinds) and `mutator_ms` = wall - total
  pause, both from the min-wall rep. Vanilla's v=0x400 has counts only.

OUTPUT
  NDJSON (append; --resume skips points already in the file), one PNG per bench,
  a small-multiples summary.png, and SUMMARY.md (also printed).
"""

import argparse
import datetime
import json
import math
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from quickbench import PERF, CI, STDLIB, COLORS as QB_COLORS, _maxrss_kib, have, parse_pauses  # noqa: E402

DEFAULT_BENCHES = "binarytrees kb matrix_multiplication LU_decomposition chameneos_redux"
DEFAULT_O = "40 80 120 200 320"
DEFAULT_S = "256k 1M 4M"
DEFAULT_HEAPS = "32 48 64 96 128 192 256 dynamic"
DEFAULT_NURSERIES = "default Fixed:4194304 Fixed:33554432"
# Benches whose argv takes a domain count after the size (the par_* set +
# chameneos_redux); run single-domain here.
DOMAIN_ARG_BENCHES = {"par_spectralnorm", "par_matmul", "par_binarytrees", "chameneos_redux"}

COLORS = dict(QB_COLORS)
COLORS.setdefault("mmtk:Bactrian", "#55A868")
EXTRA_COLORS = ["#8172B3", "#937860", "#DA8BC3", "#8C8C8C", "#CCB974", "#64B5CD"]
MARKERS = {"vanilla": "s", "mmtk:GenImmix": "o", "mmtk:Bactrian": "^",
           "mmtk:Immix": "D", "mmtk:LXR": "v"}
TIE_TOL = 0.02   # fronts within 2% in wall at the same RSS count as a tie (noise)


def split_list(s):
    return [x for x in re.split(r"[,\s]+", (s or "").strip()) if x]


def parse_args(argv):
    p = argparse.ArgumentParser(description="space-time curve sweep (vanilla vs MMTk plans)")
    p.add_argument("--vanilla", default=None, help="dir of vanilla native binaries")
    p.add_argument("--mmtk", default=None, help="dir of MMTk-built native binaries")
    p.add_argument("--plans", default="GenImmix Bactrian")
    p.add_argument("--benches", default=DEFAULT_BENCHES)
    p.add_argument("--json", dest="json_path", default=os.path.join(HERE, "spacetime.ndjson"))
    p.add_argument("--graphs", default=os.path.join(HERE, "graphs_spacetime"))
    p.add_argument("--reps", type=int, default=1)
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--quick", action="store_true", help="tiny CI sizes (smoke test)")
    p.add_argument("--no-setarch", dest="no_setarch", action="store_true")
    p.add_argument("--no-pin", dest="no_pin", action="store_true")
    p.add_argument("--cores", default="", help="taskset core list (first one used)")
    p.add_argument("--resume", action="store_true", help="skip points already in --json")
    p.add_argument("--logy", action="store_true", help="log-scale wall axis")
    p.add_argument("--plot-only", dest="plot_only", action="store_true",
                   help="don't run; plot + summarise the existing --json")
    p.add_argument("--vanilla-o", dest="vanilla_o", default=DEFAULT_O)
    p.add_argument("--vanilla-s", dest="vanilla_s", default=DEFAULT_S)
    p.add_argument("--heaps", default=DEFAULT_HEAPS)
    p.add_argument("--nurseries", default=DEFAULT_NURSERIES)
    p.add_argument("--pauses", action="store_true",
                   help="per-pause stats (MMTK_PAUSE_LOG=1 / vanilla v=0x400) in each row")
    a = p.parse_args(argv)
    a.plans = split_list(a.plans)
    a.benches = split_list(a.benches)
    a.vanilla_o = split_list(a.vanilla_o)
    a.vanilla_s = split_list(a.vanilla_s)
    a.heaps = split_list(a.heaps)
    a.nurseries = split_list(a.nurseries)
    for b in a.benches:
        if b not in PERF:
            p.error(f"unknown bench {b!r} (known: {' '.join(PERF)})")
    return a


# ---- grid ------------------------------------------------------------------
def bench_argv(bench, sizes):
    argv = [sizes[bench]]
    if bench in DOMAIN_ARG_BENCHES:
        argv.append("1")
    return argv


def knobs_key(knobs):
    return json.dumps(knobs, sort_keys=True)


def grid_points(a):
    """Yield (variant, bin_dir, knobs, env_overrides). knobs == {} is `default`."""
    pts = []
    if a.vanilla:
        pts.append(("vanilla", a.vanilla, {}, {}))
        for o in a.vanilla_o:
            for s in a.vanilla_s:
                pts.append(("vanilla", a.vanilla, {"o": o, "s": s},
                            {"OCAMLRUNPARAM": f"o={o},s={s}"}))
    if a.mmtk:
        for plan in a.plans:
            for h in a.heaps:
                for n in a.nurseries:
                    knobs, env = {}, {"MMTK_PLAN": plan, "MMTK_THREADS": "1"}
                    if h != "dynamic":
                        knobs["heap_mb"] = h
                        env["MMTK_HEAP_SIZE_MB"] = h
                    if n != "default":
                        knobs["nursery"] = n
                        env["MMTK_NURSERY"] = n
                    pts.append((f"mmtk:{plan}", a.mmtk, knobs, env))
    return pts


def knobs_label(variant, knobs):
    if not knobs:
        return "default"
    if variant == "vanilla":
        return f"o={knobs['o']},s={knobs['s']}"
    return ",".join([f"heap={knobs.get('heap_mb', 'dyn')}"]
                    + ([f"nursery={knobs['nursery']}"] if "nursery" in knobs else []))


# ---- launch ------------------------------------------------------------------
def prefix(a):
    pre = []
    if not a.no_pin and have("taskset"):
        pre += ["taskset", "-c", (a.cores.split(",")[0] if a.cores else "0")]
    if not a.no_setarch and have("setarch"):
        pre += ["setarch", os.uname().machine, "-R"]
    return pre


def child_env(overrides):
    e = {k: v for k, v in os.environ.items()
         if k != "OCAMLRUNPARAM" and not k.startswith("MMTK_")}
    e["OCAMLLIB"] = STDLIB
    e["DOMAINS"] = "1"
    e.update(overrides)
    return e


def run_once(cmd, env, timeout):
    """-> dict(wall_ms, rss_kib, exit, status, stderr_tail). status ok|err|timeout."""
    with tempfile.TemporaryFile() as errf:
        t0 = time.perf_counter()
        try:
            p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                                 stderr=errf, start_new_session=True)
        except (FileNotFoundError, PermissionError) as ex:
            return dict(wall_ms=None, rss_kib=None, exit=None, status="err",
                        stderr_tail=str(ex))
        fired = {"v": False}

        def kill():
            fired["v"] = True
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except (ProcessLookupError, OSError):
                pass
        timer = threading.Timer(timeout, kill) if timeout and timeout > 0 else None
        if timer:
            timer.start()
        rusage = None
        try:
            _, wst, rusage = os.wait4(p.pid, 0)
            rc = os.waitstatus_to_exitcode(wst)
        except ChildProcessError:
            rc = p.poll() if p.poll() is not None else -1
        finally:
            if timer:
                timer.cancel()
        wall = (time.perf_counter() - t0) * 1000.0
        p.returncode = rc
        errf.seek(0)
        text = errf.read().decode("utf-8", "replace")
        lines = [l for l in text.splitlines()
                 if l.strip() and not l.startswith("[mmtk-pause")]
        tail = lines[-1][:300] if lines else ""
    if fired["v"]:
        return dict(wall_ms=None, rss_kib=None, exit=rc, status="timeout",
                    stderr_tail=tail or f"killed after {timeout:.0f}s")
    return dict(wall_ms=wall, rss_kib=_maxrss_kib(rusage), exit=rc,
                status="ok" if rc == 0 else "err", stderr_tail=tail,
                pauses=parse_pauses(text) if rc == 0 else None)


def run_point(a, bench, argv, variant, bindir, knobs, env_over):
    exe = os.path.join(bindir, f"{bench}.native")
    base = dict(bench=bench, args=" ".join(argv), variant=variant, knobs=knobs,
                label=knobs_label(variant, knobs), reps=a.reps,
                timestamp=datetime.datetime.now().isoformat(timespec="seconds"),
                host=platform.node(), env=env_over, bin=os.path.abspath(exe))
    if not os.path.exists(exe):
        return dict(base, wall_ms=None, max_rss_mib=None, exit=None, ok=False,
                    status="missing", stderr_tail=f"no such binary {exe}")
    cmd = prefix(a) + [os.path.abspath(exe)] + argv
    env = child_env(env_over)
    if a.pauses:
        if variant == "vanilla":
            prev = env.get("OCAMLRUNPARAM", "")
            env["OCAMLRUNPARAM"] = (prev + "," if prev else "") + "v=0x400"
        else:
            env["MMTK_PAUSE_LOG"] = "1"
    walls, rsss, reps = [], [], []
    for _ in range(max(1, a.reps)):
        r = run_once(cmd, env, a.timeout)
        if r["status"] != "ok":
            return dict(base, wall_ms=None, max_rss_mib=None, exit=r["exit"], ok=False,
                        status=r["status"], stderr_tail=r["stderr_tail"],
                        cmd=cmd)
        walls.append(r["wall_ms"])
        reps.append(r)
        if r["rss_kib"] is not None:
            rsss.append(r["rss_kib"])
    extra = {}
    if a.pauses:
        best = min(reps, key=lambda r: r["wall_ms"])
        ps = best["pauses"]
        extra = dict(pauses=ps,
                     mutator_ms=(round(best["wall_ms"] - ps["total_ms"], 3)
                                 if ps and ps.get("total_ms") is not None else None))
    return dict(base, **extra, wall_ms=min(walls),
                max_rss_mib=(round(max(rsss) / 1024.0, 1) if rsss else None),
                exit=0, ok=True, status="ok", stderr_tail="", cmd=cmd)


# ---- NDJSON ------------------------------------------------------------------
def load_rows(path):
    rows = []
    if os.path.exists(path):
        with open(path) as f:
            for ln in f:
                ln = ln.strip()
                if ln:
                    try:
                        rows.append(json.loads(ln))
                    except json.JSONDecodeError:
                        pass  # tolerate a torn last line from a crashed sweep
    return rows


def row_key(r):
    return (r["bench"], r["args"], r["variant"], knobs_key(r["knobs"]))


def sweep(a, sizes):
    have_keys = {row_key(r) for r in load_rows(a.json_path)} if a.resume else set()
    pts = grid_points(a)
    total = len(pts) * len(a.benches)
    os.makedirs(os.path.dirname(os.path.abspath(a.json_path)), exist_ok=True)
    n = 0
    t_start = time.monotonic()
    with open(a.json_path, "a") as out:
        for bench in a.benches:
            argv = bench_argv(bench, sizes)
            for (variant, bindir, knobs, env_over) in pts:
                n += 1
                key = (bench, " ".join(argv), variant, knobs_key(knobs))
                if key in have_keys:
                    print(f"[{n}/{total}] {bench} {variant} {knobs_label(variant, knobs)}: (resume skip)")
                    continue
                row = run_point(a, bench, argv, variant, bindir, knobs, env_over)
                out.write(json.dumps(row) + "\n")
                out.flush()
                if row["ok"]:
                    msg = f"{row['wall_ms']:.0f} ms  {row['max_rss_mib']} MiB"
                    ps = row.get("pauses")
                    if ps:
                        msg += (f"  pauses n={ps['count']} full={ps['full']}"
                                + (f" total={ps['total_ms']:.1f}ms max={ps['max_us']:.0f}us"
                                   if ps.get("total_ms") is not None and ps.get("max_us") is not None
                                   else ""))
                else:
                    msg = f"FAIL {row['status']} exit={row['exit']}  {row['stderr_tail']}"
                el = time.monotonic() - t_start
                print(f"[{n}/{total}] {bench} {variant:<14} {knobs_label(variant, knobs):<34} {msg}"
                      f"   (elapsed {el/60:.1f} min)")


# ---- analysis ----------------------------------------------------------------
def front(points):
    """Lower-left (Pareto) front: sort by RSS, keep points whose wall strictly
    decreases. points: list of (rss, wall, row)."""
    out, best = [], math.inf
    for p in sorted(points, key=lambda p: (p[0], p[1])):
        if p[1] < best:
            out.append(p)
            best = p[1]
    return out


def front_at(fr, m):
    """Best wall achievable with RSS <= m on front fr (step function), or None."""
    v = None
    for (x, y, _) in fr:
        if x <= m:
            v = y
    return v


def classify(fm, fv):
    """Compare front fm (MMTk) against fv (vanilla) over their overlapping RSS
    range, as step functions. -> dominates | dominated | crossing | tie | disjoint."""
    if not fm or not fv:
        return "n/a"
    lo = max(fm[0][0], fv[0][0])
    hi = max(fm[-1][0], fv[-1][0])
    xs = sorted({x for (x, _, _) in fm + fv if lo <= x <= hi})
    if not xs:
        return "disjoint"
    better = worse = 0
    for x in xs:
        ym, yv = front_at(fm, x), front_at(fv, x)
        if ym is None or yv is None:
            continue
        r = ym / yv
        if r < 1 - TIE_TOL:
            better += 1
        elif r > 1 + TIE_TOL:
            worse += 1
    if better and not worse:
        # also require MMTk to reach at least as low in memory to call it full dominance
        return "dominates" if fm[0][0] <= fv[0][0] else "faster only at higher RSS"
    if worse and not better:
        return "dominated"
    if better and worse:
        return "crossing"
    return "tie" if fm[0][0] <= fv[0][0] * (1 + TIE_TOL) else "tie (MMTk needs more memory)"


def fit_curve(points):
    """Fit t = a + b/(H - c) (Jane Street's H = b/(t-a) + c). Grid over c < min H,
    linear least squares for (a, b) at each c. Returns (a, b, c) or None."""
    import numpy as np
    if len(points) < 4:
        return None
    H = np.array([p[0] for p in points], float)
    t = np.array([p[1] for p in points], float)
    hmin = H.min()
    if H.max() / hmin < 1.3:
        return None
    best = None
    for frac in np.linspace(0.0, 0.995, 400):
        c = frac * hmin
        X = np.column_stack([np.ones_like(H), 1.0 / (H - c)])
        coef, *_ = np.linalg.lstsq(X, t, rcond=None)
        a_, b_ = coef
        if b_ <= 0 or a_ <= 0:
            continue
        sse = float(((X @ coef - t) ** 2).sum())
        if best is None or sse < best[0]:
            best = (sse, a_, b_, c)
    if best is None:
        return None
    sse, a_, b_, c = best
    sst = float(((t - t.mean()) ** 2).sum())
    if sst <= 0 or 1 - sse / sst < 0.5:     # ill-conditioned / poor fit: skip
        return None
    return (a_, b_, c)


def group(rows, benches, sizes):
    """bench -> variant -> {'ok': [(rss_mib, wall_s, row)], 'fail': [row]}.
    For a duplicated point (re-run without --resume) the LAST row wins."""
    latest = {}
    for r in rows:
        b = r.get("bench")
        if b in benches and r.get("args") == " ".join(bench_argv(b, sizes)):
            latest[row_key(r)] = r
    g = {}
    for r in latest.values():
        d = g.setdefault(r["bench"], {}).setdefault(r["variant"], {"ok": [], "fail": []})
        if r.get("ok") and r.get("max_rss_mib") is not None and r.get("wall_ms") is not None:
            d["ok"].append((r["max_rss_mib"], r["wall_ms"] / 1000.0, r))
        else:
            d["fail"].append(r)
    return g


def variant_order(vs):
    return sorted(vs, key=lambda v: (v != "vanilla", v))


def color_for(v, order):
    if v in COLORS:
        return COLORS[v]
    extras = [x for x in order if x not in COLORS]
    return EXTRA_COLORS[extras.index(v) % len(EXTRA_COLORS)]


# ---- plotting ----------------------------------------------------------------
def draw_bench(ax, bench, bv, order, logy, small=False):
    for v in order:
        if v not in bv:
            continue
        d = bv[v]
        col = color_for(v, order)
        mk = MARKERS.get(v, "o")
        nfail = len(d["fail"])
        name = "vanilla 5.5.0" if v == "vanilla" else v.replace("mmtk:", "MMTk ")
        lab = f"{name} ({len(d['ok'])} pts" + (f", {nfail} failed)" if nfail else ")")
        if d["ok"]:
            xs = [p[0] for p in d["ok"]]
            ys = [p[1] for p in d["ok"]]
            ax.scatter(xs, ys, s=(14 if small else 26), marker=mk, color=col, alpha=0.45,
                       edgecolors="none", zorder=3)
            fr = front(d["ok"])
            ax.plot([p[0] for p in fr], [p[1] for p in fr], drawstyle="steps-post",
                    color=col, lw=(1.4 if small else 2.0), zorder=4, label=lab,
                    marker=mk, markersize=(3.5 if small else 5.5))
            dflt = [p for p in d["ok"] if not p[2]["knobs"]]
            if dflt:
                ax.scatter([dflt[0][0]], [dflt[0][1]], s=(60 if small else 120), marker=mk,
                           facecolors="none", edgecolors=col, linewidths=1.6, zorder=5)
            fit = fit_curve(d["ok"])
            if fit:
                import numpy as np
                a_, b_, c = fit
                hs = np.linspace(max(min(xs), c * 1.001 + 1e-9), max(xs), 200)
                ax.plot(hs, a_ + b_ / (hs - c), ls="--", lw=1.0, color=col, alpha=0.8, zorder=2)
        else:
            ax.plot([], [], color=col, marker=mk, label=lab)
    ax.set_title(bench, fontsize=(10 if small else 12))
    ax.set_xlabel("max RSS (MiB)")
    ax.set_ylabel("wall (s)")
    if logy:
        ax.set_yscale("log")
    ax.grid(ls=":", alpha=0.4)
    ax.legend(fontsize=(6.5 if small else 8.5), loc="upper right")


def plot_all(a, g):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    os.makedirs(a.graphs, exist_ok=True)
    benches = [b for b in a.benches if b in g]
    order = variant_order({v for b in benches for v in g[b]})
    note = ("markers = all grid points; line = lower-left front (sort by RSS, keep strictly "
            "faster);\nhollow ring = default config; dashed = fit t = a + b/(H - c) (Jane Street)")
    for b in benches:
        fig, ax = plt.subplots(figsize=(8.5, 5.5))
        draw_bench(ax, b, g[b], order, a.logy)
        ax.set_title(f"{b} ({g[b][order[0]]['ok'][0][2]['args'] if g[b].get(order[0], {}).get('ok') else ''})"
                     f" — space-time, lower-left is better\n", fontsize=12)
        fig.text(0.01, 0.005, note, fontsize=7, color="#555")
        fig.tight_layout(rect=[0, 0.05, 1, 1])
        out = os.path.join(a.graphs, f"spacetime_{b}.png")
        fig.savefig(out, dpi=120)
        plt.close(fig)
        print("wrote", out)
    if benches:
        ncol = min(3, len(benches))
        nrow = (len(benches) + ncol - 1) // ncol
        fig, axes = plt.subplots(nrow, ncol, figsize=(5.2 * ncol, 4.0 * nrow), squeeze=False)
        for i, b in enumerate(benches):
            draw_bench(axes[i // ncol][i % ncol], b, g[b], order, a.logy, small=True)
        for j in range(len(benches), nrow * ncol):
            axes[j // ncol][j % ncol].axis("off")
        fig.suptitle(f"Space-time fronts: vanilla vs MMTk ({platform.node()})\n"
                     "x = max RSS, y = wall; lower-left is better", fontsize=12)
        fig.text(0.01, 0.005, note, fontsize=7, color="#555")
        fig.tight_layout(rect=[0, 0.04, 1, 0.95])
        out = os.path.join(a.graphs, "summary.png")
        fig.savefig(out, dpi=120)
        plt.close(fig)
        print("wrote", out)


# ---- summary -------------------------------------------------------------------
def summary_md(a, g):
    L = ["# Space-time sweep summary", "",
         f"host `{platform.node()}`; source `{os.path.basename(a.json_path)}`. "
         "For each MMTk variant: the point on its lower-left front whose RSS is "
         "closest to vanilla's `default` point, and its wall ratio vs that vanilla "
         "point (<1 = MMTk faster). `class` compares the two fronts as step "
         "functions over their overlapping RSS range "
         f"(ties within {TIE_TOL:.0%}): dominates / dominated / crossing / tie.", ""]
    L.append("| bench | variant | vanilla default (MiB, s) | nearest MMTk front point (MiB, s) "
             "| knobs | time ratio | MMTk default (MiB, s) | class | failed pts |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    for b in [b for b in a.benches if b in g]:
        bv = g[b]
        van = bv.get("vanilla", {"ok": [], "fail": []})
        vd = [p for p in van["ok"] if not p[2]["knobs"]]
        fv = front(van["ok"])
        for v in variant_order(bv):
            if v == "vanilla":
                continue
            d = bv[v]
            fm = front(d["ok"])
            md = [p for p in d["ok"] if not p[2]["knobs"]]
            md_s = f"{md[0][0]:.0f}, {md[0][1]:.3f}" if md else "-"
            if vd and fm:
                near = min(fm, key=lambda p: abs(math.log(p[0] / vd[0][0])))
                ratio = f"{near[1] / vd[0][1]:.2f}×"
                L.append(f"| {b} | {v} | {vd[0][0]:.0f}, {vd[0][1]:.3f} | "
                         f"{near[0]:.0f}, {near[1]:.3f} | {near[2]['label']} | {ratio} | "
                         f"{md_s} | {classify(fm, fv)} | {len(d['fail'])} |")
            else:
                L.append(f"| {b} | {v} | {'-' if not vd else f'{vd[0][0]:.0f}, {vd[0][1]:.3f}'} "
                         f"| - | - | - | {md_s} | {classify(fm, fv)} | {len(d['fail'])} |")
        if van["fail"]:
            L.append(f"| {b} | vanilla | | | | | | | {len(van['fail'])} |")
    fails = [r for b in g for v in g[b] for r in g[b][v]["fail"]]
    if fails:
        L += ["", "## Failed points", "",
              "| bench | variant | knobs | status | exit | last stderr line |",
              "|---|---|---|---|---|---|"]
        for r in sorted(fails, key=lambda r: (r["bench"], r["variant"], r["label"])):
            tail = (r.get("stderr_tail") or "").replace("|", "\\|")
            L.append(f"| {r['bench']} | {r['variant']} | {r['label']} | {r['status']} | "
                     f"{r['exit']} | {tail} |")
    return "\n".join(L) + "\n"


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except Exception:
        pass
    a = parse_args(sys.argv[1:])
    sizes = CI if a.quick else PERF
    if not a.plot_only:
        if not (a.vanilla or a.mmtk):
            sys.exit("need --vanilla and/or --mmtk (or --plot-only)")
        npts = len(grid_points(a)) * len(a.benches)
        print("=" * 60)
        print(f"space-time sweep  sizes={'ci' if a.quick else 'perf'}  reps={a.reps}  "
              f"timeout={a.timeout:.0f}s  host={platform.node()}")
        print(f"vanilla={a.vanilla}  o={a.vanilla_o}  s={a.vanilla_s}  (+ default)")
        print(f"mmtk={a.mmtk}  plans={a.plans}  heaps={a.heaps}  nurseries={a.nurseries}"
              "  MMTK_THREADS=1")
        print(f"benches={a.benches}  -> {npts} points  json={a.json_path}")
        print("=" * 60)
        sweep(a, sizes)
    g = group(load_rows(a.json_path), set(a.benches), sizes)
    try:
        plot_all(a, g)
    except Exception as e:  # plotting is best-effort
        print(f"(plotting skipped: {e})", file=sys.stderr)
    md = summary_md(a, g)
    os.makedirs(a.graphs, exist_ok=True)
    with open(os.path.join(a.graphs, "SUMMARY.md"), "w") as f:
        f.write(md)
    print()
    print(md)


if __name__ == "__main__":
    main()
