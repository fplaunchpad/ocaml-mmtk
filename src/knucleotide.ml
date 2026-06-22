(* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 * Based on the Troestler/Fernandez/Le Fessant/Kashitsyn OCaml entry.
 *
 * Parallelism: the CLBG entry forks one worker per output task. This port uses
 * Domain.spawn per task. Because domains share one heap (unlike fork's
 * copy-on-write), each task builds its OWN local Hashtbl rather than mutating a
 * shared global one. k-mers up to length 31 pack into one int (64-bit).
 * Reads a FASTA stream on stdin (the section after ">THREE"). *)

let code = Array.make 256 0
let () =
  code.(Char.code 'A') <- 0; code.(Char.code 'a') <- 0;
  code.(Char.code 'T') <- 1; code.(Char.code 't') <- 1;
  code.(Char.code 'C') <- 2; code.(Char.code 'c') <- 2;
  code.(Char.code 'G') <- 3; code.(Char.code 'g') <- 3

let letter = [| 'A'; 'T'; 'C'; 'G' |]

(* dna as a string of 2-bit codes (0..3), read once, shared read-only. *)
let dna =
  let is_not_three s = String.length s < 6 || String.sub s 0 6 <> ">THREE" in
  (try while is_not_three (input_line stdin) do () done with End_of_file -> ());
  let buf = Buffer.create 1_000_000 in
  (try
     while true do
       let line = input_line stdin in
       if String.length line > 0 && line.[0] = '>' then raise End_of_file;
       String.iter (fun c -> Buffer.add_char buf (Char.unsafe_chr code.(Char.code c))) line
     done
   with End_of_file -> ());
  Buffer.contents buf

let count_table k =
  let h = Hashtbl.create 0x10000 in
  let n = String.length dna in
  if n >= k then
    for i = 0 to n - k do
      let key = ref 0 in
      for j = 0 to k - 1 do key := (!key lsl 2) lor Char.code (String.unsafe_get dna (i + j)) done;
      Hashtbl.replace h !key (1 + (match Hashtbl.find_opt h !key with Some c -> c | None -> 0))
    done;
  h

let unpack k key =
  let s = Bytes.create k in
  let key = ref key in
  for i = k - 1 downto 0 do
    Bytes.set s i letter.(!key land 3); key := !key lsr 2
  done;
  Bytes.unsafe_to_string s

let pack_seq seq =
  let key = ref 0 in
  String.iter (fun c -> key := (!key lsl 2) lor code.(Char.code c)) seq;
  !key

let write_frequencies k =
  let h = count_table k in
  let total = Hashtbl.fold (fun _ c acc -> acc + c) h 0 in
  let l = Hashtbl.fold (fun key c acc -> (unpack k key, 100. *. float c /. float total) :: acc) h [] in
  let cmp (k1, f1) (k2, f2) =
    if f1 > f2 then -1 else if f1 < f2 then 1 else String.compare k1 k2
  in
  String.concat "" (List.map (fun (s, f) -> Printf.sprintf "%s %.3f\n" s f) (List.sort cmp l))

let write_count seq =
  let h = count_table (String.length seq) in
  let c = match Hashtbl.find_opt h (pack_seq seq) with Some c -> c | None -> 0 in
  Printf.sprintf "%d\t%s" c seq

type task = Freq of int | Count of string

let () =
  let tasks =
    [ Freq 1; Freq 2; Count "GGT"; Count "GGTA"; Count "GGTATT";
      Count "GGTATTTTAATT"; Count "GGTATTTTAATTTATAGT" ]
  in
  let domains =
    List.map
      (fun t -> Domain.spawn (fun () -> match t with Freq k -> write_frequencies k | Count s -> write_count s))
      tasks
  in
  List.iter (fun dom -> print_endline (Domain.join dom)) domains
