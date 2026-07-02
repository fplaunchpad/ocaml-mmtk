# CLAUDE.md — working notes for agents & contributors on `mmtk-ocaml`

Auto-loaded context for Claude Code (and a quick orientation for humans). Keep it
short and current; deep rationale belongs in `gc/mmtk/NOTES.md`.

## What this repo is

- This repo **is** the OCaml 5.5 fork distributed as `mmtk-ocaml`: a normally-built
  OCaml whose garbage collector is **MMTk** (crates.io `mmtk` 0.32). It is *not* a
  separate binding bolted onto stock OCaml — we hack on and ship this tree.
- MMTk is **always-on** (milestone M9). Default plan is **GenImmix** (copying nursery + Immix
  mature; the generational, stock-OCaml-faithful plan). Both the bytecode
  interpreter and native code allocate through MMTk.
- **Branches.** Remote: `mmtk` → `fplaunchpad/ocaml-mmtk` (public, the only remote).
  - **`5.5+mmtk`** is the fork's **default branch and sole mainline** — all work lands
    here. Do feature/risky work on short-lived topic branches off it, then merge (or
    fast-forward) into `5.5+mmtk`. (The former `m9-mmtk-only` integration branch was
    redundant — always kept identical to `5.5+mmtk`, and it doubled CI — so it was
    removed; consolidation is done.)
  - **Base = OCaml `5.5.0` final.** The fork was advanced rc1→5.5.0 by merging the upstream
    `5.5.0` tag (commit `f5238509d`, a 6-commit release-plumbing delta) — so the vanilla
    perf baseline must be released 5.5.0, not rc1. The `5.5.0` tag is the reviewable base
    for the fork's full diff. (`upstream-5.5.0-rc1` pins the historical fork point
    `4090d6db95`; don't build or commit on it.)
  - **`benchmarks`** is an **orphan branch** (no mainline history) holding the CLBG
    benchmark suite (`benchmarks/clbg/` — the Domain.spawn-ported Benchmarks Game
    programs + golden outputs). Kept off the mainline so the OCaml tree carries no
    benchmark/external code. The suite sits at the branch **root**;
    `.github/workflows/clbg.yml` checks it out (`ref: benchmarks`) into
    `benchmarks/clbg/` (its pre-move path, which the suite Makefile's
    `ROOT := $(abspath ../..)` assumes) to run the cross-plan correctness gate. Edit
    the suite on this branch; it does NOT carry the workflows, so suite-only changes
    don't auto-trigger CI (runtime/gc changes on the mainline still do).
  - **`trunk`** is **upstream OCaml's** dev branch (a leftover in the local clone) —
    NOT ours; never build/push it. (There is no `main` here.) Never push to upstream
    `ocaml/ocaml` regardless — only `mmtk` is ours.

## Hard rules (do not violate)

- **Never push to upstream `ocaml/ocaml`.** The only remote we push is `mmtk`
  (`fplaunchpad/ocaml-mmtk`).
- **`_references/` is read-only** study material (gitignored) — never edit or commit it.
- **No "Claude"/Anthropic/`Co-Authored-By: Claude` in commit messages.**
- **Keep `README.md`, `ROADMAP.md`, and `gc/mmtk/NOTES.md` mutually consistent with
  every commit** — the milestone table, the plan, and the notes must not contradict
  each other (e.g. a bug marked fixed in NOTES shouldn't read as an open blocker in
  ROADMAP/README).
- `AI.md` is the upstream OCaml policy on AI-assisted *contributions* (tool
  disclosure, CLA, "review every part yourself"). It matters if any of this is ever
  upstreamed; it is separate from the no-Claude-in-commits convention above.
- **`#N` is a GitHub reference.** In GitHub-facing text (issue/PR comments, PR bodies,
  commit messages) a bare `#<digits>` auto-links to GH issue/PR N. Our **internal**
  ROADMAP/task/bug item numbers are NOT GitHub issues — never write them as bare
  `#<digits>` in GH-facing text (it back-links the wrong issue/PR and spams it). Write
  "internal item N" or refer by name. Alphanumeric tags (`#3c`, `#G1`, `#R3`) are safe
  (GitHub only auto-links pure-digit `#N`). In repo docs (`*.md`) `#N` is not auto-linked,
  so the existing internal `#N` usage there is fine.
- **Close resolved GH issues; keep unresolved ones open.** If an issue is genuinely fixed,
  close it citing the fix commit; if only partially addressed, comment with status and
  leave it open. Don't leave resolved issues open or close unresolved ones.

## How to work (preferences)

- **Prefer agents.** Delegate diagnosis, code/search archaeology, and parallelizable work to
  sub-agents (and `Workflow` for multi-agent fan-out / adversarial verify). Keep the heavy
  **build + validate in the main loop** — sub-agents get reaped on rest and can't carry a long
  build (see `memory/subagent-builds-reaped-on-rest.md`). Pattern: agents produce diffs/findings;
  the main loop integrates, builds, and validates.
- **Study the upstream design before changing it — `git blame` → the PR → the discussion.** This
  tree is OCaml 5.5; most runtime machinery (frame descriptors, the STW/safepoint/backup-thread
  protocol, the minor/major GC, fibers) was designed by people who knew exactly what they were doing
  (Dolan, Scherer, Sivaramakrishnan, …) and the **upstream `ocaml/ocaml` GitHub PRs carry excellent
  design rationale and discussion**. Before replacing or "simplifying" any non-trivial concurrency or
  GC code: `git blame`/`git log --follow` the critical lines to find the introducing commit, get its
  **PR number** (`git show -s --format=%B <commit>`, usually `(#NNNNN)` or the merge), and read that
  PR thread (WebFetch `github.com/ocaml/ocaml/pull/NNNNN`). Find out *why* the current shape was
  chosen and whether the alternative you're about to build was already considered and rejected.
  (Example: the frametable all-domains STW looked like a prime excision target — but the history shows
  Dolan's original design was **GC-cycle RCU**, the multicore rework swapped it for STW, and that
  rationale tells you the right MMTk replacement is cycle-keyed RCU, not a new ad-hoc barrier.) The
  local clone has the upstream history + the `trunk`/`upstream-5.5.0` branches to diff against.
  Applies to **everything**, not just frame descriptors.

## Where things live (read these first each session)

- `ROADMAP.md` — the live plan and milestone status (M0–M9). Plan of record.
- `gc/mmtk/NOTES.md` — dated design notes, deferred investigations, and
  **known-failure repros** (including saved `rr` trace paths). Newest first.
- `README.md` — overview + milestone table.
- `fork-handoff.md` — original cold-start brief.
- Binding: `gc/mmtk/` — Cargo workspace; `common/` = version-independent value/layout
  code (header, slot, scanning, object_model), `binding/` = `VMBinding` impl + C ABI
  (`api.rs`) + collection/active_plan. Patched runtime: `runtime/`.

## Build / run

- **macOS (Apple Silicon) is a full local dev host** — native **and** bytecode build,
  `sanity`, and quick perf runs all work locally, and the M4 Pro beats the (8-year-old
  Xeon Gold) Linux boxes on single-threaded work, so do day-to-day **build / sanity /
  perf locally**. (A fresh tree just needs `./configure && make -j world.opt`; if native
  linking errors with undefined `mmtk_ocaml_*`, the tree is stale-configured — re-run
  `./configure`. macOS has no `setarch`/ASLR-mmap flake, so drop `setarch x86_64 -R`
  there.) **Switch to a Linux host (turing / godel / church) when** a failure or perf
  issue is tough to debug and needs the better tooling — **`rr` reverse-debugging is
  Linux-only**, plus `perf` / `bpftrace` profiling — or when you need **many cores
  (28–56)** for parallel / multicore-scalability runs or the heavy macro-benches
  campaign. Host/path specifics for each machine go in `CLAUDE.local.md` (gitignored),
  not here.
- Non-interactive shells need: `export PATH="$HOME/.cargo/bin:$HOME/local/bin:$PATH"`
  (cargo + autoconf 2.72).
- After any binding change, rebuild the staticlib then relink the runtime:
  ```
  (cd gc/mmtk && cargo build --release)        # -> target/release/libmmtk_ocaml.a
  rm -f runtime/ocamlrun runtime/ocamlrund && make -j runtime
  ```
- Full bytecode world: `make -j all`. Native: `make -j world.opt`. **`make -j<N>`
  (e.g. `-j$(nproc)`) is safe for the whole compiler/world build, not just the
  runtime** — use it everywhere to go faster. (Multi-process build steps —
  `make bootstrap`/`world.opt` — still need `setarch x86_64 -R` for the ASLR flake.)
- Regenerate `configure`: `tools/autogen` with **autoconf 2.72** (NOT plain
  autoconf / 2.71).
- **Testsuite**: run it with **`make -C testsuite parallel`** (fans tests across
  cores — much faster than a serial `make all` or a per-dir loop). Run under
  `setarch x86_64 -R` + a plan, with a **per-test timeout via `TIMEOUT=<seconds>`**:
  `setarch x86_64 -R env MMTK_PLAN=StickyImmix MMTK_HEAP_SIZE_MB=512 make -C testsuite parallel TIMEOUT=120`.
  - **The timeout matters under MMTk.** Several tests *hang* here (statmemprof, some
    finaliser/`c-api/alloc_async`, the multidomain GC-burn tests). `TIMEOUT` is
    ocamltest's own `-timeout` (default 600s); on expiry it `SIGKILL`s the test's
    whole process group (`setpgid` in `ocamltest/run_unix.c`) — verified to reap a
    hung test cleanly. Set it low (60–120s) for MMTk runs so hangs don't sit for 10 min.
  - **NEVER wrap the run in an outer `timeout N …`** (e.g. `timeout 120 make one …`).
    If the outer timeout fires before ocamltest's own, it kills `make`/`ocamltest`
    but NOT the test's `setpgid`'d group, which is then orphaned and runs **forever**
    (we leaked ~17 runaway `ocamlrun`/`*.byte` processes on turing this way). Use
    `TIMEOUT=` instead. If you must bound a run, also `setsid` it and kill the group.
  - Needs `ocamltest` + `testsuite/lib/testing.cma` built first. **For native test
    variants also build `make ocamltest.opt`** — it produces `testsuite/lib/testing.cmxa`
    + `testsuite/tools/codegen`; without them ~50 native variants spuriously fail on the
    missing native test lib (looks like real failures but is a test-infra gap). For a
    **bytecode-only** run (no native compilers in a plain tree), set
    `native_compiler = false` / `native_dynlink = false` in
    `ocamltest/ocamltest_config.ml` and rebuild the driver — else each test's native
    variant errors and masks the bytecode result. A fresh worktree needs `make world`
    (not `make all`) first — it has no `boot/ocamlrun`.

## GC plan & env knobs

- `MMTK_PLAN` = `GenImmix` (default) | `Immix` | `StickyImmix` | `ConcurrentImmix` | `Bactrian` | `GenCopy` | `SemiSpace` | `MarkSweep` | `NoGC`.
  Native code requires a bump/Immix-Default plan (TLAB nursery-aliasing): GenImmix/Immix/StickyImmix/GenCopy/SemiSpace/ConcurrentImmix/Bactrian.
  `Bactrian` (RQ7) = copying nursery + concurrently-marked STW-evacuated Immix mature + SATB —
  stock-ARCHITECTURE (not implementation: sweep is STW at FinalMark, marking on GC workers — NOTES 2026-07-02); debug knobs `BACTRIAN_TRACE=1`, `BACTRIAN_NO_CONCURRENT=1` (see NOTES 2026-07-02).
- `MMTK_HEAP_SIZE_MB` (pin a **fixed** heap; default is a **space-overhead** heap — after each
  full GC, `heap = live × 2.2` à la stock's `Gc.space_overhead`, clamped 16 MiB..physical-RAM, so
  RSS tracks the live set. Replaced MemBalancer, whose sqrt rule under-provisioned big-live-set
  programs — binarytrees was 3.5× slower; now 1.27×. Override via `MMTK_GC_TRIGGER`), `MMTK_NURSERY`
  (raw BYTES only, e.g. `Fixed:8388608` / `Bounded:2097152,8388608`; the `2m,8m` *suffix* form does **not**
  parse — it silently falls back to mmtk-core's default, ROADMAP #21 BUG B; default = bounded **2–64 MiB scaled by the live domain
  count** (N×2–N×64 MiB, stock-parity: stock gives each domain its own 2 MiB arena) — latched from the
  domain registry at spawn/termination, consumed lazily at the next trigger check; the space-overhead
  heap gets matching headroom for the scaled portion (SCALABILITY.md UPDATE 6: par_binarytrees d=8
  5.7× faster, copied objects ÷9). An explicit `MMTK_NURSERY` pin is never scaled; opt out of scaling
  the default with `MMTK_NURSERY_PER_DOMAIN=0`. Raised 8→64 MiB on 2026-06-25: the old 8 MiB forced
  100s–1000s of near-empty minor GCs on high-alloc workloads → GenImmix 1.3–3× slower single-domain;
  `Bounded` adapts down to fit small heaps), `MMTK_THREADS`
  (GC worker count; **default = nproc**, MMTk-core's own default. We tried *forcing 1* — it cut the
  single-domain minor-GC futex park/wake cost ~1.37× — but reverted it as a band-aid: worker count does
  **not** fix multi-domain throughput scaling (STW-bound, not thread-pool-bound), so pinning 1 only bought
  a single-domain win at the price of deviating from MMTk's default. Set `MMTK_THREADS=1` yourself for the
  lowest-overhead single-domain runs; see `api.rs` init + README knobs table), `MMTK_VERBOSE=1` (prints GC stats at exit; `heap=dynamic` when unpinned).
  MMTk is the only collector — no opt-out; benchmark against stock via a separate vanilla
  OCaml 5.5 opam switch (compare at **memory parity** — report RSS alongside wall time).

## Gotchas (hard-won — don't rediscover these)

- **macOS `make world.opt` spins forever in `make ocaml` (100% CPU, log stuck at
  `GEN runtime/primitives`).** After editing runtime C files, `runtime/primitives`'s
  move-if-change recipe (gen_primitives.sh deliberately does not update the file's
  mtime when the primitives table is unchanged) sends macOS's ancient GNU make 3.81
  into an infinite re-evaluation loop (Linux's make 4.x copes, so CI never sees it).
  Fix: kill the make and `touch runtime/primitives`, then rebuild — no `make clean`
  needed.
- **ASLR mmap flake.** MMTk can abort at startup with
  `failed to mmap meta memory: File exists`. Run under **`setarch x86_64 -R`**
  (disables ASLR). Needed for reliable repros and for multi-process builds
  (`make bootstrap`/`world.opt`). It is *not* a correctness bug.
- **`rr` reverse debugging.** `rr record <exe> …` then `rr replay`. Forward
  `continue` and `reverse-continue`-to-a-**breakpoint** work fine. **Hardware
  watchpoints trip an rr/gdb async "target is running" bug — avoid them** (use
  breakpoints; or software watchpoints / a binary search over GC count). **Record
  the binary directly, not `env VAR=… exe`** — otherwise gdb's executable is
  `/usr/bin/env` and OCaml symbols never load; export the vars instead. rr disables
  ASLR itself.
  - **Multi-domain timing races (e.g. GH#15) won't reproduce under plain `rr record`**
    — its serialized schedule hides the interleaving. **Chaos mode (`rr record -h`/
    `--chaos`) ABORTS under always-on MMTk** (`failed to mmap meta memory: Inappropriate
    ioctl for device` — MMTk's meta-mmap vs rr's seccomp layout). **Workaround (KC):
    `rr record -c <N>`** — a tunable scheduling number that records OK under MMTk —
    and **VARY N** (sweep e.g. 1000 / 10000 / 100000; the default is unknown, check
    `rr record --help`). Changing that number exposes different schedules and is a
    known way to surface rare interleaving bugs — then you get a *replayable* trace,
    far better than a one-shot live backtrace.
  - **Attaching `gdb -p` on turing fails** (`ptrace_scope=1`, no passwordless sudo) with
    EPERM / "Inappropriate ioctl for device", even on a same-session child. **Workaround:
    `LD_PRELOAD` a tiny shim whose constructor calls `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)`**
    — then any process can attach. A live multi-thread `gdb` backtrace + reading the
    contended `pthread_mutex_t.__data.__owner` fields gives TID-level lock owners, which
    deterministically nailed the GH#15 4-way lock cycle without rr at all.
- **Debugging the moving GC.** Enable mmtk's `sanity` feature in
  `gc/mmtk/Cargo.toml` (full-heap re-trace after each GC) and run at a **small heap**
  — small heaps force frequent + full GCs so the checker actually runs, and it flags
  dangling edges deterministically (`Invalid reference` panic). Revert the feature
  afterwards (it's slow). Caveat: `sanity` only catches *heap* inconsistencies — a
  missed **root** won't trip it (the mutator crashes while the heap looks "clean").
- **Canonical moving-GC repro.** Compile a heavy compiler source under a chosen
  plan/heap, e.g.
  ```
  MMTK_PLAN=StickyImmix MMTK_HEAP_SIZE_MB=64 setarch x86_64 -R \
    ./runtime/ocamlrun ./ocamlc <std build flags> -c parsing/parser.ml -o /tmp/p.cmo
  ```
  (`<std build flags>` = the usual `-nostdlib -I ./stdlib -I parsing -I utils …`
  set; grab it from a `make` compile line or `gc/mmtk/NOTES.md`.) **Exit codes:**
  `2` = ran OK (the warning-as-error on standalone `parser.ml` is expected — *not* a
  crash); `139` = SIGSEGV (a real bug); `124` = timeout.
- **`pkill -f "<pattern>"` can match your own ssh command line** and kill your
  session. Use `pkill -x <comm>` or kill by PID.
- The debug runtime (`ocamlrund`) needs the committed `caml_initialize` zero-init
  assertion relaxation to run under MMTk at all.

## Per-session workflow

1. Read `ROADMAP.md` + `gc/mmtk/NOTES.md` (plan + known issues/repros) before starting.
2. Build / run / `sanity` / quick-perf **locally on macOS** (M4 Pro is fastest for
   single-threaded); switch to a Linux host (turing / godel / church) for `rr`,
   `perf`/`bpftrace`, or many-core (parallel / scalability / macro-benches) runs.
3. Small, focused commits; keep `README.md`/`ROADMAP.md` in sync; no Claude in messages.
4. Record new design rationale and any new repro (with its `rr` trace path) in
   `gc/mmtk/NOTES.md`.
5. **Verify before claiming done**: build + (for GC changes) `sanity` at a small
   heap + a regression run on the default plan (GenImmix).
