(* Disabled under MMTk: lazy values not yet supported (tabled — see ROADMAP workstream E). *)
open Domain

let () =
  let l = lazy (print_string "Lazy Forced\n") in
  let d = spawn (fun () -> Lazy.force l) in
  join d
