# Adreno KGSL Performance-Counter Tools

Experimental Android command-line tools for reading raw Adreno A8xx hardware
performance counters through the KGSL userspace ioctl interface.

The repository provides two phone-side programs:

- `adreno_perf_stream` activates selected counters and continuously records
  their deltas.
- `adreno_perf_sweep` repeatedly launches a caller-supplied workload while
  sweeping selected counter groups in hardware-sized chunks.

These are low-level research tools. They report raw, system-wide hardware
counter values; they do not attribute activity to a process, API, or kernel.
Read [docs/limitations.md](docs/limitations.md) before interpreting results.

## Tested platform

| Component | Tested configuration |
| --- | --- |
| Phone | OnePlus CPH2653 (`OP5D55L1`) |
| SoC | Qualcomm SM8750 |
| GPU | Adreno 830 v2 |
| Android | Android 15, API 35 |
| Kernel | `6.6.30-android15-8-gb5f0c188ea2a-ab12656338-4k` |
| Build toolchain | Android NDK r27d, target API 35 |

Other devices and kernels have not been validated. Counter group IDs, slot
capacities, availability, and semantics can vary between KGSL implementations.

## Requirements

Host:

- macOS or Linux
- Android NDK with an AArch64 toolchain
- GNU Make
- Python 3
- ADB

Device:

- An Adreno GPU exposed through KGSL
- `/dev/kgsl-3d0`, or another compatible path supplied on the command line
- A kernel implementing the KGSL perfcounter `GET`, `READ`, and `PUT` ioctls
- Root access or equivalent permission to use those ioctls

No Mesa build is needed at runtime. The repository includes the counter XML
used to generate its C table.

## Repository layout

```text
src/                         Streamer and sweeper C sources
include/a8xx_perf_table.inc  Generated counter-name table
data/                        Mesa/Freedreno source XML and license metadata
tools/                       Counter-table generator
scripts/deploy.sh            Build and install both tools with ADB
scripts/pull_latest_sweep.sh Pull the newest sweep without merging old results
docs/limitations.md          Compatibility and interpretation cautions
```

Build products and pulled results are intentionally ignored by Git.

## Build

Set the NDK path and build both AArch64 programs:

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
make all
```

The default target API is 35. Override it if required:

```bash
make API=34 all
```

You can also pass the NDK path directly:

```bash
make NDK=/path/to/android-ndk all
```

Outputs:

```text
build/adreno_perf_stream
build/adreno_perf_sweep
```

The Makefile discovers the NDK host-toolchain directory instead of assuming a
macOS- or Linux-specific host tag.

## Deploy

With one ADB device connected:

```bash
make deploy
```

The default installation directory is:

```text
/data/local/tmp/adreno-perf-tools/bin
```

The deploy helper accepts overrides:

```bash
ADB=/path/to/adb \
REMOTE_DIR=/data/local/tmp/my-adreno-tools \
scripts/deploy.sh
```

Run the programs through `su -c` when root is required.

## Stream selected counters

List or search available counters without opening the KGSL device:

```bash
adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_stream --list-presets'"

adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_stream -l alu'"
```

Stream two exact counters every 250 ms:

```bash
adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_stream \
  -n -i 0.25 SP_BUSY_CYCLES SP_ALU_WORKING_CYCLES'"
```

Press Ctrl+C to stop. The program releases every successfully activated
counter before exiting.

By default, each run creates a unique CSV under:

```text
/data/local/tmp/adreno-perf-tools/streams
```

Specify an exact phone-side output path with `-o`:

```bash
adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_stream \
  -n -i 0.25 \
  -o /data/local/tmp/adreno-perf-tools/streams/my_run.csv \
  SP_BUSY_CYCLES SP_ALU_WORKING_CYCLES'"
```

Existing output files are never overwritten. The `--csv` option changes the
terminal format; a CSV file is saved regardless.

The streamer does not start a workload. Start the streamer first, run the
workload separately, and then stop the streamer.

### Streamer CSV format

```text
elapsed_s,SP_BUSY_CYCLES,SP_ALU_WORKING_CYCLES
0.257070,0,0
0.519492,427407,273038
```

Each counter column is an unsigned delta from the preceding read. `elapsed_s`
uses a monotonic clock and is measured from the start of the sampling loop.

## Sweep counter groups around a workload

Inspect the built-in A830 sweep plan:

```bash
adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_sweep --list-plan'"
```

Run an arbitrary trusted workload once per counter chunk:

```bash
adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_sweep \
  --time 2 \
  --interval 0.005 \
  --benchmark-cmd \"/data/local/tmp/my_benchmark --size 1024\"'"
```

`--benchmark-cmd` is required. The public tool has no built-in benchmark and no
dependency on a particular Vulkan, OpenCL, or ML program.

The command is executed by the Android shell, normally while the sweeper is
running as root. Pass trusted commands only.

Repeat the same command in bursts:

```bash
adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_sweep \
  --time 3 \
  --bursts 10 \
  --burst-sleep 0.1 \
  --benchmark-cmd \"/data/local/tmp/my_benchmark\"'"
```

Run a width sequence by placing `{width}` in the command:

```bash
adb shell "su -c '/data/local/tmp/adreno-perf-tools/bin/adreno_perf_sweep \
  --time 4 \
  --widths 128,256,512,1024 \
  --width-sleep 0.1 \
  --benchmark-cmd \"/data/local/tmp/my_benchmark --size {width}\"'"
```

If `{width}` is absent, the sweeper appends `--width <value>` instead.

By default, a benchmark that outlives the sampling window is terminated. Use
`--no-kill-benchmark` to wait for it instead.

## Sweep output

Each run creates a unique directory resembling:

```text
/data/local/tmp/adreno-perf-tools/sweeps/sweep_YYYYMMDD_HHMMSS_PID/
├── summary.csv
├── run_config.txt
├── 01_CP/
│   ├── CP_chunk001.csv
│   ├── CP_chunk001_meta.txt
│   └── CP_chunk001_benchmark.log
└── ...
```

The metadata records activation results, selected counters, row count, and the
benchmark exit status. `summary.csv` indexes all chunks.

Pull the newest sweep into the ignored local `results/` directory:

```bash
scripts/pull_latest_sweep.sh
```

The script refuses to merge into an existing local run. Override its paths if
needed:

```bash
REMOTE_ROOT=/data/local/tmp/my-sweeps \
DEST_ROOT=/path/to/local/results \
scripts/pull_latest_sweep.sh
```

## Validate the source tree

Run all host-side checks:

```bash
make check
```

This regenerates the counter table for comparison, checks both C sources with
the host compiler, and validates the shell scripts. It does not require an
Android device.

Regenerate the committed table after intentionally changing its XML or group
mapping:

```bash
make table
make table-check
```

## Counter-table provenance

`data/a8xx_perfcntrs.xml` was copied from Mesa/Freedreno at commit
`4027f06090eec398fab0d4facaa431c4104ec367`. The generated C include is committed
so ordinary builds do not need a Mesa checkout.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for exact source paths and
licensing. Do not assume every Mesa file uses the same license.

## License

Original code in this repository is licensed under the [MIT License](LICENSE),
Copyright (c) 2026 Jerry Yun.

Mesa/Freedreno data and derived counter names retain their upstream copyright
and license notice as described in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
