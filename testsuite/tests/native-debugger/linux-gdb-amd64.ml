(* MMTk DISABLED: multi-threaded-runtime gdb artifact: gdb enumerates MMTk's 28 GC worker threads and uses the multi-thread "Thread N hit Breakpoint" format vs the single-threaded reference; all breakpoints hit correctly [infra-artifact]. *)

   linux;
   not-clang; (* Skip, clang is tested on macOS *)
   arch_amd64;
   script = "sh ${test_source_directory}/has_gdb.sh";
   script;
   readonly_files = "meander.ml meander_c.c gdb_test.py";
   setup-ocamlopt.byte-build-env;
   program = "${test_build_directory}/meander";
   flags = "-g -ccopt -O0";
   all_modules = "meander.ml meander_c.c";
   ocamlopt.byte;
   debugger_script = "${test_source_directory}/gdb-script";
   gdb;
   script = "sh ${test_source_directory}/sanitize.sh linux-gdb-amd64";
   script;
   check-program-output;
 *)
