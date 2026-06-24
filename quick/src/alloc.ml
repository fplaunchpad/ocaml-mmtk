(* alloc — allocation-throughput microbench (GC axis: nursery / minor-alloc).
 *
 * A tight loop that allocates a huge volume of small, short-lived heap blocks
 * (3-word cons cells) that are dead almost immediately. This is the no-zero /
 * nursery probe: nearly all allocation is dead-on-arrival, so the cost is
 * dominated by the bump-allocate + (with always-on zeroing) the zero-fill, and
 * the minor/nursery GC reclaiming the garbage. A no-zero allocation change
 * should show up here most strongly.
 *
 * Deterministic, self-checking: a running checksum folded over a value derived
 * from each allocated cell. Output is a single line so it can have a golden.
 *
 *   alloc N        allocate ~N cons cells (default 1_000_000)
 *
 * We keep a tiny sliding window of recently-allocated cells live (so the
 * optimiser can't delete the allocation and the cells are genuinely reachable
 * for a moment), then drop them — the whole population is short-lived. *)

type cell = { v : int; next : cell option }

let () =
  let n = try int_of_string Sys.argv.(1) with _ -> 1_000_000 in
  let checksum = ref 0 in
  (* sliding window: keep the last [window] cells alive, then they fall out of
     scope and become garbage. Small so the live set stays tiny. *)
  let window = 16 in
  let ring = Array.make window None in
  for i = 0 to n - 1 do
    (* derive a value with cheap arithmetic so it's not constant-folded *)
    let v = (i * 2654435761) land 0x3FFFFFFF in
    let prev = ring.(i land (window - 1)) in
    let c = { v; next = prev } in
    ring.(i land (window - 1)) <- Some c;
    (* fold the cell's value into the checksum so the allocation is observed *)
    checksum := (!checksum + c.v) land 0x3FFFFFFF;
    (match c.next with
     | Some p -> checksum := (!checksum lxor (p.v lsr 1)) land 0x3FFFFFFF
     | None -> ())
  done;
  Printf.printf "alloc n=%d checksum=%d\n" n !checksum
