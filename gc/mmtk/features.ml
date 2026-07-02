(* Probe runtime features under MMTk: which work, which have semantic gaps,
   which crash. Triggers GC by churning garbage past the heap (Gc.full_major may
   not drive an MMTk collection). *)
let churn () =
  for _ = 1 to 3000 do ignore (Sys.opaque_identity (Array.make 4000 0)) done

let test_lazy () =
  let n = ref 0 in
  let l = lazy (incr n; 42) in
  let a = Lazy.force l in
  churn ();
  let b = Lazy.force l in
  Printf.printf "lazy: %d,%d forced=%d (expect 42,42,1)\n%!" a b !n

let test_weak () =
  let w = Weak.create 1 in
  (* only the weak slot references it *)
  Weak.set w 0 (Some (Bytes.create 1000));
  churn (); churn ();
  match Weak.get w 0 with
  | None   -> Printf.printf "weak: CLEARED (correct weak semantics)\n%!"
  | Some _ ->
    Printf.printf "weak: still alive (weak processing not wired => strong)\n%!"

let test_ephemeron () =
  let k = ref 0 in
  let e = Ephemeron.K1.make k (Bytes.create 1000) in
  churn (); churn ();
  (* While the key is live, the data must be retained -- and crucially this must
     not crash the GC. (The key-dead => data-cleared case needs weak processing
     and is part of workstream E.) *)
  match Ephemeron.K1.query e k with
  | Some _ ->
    Printf.printf "ephemeron: data retained while key live (ok, no crash)\n%!"
  | None   -> Printf.printf "ephemeron: data cleared while key live (WRONG)\n%!"

let test_finalise () =
  let fired = ref 0 in
  let mk () =
    let b = Bytes.create 1000 in Gc.finalise (fun _ -> incr fired) b; b in
  ignore (Sys.opaque_identity (mk ()));      (* immediately dead *)
  churn (); churn ();
  Printf.printf "finalise: fired=%d (0 => finalisers not run yet)\n%!" !fired

let test_gc_module () =
  (try Gc.full_major (); Printf.printf "Gc.full_major: ok\n%!"
   with e ->
     Printf.printf "Gc.full_major: raised %s\n%!" (Printexc.to_string e));
  (try let s = Gc.stat () in
     Printf.printf "Gc.stat: live_words=%d\n%!" s.Gc.live_words
   with e -> Printf.printf "Gc.stat: raised %s\n%!" (Printexc.to_string e))

let () =
  test_lazy ();
  test_weak ();
  test_ephemeron ();
  test_finalise ();
  test_gc_module ();
  Printf.printf "features-probe done\n%!"
