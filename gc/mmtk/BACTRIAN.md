# Bactrian vs stock OCaml 5 — what is matched, what is not

`MMTK_PLAN=Bactrian` is the RQ7 vehicle: an MMTk plan built to match **stock
OCaml 5's collector architecture** so that Bactrian-vs-vanilla measures MMTk
framework/implementation cost rather than collector-design difference. This doc
is the honest axis-by-axis comparison — Bactrian is **architecture-matched, not
implementation-matched**, and the residual deltas below are exactly the terms
left in the RQ7 comparison. (Design rationale and bring-up history: NOTES
2026-07-02. Numbers: README "Performance (quick panel)".)

## TL;DR

| axis | stock OCaml 5.x | Bactrian | matched? |
|---|---|---|---|
| generational, copying minor | per-domain minor heaps, emptied wholesale at STW rendezvous | shared CopySpace nursery, per-domain TLABs, emptied wholesale at STW pause | **yes** (shape) |
| minor-GC execution | inline Cheney copy on the mutator domains | MMTk STW handshake + work packets + GC workers | no — the measured ~4x per-minor-GC floor (NOTES 2026-06-24) |
| write barrier | deletion barrier (darken old value, marking-gated) + minor remset; no read barrier | slot-granular SATB (marking-gated, no dedup bit, young-filtered) + region remset; no read barrier | **yes** (semantics; remset granularity differs, below) |
| major mark | incremental **slices on the mutator domains**, paced by allocated words | **concurrent on GC worker threads**, racing the mutators | no — mostly-concurrent both, different executor + pacing |
| major cycle STW | tiny phase-flip sections riding the domain barrier (+ STW minors) | InitialMark rides a minor pause; **FinalMark = minor GC + remark + weak/finaliser processing + full mature sweep** | no — cycle-end pause categorically bigger |
| sweep | incremental/concurrent (each domain sweeps its pools in slices) | **STW at FinalMark** (Immix sweep packets inside the pause) | **no — the biggest divergence** |
| moving | non-moving major; STW compaction only on rare explicit `Gc.compact` | non-moving during cycles; STW Immix defrag only at `Full` (user GC / emergency) | ~yes (analogous) |
| pacing | major work budgeted by allocated words (`space_overhead`) | cycle starts on mature pressure (+120% growth over post-cycle baseline) or every 8 minors (GH#5) | no — pressure-driven, not allocation-driven |
| weak/ephemeron/finaliser | processed as the cycle reaches them, per phase | young: every minor pause; mature: judged only at FinalMark/Full over complete marks | ~yes (timing detail in NOTES) |

**Consequence for RQ7 reads:** the measured parity (within ~6% of vanilla on
7/8 sequential benches) was achieved **while still paying an STW sweep that
vanilla does not pay** — so closing the two "no" rows can only improve it, and
whatever gap remains after that is the true framework floor.

## The details

### Heap structure

Stock: per-domain minor arenas; a shared, size-class **pool-based** major heap
plus large allocations. Bactrian: one shared **CopySpace** nursery handed to
domains as TLABs (native code bump-allocates in it directly; bytecode allocates
through the binding); an **Immix** mature space (lines/blocks rather than
size-class pools); MMTk's LOS for >=16 KB objects (stock has no separate large
space — large objects go straight to the major heap in both, in effect).

### Barriers

Both have *no read barrier*, an SATB/deletion write barrier active only while
marking, no barrier on initializing writes (`caml_initialize`), and a
remembered set for mature-to-young pointers. Two fidelity notes:

- Bactrian's SATB half is deliberately **slot-granular with no dedup bit and a
  young-value filter** — literally stock's `caml_darken(old)` shape — rather
  than ConcurrentImmix's per-object unlog-bit protocol (which would conflict
  with the generational barrier's ownership of that bit).
- The remset half is **coarser than stock's**: stock records a slot only when
  the *stored value* is young; the fork's region barrier records every mutated
  mature slot regardless of value. A value-filtered remset is a known cheap
  improvement (listed below).

### The major cycle

```
stock:    [STW flip] ->  mark slices on mutators (allocation-paced)
          ... minors interleave ...        [STW flip] -> sweep slices on mutators
Bactrian: [InitialMark = minor GC + snapshot seeding]
          -> GC workers mark concurrently (minors interleave; SATB catches deletions)
          -> [FinalMark = minor GC + remark + weak/finalisers + STW MATURE SWEEP]
```

Bactrian anchors every cycle transition on a minor collection (stock's phase
changes also ride STW sections that empty the minor heaps — this part is
faithful and is also what makes it sound for the concurrent marker to skip
young objects entirely; see NOTES for the soundness argument and the three
bring-up bugs). The remark at FinalMark makes marking complete regardless of
concurrent coverage — the standard SATB final-remark structure.

`Gc.major`/`full_major`/`compact` map to a STW `Full` (defrag-capable —
the analogue of stock's rare STW compaction); the GH#5 mature-pressure/cadence
trigger starts a *concurrent cycle* instead, and a completed cycle counts in
`Gc.major_collections`, as stock counts cycles.

### Pacing (and a concrete consequence)

Stock budgets major work by **allocated words**, so a major cycle always makes
progress under allocation, regardless of heap headroom. Bactrian (like the
other MMTk plans here) collects on **memory pressure**: at a big pinned heap a
program can legitimately finish with *zero* collections. That is why
`callback/test_gc_alarm` is disabled `[semantic-timing]`: ~1 GB of LOS-direct
churn never fills a 4 GB heap, so no cycle, no alarm (it passes at 512 MB;
alarm delivery itself works whenever a GC happens).

### Memory footprint

Measured on macOS/M4 (see README): an **empty program** costs ~2 MiB under
vanilla vs a fixed, program-independent startup floor of ~26 MiB under
GenImmix/Bactrian (42 Immix, 46 ConcurrentImmix, 74 LXR) — side-metadata
tables mapped at init plus initial chunk commits; the plan deltas are their
extra metadata tables. On alloc-heavy programs the premium adds the live x 2.2
dynamic-heap headroom and Immix block slack; Bactrian additionally holds
**mid-cycle floating garbage** until FinalMark's sweep — benign sequentially
(RSS == GenImmix's on the panel), but under multi-domain anti-scaling the
cycles stretch and it compounds (par_binarytrees at 8 domains ballooned to
~1.7 GB).

## Closing the gap (RQ7 next steps)

Ranked by the 2026-07-02 turing quantification (SCALABILITY.md UPDATE 4):

1. ~~**Allocation-paced cycle trigger**~~ — **DONE (2026-07-02, `33ae0009f8`;
   SCALABILITY.md UPDATE 5).** Was the measured root cause of the multi-domain
   gap: the mature-pressure/cadence trigger converted domain-scaled minor
   traffic into domain-scaled whole-heap cycles (spectralnorm d1/8/24: 71 ->
   328 -> 807 completed cycles) that stock — allocated-words-paced with a
   generous space_overhead — never runs (ZERO majors on every measured cell),
   and domain *termination* forced one exhaustive full GC per spawn. Landed:
   promotion-paced pressure floor `max(32 MiB, nursery)`, per-domain cadence
   (8 x ndomains), and minor (not exhaustive) termination collections — which
   exposed and fixed GH issue 3 (remset buffers lost at mutator deregister).
   After: spectralnorm d24 fulls 807 -> 39; binarytrees d8/d24 wall -22/-25%;
   Bactrian is no longer permanently mid-cycle and its 8-domain RSS fell
   1689 -> 504 MiB. (`test_gc_alarm` remains disabled: at large dynamic heaps
   cycles are still legitimately rare.)
2. **Mutator-paced mark slices** — let mutators drain bounded amounts of the
   Concurrent bucket at poll points (the inert `caml_major_collection_slice`
   hook), matching stock's executor and pacing model, freeing GC-worker cores
   and cutting the worker/mutator context-switch churn.
3. **Concurrent/lazy sweep** — move the FinalMark mature sweep off the pause
   (lazy line sweeping / sweep-on-allocation), matching stock's incremental
   sweep and shrinking the one categorically-bigger pause.
4. **Value-filtered remset** — only record mature slots that receive young
   values, matching stock's remset traffic.

## Running it

`MMTK_PLAN=Bactrian` (bytecode + native; dynamic heap fine). Debug knobs:
`BACTRIAN_TRACE=1` (pause + marking-traffic counters on stderr),
`BACTRIAN_NO_CONCURRENT=1` (degrade cycle requests to STW Full — the
GenImmix-equivalent control for isolating the concurrency term).
