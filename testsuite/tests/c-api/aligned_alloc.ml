(* MMTk DISABLED: asserts NOT all Atomic.t are 16-byte aligned; MMTk aligns them all [unsupported-12c]. *)

external is_aligned : 'a Atomic.t -> bool = "caml_atomic_is_aligned"
let test_is_aligned () =
  let l = List.init 100 Atomic.make in
  let all_aligned =
    List.for_all is_aligned l
  in
  assert (not all_aligned);
;;

let test_make_contended () =
  let l = List.init 100 Atomic.make_contended in
  List.iteri (fun i atomic ->
    assert (Atomic.get atomic == i);
    assert (is_aligned atomic)) l
;;

let () =
  test_is_aligned ();
  test_make_contended ();
;;
