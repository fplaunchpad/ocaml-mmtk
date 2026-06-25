(* chameneos-redux — effect-handlers version.
 *
 * Ported from ocaml-multicore/effects-examples (mvar/chameneos.ml, MVar.ml,
 * sched.ml) into ONE stdlib-only file: a cooperative-threaded scheduler built on
 * OCaml 5 effect handlers (Suspend / Resume / Fork / Yield) plus an MVar
 * synchronising N green threads. This is the panel's EFFECT / CONTINUATION
 * workload: each "meeting" suspends and resumes a delimited continuation, so the
 * run is dominated by effect-perform + continue (fiber) traffic and the
 * allocation of continuation objects — the GC axis is "lots of short-lived
 * continuations / fibers".
 *
 *   chameneos_redux N [DOMAINS]      DOMAINS also from env DOMAINS
 *                                    (defaults: N=600, DOMAINS=1)
 *
 * N is the number of meetings each chameneos rendezvous runs. The bench runs a
 * FIXED total of [games] independent chameneos games (default 8, chosen to divide
 * 1/2/4/8) and SPLITS them across DOMAINS — so total work is constant in DOMAINS
 * (STRONG scaling, like the other par_* benches: ideal speedup = #domains), and
 * the printed checksum is identical for any DOMAINS (a parallel self-check).
 *
 * Output: a single deterministic checksum line (the summed meet counts across
 * both colour sets), not the verbose CLBG spelled-out report — keeps goldens
 * small and the bench print-free in its hot loop. *)

open Effect
open Effect.Deep

(* ---- cooperative scheduler (was sched.ml) -------------------------------- *)
type 'a cont = ('a, unit) continuation
type _ eff += Fork    : (unit -> unit) -> unit eff
type _ eff += Yield   : unit eff
type _ eff += Suspend : ('a cont -> unit) -> 'a eff
type _ eff += Resume  : 'a cont * 'a -> unit eff

let run main =
  let run_q = Queue.create () in
  let enqueue t v = Queue.push (fun () -> continue t v) run_q in
  let dequeue () = if Queue.is_empty run_q then () else Queue.pop run_q () in
  let rec spawn f =
    match f () with
    | () -> dequeue ()
    | effect Yield, k -> enqueue k (); dequeue ()
    | effect (Fork f), k -> enqueue k (); spawn f
    | effect (Suspend f), k -> f k; dequeue ()
    | effect (Resume (k', v)), k -> enqueue k' v; ignore (continue k ())
  in
  spawn main

let fork f = perform (Fork f)

(* ---- MVar over that scheduler (was MVar.Make(Sched)) ---------------------- *)
module MVar = struct
  type 'a mv_state =
    | Full  of 'a * ('a * unit cont) Queue.t
    | Empty of 'a cont Queue.t

  type 'a t = 'a mv_state ref

  let create_empty () = ref (Empty (Queue.create ()))
  let create v = ref (Full (v, Queue.create ()))
  let suspend f = perform (Suspend f)
  let resume (a, b) = perform (Resume (a, b))

  let put v mv =
    match !mv with
    | Full (_v', q) -> suspend (fun k -> Queue.push (v, k) q)
    | Empty q ->
        if Queue.is_empty q then mv := Full (v, Queue.create ())
        else let t = Queue.pop q in resume (t, v)

  let take mv =
    match !mv with
    | Empty q -> suspend (fun k -> Queue.push k q)
    | Full (v, q) ->
        if Queue.is_empty q then (mv := Empty (Queue.create ()); v)
        else
          let v', t = Queue.pop q in
          mv := Full (v', q);
          resume (t, ());
          v
end

(* ---- chameneos game (was chameneos.ml) ----------------------------------- *)
module Color = struct
  type t = Blue | Red | Yellow
  let complement t t' =
    match t, t' with
    | Blue, Blue -> Blue       | Blue, Red -> Yellow    | Blue, Yellow -> Red
    | Red, Blue -> Yellow      | Red, Red -> Red        | Red, Yellow -> Blue
    | Yellow, Blue -> Red      | Yellow, Red -> Blue    | Yellow, Yellow -> Yellow
end

type chameneos = Color.t ref
type mp = Nobody of int | Somebody of int * chameneos * chameneos MVar.t

let arrive (mpv : mp MVar.t) (finish : (int * int) MVar.t) (ch : chameneos) =
  let waker = MVar.create_empty () in
  let inc x i = if x == ch then i + 1 else i in
  let rec go t b =
    let w = MVar.take mpv in
    match w with
    | Nobody 0 ->
        MVar.put w mpv;
        MVar.put (t, b) finish
    | Nobody q ->
        MVar.put (Somebody (q, ch, waker)) mpv;
        go (t + 1) @@ inc (MVar.take waker) b
    | Somebody (_q, ch', waker') ->
        MVar.put (Nobody (_q - 1)) mpv;
        let c'' = Color.complement !ch !ch' in
        ch := c''; ch' := c'';
        MVar.put ch waker';
        go (t + 1) @@ inc ch' b
  in
  go 0 0

let rec tabulate' acc f = function 0 -> acc | n -> tabulate' (f () :: acc) f (n - 1)
let tabulate f n = List.rev (tabulate' [] f n)

(* one chameneos game over [colors] with [n] meetings; returns sum of meets *)
let work colors n =
  let result = ref 0 in
  let body () =
    let fs = tabulate MVar.create_empty (List.length colors) in
    let mpv = MVar.create (Nobody n) in
    let chams = List.map (fun c -> ref c) colors in
    List.iter2 (fun fin ch -> fork (fun () -> arrive mpv fin ch)) fs chams;
    let ns = List.map MVar.take fs in
    result := List.fold_left (fun acc (m, _) -> m + acc) 0 ns
  in
  run body;
  !result

let game n =
  let open Color in
  let a = work [ Blue; Red; Yellow ] n in
  let b =
    work [ Blue; Red; Yellow; Red; Yellow; Blue; Red; Yellow; Red; Blue ] n
  in
  a + b

let n = try int_of_string Sys.argv.(1) with _ -> 600
let num_domains =
  try int_of_string Sys.argv.(2)
  with _ -> (try int_of_string (Sys.getenv "DOMAINS") with _ -> 1)
let num_domains = max 1 num_domains

(* Fixed total work = [num_games] identical games of N meetings each, split over
 * the domains (strong scaling). 8 divides the 1/2/4/8 sweep evenly. *)
let num_games = 8

let () =
  (* Run a contiguous slice [lo,hi) of the num_games games on the calling thread;
   * every game yields the same per-game checksum, so the sum is deterministic. *)
  let run_games lo hi =
    let acc = ref 0 in
    for _ = lo to hi - 1 do acc := !acc + game n done;
    !acc
  in
  let checksum =
    if num_domains <= 1 then run_games 0 num_games
    else begin
      let chunk = (num_games + num_domains - 1) / num_domains in
      let ds = Array.init num_domains (fun k ->
        let lo = k * chunk in
        let hi = min num_games (lo + chunk) in
        Domain.spawn (fun () -> run_games lo hi)) in
      Array.fold_left (fun acc d -> acc + Domain.join d) 0 ds
    end
  in
  Printf.printf "%d\n" checksum
