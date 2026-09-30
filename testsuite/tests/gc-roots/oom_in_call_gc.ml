(* TEST
 set MMTK_HEAP_SIZE_MB = "32";
*)

(* GH issue 49 (and MMTk bug #4): exhausting a pinned heap with small
   allocations makes the TLAB refill fail inside caml_call_gc, which raises
   Out_of_memory from that saved-registers window. caml_raise then runs pending
   actions (a GC poll whose refill starts another collection) before unwinding,
   so the collection's root scan still sees the caml_call_gc frame and must
   find its live registers through gc_regs. Each exhaustion must raise a clean,
   catchable Out_of_memory, repeatedly (the next caml_call_gc needs a free
   gc_regs bucket), rather than crash. *)

type t = Leaf | Node of t * t

let rec make d =
  if d = 0 then Node (Leaf, Leaf) else Node (make (d - 1), make (d - 1))

let rounds = 20

let () =
  let caught = ref 0 in
  for _ = 1 to rounds do
    let keep = ref [] in
    (try while true do keep := make 10 :: !keep done
     with Out_of_memory -> incr caught);
    keep := []
  done;
  Printf.printf "caught %d/%d\n" !caught rounds
