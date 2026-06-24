(* mutate — write-barrier probe (GC axis: generational remembered-set / barrier).
 *
 * The CLBG suite has no barrier-exercising bench. This fills that gap.
 *
 * Build a large, long-lived array of boxed refs (the array and its initial
 * contents survive a minor GC and get promoted to the mature space). Then run
 * many old->young pointer overwrites: a.(i) <- fresh_alloc, where fresh_alloc is
 * a freshly-allocated (young) boxed value stored into an old array slot. That is
 * exactly the case the generational write barrier exists to catch — each such
 * store must record the old object in the remembered set / mod-buffer so the
 * minor collector can find the young object. High store volume => the barrier
 * fires constantly, and the freshly-allocated values churn the nursery.
 *
 * Deterministic, self-checking: a checksum folded over the stored values plus a
 * final pass over the surviving array (so the stores are observed and the array
 * must really hold them). Single-line output for a golden.
 *
 *   mutate SIZE ITERS    array of SIZE boxes, ITERS overwrites
 *                        (defaults 100_000 SIZE, 5_000_000 ITERS) *)

type box = { mutable payload : int array }

let () =
  let size  = try int_of_string Sys.argv.(1) with _ ->   100_000 in
  let iters = try int_of_string Sys.argv.(2) with _ -> 5_000_000 in
  (* long-lived array of long-lived boxes; force promotion with a major GC *)
  let arr = Array.init size (fun i -> { payload = [| i; i * 3 |] }) in
  Gc.full_major ();   (* promote arr + its boxes to the mature space *)
  let checksum = ref 0 in
  (* simple deterministic LCG to pick slots without per-iter allocation *)
  let state = ref 0x9E3779B1 in
  for k = 0 to iters - 1 do
    state := (!state * 1103515245 + 12345) land 0x3FFFFFFF;
    let i = !state mod size in
    (* fresh young allocation stored into an OLD array slot -> old->young edge.
       Two allocations: the box's payload array and a new box. *)
    let v = (!state lxor k) land 0x3FFFFFFF in
    arr.(i).payload <- [| v; v + 1 |];    (* old box <- young array (barrier) *)
    checksum := (!checksum + v) land 0x3FFFFFFF
  done;
  (* observe the surviving array: every slot's payload must be reachable *)
  let live = ref 0 in
  for i = 0 to size - 1 do
    live := (!live + arr.(i).payload.(0)) land 0x3FFFFFFF
  done;
  Printf.printf "mutate size=%d iters=%d checksum=%d live=%d\n"
    size iters !checksum !live
