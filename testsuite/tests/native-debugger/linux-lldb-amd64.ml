(* MMTk DISABLED: multi-threaded-runtime lldb artifact (sibling of the disabled
   linux-gdb-amd64): MMTk runs background GC worker threads, so lldb's `bt all`
   enumerates each worker's stack (frames in mmtk::scheduler::*, futex_wait,
   spawn_gc_thread, ...) on top of the program's, diverging from the
   single-threaded stock reference. The program backtrace itself is correct; the
   extra frames are the always-on GC's threads. [infra-artifact]. *)
