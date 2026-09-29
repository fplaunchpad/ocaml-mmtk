(* TEST
 set MMTK_HEAP_SIZE_MB = "64";
 {
   bytecode;
 }{
   native;
 }
*)

(* GH issue 26 (ocaml-mmtk). A young block stored with [caml_modify] into a
   field of an already-promoted object must stay reachable whatever the field
   held before: an immediate ([None], [0]), an out-of-heap static (the [[||]]
   atom), or a closure of the other kind (ordinary vs infix pointer into a
   mutually recursive closure block). Under the LXR plan the pre-write field
   barrier used to classify the slot by its OLD value and the deferred RC
   increment then skipped (or mis-offset) the NEW referent, so it was freed
   and reused: wrong values or a crash. Each container is promoted early and
   written between collections; the fixed 64 MiB heap forces several of them. *)

let n = 200 and stride = 3000
let iters = n * stride
let expected j = if j = 0 then iters else j * stride

let report name bad =
  if bad = 0 then Printf.printf "%s: ok\n" name
  else Printf.printf "%s: %d/%d wrong\n" name bad n

let count_bad get =
  let bad = ref 0 in
  for j = 0 to n - 1 do if not (get j (expected j)) then incr bad done;
  !bad

let good_array a e =
  Array.length a = 64 && Array.for_all (fun x -> x = e) a

(* [store j a] is called with a fresh 64-word array every [stride] steps. *)
let churn store =
  for i = 1 to iters do
    let a = Array.make 64 i in
    if i mod stride = 0 then store (i / stride mod n) a
  done

let atom () =
  let keep = Array.make n [||] in
  churn (fun j a -> keep.(j) <- a);
  report "atom -> block" (count_bad (fun j e -> good_array keep.(j) e))

let immediate () =
  let keep = Array.make n None in
  churn (fun j a -> keep.(j) <- Some a);
  report "None -> Some" (count_bad (fun j e ->
    match keep.(j) with Some a -> good_array a e | None -> false))

let refs () =
  let keep = Array.init n (fun _ -> ref [||]) in
  churn (fun j a -> keep.(j) := a);
  report "ref atom -> block" (count_bad (fun j e -> good_array !(keep.(j)) e))

let twice () =
  (* first write of the epoch is a temporary; the barrier logs only it *)
  let keep = Array.make n [||] in
  churn (fun j a -> keep.(j) <- [| 0 |]; keep.(j) <- a);
  report "atom -> tmp -> block" (count_bad (fun j e -> good_array keep.(j) e))

let blit () =
  let keep = Array.make n [||] and tmp = Array.make 1 [||] in
  churn (fun j a -> tmp.(0) <- a; Array.blit tmp 0 keep j 1);
  report "blit atom -> block" (count_bad (fun j e -> good_array keep.(j) e))

let mk_infix i =
  let rec f x = if x <= 0 then i else g (x - 1)
  and g x = if x <= 0 then i else f (x - 1) in
  g
let mk_ord i = let r = ref i in fun x -> x * 0 + !r

let closures name init make =
  let keep = Array.init n (fun _ -> init (-1)) in
  churn (fun j a -> keep.(j) <- make a.(0));
  report name (count_bad (fun j e -> keep.(j) 3 = e))

let () =
  atom ();
  immediate ();
  refs ();
  twice ();
  blit ();
  closures "ordinary -> infix closure" mk_ord mk_infix;
  closures "infix -> ordinary closure" mk_infix mk_ord;
  closures "infix -> infix closure" mk_infix mk_infix;
  let keep = Array.make n None in
  churn (fun j a -> keep.(j) <- Some (mk_infix a.(0)));
  report "None -> Some infix closure" (count_bad (fun j e ->
    match keep.(j) with Some c -> c 3 = e | None -> false))
