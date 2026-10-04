# Limitations and Interpretation Notes

These tools expose raw hardware counters. Successful collection does not, by
itself, establish what a counter means for a particular workload or API.

## Compatibility is narrow

The current implementation has been tested on one OnePlus CPH2653 with an
SM8750 SoC, Adreno 830 v2, Android 15, and a vendor KGSL 6.6.30 kernel.

The following can differ on another device or kernel:

- KGSL perfcounter availability and permissions
- Counter group IDs
- Physical counter-slot capacities
- Supported selectors
- Counter names and semantics
- Register allocation behavior

The generator's group-ID mapping and the sweeper's slot-capacity plan are
therefore compatibility data, not universal A8xx specifications. Validate them
before using the tools on another target.

## Counters are system-wide

The tools request raw KGSL hardware counters and do not filter by process,
context, command buffer, Vulkan queue, or OpenCL queue. Samples can include:

- The intended workload
- Display composition
- Android UI rendering
- Other GPU clients
- Driver or firmware activity

The streamer observes activity but does not launch, synchronize, or isolate the
workload. Use controlled experiments and idle baselines.

## Counter names are not conclusions

A name such as `SP_ALU_WORKING_CYCLES` does not prove that it captures every
kind of arithmetic executed by every API. A counter that responds to a Vulkan
calibration workload may behave differently for OpenCL, graphics, firmware, or
another shader path.

Do not classify a workload as compute-bound or memory-bound from one counter.
Calibrate candidate counters with controlled ALU-heavy and memory-heavy
workloads, check scaling, and compare multiple signals.

## Samples are deltas

CSV counter values are unsigned differences between consecutive 64-bit reads.
Unsigned wraparound is intentional. They are not normalized percentages or
rates.

Sampling intervals are requested delays, not real-time guarantees. Scheduling,
ioctl, terminal, and file-I/O overhead can change the actual interval; use the
recorded monotonic `elapsed_s` values when computing rates.

Very short intervals increase overhead and may perturb the workload.

## Hardware slots are shared

Each group has a limited number of physical counter slots. Other profilers or
processes can reserve them concurrently. Activation can therefore fail even
when a selector is valid.

The streamer reports failed activations and continues with counters it obtained.
The sweeper records `GET_FAIL` entries in chunk metadata and proceeds when at
least one counter was activated.

Avoid running multiple counter tools at the same time unless contention is the
behavior being tested.

## Sweeps are not simultaneous measurements

The sweeper divides each counter group into chunks and launches the workload
again for every chunk. Counters in different chunks were measured during
different executions.

This requires a deterministic, repeatable workload. Thermal state, DVFS,
background activity, caches, and one-time initialization can drift across a
full sweep. Randomize or repeat experiments when comparing counters across
chunks.

The tested plan currently produces 141 chunks. Even a short per-chunk sampling
window therefore executes the benchmark many times.

## Benchmark commands invoke a shell

`adreno_perf_sweep` passes `--benchmark-cmd` to the Android shell through
`system()`. When the sweeper runs as root, the command also runs with elevated
privileges.

Use trusted, correctly quoted commands only. Never pass untrusted input from a
file, network request, or another user.

By default, the sweeper terminates a benchmark process group that outlives the
sampling window. `--no-kill-benchmark` waits instead and can make a chunk block
indefinitely if the workload hangs.

## Root and device stability

Root access can weaken Android's security model. Direct hardware-counter use
may interact with vendor driver assumptions, other profilers, suspend/resume,
or GPU reset behavior.

Use a development device, preserve important data, and monitor kernel logs when
testing new devices or counter mappings.
