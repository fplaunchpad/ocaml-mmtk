# CLAUDE.md — working notes for agents & contributors on `mmtk-ocaml`

Auto-loaded context for Claude Code (and a quick orientation for humans). Keep it
short and current; deep rationale belongs in `gc/mmtk/NOTES.md`.

## What this repo is

- This repo **is** the OCaml 5.5 fork distributed as `mmtk-ocaml`: a normally-built
  OCaml whose garbage collector is **MMTk** (crates.io `mmtk` 0.32). It is *not* a
  separate binding bolted onto stock OCaml — we hack on and ship this tree.
- MMTk is **always-on** (milestone M9). Default plan is **Immix**. Both the bytecode
  interpreter and native code allocate through MMTk.
- Active branch: `m9-mmtk-only`. Remote: `mmtk` → `fplaunchpad/ocaml-mmtk` (public).

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

- **macOS builds fine** for editing and bytecode work. **`rr` reverse-debugging and
  the GC-correctness repros are Linux x86-64 only** — do that work on a Linux box.
  Host/path specifics for your build machine go in `CLAUDE.local.md` (gitignored),
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
  - Needs `ocamltest` + `testsuite/lib/testing.cma` built first. For a **bytecode-only**
    run (no native compilers in a plain tree), set `native_compiler = false` /
    `native_dynlink = false` in `ocamltest/ocamltest_config.ml` and rebuild the driver
    — else each test's native variant errors and masks the bytecode result. A fresh
    worktree needs `make world` (not `make all`) first — it has no `boot/ocamlrun`.

## GC plan & env knobs

- `MMTK_PLAN` = `Immix` (default) | `StickyImmix` | `GenImmix` | `MarkSweep` | `NoGC`.
  Native code requires an **Immix-family** plan (TLAB nursery-aliasing).
- `MMTK_HEAP_SIZE_MB` (fixed heap), `MMTK_THREADS` (GC worker count),
  `MMTK_VERBOSE=1` (prints GC stats at exit). MMTk is the only collector — no
  opt-out; benchmark against stock via a separate vanilla OCaml 5.5 opam switch.

## Gotchas (hard-won — don't rediscover these)

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
2. Edit on your dev machine; build/run/debug on the Linux + `rr` host.
3. Small, focused commits; keep `README.md`/`ROADMAP.md` in sync; no Claude in messages.
4. Record new design rationale and any new repro (with its `rr` trace path) in
   `gc/mmtk/NOTES.md`.
5. **Verify before claiming done**: build + (for GC changes) `sanity` at a small
   heap + a regression run on the default plan (Immix).
