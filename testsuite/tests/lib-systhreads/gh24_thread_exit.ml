(* MMTk DISABLED: hangs until GH issue 24 (GAP-3) is fixed [gh24] *)
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

(* GH issue 24, GAP-3: a systhread that exits leaves its domain in MMTk's
   RUNNING set with nobody holding the master lock.

   Domain 1 runs two threads. [t] blocks in [Thread.delay] (domain marked
   STOPPED), then the main thread of domain 1 blocks in [Unix.read] (still
   STOPPED). [t] wakes up, leaves its blocking section (domain marked
   RUNNING) and exits: thread_detach_from_runtime releases the master lock
   without marking the domain STOPPED again. The only remaining thread of
   domain 1 is blocked in [read], so nothing on domain 1 ever reaches a
   safepoint.

   Domain 0 then allocates enough to force collections. stop_all_mutators
   waits for domain 1 to leave the RUNNING set, which never happens, and
   domain 0 is parked in that collection, so it never writes the byte that
   would unblock domain 1: the program hangs (ocamltest's timeout kills it).

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
