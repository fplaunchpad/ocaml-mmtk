(* TEST
 { bytecode; } { native; }
*)

(* Two domains race to resume the same continuation, many times, while a third
   domain keeps a large live set churning so that a concurrent plan (Bactrian,
   ConcurrentImmix) has a mark cycle open for much of the run. Exactly one
   resume per round must win; the other must raise
   Continuation_already_resumed. The suspended fiber is deep and its frames
   hold heap pointers, so a resumer that snapshotted a stack after the other
   one had taken, run or freed it would read a stack that is changing under it.
   On the resume path the stack must be claimed before it is scanned. *)

open Effect
open Effect.Deep

type _ Effect.t += Park : unit Effect.t

let depth = 1_000
let rounds = 3_000

(* Each frame keeps [r] live across the recursive call, so the stack holds
   pointers into the heap. *)
let rec deep n =
  if n = 0 then (perform Park; 0)
  else
    let r = ref n in
    let x = deep (n - 1) in
    x + !r - n + 1

exception Got of (unit, int) continuation

(* Run [deep] until it parks, and hand back its continuation. The handler is
   part of the continuation, so whichever domain resumes it gets [retc]. *)
let capture () =
  try
    match_with deep depth
      { retc = (fun v -> v);
        exnc = raise;
        effc = (fun (type a) (e : a Effect.t) ->
          match e with
          | Park -> Some (fun (k : (a, int) continuation) -> raise (Got k))
          | _ -> None) }
    |> ignore; assert false
  with Got k -> k

let stop = Atomic.make false
let slot : (int * (unit, int) continuation) option Atomic.t = Atomic.make None
let wins = Atomic.make 0
let losses = Atomic.make 0
let finished = Atomic.make 0

let racer () =
  let seen = ref 0 in
  while not (Atomic.get stop) do
    match Atomic.get slot with
    | Some (round, k) when round > !seen ->
        seen := round;
        (match continue k () with
         | v -> assert (v = depth); Atomic.incr wins
         | exception Continuation_already_resumed -> Atomic.incr losses);
        Atomic.incr finished
    | _ -> Domain.cpu_relax ()
  done

let churn () =
  let live = Array.make 64 [] in
  let i = ref 0 in
  while not (Atomic.get stop) do
    live.(!i land 63) <- List.init 20_000 (fun j -> ref j);
    incr i
  done;
  ignore (Sys.opaque_identity live)

let () =
  let c = Domain.spawn churn in
  let a = Domain.spawn racer and b = Domain.spawn racer in
  for round = 1 to rounds do
    Atomic.set slot (Some (round, capture ()));
    while Atomic.get finished < 2 * round do Domain.cpu_relax () done
  done;
  Atomic.set stop true;
  Domain.join a; Domain.join b; Domain.join c;
  assert (Atomic.get wins = rounds);
  assert (Atomic.get losses = rounds);
  print_endline "ok"
