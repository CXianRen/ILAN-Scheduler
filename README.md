# ILAN - The Interference- and Locality-Aware NUMA Scheduler
This repository is a fork of the official [LLVM project](https://github.com/llvm/llvm-project). It extends the OpenMP runtime, specifically taskloop constructs, to incorporate interference and data locality awareness for NUMA platforms. Refer to [README_LLVM](README_LLVM.md) for the original README.


## Core features
The main features of ILAN are the following:
- NUMA aware task distribution and stealing.
- Interference mitigation through moldability.
- Perf counter tracking on thread granularity.
These feature are explained in more detail in our master thesis.

### New files and their purpose
The core features of the ILAN scheduler is split into a couple of new files:
- **kmp_schedule.cpp and kmp_schedule.h**: The core scheduling decisions are made here. This is what interfaces with the standard runtime in most places.
- **kmp_routine.cpp and kmp_routine.h**: The optional moldability feature is implemented in this file.
- **kmp_perf.cpp and kmp_perf.h**: Implements the perf counter tracking functionality. Easily extended with more perf counters.
- **kmp_perf_objects.cpp and kmp_perf_objects.h**: AMD specific perf counters are grouped into a container in this file.
- **kmp_topo.cpp and kmp_topo.h**: Reads hardware topology information.


## Requirements
- The current implementation requires `hwloc` during runtime `(apt install libhwloc-dev hwloc)`.
- Requires processor support for BMI1 (Bit Manipulation Instruction Set 1).

## Limitations
- The scheduler is only verified to work on Linux platforms.
- No extensive testing has been done on different hardware topologies, which means there is a potential for incorrect behavior on certain hardware.
- ILAN requires 1-to-1 affinity (same as **OMP_PROC_BIND=true**), and therefore disregards all affinity settings normally used by the OpenMp runtime.


## Working with limited space
To limit the space usage of the llvm-project when working primarily with the openmp runtime, you can follow the below steps
to only clone the necessary folders.

```sh
git clone --filter=blob:none --no-checkout --depth 10 <llvm-project-url>
cd llvm-project
```
specify depth that you feel is necessary.

Checkout only the folders that are required for the openmp runtime.
```sh
git sparse-checkout init --cone
git sparse-checkout set openmp/ runtimes/ cmake/
git checkout main
```
