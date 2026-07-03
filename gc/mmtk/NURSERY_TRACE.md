# Bespoke OCaml nursery trace — design & decision record

**Status:** design only, no code. Records the plan for closing the per-promoted-object
gap that the profiling in `NOTES.md` (2026-07-03) and `SCALABILITY.md` (culprit 4)
identified, so the effort/risk are on the table before any mmtk-core hot-path surgery.

**One-line decision for KC:** this is a *framework-generality-vs-specialization* research
choice (RQ7 at the per-object level), not a bug fix. The safe slice (trusted field loads)
is landed; the structural remainder below is a real mmtk-core rewrite whose ceiling is
quantified here. Decide whether the demonstration is worth the risk before starting.

---

## 1. The measured problem

GenImmix promotes at ~**89 ns/object** where stock OCaml's `oldify_one` does the equivalent
at ~**40 ns**. Decomposition of GenImmix nursery-GC work (binarytrees d=1, `sample`,
GC-worker frames only; `NOTES.md` 2026-07-03):

| bucket | share | reducible? |
|---|--:|---|
| scan (`scan_ocaml_object` / `FieldSlot` / `visit_slot`) | 38% | partly |
| SFT dispatch (`is_in_mmtk_spaces` / `get_checked` / `in_space`) | 20% | **yes** |
| side-metadata (mark / line / VO / forwarding bits) | 18% | **partly** |
| nursery/immix `trace_object` core | 8% | no |
| enqueue / work-packet | 8% | **yes** |
| forward CAS + memmove + `post_copy` (the actual copy) | 7% | no |

**77% is metadata/dispatch/scan machinery; 7% is the copy.** Stock does the scan+copy off
the object **header word** with no space-function-table (SFT) lookups and no side tables.

## 2. What is already landed (the free, safe slice)

Two binding-side slices are landed — the **whole SFT-dispatch bucket (20% → 0.0%)**:
1. **Trusted field loads** (commit `0170d2db60`): `FieldSlot::load` skips its `is_in_mmtk_spaces`
   re-check for heap field slots on STW plans.
2. **S1 classify range-check** (this record's commit): `FieldSlot::classify` — run on *every field*
   to filter OCaml foreign pointers — replaces `is_in_mmtk_spaces` with a `[heap_start, heap_end)`
   range compare on STW plans (§4 S1, binding-side).

Combined, median-5 GenImmix, sanity-clean, GC-count/checksum byte-identical: **binarytrees GC −15% /
wall −11%, kb GC −8%, chameneos GC −5%**; the SFT bucket is gone. Remaining nursery GC cost: scan
50%, side-metadata 23%, copy 9% — all needing the mmtk-core changes below, not binding work.

## 3. Current per-object path (what a bespoke trace would replace)

```
GenNurseryProcessEdges::process_slot(slot)
  slot.load()                         # FieldSlot: classify (SFT, partly killed) + read
  plan.trace_object_nursery(obj)
    nursery.in_space(obj)             # CopySpace membership (VMMap descriptor check)
    nursery.trace_object(PromoteToMature)
      forwarding-bits CAS             # side-metadata (claim the copy)
      alloc_copy() into mature Immix  # bump in a per-worker Immix block
      memmove                         # the actual copy (7%)
      post_copy()                     # VO bit + line mark  (side-metadata)
    enqueue promoted obj              # work-packet / node buffer
  ... later: scan_object(promoted)    # scan_ocaml_object -> per field: classify + load + recurse
```

The nursery's *output* — promoted objects sitting in mature Immix with correct VO-bit,
line-mark, and forwarding metadata — is consumed by: the mature Immix trace (full GCs),
the `sanity` re-tracer, weak/ephemeron/finaliser processing (they query forwarding state),
defrag, and `object_pinning`. **That coupling is the risk surface**, not the copy itself.

## 4. Proposed specializations (each independently landable)

Ranked by payoff/risk. A bespoke `OCamlNurseryProcessEdges` (new `ProcessEdgesWork` impl,
selected as GenImmix/GenCopy/StickyImmix `DefaultProcessEdges` for the nursery bucket) would
adopt some subset:

**S1 — young/old test by range, not `in_space`/SFT (low risk, ~part of 20%). [binding-side LANDED]**
The **classify-side** half is done (see §2): `FieldSlot::classify` uses a `[heap_start, heap_end)`
range compare instead of `is_in_mmtk_spaces` on STW plans, eliminating the SFT bucket. The remaining
**trace-side** `in_space` inside mmtk-core's `trace_object_nursery` is a separate, smaller lookup
(`CopySpace::in_space` — a VMMap descriptor check, not the SFT); replacing it needs mmtk-core work
and is low-value now that the binding SFT is gone. Original note follows: cache `[nursery_start,
nursery_end)` at GC start; replace the per-object `in_space` with a range compare. Correctness: LOS-young objects fall
*outside* the range — must still consult LOS (one extra range compare) before concluding
"mature, don't trace". Foreign pointers (atoms/code) also fall outside — a range compare
alone cannot filter them, so keep a cheap "in any heap chunk" guard (or exploit that atoms
live in one static region).

**S2 — Cheney inline scan, no work-packet enqueue (medium risk, ~8% + locality).**
Stock oldify is a Cheney copy: scan the to-space linearly, copying children as they are
found, no separate queue. MMTk's node-buffer + work-packets (8%) exist for parallelism and
load-balancing. A per-worker Cheney scan of that worker's own promotion region keeps
parallelism (each worker owns its to-space blocks via `GCWorkerCopyContext`) while dropping
the enqueue. Risk: interaction with work-stealing/termination detection; a worker that
finishes its region must still steal. This is the largest structural change.

**S3 — header-word forwarding/marking instead of side bits (high risk, part of 18%).**
Stock stores the forwarding pointer in the header and signals "forwarded" with `hd == 0`.
MMTk uses side forwarding-bits/mark-bit + forwarding pointer in the header. Moving the bits
in-header drops their side-metadata CAS but faces **two structural blockers**, both surfaced
by KC:

- **The spec is per-binding, not per-plan.** `LOCAL_FORWARDING_BITS_SPEC` / `LOCAL_MARK_BIT_SPEC`
  are single `const`s on the one `ObjectModel` — flipping them `in_header` applies to **every**
  plan we ship, including the *concurrent* ones (ConcurrentImmix/Bactrian/LXR). You cannot make
  it in-header for GenImmix and on-side for the concurrent plans. So S3 is not a GenImmix-local
  optimization; it changes the metadata layout for all plans at once.
- **Header-word write race with lazy tag updates.** The mark/forwarding bits would share the
  header word with OCaml's own *mutator-side* header mutations — chiefly multi-domain **lazy
  forcing**, which atomically CAS-updates the tag (`caml_obj_update_tag`: Unforced → Forcing →
  Forward), plus `Obj.set_tag` and the `Forward_tag` short-circuit. Under an STW plan the GC
  writes headers only at a safepoint with mutators stopped → **no race** (this is why an
  STW-only world could adopt in-header marking safely). But under a *concurrent* plan a GC
  worker sets a mark bit **while a mutator is CAS-forcing a lazy value in the same word** → a
  lost-update race. Side metadata avoids this by construction (separate word); that is *why*
  the current specs are `side_*`. Coexistence is *possible* — both sides must do atomic RMW on
  the full word with CAS-retry (OCaml's lazy forcing already CAS-loops for multi-domain safety;
  the GC's mark would need to become an atomic `fetch_or` so a racing tag-CAS re-reads and
  re-applies over the set bit) — but it is delicate and must be proven per header-mutating path.

Net: almost certainly not worth it before S1/S2, and if taken, it is a whole-binding
concurrency change (enumerate every header-mutating mutator path; make GC mark writes
retry-safe `fetch_or`; re-verify the concurrent plans), not a constant flip.

**S4 — batched cont-stack slots (low risk, targets chameneos specifically).**
When scanning a promoted continuation, trace its fiber-stack slots directly during the frame
walk instead of constructing a `FieldSlot` per slot and re-validating each. Keep a cheap
validity guard (stack slots can be stale — GH#15). Helps the effect-handler workload class
disproportionately (that is where the cont-scan machinery doubles the per-object cost).

## 5. Correctness obligations (any bespoke path MUST preserve)

1. **Infix redirection** — interior closure pointers trace/forward the *parent* (the existing
   `FieldSlot::classify` infix logic; a range test must not lose it).
2. **Forwarding-protocol interop** — whatever forwarding representation is used, `sanity`,
   weak/ephemeron/finaliser processing, the mature Immix trace, and defrag must all still read
   "is this forwarded / to where". S3 is the one that breaks this; S1/S2 keep MMTk's protocol.
3. **Foreign-pointer filter** — atoms (static zero-size blocks), code addresses, pre-MMTk
   objects must not be traced. A range test needs an explicit non-heap fallback.
4. **LOS-young** — large young objects live in LOS, are *traced not copied*; the fast path must
   route them to `los.trace_object`.
5. **VO-bit + line marks** — mature Immix's own sweep/defrag depends on the promoted objects
   carrying correct VO and line metadata; `post_copy` cannot simply be skipped.
6. **Pinning** (`object_pinning`) and **multi-worker** correctness (per-worker to-space).
7. **Header-word write races** (only if S3): GC mark/forwarding writes must not lose, or be lost
   to, mutator header mutations (lazy forcing `caml_obj_update_tag`, `Obj.set_tag`, `Forward_tag`
   short-circuit). STW plans: safe (mutators stopped at GC). Concurrent plans: need atomic
   `fetch_or` GC writes + the mutator's existing CAS-retry to coexist — proven per path. See §4 S3.
   Also add S3 to the `sanity`/checksum matrix run *under the concurrent plans while forcing lazy
   values*, the exact race window.

## 6. Expected ceiling (why it still won't reach 40 ns)

Aggressive S1+S2+S4 could recover ~half of SFT(20%) + enqueue(8%) + part of side-metadata,
≈ **89 → ~68 ns** (roughly −25% GC time, i.e. binarytrees wall toward −15%). Reaching stock's
40 ns needs S3 *and* dropping Immix's line/VO structure (which stock has no analogue of) —
at which point one has re-implemented `oldify_one` inside MMTk and the framework's value for
the nursery is the question, not the answer. **That gap is the RQ7 finding**: a plan-general
framework pays ~2× stock's per-object nursery cost; ~half is recoverable with specialization,
the rest is the structural price of SFT + side-metadata generality.

## 7. Validation plan (when/if built)

Per-specialization, in order: (a) mmtk `sanity` feature clean at 32–64 MiB across
GenImmix/Immix/StickyImmix/GenCopy; (b) byte-identical checksums vs the current build on the
full quick panel; (c) `par_binarytrees` d=1..8 correctness; (d) full testsuite × {GenImmix,
Immix}; (e) median-5 A/B for the claimed delta. The concurrent plans (ConcurrentImmix/
Bactrian/LXR) must be gated out (as trusted-field-loads already are) unless separately proven.

## 8. Cross-language compatibility: *where* a specialization lives decides it

KC's question — "if we use the OCaml header for marking, do we make the plan incompatible
with other languages?" — is the load-bearing architectural constraint, so it gates the whole
design. The answer turns entirely on **plan vs binding**:

- A **plan** (mmtk-core `src/plan/…`: GenImmix, Immix, …) is the collector algorithm and is
  meant to be language-agnostic — one plan serves OpenJDK, Ruby, Julia, V8, OCaml. Its value
  is precisely that portability.
- A **binding** (`VMBinding`: `ObjectModel`, `Scanning`, `Slot`, `ActivePlan`, `Collection`)
  is the language-specific layer. Plans reach the object's header/metadata **only** through
  `VM::VMObjectModel::…`, never by hardcoding a layout.

So the rule for each proposed specialization:

| specialization | lives in | breaks cross-language compat? |
|---|---|---|
| S1 range-check nursery membership | plan (generic) | **no** — helps every language; upstreamable |
| S2 Cheney inline scan | plan (generic) | **no** — a scanning strategy, language-blind; upstreamable |
| S3 header-word forwarding/marking | **binding `ObjectModel`** | **no, if done right** (see below) |
| `scan_ocaml_object` field walk | binding `Scanning` | already OCaml-only; plan never sees it |

**The header-marking worry, resolved.** You do *not* express "forwarding/mark bits in the
header" by forking a plan to read the OCaml header — that would bake OCaml into shared
collector code and make an OCaml-only plan (the incompatibility KC is worried about, and a
real architectural violation). You express it through the **ObjectModel metadata specs**,
which already exist for exactly this: `LOCAL_FORWARDING_POINTER_SPEC`,
`LOCAL_FORWARDING_BITS_SPEC`, `LOCAL_MARK_BIT_SPEC` each say *where* their bits go —
`in_header(offset)` **or** on-side — as a **per-binding choice**. The plan does
`VM::VMObjectModel::LOCAL_FORWARDING_BITS_SPEC.store(...)` and is oblivious to the answer.
Our binding **already** places the forwarding *pointer* in the header
(`LOCAL_FORWARDING_POINTER_SPEC::in_header(0)`); the forwarding *bits* and *mark bit* are
currently on-side (`LOCAL_FORWARDING_BITS_SPEC::side_first()`,
`LOCAL_MARK_BIT_SPEC::side_after(…)` in `binding/src/object_model.rs`). S3 is exactly flipping
those two constants to `::in_header(offset)` in OCaml's spare header bits — the old color bits,
which have no owner now that stock's mark/sweep GC is gone. It touches **only the OCaml
binding's ObjectModel**; Java/Ruby keep their own specs, the plan is unchanged, compat
preserved. **Bit-budget caveat:** OCaml's header has just 2 free color bits — enough for the
2-bit forwarding *state* **or** a separate mark bit, not obviously both — so a precise
accounting (can forwarding-state and mark-bit share, or does one stay on-side?) is itself part
of the S3 design, and is why S3 is ranked last.

**The honest caveat — portable *mechanism*, language-specific *benefit*.** The ObjectModel
indirection keeps the plan portable, but the *win* is OCaml-specific: it pays off only because
OCaml's header has spare bits and a uniform one-word header. A Ruby/Julia binding making the
same in-header choice might have no spare bits, or a non-uniform header, and would get less (or
would keep the bits on-side). That is the healthy MMTk split, and it reframes the RQ7 finding:
**MMTk gives you a portable mechanism; matching stock's speed means feeding it a
language-tuned *policy* through the binding — the framework stays general, the tuning does
not.** One cost survives even done-right: the spec is read through `VMObjectModel`, so unless
the specs are `const` enough for LLVM to fold the placement at each call site (they often are —
worth confirming), you pay a small indirection stock does not. That residual *is* part of the
framework tax and cannot be removed without forking the plan.

**Consequence for the ranking.** S1 and S2 are not "OCaml specialization" at all — they are
generic plan improvements that help every MMTk language and could be upstreamed; they carry no
compat risk. Only S3 is a per-language layout choice, and the ObjectModel abstraction already
lets us take it binding-locally without forking a plan. **Nothing in this design requires an
OCaml-only plan.** The moment a proposal would (hardcoding OCaml header reads into mmtk-core
`src/plan/`), reject it and push the same idea through `ObjectModel`/`Scanning` instead.

## 9. Recommendation

The **decomposition is the publishable result** on its own. The bespoke trace is worth
building only as a *demonstration* that the gap is closable (a paper figure), because plain
`Immix` already sidesteps the entire generational per-object tax on the workloads that hurt
(chameneos d=1 0.90 s, beats vanilla). If undertaken, do **S1 + S4 first** (low risk, no
forwarding-protocol change), measure, and stop there unless the figure needs S2. **S3 is still
worth exploring** — but as a scoped concurrency study (can header-word marking coexist with
OCaml's lazy-tag CAS via retry-safe atomics?), *not* as a quick constant flip; its blast radius
is all plans, so it is the last thing to touch and the one that most needs an isolated proof
before any panel measurement.
