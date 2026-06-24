(* Port of sandmark multicore-numerical/matrix_multiplication.ml (sequential).
 *
 * Changes from upstream: seed Random for determinism (Random.init 42 before
 * building the operands), drop print_matrix, and instead fold an integer
 * checksum over the result matrix and print one stable line. argv[1] = size. *)

let size = try int_of_string Sys.argv.(1) with _ -> 1024

let matrix_multiply res x y =
  let i_n = Array.length x in
  let j_n = Array.length y.(0) in
  let k_n = Array.length y in

  for i = 0 to i_n - 1 do
    for j = 0 to j_n - 1 do
      let w = ref 0 in
      for k = 0 to k_n - 1 do
        w := !w + x.(i).(k) * y.(k).(j);
      done;
      res.(i).(j) <- !w
    done
  done

let () =
  Random.init 42;
  let m1 = Array.init size (fun _ -> Array.init size (fun _ -> Random.int 100)) in
  let m2 = Array.init size (fun _ -> Array.init size (fun _ -> Random.int 100)) in
  let res = Array.make_matrix size size 0 in

  matrix_multiply res m1 m2;

  let acc = ref 0 in
  for i = 0 to size - 1 do
    for j = 0 to size - 1 do
      acc := !acc * 1000003 + res.(i).(j)
    done
  done;
  Printf.printf "matmul %d checksum %d\n" size !acc
