(* Port of sandmark multicore-numerical/LU_decomposition.ml (sequential).
 *
 * Changes from upstream: seed Random for determinism (Random.init 42 before
 * building the matrix), drop the _l/_u reconstruction, and instead fold an
 * integer checksum over the LU result via the low bits of each float (avoids
 * floating-point text-format flake) and print one stable line.
 * argv[1] = mat_size. *)

let mat_size = try int_of_string Sys.argv.(1) with _ -> 1200

module SquareMatrix = struct
  let create f : float array =
    let fa = Array.create_float (mat_size * mat_size) in
    for i = 0 to mat_size * mat_size - 1 do
      fa.(i) <- f (i / mat_size) (i mod mat_size)
    done;
    fa

  let get (m : float array) r c = m.(r * mat_size + c)
  let set (m : float array) r c v = m.(r * mat_size + c) <- v
  let copy = Array.copy
end

open SquareMatrix

let lup (a0 : float array) =
  let a = copy a0 in
  for k = 0 to (mat_size - 2) do
    for row = k + 1 to (mat_size - 1) do
        let factor = get a row k /. get a k k in
        for col = k + 1 to mat_size-1 do
            set a row col (get a row col -. factor *. (get a k col))
        done;
        set a row k factor
    done
  done ;
  a

let () =
  Random.init 42;
  let a = create (fun _ _ -> (Random.float 100.0)+.1.0) in
  let lu = lup a in
  let acc = ref 0 in
  for i = 0 to mat_size - 1 do
    for j = 0 to mat_size - 1 do
      acc := !acc * 31
             + (Int64.to_int
                  (Int64.logand (Int64.bits_of_float (get lu i j)) 0xFFFFFFL))
    done
  done;
  Printf.printf "lu %d checksum %d\n" mat_size !acc
