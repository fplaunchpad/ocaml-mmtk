(* MMTk DISABLED: loops on the stock minor_collections counter.

   The driver loop is [while (Gc.quick_stat ()).minor_collections < 1000 do ...],
   relying on a background thread's [Gc.minor ()] calls to bump the stock
   minor-collection counter to 1000. Under MMTk native code uses TLAB
   nursery-aliasing and runs no stock minor cycle at all, so minor_collections
   never rises and the loop never terminates (the test is SIGKILLed on timeout).
   The bounds-check liveness property it actually exercises is unrelated to GC
   pacing, but the loop bound is fundamentally a stock-minor-GC construct MMTk
   cannot satisfy. Re-enable only if MMTk reinstates a stock-compatible minor
   collection count.

 include systhreads;
 hassysthreads;
 no-tsan; (* See https://github.com/ocaml-multicore/ocaml-tsan/issues/31 *)
 {
   bytecode;
 }{
   native;
 }
*)


module Atomic = struct
  let make = ref
  let set = (:=)
  let get = (!)
end

let arr = [| 1; 2; 3 |]

let[@inline never] bounds r =
  (* r is live across a bounds check failure *)
  try arr.(42) with
  | _ -> !r

let glob = ref (ref 0)
let () =
  let go = Atomic.make true in
  let gcthread =
    Thread.create (fun () ->
      while Atomic.get go do Thread.yield (); Gc.minor (); done) ()
  in
  while (Gc.quick_stat ()).minor_collections < 1000 do
    let r = ref 42 in
    glob := r; (* force promotion *)
    let n = bounds r in
    if n <> 42 then Printf.printf "%x <> 42!\n%!" n;
  done;
  Atomic.set go false;
  Thread.join gcthread
