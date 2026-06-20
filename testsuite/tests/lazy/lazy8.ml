(* Disabled under MMTk: lazy values not yet supported (tabled — see ROADMAP workstream E). *)
exception E

let main () =
  let l = lazy (raise E) in

  begin try Lazy.force_val l with
  | E -> ()
  end;

  begin try Lazy.force_val l with
  | Lazy.Undefined -> ()
  end;

  let d = Domain.spawn (fun () ->
    begin try Lazy.force_val l with
    | Lazy.Undefined -> ()
    end)
  in
  Domain.join d;
  print_endline "OK"

let _ = main ()
