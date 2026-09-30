(* TEST
 modules = "explicit_gc_stubs.c";
*)

(* Explicit Gc requests from two domains at once, while a third domain
   allocates and a fourth sits in a blocking section (Condition.wait). Each
   request must return after a pause that completed after the call (and,
   for the major requests, a full-heap one; see explicit_gc.ml), and must not
   hang waiting for the blocked domain. *)

external completed_pauses : unit -> int
  = "test_completed_pauses" [@@noalloc]
external completed_full_pauses : unit -> int
  = "test_completed_full_pauses" [@@noalloc]

let plan = Option.value (Sys.getenv_opt "MMTK_PLAN") ~default:"GenImmix"
let collects = plan <> "NoGC"
let full_guaranteed = plan <> "Bactrian"

let ops = [| "Gc.minor", false, Gc.minor;
             "Gc.major", true, Gc.major;
             "Gc.full_major", true, Gc.full_major;
             "Gc.compact", true, Gc.compact |]

let requester id () =
  let live = Array.init 64 (fun i -> Array.make 16 (id + i)) in
  let junk = ref [] in
  for k = 1 to 40 do
    let name, full, operation = ops.((id + k) mod 4) in
    let before = completed_pauses () in
    let full_before = completed_full_pauses () in
    operation ();
    let after = completed_pauses () in
    let full_after = completed_full_pauses () in
    if collects then begin
      if after <= before then
        Printf.printf "%s returned before a pause completed\n%!" name;
      if full && full_guaranteed && full_after <= full_before then
        Printf.printf "%s returned before a full pause completed\n%!" name
    end else if after <> before then
      Printf.printf "%s collected under NoGC\n%!" name;
    for _ = 1 to 1000 do junk := [Array.make 4 k] done;
    Array.iteri (fun i a -> Array.iter (fun v -> assert (v = id + i)) a) live
  done;
  ignore (Sys.opaque_identity !junk)

let stop = Atomic.make false

let allocator () =
  let r = ref [] in
  while not (Atomic.get stop) do
    r := [];
    for i = 1 to 1000 do r := i :: !r done
  done;
  ignore (Sys.opaque_identity !r)

let m = Mutex.create ()
let c = Condition.create ()

let blocker () =
  Mutex.lock m;
  while not (Atomic.get stop) do Condition.wait c m done;
  Mutex.unlock m

let () =
  let b = Domain.spawn blocker in
  let a = Domain.spawn allocator in
  let rs = List.init 2 (fun id -> Domain.spawn (requester id)) in
  List.iter Domain.join rs;
  Mutex.lock m;
  Atomic.set stop true;
  Condition.broadcast c;
  Mutex.unlock m;
  Domain.join a;
  Domain.join b;
  print_endline "ok"
