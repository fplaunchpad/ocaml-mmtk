(* TEST *)

(* A continuation that is suspended across a collection and then resumed
   must not keep its stack's referents alive afterwards, and walking its
   stack for a backtrace (take, then put back) must not release them.

   Under the LXR plan a collection promotes the suspended continuation and
   counts its stack's referents as the continuation's fields. Resuming it
   must give back those counts (else each round's garbage stays counted until
   a backup trace, and the used heap grows by about 180 KiB per round here),
   and Effect.Shallow.get_callstack, which takes the stack and puts it back,
   must not (else the list below is freed while its stack still uses it). The
   heap-growth check is LXR-only: under the tracing plans the dead lists stay
   in the mature space until a major collection. *)

open Effect
open Effect.Shallow

type _ Effect.t += Y : unit Effect.t

let saved : (unit, unit) continuation option ref = ref None

let run () =
  let cells = List.init 64 (fun i -> Array.make 256 i) in
  perform Y;
  for _ = 1 to 200 do ignore (Sys.opaque_identity (Array.make 256 (-1))) done;
  Gc.minor ();
  List.iteri (fun i a ->
    if a.(0) <> i || a.(255) <> i then begin
      Printf.printf "cell %d corrupted\n%!" i; exit 1
    end) cells

let handler =
  { retc = (fun () -> ());
    exnc = raise;
    effc = (fun (type a) (e : a Effect.t) ->
      match e with
      | Y -> Some (fun (k : (a, unit) continuation) -> saved := Some k)
      | _ -> None) }

let used_kib () =
  Gc.minor (); Gc.minor ();
  (Gc.stat ()).Gc.live_words * (Sys.word_size / 8) / 1024

let () =
  let lxr = Sys.getenv_opt "MMTK_PLAN" = Some "LXR" in
  let before = used_kib () in
  for _ = 1 to 100 do
    continue_with (fiber run) () handler;
    Gc.minor (); Gc.minor ();
    let k = Option.get !saved in
    ignore (get_callstack k 10);
    Gc.minor ();
    saved := None;
    continue_with k () handler
  done;
  let growth = used_kib () - before in
  if lxr && growth > 4096 then
    Printf.printf "used heap grew by %d KiB over 100 rounds\n" growth;
  print_endline "ok"
