# Research questions for `ocaml-mmtk`

`ocaml-mmtk` is OCaml 5.5 with **MMTk as its only garbage collector** — multicore, native + bytecode,
moving + generational, with the whole collector swappable at startup. The engineering bring-up (the
ROADMAP's M0–M7, and ~90% of M9) is essentially done. This document is about the *other* axis: what
**research** this platform enables. The ROADMAP correctness/perf tail is deferrable engineering; the
questions below are the reason to keep going.

This is a living document — sharpen, cut, and re-prioritise as the questions firm up.

---

## Thesis: `ocaml-mmtk` is a GC research platform, and OCaml fills a gap in it

MMTk's purpose (Blackburn et al., *Oil and Water?*, ICSE'04; *Rust as a language for high-performance
GC*, ISMM'16) is to implement many collectors once and compare them rigorously under one framework.
It has language bindings for **Java** (JikesRVM/OpenJDK), **Julia**, **CRuby**, **V8**, **.NET**.

There is **no functional, immutable-by-default, statically-typed, multicore, effect-handler language**
in that set. OCaml is a genuinely distinct point in the design space — and several of its properties
are exactly the ones GC research cares about *and* that the recent Julia/Ruby bindings found themselves
fighting:

| Property | Julia / Ruby (per the ISMM'25 reports) | OCaml |
|---|---|---|
| Root reporting | conservative / elision complexity | **precise** (`caml_do_roots`, `caml_scan_stack`) |
| Object motion | non-moving assumptions, friction with copying | **moving-friendly** by design (no-naked-pointers) |
| Mutation rate | high (imperative) | **low** (immutable-by-default) |
| Concurrency model | mostly single-threaded heaps | **multicore** (domains, mostly domain-local) |
| Control flow | — | **effect handlers / first-class continuations** |

So OCaml is not "another binding"; it is a **contrast case** that lets us ask whether a GC-friendly
*language design* changes the GC tradeoffs that the literature has measured on imperative languages.

---

## The current frontier (what to build on)

- **MMTk as a comparison vehicle** — Blackburn et al., *Oil and Water?* (ICSE'04); Immix (Blackburn &
  McKinley, PLDI'08); *Rust as a language for high-performance GC implementations* (ISMM'16).
- **Language-binding practitioner reports** — *Reconsidering Garbage Collection in Julia: A
  Practitioner Report* (Blackburn et al., ISMM'25) and *Reworking Memory Management in CRuby: A
  Practitioner Report* (Wang, Blackburn, Zhu, Valentine-House, ISMM'25). Both catalogue
  **language-specific impedance** between a runtime's assumptions and a GC framework's expectations;
  the Julia report's thesis is that *"the impedance ... cannot be eliminated through engineering alone"*
  — GC portability needs language-level design. OCaml is the natural, and contrasting, next entry.
- **Low-latency GC** — LXR (Zhao, Blackburn, McKinley, *Low-Latency, High-Throughput Garbage
  Collection*, PLDI'22): reference counting + Immix backup tracing, **sub-millisecond max pauses** with
  competitive throughput. Crucially, its effectiveness is **gated by write-barrier cost / mutation
  rate** — high-mutation workloads erode RC's latency advantage.
- **Parallelism / engineering abstractions** — *Work Packets* (Blackburn group, PACMPL/OOPSLA'25);
  *Memory Management on Mobile Devices* (Sareen, Blackburn, Hamouda, Gidra, ISMM'24).
- **Precise mark-region** — *Nofl: A Precise Immix* (2025) — relevant given OCaml's precise rooting.
- **Methodology** — *Myths and Realities* (Blackburn et al., SIGMETRICS'04); *Distilling the Real Cost
  of Production Garbage Collectors* (ISPASS'22); *Evaluating GC Performance Across Managed Language
  Runtimes* (ICSE'25); MemBalancer heap-sizing (ISMM'19) for heap-size-normalised comparison.
- **OCaml side** — the Multicore OCaml GC (Sivaramakrishnan et al., *Retrofitting Parallelism onto
  OCaml*, ICFP'20); the memory model (*Bounding Data Races in Space and Time*, PLDI'18); effect
  handlers (*Retrofitting Effect Handlers onto OCaml*, PLDI'21).

---

## Research questions

### RQ1 — Does OCaml's immutability make low-latency GC unusually effective? *(flagship)*

**Hook.** LXR's whole bet — and the bet of concurrent collectors generally — is throttled by
write-barrier / mutation cost. OCaml is immutable-by-default: mutation is rare, so the barrier cost
that erodes RC/concurrent collection elsewhere is *largely absent*.

**Hypothesis (falsifiable).** OCaml is a near-ideal target for low-pause GC: an RC-Immix/LXR-style
collector, or a concurrent-marking (SATB) plan, achieves sub-millisecond pauses at a *smaller*
throughput cost than the literature reports for imperative languages — and the size of that cost
**correlates with the program's mutation rate**.

**How to test.** Port LXR's RC (or wire ConcurrentImmix + an SATB barrier) into the binding —
OCaml's `caml_modify`/`caml_initialize` already have barrier hooks (wired in ROADMAP #3). Measure
pause-time + throughput vs OCaml's own STW GC, and *regress the gap against mutation rate* across
modules that vary in `ref`/mutable-array usage. The immutability claim lives or dies on that
correlation.

**Why it's research, not tuning.** It's a claim about a *language property* (mutability) predicting a
*GC design outcome* (low-latency feasibility) — generalisable beyond OCaml. **Venue:** PLDI/ISMM.
**Risk:** high (needs RC/concurrent plan + barrier), high upside. Serves the charter's
"reliable/trustworthy" via predictable latency.

### RQ2 — How does a multicore *functional* workload map onto the GC design space? *(characterization; lowest risk; precursor to RQ1)*

**The platform.** Finish M9, then ROADMAP #15 wires the rest of MMTk's plans cheaply — giving *one
language, one runtime, one set of workloads, N collectors* (Immix, GenImmix, StickyImmix, MarkSweep,
SemiSpace, MarkCompact, Compressor, ConcurrentImmix).

**Questions.** Which collector fits OCaml's profile — high allocation rate, small short-lived objects,
a strong generational hypothesis, low mutation — and *why*? How does the answer differ from the
DaCapo/Java-shaped conventional wisdom, and from the Julia/Ruby findings? Does OCaml's mostly
domain-local multicore heap change the calculus (per-domain vs shared collection)?

**Needs.** #8 → #15, plus a benchmark suite (Sandmark + the OCaml compiler itself + the CLBG suite
already in-repo). **Venue:** ISMM. **Risk:** low — first systematic GC comparison for a functional
multicore language; produces the data that *motivates and frames* RQ1.

### RQ3 — Effect handlers / fibers as a GC workload *(novel, narrower)*

OCaml 5's effect handlers allocate **fiber stacks** and **first-class continuations** on the heap;
capture/resume churns many small stack objects. No prior GC study targets this (effects are new and
near-unique to OCaml). **Questions:** how do continuations/fibers stress collectors; does
moving/compaction relieve fiber-stack fragmentation; what's the cost of scanning many small stacks
precisely? **Needs:** effect-heavy benchmarks + the platform. **Venue:** ISMM/OOPSLA.

### RQ4 — Practitioner report: retrofitting MMTk onto a *GC-friendly* language *(bank-it / experience; near-term)*

The Julia and CRuby reports established the format; OCaml is the **contrast**. Where those runtimes
fought conservative roots, non-moving assumptions, and undefined FFI GC-safe regions, OCaml was
*designed* with precise rooting, no-naked-pointers (moving-friendly), and explicit safepoints — so it
tests the converse of the Julia thesis: **what does a GC-friendly language design buy you when
retrofitting a third-party GC?**

Concrete findings already in hand to report:
- **Precise rooting reuse** — `caml_do_roots`/`caml_scan_stack` fed straight into MMTk's root factory.
- **No-naked-pointers ⇒ moving for free** — Immix/compaction worked without the Julia-style non-moving
  retrofit.
- **TLAB nursery-aliasing** — aliasing OCaml's inlined bump-pointer minor allocator onto an MMTk Immix
  block (the technique that made *native* code work).
- **The GC-safe-region impedance, concretely.** bug #3 (the MMTk stop-the-world barrier silently
  becoming a no-op because the blocking-section safe-stopped counter underflowed) is a precise,
  generalisable instance of exactly the "when can the GC safely run during native/foreign execution"
  problem the Julia report flags — here as a *coordination* bug between two STW mechanisms. bug #4
  (a gc_regs bucket leaked when an OOM raise unwound through the GC entry) is a second one.

**Needs:** ~nothing new — write up the bring-up + the bugs-as-findings. **Venue:** ISMM Practitioner
Report. **Risk:** lowest; establishes the platform's credibility and the ISMM relationship.

### RQ5 — Adjacent / optional

- **(a) Methodology** — heap-size-normalised, "real cost"–style cross-collector (and cross-runtime, cf.
  ICSE'25) comparison done right on a functional language.
- **(b) Mathematical guardrails (POPL/CPP angle, on-charter)** — a **mechanised specification + proof of
  the MMTk↔OCaml safepoint / GC-safe-region protocol** — the very invariant bug #3 violated. "Trustworthy
  software with mathematical guardrails" applied to the GC-runtime interface.
- **(c) AI-driven systems research (charter's AI-agents focus)** — this binding, and its subtle GC bugs
  (bug #3 STW underflow; bug #4 gc_regs, root-caused under `rr`), were largely AI-agent-driven. A data
  point on whether agents can do real systems/GC research, with honest failure modes (e.g. the
  stale-binary instrumentation that produced a *wrong* root cause before `rr` corrected it).

---

## What each question needs from the platform

- **Common prerequisite:** finish M9 (ROADMAP #8 — retire the always-false `Is_young` reservation +
  header/metadata reconciliation) so plan-swapping is clean. Then **#15 wires the non-Immix plans
  cheaply** — exactly the "do it immediately after vanilla removal" step.
- **RQ1:** + ConcurrentImmix and/or an LXR-style RC plan + the SATB/RC write barrier + a latency
  harness. *(This is the real research engineering.)*
- **RQ2 / RQ3:** + a benchmark suite — Sandmark, the compiler, CLBG (in-repo), and effect
  microbenchmarks for RQ3.
- **RQ4:** essentially the write-up; nothing new to build.

## Suggested sequencing

1. **RQ4 (practitioner report)** — bank the existing bring-up + bugs-as-findings. Lowest risk,
   near-term, and it opens the door at ISMM. Needs no new engineering.
2. **RQ2 (characterization)** — finish #8, wire #15, build the benchmark harness, run the comparison.
   Produces the data that motivates RQ1 and is a paper in its own right.
3. **RQ1 (flagship)** — the immutability ⇒ low-latency hypothesis. The high-upside PLDI/ISMM claim;
   start the RC/concurrent plan engineering once RQ2 has framed the question.
4. **RQ3 / RQ5** — opportunistic, as the platform and interest allow.

The point of identifying the question first: it tells us the *minimum* platform to build (for RQ4,
almost nothing; for RQ2, #8 → #15 + benchmarks; for RQ1, a new collector) — so we don't grind the
whole correctness/perf tail to 100% before we know which 20% the research actually needs.

---

## Sources / key reading

- LXR — Zhao, Blackburn, McKinley, *Low-Latency, High-Throughput Garbage Collection*, PLDI'22:
  https://www.steveblackburn.org/pubs/papers/lxr-pldi-2022.pdf
- *Reconsidering Garbage Collection in Julia: A Practitioner Report*, ISMM'25:
  https://www.steveblackburn.org/pubs/papers/julia-ismm-2025.pdf
- *Reworking Memory Management in CRuby: A Practitioner Report*, ISMM'25 (Wang, Blackburn, Zhu,
  Valentine-House).
- *Work Packets: A New Abstraction for GC Software Engineering...*, PACMPL/OOPSLA'25:
  https://dl.acm.org/doi/10.1145/3763139
- *Nofl: A Precise Immix*, 2025: https://arxiv.org/pdf/2503.16971
- Steve Blackburn — publications: https://www.steveblackburn.org/ ; dblp:
  https://dblp.org/pid/b/StephenMBlackburn.html
- ISMM: https://www.sigplan.org/Conferences/ISMM/
- Background: Immix (PLDI'08); *Oil and Water?* (ICSE'04); *Rust as a language for high-performance GC*
  (ISMM'16); *Myths and Realities* (SIGMETRICS'04); MemBalancer (ISMM'19).
