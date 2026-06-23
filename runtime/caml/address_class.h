/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*              Damien Doligez, projet Para, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 1996 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

/* Classification of addresses for GC and runtime purposes. */

/* Multicore runtime supports only the "no naked pointers" mode where any
   out-of-heap pointers are not observable by the GC. The out-of-heap pointers
   are either:

   - wrapped in Abstract_tag or Custom_tag objects, or
   - have a valid header with colour `NOT_MARKABLE`, or
   - made to look like immediate values by tagging the least significant bit so
     that the GC does not follow it. This strategy has the downside that
     out-of-heap pointers may not point to odd addresses.

   A valid value is either:
   - a tagged integer (Is_long)
   - a pointer to the minor heap
   - a pointer to a well-formed block outside the minor heap. It may be in the
     major heap, or static data allocated by the OCaml code or the OCaml
     runtime, or a foreign pointer.

   To create a well-formed block outside the heap that the GC will not scan,
   one can use the Caml_out_of_heap_header from mlvalues.h.
*/

#ifndef CAML_ADDRESS_CLASS_H
#define CAML_ADDRESS_CLASS_H

#include "config.h"
#include "misc.h"
#include "mlvalues.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Under always-on MMTk there is no stock per-domain minor heap and no
   minor-heaps address-space reservation: native code allocates from a TLAB
   aliased onto an MMTk Immix block and bytecode allocates straight into MMTk,
   both OUTSIDE any reserved [caml_minor_heaps_start, caml_minor_heaps_end)
   range. So no live object is ever "young" in the stock sense and Is_young(val)
   is always false. We fold the constant here (the reservation + its bounding
   variables have been removed from domain.c); every former consumer therefore
   runs its always-false branch, which has been audited to be the MMTk-correct
   behaviour (MMTk owns the whole heap; the young/old split was the stock minor
   remembered set). The Is_block assertion side effect is retained. */

#define Is_young(val) \
  (CAMLassert (Is_block (val)), 0)

#define Is_block_and_young(val) (Is_block(val) && 0)

/* These definitions are retained for backwards compatibility with OCaml 4 */
#define Is_in_heap_or_young(a) 1
#define Is_in_value_area(a) 1

#ifdef __cplusplus
}
#endif

#endif /* CAML_ADDRESS_CLASS_H */
