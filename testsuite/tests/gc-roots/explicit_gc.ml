(* TEST
 modules = "explicit_gc_stubs.c";
*)

(* Gc.minor, Gc.major, Gc.full_major and Gc.compact return only after an MMTk
   pause requested by the call has completed: the completed-pause count read
   right after the call has moved. For the major requests the completed pause
   is full-heap, except under Bactrian, where a request made while a
   concurrent cycle is in flight gets a nursery pause (the cycle ends at a
   later FinalMark). NoGC never collects. Live data survives. *)

external completed_pauses : unit -> int
  = "test_completed_pauses" [@@noalloc]
external completed_full_pauses : unit -> int
  = "test_completed_full_pauses" [@@noalloc]

let plan = Option.value (Sys.getenv_opt "MMTK_PLAN") ~default:"GenImmix"
let collects = plan <> "NoGC"
let full_guaranteed = plan <> "Bactrian"

let () =
  let live = Array.init 256 (fun i -> Array.init 64 (fun j -> i + j)) in
  let check ~full name operation =
    for _ = 1 to 16 do
      let before = completed_pauses () in
      let full_before = completed_full_pauses () in
      operation ();
      let after = completed_pauses () in
      let full_after = completed_full_pauses () in
      if collects then begin
        if after <= before then
          Printf.printf "%s returned before a pause completed\n" name;
        if full && full_guaranteed && full_after <= full_before then
          Printf.printf "%s returned before a full pause completed\n" name
      end else if after <> before || full_after <> full_before then
        Printf.printf "%s collected under NoGC\n" name;
      Array.iteri (fun i row ->
        Array.iteri (fun j value -> assert (value = i + j)) row) live
    done
  in
  check ~full:true "Gc.major" Gc.major;
  check ~full:true "Gc.full_major" Gc.full_major;
  check ~full:false "Gc.minor" Gc.minor;
  check ~full:true "Gc.compact" Gc.compact;
  print_endline "ok"
