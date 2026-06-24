(* par_alloc — parallel allocation churn (GC axis: parallel nursery + GC-worker
 * scaling + STW coordination across domains).
 *
 * N domains each run an independent allocation-churn loop (the same kind of
 * short-lived 3-word cons-cell churn as the sequential `alloc` bench). The total
 * work is FIXED (N_TOTAL cells split evenly across domains), so:
 *   - wall time at DOMAINS=1 vs DOMAINS=k measures parallel speedup, and
 *   - the checksum is DOMAINS-INDEPENDENT (a deterministic per-cell value summed
 *     over the same global index range regardless of how it's partitioned), so
 *     1 vs 4 domains must produce identical output -> a correctness self-check
 *     that the parallel split is sound.
 *
 *   par_alloc N_TOTAL [DOMAINS]      DOMAINS also taken from env DOMAINS
 *                                    (defaults: N_TOTAL=8_000_000, DOMAINS=1) *)

let per_cell_value i = (i * 2654435761) land 0x3FFFFFFF

(* Each domain churns cells for global indices [lo, hi), keeping only a tiny
   sliding window live, and returns a partial checksum over [lo,hi). *)
let worker lo hi =
  let window = 16 in
  let ring = Array.make window 0 in   (* hold derived ints; cells are transient *)
  let checksum = ref 0 in
  for i = lo to hi - 1 do
    let v = per_cell_value i in
    (* allocate a real short-lived cons cell referencing a fresh box *)
    let cell = (v, ring.(i land (window - 1))) in
    ring.(i land (window - 1)) <- fst cell;
    checksum := (!checksum + fst cell) land 0x3FFFFFFF
  done;
  !checksum

let () =
  let n_total = try int_of_string Sys.argv.(1) with _ -> 8_000_000 in
  let domains =
    try int_of_string Sys.argv.(2)
    with _ -> (try int_of_string (Sys.getenv "DOMAINS") with _ -> 1)
  in
  let domains = max 1 domains in
  (* even split of the global index range across domains *)
  let chunk = (n_total + domains - 1) / domains in
  let spawned =
    Array.init domains (fun d ->
      let lo = d * chunk in
      let hi = min n_total (lo + chunk) in
      Domain.spawn (fun () -> worker lo hi))
  in
  let total = Array.fold_left (fun acc dom -> (acc + Domain.join dom) land 0x3FFFFFFF)
                0 spawned in
  Printf.printf "par_alloc n=%d domains=%d checksum=%d\n" n_total domains total
