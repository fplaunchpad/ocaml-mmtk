(* TEST
 set MMTK_HEAP_SIZE_MB = "128";
*)

(* MMTk HEAP PINNED: the loop below runs until 20 major collections have
   happened. Stock OCaml paces major cycles by allocated words, so that is
   quick at any heap size. Under MMTk a non-generational plan (Immix,
   ConcurrentImmix, SemiSpace) collects only when its heap is full, so 20
   cycles cost 20 heap-worths of allocation: at the all-plans CI heap of
   4096 MB that took about 75 s on an M4 Pro and exceeded the 120 s per-test
   timeout on CI. A small fixed heap keeps the 20 natural collections cheap
   on every plan (Immix ~1 s, GenImmix ~10 s at 128 MB) and leaves what the
   test checks unchanged. The default dynamic heap is not used because under
   Immix it grew to 12-14 GB on this test. *)

let () = Random.init 12345

let size, num_gcs =
  1000, 20

type block = int array

type objdata =
  | Present of block
  | Absent of int  (* GC count at time of erase *)


type bunch = {
  objs : objdata array;
  wp : block Weak.t;
}

let data =
  Array.init size (fun i ->
    let n = 1 + Random.int size in
    {
      objs = Array.make n (Absent 0);
      wp = Weak.create n;
    }
  )

let gccount () = (Gc.quick_stat ()).Gc.major_collections

type change = No_change | Fill | Erase

(* Check the correctness condition on the data at (i,j):
   1. if the block is present, the weak pointer must be full
   2. if the block was removed at GC n, and the weak pointer is still
      full, then the current GC must be at most n+2.
      (could have promotion from minor during n+1 which keeps alive in n+1,
      so will die at n+2)

   Then modify the data in one of the following ways:
   1. if the block and weak pointer are absent, fill them
   2. if the block and weak pointer are present, randomly erase the block
*)
let check_and_change i j =
  let gc1 = gccount () in
  let change =
    (* we only read data.(i).objs.(j) in this local binding to ensure
        that it does not remain reachable on the bytecode stack
        in the rest of the function below, when we overwrite the value
        and try to observe its collection.  *)
    match data.(i).objs.(j), Weak.check data.(i).wp j with
    | Present x, false -> assert false
    | Absent n, true -> assert (gc1 <= n+2); No_change
    | Absent _, false -> Fill
    | Present _, true ->
      if Random.int 10 = 0 then Erase else No_change
  in
  begin match change with
  | No_change -> ()
  | Fill ->
    let x = Array.make (1 + Random.int 10) 42 in
    data.(i).objs.(j) <- Present x;
    Weak.set data.(i).wp j (Some x);
  | Erase ->
    data.(i).objs.(j) <- Absent gc1;
    let gc2 = gccount () in
    if gc1 <> gc2 then data.(i).objs.(j) <- Absent gc2;
  end

let dummy = ref [||]

let () =
  while gccount () < num_gcs do
    dummy := Array.make (Random.int 300) 0;
    let i = Random.int size in
    let j = Random.int (Array.length data.(i).objs) in
    check_and_change i j;
  done
