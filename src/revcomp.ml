(* The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 * Based on the Paolo Ribeca / Roman Kashitsyn OCaml entry.
 *
 * Parallelism: the CLBG entry forks per FASTA section with pipe handshaking to
 * keep output ordered. This port reads all sections, then uses Domain.spawn to
 * reverse-complement each section in parallel, joining in input order so the
 * output is identical. Reads a FASTA stream on stdin. *)

let comp = Array.make 256 0
let () =
  for i = 0 to 255 do comp.(i) <- i done;
  let pairs =
    [ ('A','T'); ('C','G'); ('G','C'); ('T','A'); ('U','A');
      ('M','K'); ('R','Y'); ('W','W'); ('S','S'); ('Y','R'); ('K','M');
      ('V','B'); ('H','D'); ('D','H'); ('B','V'); ('N','N') ]
  in
  List.iter
    (fun (a, b) ->
       comp.(Char.code (Char.lowercase_ascii a)) <- Char.code b;
       comp.(Char.code (Char.uppercase_ascii a)) <- Char.code b)
    pairs

let width = 60

(* reverse-complement [seq] and wrap at [width] columns. *)
let revcomp_wrapped seq =
  let l = Bytes.length seq in
  let out = Buffer.create (l + l / width + 2) in
  let col = ref 0 in
  for i = l - 1 downto 0 do
    Buffer.add_char out (Char.unsafe_chr comp.(Char.code (Bytes.get seq i)));
    incr col;
    if !col = width then (Buffer.add_char out '\n'; col := 0)
  done;
  if !col > 0 then Buffer.add_char out '\n';
  Buffer.contents out

let read_sections () =
  let sections = ref [] in
  let cur_hdr = ref None and cur = Buffer.create 65536 in
  let flush () =
    match !cur_hdr with
    | Some h -> sections := (h, Bytes.of_string (Buffer.contents cur)) :: !sections; Buffer.clear cur
    | None -> ()
  in
  (try
     while true do
       let line = input_line stdin in
       if String.length line > 0 && line.[0] = '>' then (flush (); cur_hdr := Some line)
       else Buffer.add_string cur line
     done
   with End_of_file -> ());
  flush ();
  List.rev !sections

let () =
  let sections = read_sections () in
  let domains =
    List.map (fun (hdr, seq) -> Domain.spawn (fun () -> (hdr, revcomp_wrapped seq))) sections
  in
  List.iter
    (fun dom -> let (hdr, body) = Domain.join dom in print_string hdr; print_char '\n'; print_string body)
    domains
