(* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 *
 * Contributed by Sebastien Loisel
 * Cleanup by Troestler Christophe
 * Modified by Mauricio Fernandez
 *
 * Parallel port of sandmark multicore-numerical/spectralnorm2_multicore.ml to
 * raw Domain.spawn (no Domainslib). Domainslib's Task.parallel_for in
 * eval_A_times_u / eval_At_times_u is replaced by the stdlib-only parallel_for
 * helper below; the pool is dropped. The upstream multicore version omits the
 * print, so the missing result line is added back.
 *
 *   par_spectralnorm SIZE [DOMAINS]      DOMAINS also from env DOMAINS
 *                                        (defaults: SIZE=2000, DOMAINS=1)
 *
 * Output is domain-count-INDEPENDENT (each row v.(i) is computed from the whole
 * of u, never from other rows of v in the same pass), so DOMAINS=1 and DOMAINS=4
 * print the identical float line — a parallel-correctness self-check. *)

let parallel_for lo hi body ndom =
  if ndom <= 1 then for i = lo to hi do body i done
  else begin
    let n = hi - lo + 1 in let chunk = (n + ndom - 1) / ndom in
    let ds = Array.init ndom (fun k ->
      let s = lo + k*chunk in let e = min hi (s+chunk-1) in
      Domain.spawn (fun () -> for i = s to e do body i done)) in
    Array.iter Domain.join ds
  end

let n = try int_of_string Sys.argv.(1) with _ -> 2000
let num_domains =
  try int_of_string Sys.argv.(2)
  with _ -> (try int_of_string (Sys.getenv "DOMAINS") with _ -> 1)
let num_domains = max 1 num_domains

let eval_A i j = 1. /. float((i+j)*(i+j+1)/2+i+1)

let eval_A_times_u u v =
  let n = Array.length v - 1 in
  parallel_for 0 n (fun i ->
    let vi = ref 0. in
    for j = 0 to n do vi := !vi +. eval_A i j *. u.(j) done;
    v.(i) <- !vi) num_domains

let eval_At_times_u u v =
  let n = Array.length v - 1 in
  parallel_for 0 n (fun i ->
    let vi = ref 0. in
    for j = 0 to n do vi := !vi +. eval_A j i *. u.(j) done;
    v.(i) <- !vi) num_domains

let eval_AtA_times_u u v =
  let w = Array.make (Array.length u) 0.0 in
  eval_A_times_u u w; eval_At_times_u w v

let () =
  let u = Array.make n 1.0  and  v = Array.make n 0.0 in
  for _i = 0 to 9 do
    eval_AtA_times_u u v; eval_AtA_times_u v u
  done;

  let vv = ref 0.0  and  vBv = ref 0.0 in
  for i=0 to n-1 do
    vv := !vv +. v.(i) *. v.(i);
    vBv := !vBv +. u.(i) *. v.(i)
  done;
  Printf.printf "%0.9f\n" (sqrt(!vBv /. !vv))
