(* TEST
 set MMTK_HEAP_SIZE_MB = "32";
 set MMTK_CHECK_RUNNING = "lost";
 include systhreads;
 hassysthreads;
 {
   bytecode;
 }{
   native;
 }
*)

(* GH issue 24: a thread that holds its domain's master lock and runs OCaml
   must find the domain in MMTk's RUNNING set; otherwise a collection started
   by another domain treats the domain as safe-stopped and scans the live
   stack of a running thread.

   Each worker domain runs a compute systhread, which builds and re-checks
   boxed structures, computes [fib] and calls [Thread.yield], next to a
   blocker systhread that keeps entering a blocking section
   ([Thread.delay]), and a churning domain keeps collections coming. The
   master lock keeps passing from a thread entering a blocking section to a
   thread waiting in [Thread.yield] (GAP-4: the domain used to be marked
   STOPPED by the releasing thread and nothing marked it RUNNING again) and
   between the two threads through blocking sections (GAP-1: the releasing
   thread's STOPPED mark, made after the unlock, could erase the RUNNING
   mark of the thread that had just taken the lock). Before the fix this
   program crashed or reported CORRUPTION in most runs.

   MMTK_CHECK_RUNNING=lost turns on the runtime's RUNNING-set check (see
   runtime/caml/mmtk.h), which aborts at the first thread found running
   OCaml while its domain has lost its RUNNING mark, naming the check site
   and who last marked the domain STOPPED. Violations by a domain that has
   had no RUNNING edge since it was bound (GAP-6: the native main domain
   used to run until its first blocking section or collection without one)
   are counted and printed at exit, which also fails the test. *)

let ndom = 4
let rounds = 2000

let rec build acc n =
  if n = 0 then acc else build ((n, Some (float_of_int n)) :: acc) (n - 1)

let verify l =
  let rec go i = function
    | [] -> i = 301
    | (k, Some f) :: tl -> k = i && f = float_of_int i && go (i + 1) tl
    | (_, None) :: _ -> false
  in
  go 1 l

let rec fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)

let compute () =
  let ok = ref true in
  let keep = ref [] in
  for r = 1 to rounds do
    let l = build [] 300 in
    if r land 7 = 0 then keep := l :: (if r land 255 = 0 then [] else !keep);
    Thread.yield ();
    if fib 22 <> 17711 then ok := false;
    if not (verify l) then ok := false;
    List.iter (fun l -> if not (verify l) then ok := false) !keep
  done;
  !ok

let blocker stop () =
  while not (Atomic.get stop) do Thread.delay 1e-4 done

let domain_body () =
  let stop = Atomic.make false in
  let res = ref false in
  let b = Thread.create (blocker stop) () in
  let c = Thread.create (fun () -> res := compute ()) () in
  Thread.join c;
  Atomic.set stop true;
  Thread.join b;
  !res

let () =
  let ds = List.init ndom (fun _ -> Domain.spawn domain_body) in
  let stop = Atomic.make false in
  let churner = Domain.spawn (fun () ->
    let k = ref [] in
    while not (Atomic.get stop) do
      k := Array.make 32 0 :: (if List.length !k > 64 then [] else !k)
    done) in
  let results = List.map Domain.join ds in
  Atomic.set stop true;
  Domain.join churner;
  print_endline (if List.for_all Fun.id results then "ok" else "CORRUPTION")
