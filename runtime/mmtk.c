/**************************************************************************/
/*                                                                        */
/*                MMTk garbage collector glue (bytecode)                  */
/*                                                                        */
/**************************************************************************/

/* Implementation of the bytecode<->MMTk glue. Compiled only into the
 * bytecode runtime (runtime_BYTECODE_ONLY_C_SOURCES). All of it is behind
 * #ifndef NATIVE_CODE so a stray native build is a no-op. */

#define CAML_INTERNALS

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "caml/config.h"

#ifndef NATIVE_CODE

#include "caml/mlvalues.h"
#include "caml/domain_state.h"
#include "caml/misc.h"
#include "caml/mmtk.h"

/* The in-tree MMTk binding's C ABI (gc/mmtk/include/mmtk_ocaml.h). */
#include "../gc/mmtk/include/mmtk_ocaml.h"

int caml_mmtk_enabled = 0;

static int caml_mmtk_initialised = 0;

/* Objects this size (bytes) or larger are routed to MMTk's large object
 * space. Conservative: smaller than the smallest line/block in collecting
 * plans, so it is also correct for Immix later. */
#define CAML_MMTK_LOS_THRESHOLD (16 * 1024)

/* AllocationSemantics codes shared with the Rust ABI (see api.rs). */
#define CAML_MMTK_SEM_DEFAULT 0
#define CAML_MMTK_SEM_LOS     2

void caml_mmtk_init(void)
{
  if (caml_mmtk_initialised) return;

  const char *plan = getenv("MMTK_PLAN");
  if (plan == NULL || plan[0] == '\0') plan = "NoGC";

  size_t heap_mb = 1024;
  const char *heap_env = getenv("MMTK_HEAP_SIZE_MB");
  if (heap_env != NULL && heap_env[0] != '\0') {
    long v = strtol(heap_env, NULL, 10);
    if (v > 0) heap_mb = (size_t)v;
  }

  mmtk_ocaml_init(heap_mb * 1024 * 1024, plan);
  caml_mmtk_initialised = 1;

  if (getenv("MMTK_VERBOSE") != NULL)
    fprintf(stderr, "[mmtk] initialised: plan=%s heap=%zuMiB\n", plan, heap_mb);
}

/* MMTk is opt-in during bring-up: it manages the heap only when MMTK_ENABLED is
   set to something other than "0"/empty. Default off, so a normal build and the
   self-hosting compiler bootstrap run on OCaml's stock GC. NoGC in particular
   cannot sustain the compiler build (it never reclaims), so always-on would
   break `make`. Enable explicitly to exercise MMTk:
     MMTK_ENABLED=1 [MMTK_PLAN=NoGC] [MMTK_HEAP_SIZE_MB=1024] ./runtime/ocamlrun prog.byte */
static int caml_mmtk_wanted(void)
{
  const char *e = getenv("MMTK_ENABLED");
  return e != NULL && e[0] != '\0' && strcmp(e, "0") != 0;
}

void caml_mmtk_domain_init(caml_domain_state *dom)
{
  if (!caml_mmtk_wanted()) return;  /* stock GC unless explicitly enabled */
  caml_mmtk_init();
  dom->mmtk_mutator = mmtk_ocaml_bind_mutator((uintptr_t)dom);
  caml_mmtk_enabled = 1;
}

Caml_inline int caml_mmtk_semantics(mlsize_t wosize)
{
  size_t bytes = (size_t)(Whsize_wosize(wosize)) * sizeof(value);
  return bytes >= CAML_MMTK_LOS_THRESHOLD ? CAML_MMTK_SEM_LOS
                                          : CAML_MMTK_SEM_DEFAULT;
}

/* Note on the header: the binding writes (wosize << 10) | tag, which is the
   correct OCaml header for all non-mixed blocks (the wosize shift is fixed at
   HEADER_TAG_BITS + HEADER_COLOR_BITS = 10, independent of any reserved bits,
   which sit above wosize and stay 0). Mixed blocks (which use reserved bits)
   are not handled yet; NoGC never scans, so this is correct for M1.
   TODO(M2/mixed-blocks): thread `reserved` through the binding's alloc ABI. */

value caml_mmtk_alloc_small(mlsize_t wosize, tag_t tag, reserved_t reserved)
{
  (void)reserved;
  return (value)mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag,
                                 CAML_MMTK_SEM_DEFAULT);
}

value caml_mmtk_alloc_shr(mlsize_t wosize, tag_t tag, reserved_t reserved)
{
  (void)reserved;
  return (value)mmtk_ocaml_alloc(Caml_state->mmtk_mutator, wosize, tag,
                                 caml_mmtk_semantics(wosize));
}

#endif /* NATIVE_CODE */
