(* Parallel port of sandmark multicore-numerical/binarytrees5_multicore.ml to
 * raw Domain.spawn (no Domainslib).
 *
 * The upstream multicore version splits, within each depth class, the [niter]
 * tree builds across domains (Domainslib async/await over i*niter/num_domains
 * .. (i+1)*niter/num_domains-1) and sums the per-domain checks. We do the same
 * with the stdlib-only parallel_for helper, accumulating each domain's partial
 * sum into a per-class total. Because every tree of depth d has the same check
 * value, the per-class total is the SAME however the niter builds are split
 * across domains, so the folded checksum is domain-count-INDEPENDENT — a
 * parallel-correctness self-check.
 *
 *   par_binarytrees DEPTH [DOMAINS]      DOMAINS also from env DOMAINS
 *                                        (defaults: DEPTH=10, DOMAINS=1) *)

let parallel_for lo hi body ndom =
  if ndom <= 1 then for i = lo to hi do body i done
  else begin
    let n = hi - lo + 1 in let chunk = (n + ndom - 1) / ndom in
    let ds = Array.init ndom (fun k ->
      let s = lo + k*chunk in let e = min hi (s+chunk-1) in
      Domain.spawn (fun () -> for i = s to e do body i done)) in
    Array.iter Domain.join ds
  end

type 'a tree = Empty | Node of 'a tree * 'a tree

let rec make d =
(* if d = 0 then Empty *)
  if d = 0 then Node(Empty, Empty)
  else let d = d - 1 in Node(make d, make d)

let rec check t =
  match t with
  | Empty -> 0
  | Node(l, r) -> 1 + check l + check r

let depth = try int_of_string Sys.argv.(1) with _ -> 10
let num_domains =
  try int_of_string Sys.argv.(2)
  with _ -> (try int_of_string (Sys.getenv "DOMAINS") with _ -> 1)
let num_domains = max 1 num_domains

let min_depth = 4
let max_depth = max (min_depth + 2) depth
let stretch_depth = max_depth + 1

let () =
  (* the stretch tree (built once, discarded) *)
  let _ = check (make stretch_depth) in
  ()

(* a long-lived tree kept alive across the whole run (a real mature live set) *)
let long_lived_tree = make max_depth

(* Build [niter] trees of depth [d] split across [num_domains], sum the checks.
   The total is independent of how the niter builds are distributed. *)
let depth_class d niter =
  let parts = Array.make num_domains 0 in
  parallel_for 0 (num_domains - 1) (fun ind ->
    let st = ind * niter / num_domains in
    let en = ((ind + 1) * niter / num_domains) - 1 in
    let c = ref 0 in
    for _ = st to en do c := !c + check (make d) done;
    parts.(ind) <- !c) num_domains;
  Array.fold_left (+) 0 parts

let () =
  let checksum = ref 0 in
  for i = 0 to ((max_depth - min_depth) / 2 + 1) - 1 do
    let d = min_depth + i * 2 in
    let niter = 1 lsl (max_depth - d + min_depth) in
    let total = depth_class d niter in
    checksum := (!checksum * 1000003 + d * 31 + niter + total) land 0x3FFFFFFF
  done;
  let ll_check = check long_lived_tree in
  Printf.printf "par_binarytrees depth=%d checksum=%d long_lived_check=%d\n"
    max_depth !checksum ll_check
