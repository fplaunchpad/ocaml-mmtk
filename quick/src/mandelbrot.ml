(*
 * The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 *
 * Contributed by Paolo Ribeca
 *
 * (Very loosely based on previous version Ocaml #3,
 *  which had been contributed by
 *   Christophe TROESTLER
 *  and enhanced by
 *   Christian Szegedy and Yaron Minsky)
 *
 * Port of sandmark benchmarksgame/mandelbrot6.ml. argv[1] = w (= h).
 * The upstream program emits a binary P4 PGM bitmap; this port instead folds an
 * integer checksum over the exact byte stream it would have emitted and prints a
 * single stable line, so the golden is plain text (and independent of binary I/O).
 *)

let niter = 50
let limit = 4.

let () =
  let w = int_of_string (Array.get Sys.argv 1) in
  let h = w in
  let fw = float w /. 2. and fh = float h /. 2. in
  (* checksum folded over every byte the upstream would output_byte *)
  let acc = ref 0 in
  let emit byte = acc := !acc * 31 + (byte land 0xff) in
  let red_h = h - 1 and red_w = w - 1 and byte = ref 0 in
  for y = 0 to red_h do
    let ci = float y /. fh -. 1. in
    for x = 0 to red_w do
      let cr = float x /. fw -. 1.5
      and zr = ref 0. and zi = ref 0. and trmti = ref 0. and n = ref 0 in
      begin try
	while true do
	  zi := 2. *. !zr *. !zi +. ci;
	  zr := !trmti +. cr;
	  let tr = !zr *. !zr and ti = !zi *. !zi in
	  if tr +. ti > limit then begin
	    byte := !byte lsl 1;
	    raise Exit
	  end else if incr n; !n = niter then begin
	    byte := (!byte lsl 1) lor 0x01;
	    raise Exit
	  end else
	    trmti := tr -. ti
	done
      with Exit -> ()
      end;
      if x mod 8 = 7 then emit !byte
    done;
    let rem = w mod 8 in
    if rem != 0 then (* the row doesnt divide evenly by 8 *)
      emit (!byte lsl (8 - rem)) (* output last few bits *)
  done;
  Printf.printf "mandelbrot %d checksum %d\n" w !acc
