# Space-time sweep summary

host `KCs-Mac-mini.local`; source `spacetime-m4.ndjson`. For each MMTk variant: the point on its lower-left front whose RSS is closest to vanilla's `default` point, and its wall ratio vs that vanilla point (<1 = MMTk faster). `class` compares the two fronts as step functions over their overlapping RSS range (ties within 2%): dominates / dominated / crossing / tie.

| bench | variant | vanilla default (MiB, s) | nearest MMTk front point (MiB, s) | knobs | time ratio | MMTk default (MiB, s) | class | failed pts |
|---|---|---|---|---|---|---|---|---|
| binarytrees | mmtk:Bactrian | 91, 1.636 | 159, 1.856 | heap=64 | 1.13× | 233, 1.368 | dominated | 8 |
| binarytrees | mmtk:GenImmix | 91, 1.636 | 156, 1.791 | heap=64,nursery=Fixed:33554432 | 1.09× | 221, 1.284 | dominated | 6 |
| binarytrees | mmtk:Immix | 91, 1.636 | 132, 2.581 | heap=64,nursery=Fixed:33554432 | 1.58× | 194, 2.749 | dominated | 6 |
| kb | mmtk:Bactrian | 8, 0.454 | 82, 0.505 | heap=32,nursery=Fixed:4194304 | 1.11× | 87, 0.471 | dominated | 0 |
| kb | mmtk:GenImmix | 8, 0.454 | 82, 0.519 | heap=128,nursery=Fixed:4194304 | 1.14× | 91, 0.483 | dominated | 0 |
| kb | mmtk:Immix | 8, 0.454 | 86, 0.465 | heap=32,nursery=Fixed:4194304 | 1.02× | 103, 0.450 | dominated | 0 |
| matrix_multiplication | mmtk:Bactrian | 19, 0.744 | 97, 0.590 | heap=256,nursery=Fixed:33554432 | 0.79× | 98, 0.596 | faster only at higher RSS | 0 |
| matrix_multiplication | mmtk:GenImmix | 19, 0.744 | 41, 0.587 | heap=192,nursery=Fixed:33554432 | 0.79× | 112, 0.649 | faster only at higher RSS | 0 |
| matrix_multiplication | mmtk:Immix | 19, 0.744 | 77, 0.587 | heap=64,nursery=Fixed:4194304 | 0.79× | 78, 0.596 | faster only at higher RSS | 0 |
| LU_decomposition | mmtk:Bactrian | 17, 0.795 | 86, 0.854 | heap=96,nursery=Fixed:4194304 | 1.07× | 91, 0.827 | dominated | 0 |
| LU_decomposition | mmtk:GenImmix | 17, 0.795 | 86, 0.856 | heap=64,nursery=Fixed:4194304 | 1.08× | 90, 0.825 | dominated | 0 |
| LU_decomposition | mmtk:Immix | 17, 0.795 | 98, 1.045 | heap=32 | 1.32× | 98, 1.041 | dominated | 0 |
| chameneos_redux | mmtk:Bactrian | 37, 1.370 | 120, 3.391 | heap=128,nursery=Fixed:4194304 | 2.48× | 124, 3.253 | dominated | 0 |
| chameneos_redux | mmtk:GenImmix | 37, 1.370 | 118, 3.105 | heap=dyn,nursery=Fixed:4194304 | 2.27× | 127, 2.858 | dominated | 0 |
| chameneos_redux | mmtk:Immix | 37, 1.370 | 117, 1.053 | heap=32,nursery=Fixed:4194304 | 0.77× | 118, 1.112 | faster only at higher RSS | 0 |

## Failed points

| bench | variant | knobs | status | exit | last stderr line |
|---|---|---|---|---|---|
| binarytrees | mmtk:Bactrian | heap=32 | err | -11 |  |
| binarytrees | mmtk:Bactrian | heap=32,nursery=Fixed:33554432 | err | -11 |  |
| binarytrees | mmtk:Bactrian | heap=32,nursery=Fixed:4194304 | err | -11 |  |
| binarytrees | mmtk:Bactrian | heap=48 | err | -11 |  |
| binarytrees | mmtk:Bactrian | heap=48,nursery=Fixed:33554432 | err | -11 |  |
| binarytrees | mmtk:Bactrian | heap=48,nursery=Fixed:4194304 | err | -11 |  |
| binarytrees | mmtk:Bactrian | heap=64,nursery=Fixed:33554432 | err | -11 |  |
| binarytrees | mmtk:Bactrian | heap=96,nursery=Fixed:33554432 | err | -11 |  |
| binarytrees | mmtk:GenImmix | heap=32 | err | -11 |  |
| binarytrees | mmtk:GenImmix | heap=32,nursery=Fixed:33554432 | err | -11 |  |
| binarytrees | mmtk:GenImmix | heap=32,nursery=Fixed:4194304 | err | -11 |  |
| binarytrees | mmtk:GenImmix | heap=48 | err | -11 |  |
| binarytrees | mmtk:GenImmix | heap=48,nursery=Fixed:33554432 | err | -11 |  |
| binarytrees | mmtk:GenImmix | heap=48,nursery=Fixed:4194304 | err | -11 |  |
| binarytrees | mmtk:Immix | heap=32 | err | 2 | Fatal error: exception Out_of_memory |
| binarytrees | mmtk:Immix | heap=32,nursery=Fixed:33554432 | err | 2 | Fatal error: exception Out_of_memory |
| binarytrees | mmtk:Immix | heap=32,nursery=Fixed:4194304 | err | 2 | Fatal error: exception Out_of_memory |
| binarytrees | mmtk:Immix | heap=48 | err | 2 | Fatal error: exception Out_of_memory |
| binarytrees | mmtk:Immix | heap=48,nursery=Fixed:33554432 | err | 2 | Fatal error: exception Out_of_memory |
| binarytrees | mmtk:Immix | heap=48,nursery=Fixed:4194304 | err | 2 | Fatal error: exception Out_of_memory |
