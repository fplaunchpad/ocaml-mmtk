(* MMTk DISABLED: heap-pressure vs allocation pacing: the test's ~1 GB churn is almost entirely LOS-direct large arrays (96 KB >= the 16 KB LOS threshold) with only ~2 MB young allocation, so at a large pinned heap (CI: MMTK_HEAP_SIZE_MB=4096) MMTk legitimately runs ZERO collections before the program ends and no Gc.create_alarm can fire (verified 0 GCs at 4096 MB; passes at 512 MB where the heap fills — alarm delivery itself works whenever a collection happens). Stock paces major cycles by allocated words, so its alarm always fires regardless of heap headroom [semantic-timing]. *)

let success () = exit 0
let failure () = failwith "The end was reached without triggering the GC alarm"

let () =
  let _ = Gc.create_alarm success in
  let g = Array.init 120000 (fun i -> Array.init 1 (fun i -> i)) in
  for i = 0 to 10000 do
    let a = Array.init 12000 (fun i -> i) in
    g.(i) <- a;
    a.(0) <- 42;
  done;
  failure ()
