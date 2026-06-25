(* MMTk DISABLED: stock-GC pacing + GC backup thread.

   This test asserts a stock-collector pacing invariant that does not hold under
   MMTk: it allocates a fixed-depth tree (make 22 / make 24) and then asserts
   [(Gc.quick_stat ()).major_collections > n] — i.e. that a specific amount of
   allocation deterministically forces at least one major collection. Under MMTk
   the major-collection count reported by Gc.stat is MMTk's own collection count,
   which is driven by MMTk's heap trigger (heap size / Immix occupancy), not by
   the stock major-slice pacing this assumes; at the testsuite heap size that
   allocation does not necessarily trigger a collection, so the assertion fails.

   The test is also specifically about the OCaml GC *backup thread* (a domain
   forced to run a full GC while another domain blocks/waits) — a stock-GC
   construct MMTk does not have. Re-enable only if MMTk grows stock-compatible
   major-cycle pacing. *)

type 'a tree = Empty | Node of 'a tree * 'a tree

let rec make d =
  if d = 0 then Node(Empty, Empty)
  else let d = d - 1 in Node(make d, make d)

(* you need to use Gc.quick_stat, because Gc.stat forces a major cycle *)
let major_collections () =
  (Gc.quick_stat ()).major_collections

(* test to force domain to do a full GC while another is waiting *)
let _ =
  let sem = Semaphore.Binary.make false in
  let d = Domain.spawn (fun _ -> Semaphore.Binary.acquire sem) in
  Gc.full_major ();
  let n = major_collections () in
  ignore (make 22);
  assert ((major_collections ()) > n);
  Semaphore.Binary.release sem;
  Domain.join d;
  print_endline "wait OK"

(* test to force domain to do a full GC while another is blocking *)
let _ =
  let _ = Domain.spawn (fun _ ->
    Unix.sleep 10000
  ) in
  Gc.full_major ();
  let n = major_collections () in
  ignore (make 24);
  assert ((major_collections ()) > n);
  print_endline "sleep OK"
