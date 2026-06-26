(* TEST
 {
   bytecode;
 }{
   native;
 }
*)

(* excise Phase 3b: exercise caml_stop_all_domains' cancel + MMTk-deregister path.

   The main domain spawns a few peers that loop forever and are NEVER joined,
   does a little work, prints "ok", then exits the process while the peers are
   still running. At shutdown caml_domain_alone() is false, so caml_shutdown
   calls caml_stop_all_domains(), which must pthread_cancel + mmtk-deregister +
   terminate-backup each peer and then let the process exit CLEANLY (no hang).

   Kept deliberately LIGHT (few peers, no heavy allocation) to avoid the
   pre-existing multi-domain scheduler livelock (GH#15) that blocks heavy
   multi-domain validation locally; the heavy exit-stress variant runs on turing. *)

let n_peers = 3

let () =
  let started = Atomic.make 0 in
  for _ = 1 to n_peers do
    ignore (Domain.spawn (fun () ->
      Atomic.incr started;
      while true do Domain.cpu_relax () done))
  done;
  while Atomic.get started < n_peers do Domain.cpu_relax () done;
  let s = ref 0 in
  for i = 1 to 1000 do s := !s + i done;
  assert (!s = 500500);
  print_endline "ok";
  exit 0
