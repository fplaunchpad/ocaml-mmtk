(* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 * Based on the Troestler/Fernandez OCaml entry.
 *
 * Parallelism: the CLBG entry parallelises the per-depth work with Unix.fork.
 * Under MMTk, fork orphans the GC worker threads, so this port uses
 * Domain.spawn instead — one domain per depth class. Each domain allocates a
 * large number of short-lived trees, so this is a genuine multi-domain GC
 * stress test. Output order is preserved (spawn all, join in index order). *)

type 'a tree = Empty | Node of 'a tree * 'a tree

let rec make d = if d = 0 then Node (Empty, Empty) else let d = d - 1 in Node (make d, make d)
let rec check = function Empty -> 0 | Node (l, r) -> 1 + check l + check r

let min_depth = 4

let () =
  let max_depth =
    let n = try int_of_string (Array.get Sys.argv 1) with _ -> 10 in
    max (min_depth + 2) n
  in
  let stretch_depth = max_depth + 1 in
  Printf.printf "stretch tree of depth %i\t check: %i\n" stretch_depth (check (make stretch_depth));
  let long_lived_tree = make max_depth in
  let depths = Array.init ((max_depth - min_depth) / 2 + 1) (fun i -> min_depth + i * 2) in
  let worker d =
    let niter = 1 lsl (max_depth - d + min_depth) in
    let c = ref 0 in
    for _ = 1 to niter do c := !c + check (make d) done;
    (niter, !c)
  in
  let domains = Array.map (fun d -> Domain.spawn (fun () -> worker d)) depths in
  Array.iteri
    (fun i dom ->
       let (niter, c) = Domain.join dom in
       Printf.printf "%i\t trees of depth %i\t check: %i\n" niter depths.(i) c)
    domains;
  Printf.printf "long lived tree of depth %i\t check: %i\n" max_depth (check long_lived_tree)
