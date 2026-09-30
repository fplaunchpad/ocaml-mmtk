#include <stddef.h>
#include <caml/mlvalues.h>

extern size_t mmtk_ocaml_total_gc_count(void);
extern size_t mmtk_ocaml_gc_count(void);

/* Completed MMTk pauses, and completed full-heap pauses, read without
   allocating: an allocation could give a still-pending collection a chance
   to run before the caller's check. */
CAMLprim value test_completed_pauses(value unit)
{
  return Val_long(mmtk_ocaml_total_gc_count());
}

CAMLprim value test_completed_full_pauses(value unit)
{
  return Val_long(mmtk_ocaml_gc_count());
}
