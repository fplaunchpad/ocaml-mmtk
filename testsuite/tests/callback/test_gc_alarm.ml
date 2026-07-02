(* MMTk DISABLED: LOS-heavy churn runs 0 GCs at the CI 4 GB heap, so no Gc alarm can fire; passes at 512 MB [semantic-timing]. *)

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
