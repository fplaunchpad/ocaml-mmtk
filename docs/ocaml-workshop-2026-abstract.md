# ocaml-mmtk: OCaml on the Memory Management Toolkit as a testbed for functional-language GC

> ⚠️ **NOT submission-ready text.** The OCaml Workshop 2026 CFP states: *"Proposals
> largely written by LLMs are not acceptable and will be desk-rejected"* (LLM use for
> grammar only is fine). This file is a **content scaffold** to write *from*, in your own
> words — do not submit this prose. Deadline **July 1 AoE**; submit at
> https://types-hotcrp.paris.inria.fr/ocaml26/

**Authors:** _[TODO: KC as lead + collaborators]_

---

Functional languages share a memory profile: data is immutable by default, programs
allocate a high volume of small, short-lived objects, and — because almost all writes
are initialising writes that need no barrier — the *mutation rate* is low. Garbage-
collection theory predicts this profile should favour a particular design: low-latency
collectors that avoid a **read barrier** and instead pay only a cheap **write barrier**
(reference-counting Immix / LXR, or concurrent marking with a snapshot-at-the-beginning
barrier). That write barrier is cheap precisely when stores are rare. The prediction has
never been tested directly, because no immutable-by-default language existed inside a
framework that can swap collectors and compare them on equal terms.

We built one. **`ocaml-mmtk`** replaces OCaml 5.5's garbage collector with **MMTk**, the
Memory Management Toolkit: we wrote the MMTk bindings, MMTk is now OCaml's *only*
collector for both bytecode and native code, and the standard MMTk plans — NoGC,
MarkSweep, SemiSpace, Immix, GenImmix, GenCopy, StickyImmix, and a concurrent marker —
all work and are selectable at startup. It self-hosts: the compiler builds itself on an
MMTk-managed heap. This makes OCaml the first functional, immutable-by-default, multicore
language available as a common substrate for comparing collectors — and a natural
contrast to the recent Julia and CRuby MMTk reports, whose runtimes had to fight
conservative roots and non-moving assumptions that OCaml, by design, does not have.

Early measurements support the hypothesis. With a concurrent marker and a snapshot write
barrier, the **write barrier is essentially free** on OCaml workloads (under 0.1% of
execution on the most write-heavy benchmark), while moving marking off the pause **cut the
maximum stop-the-world pause by 3–4×**. Separately, MMTk eagerly zeroes memory that OCaml
then immediately initialises; removing that redundant work — safe because of OCaml's own
allocation discipline — **recovered 15–22% on allocation-heavy programs** with no effect
on pause times. (These are preliminary, single-configuration numbers; the rigorous
heap-size-normalised comparison against stock OCaml is in progress.)

The talk presents the platform and this first evidence, and frames the question we can now
answer quantitatively: does the residual cost of read-barrier-free collection scale with a
program's **mutation rate** — as the immutability argument predicts — rather than its
allocation rate? If it does, the design generalises beyond OCaml to the functional-language
family (Haskell, Erlang) that shares the same profile. We will also outline the next steps:
a reference-counting (LXR-style) plan, and a collector that mirrors OCaml's own generational
design inside MMTk for an apples-to-apples comparison.

---

### Selected references

1. Sivaramakrishnan et al. *Retrofitting Parallelism onto OCaml.* ICFP 2020.
2. Zhao, Blackburn & McKinley. *Low-Latency, High-Throughput Garbage Collection* (LXR). PLDI 2022.
3. Lin, Blackburn, Hosking & Norrish. *Rust as a Language for High-Performance GC Implementation.* ISMM 2016.
4. de Souza Amorim et al. *Reconsidering Garbage Collection in Julia: A Practitioner Report.* ISMM 2025.
5. Wang, Blackburn, Zhu & Valentine-House. *Reworking Memory Management in CRuby: A Practitioner Report.* ISMM 2025.
6. Dolan. *Lifetime Dispersion and Generational GC: An Intellectual Abstract.* ISMM 2025.
