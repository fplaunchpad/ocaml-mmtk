(* MMTk DISABLED: stock compaction semantics.

   This test encodes the stock compactor's exact pacing: it asserts that one
   Gc.compact () performs *three* additional major collections
   (major_collections == before+3) and bumps the separate compactions counter by
   one (compactions == before+1). Under MMTk, Gc.compact () runs a single ordinary
   MMTk whole-heap collection (which Immix may defrag) — so major_collections
   rises by exactly one and there is no distinct compactions counter (it stays 0).
   MMTk has no stock-equivalent three-cycle compaction pass, so these counts can
   never match. Re-enable only under a stock-compatible compaction model. *)

let () =
  Gc.full_major (); (* do a major before compaction so there isn't any pending major
                  cycles when we do compaction. *)
  let heap_stats_before = Gc.quick_stat () in
  Gc.compact ();
  let heap_stats_after = Gc.quick_stat () in
  (* assert that we have done an additional three major collections *)
  assert (heap_stats_after.major_collections == heap_stats_before.major_collections+3);
  (* also a compaction! *)
  assert (heap_stats_after.compactions == heap_stats_before.compactions+1)
