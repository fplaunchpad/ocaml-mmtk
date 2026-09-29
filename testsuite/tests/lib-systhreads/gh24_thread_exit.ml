(* TEST
 set MMTK_HEAP_SIZE_MB = "32";
 include systhreads;
 hassysthreads;
 {
   bytecode;
 }{
   native;
 }
*)

(* GH issue 24, GAP-3: a systhread that exited used to leave its domain in
   MMTk's RUNNING set with nobody holding the master lock.

   Domain 1 runs two threads. [t] blocks in [Thread.delay], then the main
   thread of domain 1 blocks in [Unix.read]. [t] wakes up, leaves its
   blocking section (domain RUNNING) and exits. Before the fix,
   thread_detach_from_runtime released the master lock without marking the
   domain STOPPED; the only remaining thread of domain 1 is blocked in
   [read], so nothing on domain 1 reached a safepoint again. Domain 0 then
   allocates enough to force collections: stop_all_mutators waited for
   domain 1 forever, and domain 0, parked in that collection, never wrote
   the byte that would unblock domain 1, so the program hung. Now a
   master-lock release with no waiting thread marks the domain STOPPED, so
   the collections complete and the program prints "ok".

   The collections are forced by allocation under the small pinned heap set
   above (256 MiB allocated through a 32 MiB heap), not by [Gc.full_major],
   which is not relied on to run a collection in this fork. *)

let alloc_mb = 256

let churn () =
  let words = alloc_mb * 1024 * 1024 / 8 in
  let n = ref 0 and keep = ref [] in
  while !n < words do
    keep := Array.make 16 !n :: (if !n land 0xfff = 0 then [] else !keep);
    n := !n + 17
  done;
  ignore (Sys.opaque_identity !keep)

let () =
  let r, w = Unix.pipe () in
  let d = Domain.spawn (fun () ->
    let t = Thread.create (fun () -> Thread.delay 0.05) () in
    let buf = Bytes.create 1 in
    ignore (Unix.read r buf 0 1);
    Thread.join t) in
  Unix.sleepf 0.2;
  churn ();
  ignore (Unix.write w (Bytes.of_string "x") 0 1);
  Domain.join d;
  print_endline "ok"
