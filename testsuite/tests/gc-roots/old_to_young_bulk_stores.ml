(* TEST
 set MMTK_HEAP_SIZE_MB = "32";
 {
   bytecode;
 }{
   native;
 }
*)

(* Old-to-young pointers created by runtime primitives that store into a heap
   block without going through caml_modify must still be remembered by the
   generational write barrier. Each case below ages a container (so it is
   mature), stores freshly allocated (nursery) blocks into it through one such
   primitive, drops every other reference to them, then allocates enough to
   run several nursery collections before checking the stored blocks.

   - Array.fill (caml_uniform_array_fill): the store loop is a plain store.
     Native code used to skip the region barrier here, so under the
     generational plans (GenImmix, the default, StickyImmix, GenCopy,
     Bactrian) the nursery GC freed the filled blocks and the array was left
     pointing at reused memory.
   - Marshal.from_string of data written with No_sharing: intern.c fills
     blocks with plain stores and remembers the ones placed outside the
     nursery by walking its object table, which No_sharing data does not
     have. Blocks of 2056 bytes and up are placed outside the nursery under
     Bactrian's default medium pretenuring, so this case only bites there.

   The heap is pinned small so that the churn below triggers collections
   quickly on every plan. Non-generational plans pass trivially. Containers
   start out holding a heap block rather than the [||] atom: under LXR an
   out-of-heap old value is a separate bug (GH issue 26) that this test is
   not about. *)

let n = 100

let placeholder = Array.make 16 (-1)

let churn () =
  for i = 1 to 500_000 do
    ignore (Sys.opaque_identity (Array.make 9 i))
  done

let[@inline never] young j = Array.make 16 (7 * j + 1)

let ok j (a : int array) =
  Array.length a = 16 && a.(0) = 7 * j + 1 && a.(15) = 7 * j + 1

let report name bad =
  if bad = 0 then Printf.printf "%s: ok\n%!" name
  else Printf.printf "%s: %d/%d corrupted\n%!" name bad n

let count_bad f =
  let bad = ref 0 in
  for j = 0 to n - 1 do if not (f j) then incr bad done;
  !bad

(* One young block per slot of a mature array. *)
let fill_one () =
  let c = Array.make n placeholder in
  churn ();
  for j = 0 to n - 1 do Array.fill c j 1 (young j) done;
  churn (); churn ();
  report "Array.fill, one slot" (count_bad (fun j -> ok j c.(j)))

(* One young block over a whole range of a mature array. *)
let fill_range () =
  let cs = Array.init n (fun _ -> Array.make 8 placeholder) in
  churn ();
  for j = 0 to n - 1 do Array.fill cs.(j) 0 8 (young j) done;
  churn (); churn ();
  report "Array.fill, range"
    (count_bad (fun j -> Array.for_all (ok j) cs.(j)))

(* Into an array large enough for the large-object space. *)
let fill_large () =
  let c = Array.make 4096 placeholder in
  churn ();
  for j = 0 to n - 1 do Array.fill c (j * 40) 1 (young j) done;
  churn (); churn ();
  report "Array.fill, large array" (count_bad (fun j -> ok j c.(j * 40)))

(* A 300-word (2408-byte) outer array of small blocks, unmarshaled. *)
let marshal flags name =
  let s =
    Marshal.to_string (Array.init 300 (fun k -> [| k; k |])) flags in
  churn ();
  let bs = Array.init n (fun _ -> (Marshal.from_string s 0 : int array array))
  in
  churn (); churn ();
  let good (b : int array array) =
    let r = ref (Array.length b = 300) in
    Array.iteri (fun k a ->
        if Array.length a <> 2 || a.(0) <> k || a.(1) <> k then r := false) b;
    !r
  in
  report name (count_bad (fun j -> good bs.(j)))

let () =
  fill_one ();
  fill_range ();
  fill_large ();
  marshal [] "Marshal";
  marshal [Marshal.No_sharing] "Marshal, No_sharing"
