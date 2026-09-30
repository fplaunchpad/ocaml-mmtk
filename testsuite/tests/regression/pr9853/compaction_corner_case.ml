(* MMTk DISABLED: calls Gc.compact 25001 times; each is now a synchronous full
   stop-the-world collection (about 0.5 ms each locally, more on a slow CI
   runner), so the bytecode variant exceeds the 120 s CI timeout. The test
   targets a stock-compactor corner case that MMTk has no counterpart for.
   [semantic-timing] *)
(* TEST
 no-tsan; (* Takes too much time with tsan *)
 {
 bytecode;
 }
 {
 native;
 }
*)

(* Compaction crash when there is only one heap chunk and it is fully used. *)
let c = ref []

let () =
  for i = 0 to 25000 do
    c := 0 :: !c;
    Gc.compact ()
  done
