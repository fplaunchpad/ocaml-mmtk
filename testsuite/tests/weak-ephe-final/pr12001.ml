(* MMTk DISABLED: on ConcurrentImmix and LXR, "finalised" line missing:
   finaliser not invoked on the test's Gc.full_major [semantic-timing]. An
   explicit collection there completes one pause, not a whole marking cycle or
   backup trace. Passes on the stop-the-world plans since explicit collections
   wait for their pause (GH issue 21). *)

let [@inline never] foo () =
  let s = "Hello" ^ " world!" in
  Gc.finalise_last (fun () -> print_endline "finalised") s;
  Gc.minor ();
  s

let [@inline never] bar () =
  let s = foo () in
  print_endline s

let _ =
  bar ();
  Gc.full_major ()
