/* Standalone smoke test for the in-tree MMTk binding C ABI.
 * Links directly against libmmtk_ocaml.a (no OCaml runtime).
 * Proves: init NoGC, bind a mutator, allocate blocks, query the heap. */
#include "mmtk_ocaml.h"
#include <stdio.h>
#include <stdint.h>

int main(void) {
    mmtk_ocaml_init((size_t)64 * 1024 * 1024, "NoGC");

    /* Use a fake domain-state address as the mutator key. */
    MMTk_Mutator m = mmtk_ocaml_bind_mutator((uintptr_t)0x100000);

    int ok = 1;
    for (int i = 0; i < 1000; i++) {
        /* wosize=2, tag=0 (ordinary block) */
        void *p = mmtk_ocaml_alloc(m, 2, 0, 0);
        if (p == NULL) { printf("alloc %d returned NULL\n", i); ok = 0; break; }
        /* write the two fields (immediates) to touch the memory */
        ((uintptr_t *)p)[0] = 1; /* Val_int(0) */
        ((uintptr_t *)p)[1] = 3; /* Val_int(1) */
        if (!mmtk_ocaml_is_in_mmtk_spaces(p)) {
            printf("alloc %d: %p NOT in mmtk spaces\n", i, p);
            ok = 0; break;
        }
    }

    printf(ok ? "SMOKE OK: 1000 NoGC allocations in MMTk spaces\n"
              : "SMOKE FAIL\n");
    return ok ? 0 : 1;
}
