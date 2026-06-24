(* Parallel port of sandmark multicore-numerical/matrix_multiplication_multicore.ml
 * to raw Domain.spawn (no Domainslib).
 *
 * Domainslib's Task.parallel_for over the outer row loop is replaced by the
 * stdlib-only parallel_for helper below; the pool is dropped. Random is seeded
 * (Random.init 42) so the operands are deterministic and identical to the
 * sequential matrix_multiplication bench, and the same integer checksum is
 * folded over the result — so par_matmul's checksum EQUALS the sequential
 * matmul checksum at the same size (a parallel-correctness self-check), and is
 * domain-count-INDEPENDENT (each res.(i).(j) is written by exactly one domain).
 *
 *   par_matmul SIZE [DOMAINS]            DOMAINS also from env DOMAINS
 *                                        (defaults: SIZE=1024, DOMAINS=1) *)

let parallel_for lo hi body ndom =
  if ndom <= 1 then for i = lo to hi do body i done
  else begin
    let n = hi - lo + 1 in let chunk = (n + ndom - 1) / ndom in
    let ds = Array.init ndom (fun k ->
      let s = lo + k*chunk in let e = min hi (s+chunk-1) in
      Domain.spawn (fun () -> for i = s to e do body i done)) in
    Array.iter Domain.join ds
  end

let size = try int_of_string Sys.argv.(1) with _ -> 1024
let num_domains =
  try int_of_string Sys.argv.(2)
  with _ -> (try int_of_string (Sys.getenv "DOMAINS") with _ -> 1)
let num_domains = max 1 num_domains

let matrix_multiply ndom res x y =
  let i_n = Array.length x in
  let j_n = Array.length y.(0) in
  let k_n = Array.length y in
  parallel_for 0 (i_n - 1) (fun i ->
    for j = 0 to j_n - 1 do
      let w = ref 0 in
      for k = 0 to k_n - 1 do
        w := !w + x.(i).(k) * y.(k).(j);
      done;
      res.(i).(j) <- !w
    done) ndom

let () =
  Random.init 42;
  let m1 = Array.init size (fun _ -> Array.init size (fun _ -> Random.int 100)) in
  let m2 = Array.init size (fun _ -> Array.init size (fun _ -> Random.int 100)) in
  let res = Array.make_matrix size size 0 in

  matrix_multiply num_domains res m1 m2;

  let acc = ref 0 in
  for i = 0 to size - 1 do
    for j = 0 to size - 1 do
      acc := !acc * 1000003 + res.(i).(j)
    done
  done;
  Printf.printf "matmul %d checksum %d\n" size !acc
