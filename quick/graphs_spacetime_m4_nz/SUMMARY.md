# Space-time sweep summary

host `KCs-Mac-mini.local`; source `spacetime-m4-nz.ndjson`. For each MMTk variant: the point on its lower-left front whose RSS is closest to vanilla's `default` point, and its wall ratio vs that vanilla point (<1 = MMTk faster). `class` compares the two fronts as step functions over their overlapping RSS range (ties within 2%): dominates / dominated / crossing / tie.

| bench | variant | vanilla default (MiB, s) | nearest MMTk front point (MiB, s) | knobs | time ratio | MMTk default (MiB, s) | class | failed pts |
|---|---|---|---|---|---|---|---|---|
| binarytrees | mmtk:Bactrian | 91, 1.507 | 101, 1.740 | heap=64 | 1.15× | 178, 1.296 | dominated | 8 |
| binarytrees | mmtk:GenImmix | 91, 1.507 | 94, 1.641 | heap=64,nursery=Fixed:33554432 | 1.09× | 167, 1.215 | dominated | 6 |
| binarytrees | mmtk:Immix | 91, 1.507 | 90, 2.413 | heap=64,nursery=Fixed:4194304 | 1.60× | 153, 2.486 | dominated | 6 |
| kb | mmtk:Bactrian | 8, 0.398 | 22, 0.477 | heap=48,nursery=Fixed:4194304 | 1.20× | 27, 0.438 | dominated | 0 |
| kb | mmtk:GenImmix | 8, 0.398 | 23, 0.487 | heap=128,nursery=Fixed:4194304 | 1.22× | 31, 0.454 | dominated | 0 |
| kb | mmtk:Immix | 8, 0.398 | 44, 0.433 | heap=32 | 1.09× | 61, 0.425 | dominated | 0 |
| matrix_multiplication | mmtk:Bactrian | 19, 0.760 | 31, 0.581 | heap=256,nursery=Fixed:33554432 | 0.76× | 33, 0.598 | faster only at higher RSS | 0 |
| matrix_multiplication | mmtk:GenImmix | 19, 0.760 | 27, 0.580 | heap=96,nursery=Fixed:33554432 | 0.76× | 48, 0.642 | faster only at higher RSS | 0 |
| matrix_multiplication | mmtk:Immix | 19, 0.760 | 30, 0.578 | heap=128,nursery=Fixed:33554432 | 0.76× | 31, 0.596 | faster only at higher RSS | 0 |
| LU_decomposition | mmtk:Bactrian | 17, 0.817 | 26, 0.844 | heap=128,nursery=Fixed:4194304 | 1.03× | 31, 0.813 | dominated | 0 |
| LU_decomposition | mmtk:GenImmix | 17, 0.817 | 26, 0.843 | heap=64,nursery=Fixed:4194304 | 1.03× | 31, 0.813 | dominated | 0 |
| LU_decomposition | mmtk:Immix | 17, 0.817 | 47, 1.025 | heap=32 | 1.25× | 48, 1.030 | dominated | 0 |
| chameneos_redux | mmtk:Bactrian | 37, 1.328 | 54, 3.330 | heap=256,nursery=Fixed:4194304 | 2.51× | 64, 3.278 | dominated | 0 |
| chameneos_redux | mmtk:GenImmix | 37, 1.328 | 58, 2.993 | heap=48,nursery=Fixed:4194304 | 2.25× | 65, 2.775 | dominated | 0 |
| chameneos_redux | mmtk:Immix | 37, 1.328 | 74, 1.016 | heap=32,nursery=Fixed:33554432 | 0.77× | 75, 1.007 | faster only at higher RSS | 0 |

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
