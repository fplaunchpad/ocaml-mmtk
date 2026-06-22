(* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 * Based on the Paolo Ribeca OCaml entry.
 *
 * Parallelism: the CLBG entry forks workers over horizontal row bands. This
 * port uses Domain.spawn over contiguous row bands instead. Each worker
 * returns its band of the PBM bitmap as a string; bands are concatenated in
 * order, so the output is identical regardless of worker count. *)

let niter = 50
let limit = 4.0

let worker w h lo hi =
  let buf = Buffer.create (((w + 7) / 8) * (hi - lo)) in
  let byte = ref 0 and nbits = ref 0 in
  for y = lo to hi - 1 do
    let ci = 2.0 *. float y /. float h -. 1.0 in
    for x = 0 to w - 1 do
      let cr = 2.0 *. float x /. float w -. 1.5 in
      let zr = ref 0.0 and zi = ref 0.0 and tr = ref 0.0 and ti = ref 0.0 and n = ref 0 in
      while !n < niter && !tr +. !ti <= limit do
        zi := 2.0 *. !zr *. !zi +. ci;
        zr := !tr -. !ti +. cr;
        tr := !zr *. !zr;
        ti := !zi *. !zi;
        incr n
      done;
      byte := (!byte lsl 1) lor (if !tr +. !ti <= limit then 1 else 0);
      incr nbits;
      if !nbits = 8 then (Buffer.add_char buf (Char.chr !byte); byte := 0; nbits := 0)
    done;
    if !nbits > 0 then begin
      Buffer.add_char buf (Char.chr (!byte lsl (8 - !nbits)));
      byte := 0; nbits := 0
    end
  done;
  Buffer.contents buf

let () =
  let w = int_of_string (Array.get Sys.argv 1) in
  let h = w in
  let nworkers = max 1 (Domain.recommended_domain_count ()) in
  let band i = i * h / nworkers in
  Printf.printf "P4\n%d %d\n" w h;
  let domains =
    Array.init nworkers (fun i -> Domain.spawn (fun () -> worker w h (band i) (band (i + 1))))
  in
  Array.iter (fun dom -> print_string (Domain.join dom)) domains
