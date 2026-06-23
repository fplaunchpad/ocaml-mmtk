# Research questions for `ocaml-mmtk`

`ocaml-mmtk` is OCaml 5.5 with **MMTk as its only garbage collector** — multicore, native + bytecode,
moving + generational, with the whole collector swappable at startup. The engineering bring-up (the
ROADMAP's M0–M7, and ~90% of M9) is essentially done. This document is about the *other* axis: what
**research** this platform enables. The ROADMAP correctness/perf tail is deferrable engineering; the
questions below are the reason to keep going.

This is a living document — sharpen, cut, and re-prioritise as the questions firm up. Citations have
been verified against the source PDFs / dblp; a few that the v1 draft over-claimed have been
corrected in place (see the notes on LXR and the Julia report below).

---

## Thesis: `ocaml-mmtk` is a GC research platform, and OCaml fills a gap in it

MMTk's reason to exist is to implement many collectors *once* from shared components and compare them
rigorously on a common substrate (Blackburn, Cheng & McKinley, *Oil and Water?*, ICSE'04; Lin,
Blackburn, Hosking & Norrish, *Rust as a language for high-performance GC implementation*, ISMM'16).
It has language bindings for **Java** (JikesRVM / OpenJDK), **Julia**, **CRuby**, **V8**, and **.NET**.

There is **no functional, immutable-by-default, statically-typed, multicore, effect-handler language**
in that set. OCaml is a genuinely distinct point in the design space — and several of its properties
are exactly the ones GC research cares about *and* that the recent Julia/Ruby MMTk bindings found
themselves fighting:

| Property | Julia / CRuby (per the ISMM'25 reports) | OCaml |
|---|---|---|
| Root reporting | conservative; ambiguous/"dominated" roots need pinning | **precise** (`caml_do_roots`, `caml_scan_stack`) |
| Object motion | non-moving assumptions baked in; needed retrofitting | **moving-friendly** by design (no naked pointers) |
| Mutation | high (imperative); WB-unprotected objects | **low** (immutable-by-default; init writes need no barrier) |
| Concurrency model | mostly single-heap (GIL in CRuby) | **multicore** (domains, mostly domain-local) |
| Control flow | — | **effect handlers / first-class continuations** |

A precise nuance the v1 draft got slightly wrong, worth stating exactly because the audience will
notice: the Julia report does **not** claim impedance "cannot be eliminated through engineering
alone" — that phrase is not in the paper. What it says is that a non-moving collector is *"fundamentally
and unavoidably exposed to fragmentation and reduced locality"* (de Souza Amorim et al., ISMM'25), i.e.
the limitation is **inherent to the design**, not removable by tuning. The Julia and CRuby reports
*do* document, concretely, that those runtimes had non-moving assumptions, conservative/ambiguous
roots, and undefined GC-safe-region semantics baked in, and that retrofitting a moving/general GC
meant changing the runtime, not just the binding. OCaml is the **contrast case**: it was *designed*
(for its own multicore GC; Sivaramakrishnan et al., ICFP'20) with precise rooting, no naked pointers,
and explicit safepoints. So the platform lets us ask whether a GC-friendly *language design* actually
changes the GC tradeoffs the literature has measured on imperative languages — and what such a design
buys a third-party-GC retrofit.

---

## The baseline: OCaml's own multicore GC (the thing `ocaml-mmtk` replaces)

Every RQ here is measured against, and motivated by, the Multicore OCaml collector described in
**Sivaramakrishnan, Dolan, White, Jaffer, Kelly, Sahoo, Parimala, Dhiman & Madhavapeddy,
*Retrofitting Parallelism onto OCaml*, PACMPL 4(ICFP), Article 113, 2020.** It is essential context,
so state it precisely:

- **Three stated requirements.** *R1 feature compatibility* — a well-typed serial program stays
  well-typed and same-semantics on the parallel runtime; *R2 performance compatibility* — its
  performance profile and GC pauses don't regress; *R3 parallel scaling* — minimise pauses, then run
  as fast as the cores allow. These constraints, not algorithmic ambition, drove the design.
- **Hybrid structure.** Each domain has a **small (256 K-word default) thread-local bump-pointer
  minor heap**; minor collection **copies** survivors into a **shared major heap** collected by a
  **non-moving, mostly-concurrent, mark-and-sweep** collector modelled on VCGC (incremental, with an
  optional stop-the-world compaction phase).
- **Stop-the-world minor, on purpose.** They chose a **stop-the-world parallel** minor collector
  (`ParMinor`) over a concurrent one (`ConcMinor`) **specifically to avoid a read barrier** — *"stock
  OCaml does not use read barriers and the C API also works under this assumption"*; making reads into
  safepoints would force every C-API user to change their code, violating R1. Domains rendezvous via
  the interrupt mechanism + a barrier to agree a collection can start.
- **The mutation/immutability rationale — in the authors' own words.** *"Being a functional
  programming language, OCaml code usually exhibits a high rate of allocation with most objects being
  small and short-lived."* And, load-bearing for RQ1: *"Many objects in OCaml are immutable. For
  immutable objects, the initialising writes (the only ones) are done without barriers and reads
  require no barriers."* The major-heap **write barrier is SATB** (snapshot-at-the-beginning, à la
  Yuasa 1990): on overwrite it greys the old referent.
- **Headline numbers.** `ParMinor` is **3.5% slower** than stock sequential OCaml (geomean; `ConcMinor`
  4.9%, the gap being the read barrier), while using **61% less memory** (`ConcMinor` 54%).

That last set of facts is the frame for everything below: OCaml's designers reached for a generational,
mostly-domain-local, SATB, **read-barrier-free** design *because of* the language's allocation profile,
and explicitly leaned on immutability to elide barriers. `ocaml-mmtk` lets us test, with a different
collector family underneath the same language, whether that profile generalises into the GC-design
predictions the broader literature has only ever measured on imperative workloads.

---

## The current frontier (what to build on)

- **MMTk as a comparison vehicle** — *Oil and Water?* (Blackburn, Cheng & McKinley, ICSE'04); Immix
  (Blackburn & McKinley, PLDI'08); *Rust as a language for high-performance GC implementation* (Lin,
  Blackburn, Hosking & Norrish, ISMM'16). The methodology ancestor is *Myths and Realities* (Blackburn,
  Cheng & McKinley, SIGMETRICS'04): implement the canonical collectors in one framework so only the
  *policy* differs, then compare.
- **Language-binding practitioner reports** — *Reconsidering Garbage Collection in Julia* (de Souza
  Amorim, Lin, Blackburn, Netto, Baraldi, Daly, Hosking, Pamnany & Smith, ISMM'25) and *Reworking
  Memory Management in CRuby* (Wang, Blackburn, Zhu & Valentine-House, ISMM'25). Both catalogue
  **language-specific impedance** between a runtime's assumptions and a general GC framework, and both
  hit the same wall — conservative/ambiguous roots needing VO-bit pinning, non-moving assumptions, and
  ill-defined GC-safe regions for the FFI. OCaml is the natural, *contrasting* next entry.
- **Low-latency GC** — **LXR** (Zhao, Blackburn & McKinley, *Low-Latency, High-Throughput Garbage
  Collection*, PLDI'22; extended version arXiv:2210.17175): reference counting on a hierarchical Immix
  heap + occasional concurrent SATB backup tracing, no read barrier. *Correction to the v1 framing:*
  LXR does **not** state that its effectiveness is "gated by mutation rate." Its argument runs the
  other way — a **cheap write barrier (1.6% mutator overhead) and the *absence* of a read barrier are
  the enabling design choices**, justified by stores being ~an order of magnitude rarer than loads
  (≈4.3/µs vs 64.3/µs), which makes read barriers ~5× more expensive than a store-side remembering
  barrier. The honest, *defensible* version of the v1 claim is therefore: **LXR's whole design bets on
  store/mutation frequency being low enough to keep the write barrier cheap and skip the read barrier —
  and OCaml's allocation profile (low mutation, immutable-by-default) is exactly the regime that bet
  assumes.** Numbers: in a tight heap, 7.8× throughput and 10× better 99.99% tail latency vs Shenandoah;
  in a moderate heap, +4% throughput over G1 and +43% over Shenandoah.
- **The "how generational is this workload?" measure** — Dolan, *Lifetime Dispersion and Generational
  GC: An Intellectual Abstract*, ISMM'25 (DOI 10.1145/3735950.3735958). Introduces **lifetime
  dispersion** (a Gini-coefficient measure of how concentrated object lifetimes are) as a *composable*
  predictor of how much generational collection helps. This is the right instrument for RQ1/RQ2: it
  turns "this program mutates / ages a lot" from hand-waving into a measured covariate, and it is by a
  core OCaml-GC author.
- **Concurrent / low-pause lineage (the latency line the v1 draft under-cited).** SATB marking
  originates with **Yuasa, *Real-time garbage collection on general-purpose machines*, JSS 11(3), 1990**
  (deletion / pre-write barrier); the tricolor + insertion-barrier alternative is **Dijkstra, Lamport,
  Martin, Scholten & Steffens, CACM 1978**. Production concurrent compactors: **C4** (Tene, Iyengar &
  Wolf, ISMM'11) and its predecessor **the Pauseless GC algorithm** (Click, Tene & Wolf, VEE'05), both
  built on a self-healing **load-value (read) barrier**; **Shenandoah** (Flood, Kennke, Dinn, Haley &
  Westrelin, PPPJ'16), SATB marking + concurrent evacuation (Brooks pointers → later load-reference
  barriers); and **ZGC** (OpenJDK JEPs 333/377; generational JEP 439, JDK 21), colored-pointer load
  barriers. The recurring theme — and OCaml's relevance — is that every one of these pays a **read
  barrier** to compact concurrently, the exact cost OCaml's designers refused and LXR avoids.
- **Parallelism / engineering abstractions** — *Work Packets* (Zhao, Blackburn & McKinley, PACMPL
  9(OOPSLA2), Art. 361, 2025; DOI 10.1145/3763139). NUMA / multicore GC — *NumaGiC* (Gidra, Thomas,
  Sopena, Shapiro & Nguyen, ASPLOS'15) and the scalability study (Gidra et al., ASPLOS'13); *Memory
  Management on Mobile Devices* (Sareen, Blackburn, Hamouda & Gidra, ISMM'24). (Gidra is on the
  OCaml-MMTk effort — the NUMA/GC pedigree is in the building.)
- **Precise mark-region** — *Nofl: A Precise Immix* (Wingo, arXiv:2503.16971, 2025; preprint) — relevant
  given OCaml's precise rooting and small objects.
- **Methodology** — *Distilling the Real Cost of Production Garbage Collectors* (Cai, Blackburn, Bond &
  Maas, ISPASS'22) and its lower-bound-overhead method; *Evaluating Garbage Collection Performance
  Across Managed Language Runtimes* (Wang, Dou, Liang, Wang, Wang, Wei & Huang, ICSE'25, the cross-runtime
  GEAR methodology); heap-size normalisation via **MemBalancer** — note this is **Kirisame, Shenoy &
  Panchekha, *Optimal Heap Limits for Reducing Browser Memory Use*, PACMPL 6(OOPSLA2), Art. 160, 2022**,
  *not* an ISMM'19 paper (the v1 draft mis-dated it). For online heap sizing under control theory, White,
  Singer, Aitken & Jones, ISMM'13.
- **GC observability** — *Improving Garbage Collection Observability with Performance Tracing* (Huang,
  Blackburn & Cai, MPLR'23): built **on MMTk**, eBPF/LTTng tracepoints at near-zero overhead; the
  artifact lives in `mmtk-core/tools/tracing`. The classic ancestor is *GCspy* (Printezis & Jones,
  OOPSLA'02). This is a thin academic area and a real opening (see RQ5).
- **Formal / mechanised** — there is already **a mechanically verified GC for OCaml**: Shamsu, Kafle,
  Maroo, Nagar, Bhargavan & Sivaramakrishnan, *A Mechanically Verified Garbage Collector for OCaml*,
  JAR 69(2), Art. 11, 2025 — a STW mark-and-sweep collector verified in **F\*/Low\***, extracted to C via
  KaRaMeL, wired into the OCaml 4.14 runtime. **It verifies the collector algorithm, not the
  mutator↔collector coordination protocol** — which is exactly the gap RQ5b targets. Adjacent: McCreight,
  Shao, Lin & Li, *A General Framework for Certifying GCs and Their Mutators*, PLDI'07; Gammie, Hosking &
  Engelhardt, *Relaxing Safely: Verified On-the-Fly GC for x86-TSO*, PLDI'15; Zakowski et al., *Verifying
  a Concurrent GC Using a Rely-Guarantee Methodology*, ITP'17; the verified GC for CakeML (Sandberg
  Ericsson, Myreen & Åman Pohjola, ITP'17); CertiGC (Wang, Cao, Mohan & Hobor, OOPSLA'19); and the heap-
  space-bound separation logic IrisFit (Moine, Charguéraud & Pottier, TOPLAS'25).
- **OCaml side** — the memory model, *Bounding Data Races in Space and Time* (Dolan, Sivaramakrishnan &
  Madhavapeddy, PLDI'18); effect handlers, *Retrofitting Effect Handlers onto OCaml* (Sivaramakrishnan,
  Dolan, White, Kelly, Jaffer & Madhavapeddy, PLDI'21); `runtime_events` (Jaffer & Ferris, OCaml
  Workshop'22 — a talk, no proceedings).

---

## Research questions

### RQ1 — Does OCaml's immutability make read-barrier-free low-latency GC unusually effective? *(flagship)*

**Hook.** The concurrent-compaction lineage (C4, ZGC, Shenandoah) buys low pauses with a **read
barrier**, and OCaml's own designers refused that barrier (choosing `ParMinor`) precisely to keep the
C API and the language's no-read-barrier assumption intact. LXR shows you can get sub-millisecond-class
latency *without* a read barrier if the **write** barrier stays cheap — and write-barrier cost is
governed by mutation / store frequency. OCaml is immutable-by-default: in the ICFP'20 authors' own
words, *"the initialising writes (the only ones) are done without barriers and reads require no
barriers."* So OCaml is, on paper, the regime LXR's bet assumes — only more so.

**Hypothesis (falsifiable).** An RC-Immix/LXR-style collector (or a ConcurrentImmix + SATB plan) on
OCaml achieves sub-millisecond max pauses at a *smaller* throughput cost than the literature reports for
imperative/Java workloads — and, decisively, **the residual throughput cost rises with the program's
mutation rate / lifetime dispersion** (Dolan's Gini measure), not with allocation rate. If we instead
find the cost is dominated by something mutation-independent (allocation rate, tracing volume, root
scanning), the immutability story is *wrong* and RQ1 fails honestly.

**How to test.** Wire ConcurrentImmix + an SATB write barrier (ROADMAP #15's one high-value unwired
plan) and/or port LXR's RC barrier into the binding — OCaml's `caml_modify`/`caml_initialize` already
carry MMTk barrier hooks (ROADMAP Phase 2 #3; the native barrier is live). Measure pause-time +
throughput vs OCaml's own STW GC (a vanilla 5.5 opam switch) on Sandmark + the compiler, and **regress
the residual throughput gap against measured mutation rate and lifetime dispersion** across modules /
benchmarks that span the mutability spectrum (pure-functional ↔ `ref`/`Bytes`/mutable-array heavy). The
immutability claim lives or dies on that regression slope.

**The `lazy` corner — an open sub-question, and a deliberate break-it test.** RQ1's correctness rests on
the SATB deletion barrier covering *every* edge-deleting in-place mutation. OCaml is immutable-by-default,
but `lazy` is the sharp exception, and it is exactly where the SATB obligation concentrates: **forcing a
lazy mutates the suspension in place** — it overwrites the thunk + its captured environment with the result
(or installs a `Forward_tag`). Under concurrent marking that exposes two distinct failure modes. (i)
*Missed deletion barrier* — the thunk's captured environment may be reachable *only* through the
suspension; if forcing doesn't grey the old suspension, the concurrent marker loses it → collected-while-
referenced → dangling. (ii) *Force-vs-mark race* — the tag transitions `Lazy`/`Forcing` → `Forward`/result
while the marker scans the block, and multi-domain forcing layers OCaml's `Forcing`/`Undefined` protocol on
top; the binding's `scan_object` can mis-scan a half-updated lazy. Stock OCaml's mostly-concurrent major is
*itself* an SATB marker that already solved this — but bug-#3's work rewired `caml_modify` to MMTk's
*generational* barrier and M9 deleted the stock concurrent major, so **no SATB path is wired today**;
ConcurrentImmix must re-introduce it *and* prove the lazy-forcing path is covered. The research move is to
**implement it and then deliberately try to break it**: heavy multi-domain lazy forcing under concurrent
marking, at a small heap (frequent cycles) with MMTk `sanity` on, characterising each break (missed-barrier
dangling vs force-vs-mark race vs a genuine protocol gap) as fixable-or-open. The currently-disabled
`lazy/…force` testsuite test is the natural gate. This is a falsifiable probe of the thesis itself: if
OCaml's immutability is what makes concurrent GC cheap, lazy is the one place that bet is stressed — and
whether OCaml's *own* lazy/SATB protocol composes cleanly with a *third-party* concurrent marker (rather
than OCaml's bespoke one) is genuinely open. A clean composition strengthens RQ1; a fundamental conflict is
itself a publishable finding about language-runtime/GC-framework impedance, extending the RQ4 contrast story
into the *concurrent* regime.

**Preliminary result (2026-06-23 — bytecode, non-moving concurrent regime): SATB composes cleanly; no lazy
breakage found.** ConcurrentImmix (shipped in mmtk-core 0.32; `PlanSelector::ConcurrentImmix`) + an SATB
deletion barrier were implemented in ~82 lines, *reusing OCaml's existing slot-granularity barrier shape*
via mmtk-core's `memory_region_copy_pre` (the object-granularity `object_reference_write_pre` was the wrong
fit — `caml_modify` has no src object); `caml_modify` and `Array.fill` fire it **pre-store**, gated on the
concurrent plan; `needs_prepare_mutator` needed zero binding work. Under MMTk `sanity`, a multi-domain lazy
stressor (4 domains × 200k thunks × 6 rounds) gave a checksum *identical* to Immix and StickyImmix — no
forced result lost across the force-vs-mark window — reproduced twice. **Mechanism (why it's safe):** the
only edge-deletion in forcing (clearing the thunk's field, `Obj.set_field b 0 ()`) routes through
`caml_modify` → SATB-covered; the lazy *retag* only CASes the header tag (deletes no edge), and MMTk holds
mark state in *side-metadata*, so the tag flip cannot corrupt the marker's view. So OCaml's lazy/SATB
protocol *does* compose with a third-party concurrent marker — a positive RQ1 signal.

**Force-vs-relocate — now tested, also clean, and the reason why is the interesting part (2026-06-23).** The
first stressor only exercised force-vs-*mark* (`objects_copied` stayed 0 — lazy blocks are short-lived /
line-recycled). A second stressor (200k-cell persistent mature set; 60k forces/round × 40; thunks capturing
long-lived cells; results stored back to fragment mature Immix blocks) drove **defragging Full pauses with
`objects_copied > 0` *during the forcing window*** — genuinely hitting force-vs-relocate. Result:
ConcurrentImmix (80/96/128 MB), Immix, and StickyImmix all produce the *identical* checksum, EXIT 0, zero
sanity `Invalid reference`. **Mechanism (confirmed in mmtk-core source):** ConcurrentImmix's concurrent
phases are *strictly non-moving* (`ConcurrentTraceObjects::trace_object` asserts `object == new_object`);
relocation happens *only* on STW Full pauses with the SATB barrier *deactivated*, via the ordinary
moving-Immix trace + the binding's existing forwarding path. Liveness (SATB) and movement (forwarding) are
**disjoint, already-validated paths that never combine.** **The sharp implication for RQ1:** the truly hard
hazard — a lazy forced *during a concurrent relocation* — **does not exist for this collector, because it
never moves concurrently.** Concurrent *compaction* is exactly the design point that demands a *read*
barrier (C4 / ZGC / Shenandoah) — the cost OCaml's designers refused. So OCaml/MMTk gets cheap concurrent
*marking* (SATB, no read barrier) with lazy correctness for free, and the lazy-vs-concurrent-move hazard is
moot *precisely because* the design stays read-barrier-free. **A concurrent-compacting plan would reopen
it** — and that is exactly where RQ1's immutability bet would meet its real test (and where an LXR/RC-style
or read-barrier design becomes the interesting comparison). Net: the lazy question is **closed for
ConcurrentImmix** (clean, mechanism-explained) — in bytecode **and native**. Diagnosis showed the expected
native gaps were already closed (native reaches `caml_modify` via the out-of-line extcall; mmtk-core
eager-marks acquired lines, so allocate-black is automatic), and a real **atomics-SATB-ordering bug was found
+ fixed** (pointer-valued `Atomic.exchange`/`compare_and_set` greyed the slot *after* the store — a soundness
hole shared by bytecode + native). Still deferred (perf, not correctness): an UNLOG-bit barrier gate (every
concurrent-plan `caml_modify` currently buffers).

**Related work / what's genuinely new.** LXR established the read-barrier-free low-latency design *on
Java*; the concurrent compactors established the latency line *with* read barriers. **No one has tested
the language-property prediction** — that a low-mutation, immutable-by-default language makes this design
class cheaper — because there was no immutable-by-default language in a multi-collector framework to test
it on. Dolan'25 gives the covariate to make the test quantitative. That is the new contribution: a
*language-property → GC-design-outcome* law, not another collector.

**Venue:** PLDI / ISMM. **Risk:** high (needs a concurrent/RC plan + barrier + latency harness), high
upside. **Novelty: strong** — the *prediction-tested-across-the-mutation-spectrum* framing is new;
"RC works on OCaml" alone would not be. Serves the charter's reliability/trustworthiness via predictable
latency.

### RQ2 — How does a multicore *functional* workload map onto the GC design space? *(characterization; lowest research risk; precursor to RQ1)*

**The platform.** Finish M9, then ROADMAP #15 wires the rest of MMTk's plans cheaply — *one language,
one runtime, one set of workloads, N collectors* (Immix, GenImmix, StickyImmix, MarkSweep, SemiSpace,
GenCopy, MarkCompact, Compressor, ConcurrentImmix). This is *Myths and Realities* / *Distilling the Real
Cost* methodology applied to a language family those studies never covered.

**Questions.** Which collector fits OCaml's profile — high allocation rate, small short-lived objects, a
strong generational hypothesis, low mutation — and *why*? How does the answer differ from the DaCapo/Java
conventional wisdom and from the Julia/CRuby findings? Does OCaml's mostly-domain-local multicore heap
change the calculus (per-domain vs shared collection, NUMA placement à la NumaGiC)? Crucially, *quantify
the workload first*: report each benchmark's allocation rate, survival rate, **lifetime dispersion
(Dolan'25)**, and mutation rate, then show which plan wins where — so the comparison is heap-size-
normalised (MemBalancer) and explanatory, not a leaderboard.

**Related work / new.** The MMTk practitioner reports characterise the *retrofit*, not the
collector-choice landscape; *Distilling* and *Myths and Realities* characterise Java. The **first
systematic, mechanism-explained GC comparison for a functional multicore language** is new, and it
produces the data that *motivates and frames* RQ1.

**Needs.** #8 → #15, plus a benchmark suite (Sandmark — the OCaml community's own suite — + the compiler
itself + the in-repo CLBG suite). **Venue:** ISMM. **Risk:** low. **Novelty: solid** as a
characterization paper; honestly, *medium* in raw novelty (it is "the Blackburn-group methodology on a
new language"), but the functional / domain-local angle and the lifetime-dispersion framing lift it.

### RQ3 — Effect handlers / fibers as a GC workload *(novel, narrower — but get the mechanism right)*

OCaml 5's effect handlers represent continuations as **real call stacks (fibers)**. **Correction to the
v1 framing, and it matters for credibility:** per *Retrofitting Effect Handlers onto OCaml* (PLDI'21),
fiber **stacks are allocated on the C heap (`malloc`/`free`) with a free-list "stack cache," *not* on
the OCaml GC heap.** Only the small **first-class continuation object is a GC-heap object**, pointing at
the malloc'd fiber; fibers are **one-shot** (multi-shot would force a stack copy per resume); and because
OCaml emits no interior stack pointers, **relocating a fiber needs only two `fiber_info` fields fixed.**
So the GC's job is not "collect millions of heap-resident stacklets" — it is to **scan the roots inside
many live fiber stacks precisely** and keep the continuation→fiber linkage coherent under a moving
collector. (`ocaml-mmtk` already had to learn this: a real bug was the binding never scanning
continuation fiber stacks — see `NOTES.md`.)

**Hypothesis (falsifiable).** Effect-heavy / continuation-churning OCaml programs stress collectors
through **root-scanning cost over many live fiber stacks**, not through GC-heap pressure; therefore a
plan's ranking on effect-heavy workloads is predicted by its **root-scan / STW-pause behaviour**, and is
largely *insensitive* to the moving-vs-non-moving choice (since fibers aren't in the GC heap). If instead
moving plans clearly win or lose on effect benchmarks, the mechanism differs from this account and we
report *that*.

**Related work / new.** Farvardin & Reppy (*From Folklore to Fact*, PLDI'20) compare stack/continuation
*implementations* and their allocator/GC interaction, but **no GC paper takes a continuation-/fiber-heavy
heap as its workload** — confirmed gap. Effect handlers are new and near-unique to OCaml, so this is a
genuinely first-of-kind GC characterization. **Venue:** ISMM / OOPSLA. **Risk:** medium. **Novelty:
genuine but narrow** — the contribution is the measurement + the (possibly negative / surprising)
mechanism result, so it must be honest about what fibers do and don't put in the collected heap.

### RQ4 — Practitioner report: retrofitting MMTk onto a *GC-friendly* language *(bank-it / experience; near-term)*

The Julia and CRuby reports established the format; OCaml is the **contrast**. Where those runtimes fought
non-moving assumptions, conservative/ambiguous roots, and undefined GC-safe regions, OCaml was *designed*
(ICFP'20) with precise rooting, no naked pointers, and explicit safepoints. So this report tests the
converse of the Julia/CRuby experience: **what does a GC-friendly language design actually buy you when
retrofitting a third-party general-purpose GC — and where does it still bite?**

Concrete findings already in hand:
- **Precise rooting reuse** — `caml_do_roots`/`caml_scan_stack` fed straight into MMTk's root factory;
  no VO-bit conservative-pinning machinery (the centrepiece of *both* the Julia and CRuby reports) was
  needed.
- **No naked pointers ⇒ moving for free** — Immix/compaction worked without the Julia-style non-moving
  retrofit; relocation of ordinary blocks, closures, infix/interior pointers, and fiber linkages all
  worked once interior-pointer offsets were tracked in the slot.
- **TLAB nursery-aliasing** — aliasing OCaml's inlined bump-pointer minor allocator onto an MMTk Immix
  block; the technique that made *native* code work with no codegen change.
- **The GC-safe-region impedance, concretely — the converse-but-not-zero finding.** Even a
  safepoint-designed runtime still has interface hazards. **Bug #3**: MMTk's stop-the-world barrier
  silently became a no-op because OCaml's blocking-section "safe-stopped" counter *underflowed* (a
  `usize` wrap), so the GC scanned domains that had not actually stopped — tracing their live, mutating
  stacks. This is exactly the "when can the GC safely run during foreign/native execution" problem the
  Julia report flags, here as a precise *coordination* bug between two safepoint protocols. **Bug #4**: a
  `gc_regs` register-save bucket leaked when an `Out_of_memory` raise unwound *through* the GC entry path
  (RESTORE_ALL_REGS never ran), faulting the next collection. **Bug #2**: native unmarshalling allocated
  objects *outside* MMTk spaces (the MMTk alloc path was `#ifndef NATIVE_CODE`), so they went untraced and
  their referents were collected — the classic "every allocation path must reach the GC" lesson. All three
  are generalisable GC-runtime-interface findings, root-caused under `rr`.

**Honest framing.** The headline is not "OCaml was easy." It is **"a GC-friendly design eliminates the
*root/motion* impedance the Julia/CRuby reports spent most of their effort on, but the *coordination*
impedance (safepoints, GC-safe regions, every-alloc-path-traced) persists and bit us three times."** That
is a more interesting and more honest contribution than "it just worked."

**Needs:** ~nothing new — write up the bring-up + the bugs-as-findings. **Venue:** ISMM Practitioner
Report. **Risk:** lowest; establishes credibility and the ISMM relationship. **Novelty: as an experience
report, appropriate** — the value is the *contrast* with two existing reports, not a new technique.

### RQ5 — Adjacent / optional

- **(a) Methodology.** Heap-size-normalised (MemBalancer), lower-bound-overhead (*Distilling*),
  cross-runtime (GEAR, ICSE'25) comparison done right on a functional language — largely subsumed by RQ2,
  worth keeping only if a distinct methodological contribution emerges (e.g. lifetime dispersion as a
  *cross-language* normaliser).
- **(b) Mathematical guardrails — mechanised spec of the safepoint / GC-safe-region protocol (POPL / CPP /
  ITP; on-charter).** There is already a mechanically verified *collector* for OCaml (Shamsu et al., JAR'25,
  F\*/Low\*) — but it verifies the **collector algorithm in isolation, not the mutator↔collector
  coordination protocol**. And the broader literature confirms a gap: no published work takes a
  **safepoint / GC-safe-region / STW-handshake protocol as its primary verified artifact** (it appears only
  as a sub-component of full concurrent-collector proofs — Gammie et al.'15, McCreight et al.'07). Bug #3
  was a *violation of exactly this invariant*. A mechanised model + proof of the MMTk↔OCaml safepoint /
  GC-safe-region protocol — the contract that "no domain is scanned until it has actually stopped, and no
  GC runs while a domain holds raw heap pointers in a blocking section" — would be a clean, on-charter
  "trustworthy software with mathematical guardrails" result, anchored to a real bug. **Novelty: strong**
  (the gap is real and confirmed); **risk: high** (verification effort; needs the right abstraction of the
  protocol). **Venue:** CPP / ITP / POPL.
- **(c) GC observability for a multi-collector functional runtime.** GC telemetry is a thin academic area
  (essentially GCspy'02 and the MMTk-based Huang et al., MPLR'23). OCaml already ships `runtime_events`
  (zero-overhead per-domain ring buffers). Combining MMTk's tracing hooks with `runtime_events` to give
  *plan-agnostic, low-overhead* GC observability across the swappable plans — and using it to *explain*
  the RQ2 results — is a plausible MPLR-scale contribution. **Novelty: medium; risk: low.**
- **(d) AI-driven systems research (charter's AI-agents focus).** This binding, and its subtle GC bugs
  (bug #3 STW underflow; bug #4 `gc_regs`; bug #2 off-heap intern — all root-caused under `rr`), were
  largely AI-agent-driven. A candid data point on whether agents can do real systems / GC research, *with
  the failure modes named*: e.g. the stale-binary instrumentation pass that produced a confidently *wrong*
  root cause for bug #4 before `rr` corrected it (recorded in `NOTES.md`). Honest about both the wins and
  the "confirmed mechanism beats guessed mechanism" lesson. **Novelty: meta / experience; venue:** a
  workshop or an experience track.

---

### RQ6 — Is a read-barrier concurrent compactor worth evolving OCaml's C API for? *(inverts RQ1; a language-design × GC-design tradeoff)*

OCaml deliberately stayed **read-barrier-free**: ICFP'20 chose `ParMinor` over `ConcMinor` to keep the C
API's raw-read assumption intact (R1) — C stubs read OCaml values directly (`Field`, `Bytes_val`, …) with no
load barrier. RQ1 asks whether that constraint is *cheap* — whether the read-barrier-free low-latency designs
(ConcurrentImmix/SATB, LXR-style RC) are unusually effective on OCaml's immutable heap. **RQ6 asks the
converse: what does the constraint COST?** Is a read-barrier *concurrent compactor* (ZGC / C4 / Shenandoah-style
on-the-fly evacuation) enough better — on max pause, fragmentation, locality, tail latency — to justify
**evolving OCaml's C API** to tolerate it (a read barrier in the accessors, or object pinning across FFI calls
— the VO-bit machinery of RQ4)?

**Why it's real, and decision-relevant.** Every current plan is C-API-safe precisely because it moves only at
STW (FAQ Q6), which bounds pause time by the STW evacuation. A concurrent compactor removes that bound — the
headline win of the ZGC/C4 line — but at the price OCaml refused. Measuring the delta between the best
read-barrier-free plan (RQ1) and a read-barrier concurrent compactor, on OCaml workloads, answers whether the
ICFP'20 R1 choice leaves latency on the table: if the read-barrier-free plans already hit the target, the C
API stays as-is; if the concurrent compactor decisively wins, that is a concrete, measured argument (and a
roadmap) for evolving the FFI. It is the natural sequel to RQ1 (the read-barrier-free hypothesis) and RQ4
(the FFI/pinning impedance the practitioner reports hit), now in the concurrent-*moving* regime.

**Related / new.** The concurrent-compaction lineage measured the read-barrier cost *on Java*; nobody has
measured what a language that *deliberately avoided* the read barrier (for its C API) gives up by doing so —
or what evolving the API would buy. **Needs:** a read-barrier concurrent-moving plan in MMTk (not in 0.32 —
future / port) + a pinning-or-read-barrier C-API prototype. **Venue:** PLDI / ISMM. **Risk:** high (needs the
plan + an API change). **Novelty: strong.**

---

## What each question needs from the platform

- **Common prerequisite:** finish M9 (ROADMAP #8 — retire the always-false `Is_young` reservation +
  header/metadata reconciliation) so plan-swapping is clean. Then **#15 wires the non-Immix plans
  cheaply** — the "do it immediately after vanilla removal" step. Per the ROADMAP's own triage,
  `SemiSpace`/`PageProtect` are cheap, `MarkCompact`/`Compressor` medium, `GenCopy` ≈ GenImmix, and
  **`ConcurrentImmix` (a SATB write barrier) is the one high-effort, high-value plan** — and it is RQ1's
  enabler.
- **RQ1:** + ConcurrentImmix and/or an LXR-style RC plan + the SATB/RC write barrier + a latency harness +
  a mutation-rate / lifetime-dispersion instrument. *(This is the real research engineering.)*
- **RQ2 / RQ3:** + a benchmark suite — Sandmark, the compiler, CLBG (in-repo), effect microbenchmarks for
  RQ3 — plus a per-benchmark allocation / survival / dispersion / mutation profiler.
- **RQ4:** essentially the write-up; nothing new to build.
- **RQ5(b):** an extracted, abstractable model of the safepoint / blocking-section protocol (the bug-#3
  site) in a proof assistant — not platform engineering, but it depends on pinning the protocol down
  precisely.
- **RQ5(c):** wire MMTk's tracing hooks to `runtime_events`.

## Suggested sequencing

1. **RQ4 (practitioner report)** — bank the existing bring-up + bugs-as-findings, framed as the *contrast*
   with Julia/CRuby. Lowest risk, near-term, opens the ISMM door. No new engineering.
2. **RQ2 (characterization)** — finish #8, wire #15, build the benchmark + profiling harness, run the
   comparison. Produces the data that motivates RQ1 and is a paper in its own right.
3. **RQ1 (flagship)** — the immutability ⇒ read-barrier-free-low-latency hypothesis, tested across the
   mutation spectrum. The high-upside PLDI/ISMM claim; start the RC / ConcurrentImmix engineering once
   RQ2 has framed the question.
4. **RQ3 / RQ5** — opportunistic, as the platform and interest allow; RQ5(b) is the standout long-game
   (real, confirmed gap; on-charter).

The point of identifying the question first: it tells us the *minimum* platform to build (for RQ4, almost
nothing; for RQ2, #8 → #15 + benchmarks; for RQ1, a new collector) — so we don't grind the whole
correctness/perf tail to 100% before we know which 20% the research actually needs.

---

## Sources / key reading

*Citations verified against source PDFs / dblp. Author order and venue checked.*

**The baseline**
- Sivaramakrishnan, Dolan, White, Jaffer, Kelly, Sahoo, Parimala, Dhiman & Madhavapeddy,
  *Retrofitting Parallelism onto OCaml*, PACMPL 4(ICFP), Art. 113, 2020. DOI 10.1145/3408995.
  PDF: https://kcsrk.info/papers/retro-parallel_icfp_20.pdf · arXiv:2004.11663
- Dolan, Sivaramakrishnan & Madhavapeddy, *Bounding Data Races in Space and Time*, PLDI'18.
  DOI 10.1145/3192366.3192421.
- Sivaramakrishnan, Dolan, White, Kelly, Jaffer & Madhavapeddy, *Retrofitting Effect Handlers onto
  OCaml*, PLDI'21. DOI 10.1145/3453483.3454039. arXiv:2104.00250.

**MMTk framework + practitioner reports**
- Blackburn, Cheng & McKinley, *Oil and Water? High Performance GC in Java with MMTk*, ICSE'04.
  DOI 10.1109/ICSE.2004.1317436.
- Lin, Blackburn, Hosking & Norrish, *Rust as a language for high-performance GC implementation*,
  ISMM'16. DOI 10.1145/2926697.2926707.
- de Souza Amorim, Lin, Blackburn, Netto, Baraldi, Daly, Hosking, Pamnany & Smith, *Reconsidering
  Garbage Collection in Julia: A Practitioner Report*, ISMM'25. DOI 10.1145/3735950.3735957.
  PDF: https://www.steveblackburn.org/pubs/papers/julia-ismm-2025.pdf
- Wang, Blackburn, Zhu & Valentine-House, *Reworking Memory Management in CRuby: A Practitioner Report*,
  ISMM'25. DOI 10.1145/3735950.3735960. PDF: https://www.steveblackburn.org/pubs/papers/ruby-ismm-2025.pdf

**Collectors**
- Blackburn & McKinley, *Immix: A Mark-Region GC...*, PLDI'08. DOI 10.1145/1375581.1375586.
- Zhao, Blackburn & McKinley, *Low-Latency, High-Throughput Garbage Collection* (LXR), PLDI'22.
  DOI 10.1145/3519939.3523440. PDF: https://www.steveblackburn.org/pubs/papers/lxr-pldi-2022.pdf ·
  extended arXiv:2210.17175.
- Zhao, Blackburn & McKinley, *Work Packets...*, PACMPL 9(OOPSLA2), Art. 361, 2025. DOI 10.1145/3763139.
- Wingo, *Nofl: A Precise Immix*, arXiv:2503.16971, 2025 (preprint).
- Tene, Iyengar & Wolf, *C4: The Continuously Concurrent Compacting Collector*, ISMM'11.
  DOI 10.1145/1993478.1993491. · Click, Tene & Wolf, *The Pauseless GC Algorithm*, VEE'05.
  DOI 10.1145/1064979.1064988.
- Flood, Kennke, Dinn, Haley & Westrelin, *Shenandoah...*, PPPJ'16. DOI 10.1145/2972206.2972210.
- ZGC: OpenJDK JEP 333 (JDK 11), JEP 377 (JDK 15), JEP 439 generational (JDK 21).
- Yuasa, *Real-time garbage collection on general-purpose machines*, J. Systems and Software 11(3), 1990.
  DOI 10.1016/0164-1212(90)90084-Y (SATB origin). · Dijkstra, Lamport, Martin, Scholten & Steffens,
  *On-the-fly garbage collection...*, CACM 21(11), 1978. DOI 10.1145/359642.359655.

**NUMA / mobile / generational measure**
- Gidra, Thomas, Sopena, Shapiro & Nguyen, *NumaGiC...*, ASPLOS'15. DOI 10.1145/2694344.2694361. ·
  Gidra, Thomas, Sopena & Shapiro, *A study of the scalability of STW GCs on multicores*, ASPLOS'13.
  DOI 10.1145/2451116.2451142.
- Sareen, Blackburn, Hamouda & Gidra, *Memory Management on Mobile Devices*, ISMM'24.
  DOI 10.1145/3652024.3665510.
- Dolan, *Lifetime Dispersion and Generational GC: An Intellectual Abstract*, ISMM'25.
  DOI 10.1145/3735950.3735958.
- Lieberman & Hewitt, *A Real-Time GC Based on the Lifetimes of Objects*, CACM 26(6), 1983.
  DOI 10.1145/358141.358147. · Ungar, *Generation Scavenging...*, SDE 1, 1984. DOI 10.1145/800020.808261.
  · Appel, *Garbage Collection Can Be Faster Than Stack Allocation*, IPL 25(4), 1987.

**Functional-language GC**
- Marlow & Peyton Jones, *Multicore garbage collection with local heaps*, ISMM'11.
  DOI 10.1145/1993478.1993482. · Marlow, Harris, James & Peyton Jones, *Parallel generational-copying GC
  with a block-structured heap*, ISMM'08. DOI 10.1145/1375634.1375637.
- Sagonas & Wilhelmsson, *Efficient memory management for concurrent programs that use message passing*,
  Sci. Comput. Program. 62(2), 2006. DOI 10.1016/j.scico.2006.02.006 (Erlang per-process heaps).
- Farvardin & Reppy, *From Folklore to Fact: Comparing Implementations of Stacks and Continuations*,
  PLDI'20. DOI 10.1145/3385412.3385994.

**Methodology / heap sizing / observability**
- Blackburn, Cheng & McKinley, *Myths and Realities: The Performance Impact of GC*, SIGMETRICS'04.
  DOI 10.1145/1005686.1005693.
- Cai, Blackburn, Bond & Maas, *Distilling the Real Cost of Production Garbage Collectors*, ISPASS'22.
  DOI 10.1109/ISPASS55109.2022.00005. arXiv:2112.07880.
- Wang, Dou, Liang, Wang, Wang, Wei & Huang, *Evaluating GC Performance Across Managed Language Runtimes*
  (GEAR), ICSE'25. DOI 10.1109/ICSE55347.2025.00218.
- Kirisame, Shenoy & Panchekha, *Optimal Heap Limits for Reducing Browser Memory Use* (MemBalancer),
  PACMPL 6(OOPSLA2), Art. 160, 2022. DOI 10.1145/3563323. arXiv:2204.10455.
- Huang, Blackburn & Cai, *Improving Garbage Collection Observability with Performance Tracing*, MPLR'23.
  DOI 10.1145/3617651.3622986. · Printezis & Jones, *GCspy: An Adaptable Heap Visualisation Framework*,
  OOPSLA'02. DOI 10.1145/582419.582451.

**Formal / mechanised GC**
- Shamsu, Kafle, Maroo, Nagar, Bhargavan & Sivaramakrishnan, *A Mechanically Verified Garbage Collector
  for OCaml*, JAR 69(2), Art. 11, 2025. DOI 10.1007/s10817-025-09721-0 (collector verified, **not** the
  coordination protocol).
- McCreight, Shao, Lin & Li, *A General Framework for Certifying GCs and Their Mutators*, PLDI'07.
  DOI 10.1145/1250734.1250788. · Gammie, Hosking & Engelhardt, *Relaxing Safely: Verified On-the-Fly GC
  for x86-TSO*, PLDI'15. DOI 10.1145/2737924.2738006. · Zakowski, Cachera, Demange, Petri, Pichardie,
  Jagannathan & Vitek, *Verifying a Concurrent GC Using a Rely-Guarantee Methodology*, ITP'17.
  DOI 10.1007/978-3-319-66107-0_31.
- Sandberg Ericsson, Myreen & Åman Pohjola, *A Verified Generational GC for CakeML*, ITP'17.
  DOI 10.1007/978-3-319-66107-0_28. · Wang, Cao, Mohan & Hobor, *Certifying Graph-Manipulating C Programs
  via Localizations within Data Structures* (CertiGC), PACMPL 3(OOPSLA), Art. 171, 2019. DOI 10.1145/3360597.
- Moine, Charguéraud & Pottier, *Will It Fit? Verifying Heap Space Bounds... under Garbage Collection*
  (IrisFit), ACM TOPLAS 47(1), Art. 3, 2025. DOI 10.1145/3716312.

**Platform**
- ROADMAP.md (milestones, plan-wiring #15, the bugs as findings) · gc/mmtk/NOTES.md (dated root-cause
  notes, `rr` traces) · Sandmark: https://github.com/ocaml-bench/sandmark
- Steve Blackburn — publications: https://www.steveblackburn.org/ · ISMM: https://www.sigplan.org/Conferences/ISMM/
