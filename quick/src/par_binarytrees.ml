(* par_binarytrees — parallel binary-trees (GC axis: parallel allocation + a
 * real live set + cross-domain STW coordination).
 *
 * Based on the CLBG binarytrees kernel (make/check), but restructured so the
 * TOTAL work is fixed and partitioned across DOMAINS worker domains, so:
 *   - DOMAINS=1 vs k measures parallel speedup on alloc-heavy work that also
 *     keeps a live set (the long-lived tree + in-flight trees), exercising STW
 *     coordination across domains, and
 *   - the checksum is DOMAINS-INDEPENDENT: the set of (depth, iteration) tasks
 *     and their contributions are fixed; only their assignment to domains
 *     changes. So 1 vs 4 domains must produce identical output -> self-check.
 *
 *   par_binarytrees DEPTH [DOMAINS]    DOMAINS also from env DOMAINS
 *                                      (defaults: DEPTH=16, DOMAINS=1)
 *
 * Total allocation is dominated by the depth classes 4,6,...,DEPTH, each built
 * 2^(DEPTH-d+4) times — the standard CLBG schedule — distributed round-robin
 * across the domains so each domain does a comparable mix of cheap (deep, few)
 * and expensive (shallow, many) classes. *)

type tree = Empty | Node of tree * tree

let rec make d = if d = 0 then Node (Empty, Empty) else let d = d - 1 in Node (make d, make d)
let rec check = function Empty -> 0 | Node (l, r) -> 1 + check l + check r

let min_depth = 4

(* One depth class: build [niter] trees of depth [d], sum their checks.
   Contribution depends only on (d, niter), not on which domain runs it. *)
let depth_class d niter =
  let c = ref 0 in
  for _ = 1 to niter do c := !c + check (make d) done;
  (d, niter, !c)

let () =
  let max_depth =
    let n = try int_of_string Sys.argv.(1) with _ -> 16 in
    max (min_depth + 2) n
  in
  let domains =
    try int_of_string Sys.argv.(2)
    with _ -> (try int_of_string (Sys.getenv "DOMAINS") with _ -> 1)
  in
  let domains = max 1 domains in
  (* a long-lived tree kept alive across the whole run (a real mature live set) *)
  let long_lived = make max_depth in
  (* the fixed task list: one per depth class 4,6,...,max_depth *)
  let depths = Array.init ((max_depth - min_depth) / 2 + 1) (fun i -> min_depth + i * 2) in
  let tasks =
    Array.map (fun d -> (d, 1 lsl (max_depth - d + min_depth))) depths
  in
  (* round-robin tasks to domains; each domain runs its assigned classes *)
  let assigned = Array.make domains [] in
  Array.iteri (fun i (d, niter) ->
    let dom = i mod domains in
    assigned.(dom) <- (d, niter) :: assigned.(dom)) tasks;
  let spawned =
    Array.init domains (fun dom ->
      let my = assigned.(dom) in
      Domain.spawn (fun () -> List.map (fun (d, niter) -> depth_class d niter) my))
  in
  (* gather all class results, sum into a single domain-independent checksum *)
  let checksum = ref 0 in
  Array.iter (fun dom ->
    List.iter (fun (d, niter, c) ->
      checksum := (!checksum + d * 1000003 + niter * 31 + c) land 0x3FFFFFFF)
      (Domain.join dom)) spawned;
  let ll_check = check long_lived in
  Printf.printf "par_binarytrees depth=%d domains=%d checksum=%d long_lived_check=%d\n"
    max_depth domains !checksum ll_check
