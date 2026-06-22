(* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 * Based on the Paolo Ribeca OCaml entry (after the Oleg Mazurov Java version).
 *
 * Parallelism: the CLBG entry spawns worker *processes* over contiguous
 * permutation-index ranges. This port uses Domain.spawn over the same ranges.
 * Each domain owns an independent Perm state (setup at its range start), so
 * there is no shared mutable state; the checksum (sum) and maxflips (max) are
 * order-independent reductions. *)

module Perm = struct
  type t = { p : int array; pp : int array; c : int array }

  let facts =
    let n = 20 in
    let res = Array.make (n + 1) 1 in
    for i = 1 to n do res.(i) <- i * res.(i - 1) done;
    res

  let setup n idx =
    let res = { p = Array.init n (fun i -> i); pp = Array.make n 1; c = Array.make n 1 } in
    let idx = ref idx in
    for i = n - 1 downto 0 do
      let d = !idx / facts.(i) in
      res.c.(i) <- d;
      idx := !idx mod facts.(i);
      Array.blit res.p 0 res.pp 0 (i + 1);
      for j = 0 to i do
        res.p.(j) <- if j + d <= i then res.pp.(j + d) else res.pp.(j + d - i - 1)
      done
    done;
    res

  let next { p; c; _ } =
    let f = ref p.(1) in
    p.(1) <- p.(0); p.(0) <- !f;
    let i = ref 1 in
    while (let aug = c.(!i) + 1 in c.(!i) <- aug; aug > !i) do
      c.(!i) <- 0; incr i;
      let n = p.(1) in
      p.(0) <- n;
      for j = 1 to !i - 1 do p.(j) <- p.(j + 1) done;
      p.(!i) <- !f; f := n
    done

  let count { p; pp; _ } =
    let f = ref p.(0) and res = ref 1 in
    if p.(!f) <> 0 then begin
      let len = Array.length p in
      for i = 0 to len - 1 do pp.(i) <- p.(i) done;
      while pp.(!f) <> 0 do
        incr res;
        let lo = ref 1 and hi = ref (!f - 1) in
        while !lo < !hi do
          let t = pp.(!lo) in pp.(!lo) <- pp.(!hi); pp.(!hi) <- t; incr lo; decr hi
        done;
        let ff = !f in
        let t = pp.(ff) in pp.(ff) <- ff; f := t
      done
    end;
    !res
end

let nworkers = max 1 (Domain.recommended_domain_count ())

let () =
  let n = int_of_string (Array.get Sys.argv 1) in
  let total = Perm.facts.(n) in
  let bound i = i * total / nworkers in
  let worker w =
    let lo = bound w and hi = bound (w + 1) in
    let p = Perm.setup n lo in
    let csum = ref 0 and maxf = ref 0 in
    for idx = lo to hi - 1 do
      let f = Perm.count p in
      csum := !csum + (if idx land 1 = 0 then f else - f);
      if f > !maxf then maxf := f;
      (* advance only within this chunk; never step past the chunk's last
         permutation (stepping past the global final permutation overflows). *)
      if idx < hi - 1 then Perm.next p
    done;
    (!csum, !maxf)
  in
  let domains = Array.init nworkers (fun w -> Domain.spawn (fun () -> worker w)) in
  let csum = ref 0 and maxf = ref 0 in
  Array.iter
    (fun dom -> let (c, m) = Domain.join dom in csum := !csum + c; if m > !maxf then maxf := m)
    domains;
  Printf.printf "%d\nPfannkuchen(%d) = %d\n" !csum n !maxf
