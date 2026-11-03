# Nestdaq Analyzer

Online analysis tools for the NestDAQ data acquisition system.

> **Status: under development.** The generic part (data format, DAQ health, per-channel monitor, file replay)
> builds and is tested with generated data. The FairMQ device has not been compiled yet.
> Experiment-specific monitors (detectors, trigger) are being added.

## What it does

NestDAQ assembles the data of the front-end modules (FEMs, for example HR-TDC and LR-TDC boards) into
time frames. The online analyzer reads these time frames and fills histograms that a shift member can watch
in a browser (THttpServer / JSROOT), together with warnings:

| Page | Contents |
|---|---|
| Summary | status per check (DAQ, channels, detectors, trigger, analyzer), list of warnings, time frames seen by the analyzer |
| DAQ | time frame flags, words per module and type, heartbeat delimiter flags, missing or repeated heartbeat frames, data rate per module |
| Channels/&lt;module&gt; | hit pattern, rate, ToT and time within the heartbeat frame for every channel; hot channels and channels that went silent |

The same monitor modules run on any input: a file written by the FileSink (`nda-replay`), or a FairMQ channel
of the running DAQ (`nda-online-monitor`). See [docs/online-input.md](docs/online-input.md).

## Layout

```
NestdaqAnalyzer/
├── CMakeLists.txt
├── core/
│   ├── format/        time frame parsing, TDC word decoding, FileSink reader (no ROOT, no FairMQ)
│   ├── monitor/       module interface, histogram registry, engine, warnings (ROOT)
│   └── modules/       generic modules: DaqHealthModule, ChannelModule
├── apps/              nda-replay (reads files, no FairMQ)
├── devices/           nda-online-monitor (FairMQ device, built only when FairMQ is found)
├── test/              unit tests
├── docs/              online-input.md
├── external/
│   └── nestdaq-user-impl/   data format headers (git submodule, pinned)
├── NestdaqTFSlicer/, analyzer/   original devices (see below)
└── README.md
```

Experiment-specific modules will go to `experiments/<experiment>/`.

## Build

Requirements: CMake 3.16 or newer, a C++17 compiler, ROOT with the `http` feature
(tested with ROOT 6.38 and gcc 14). FairMQ is only needed for the device.

```bash
git submodule update --init
cmake -S . -B build
cmake --build build -j
ctest --test-dir build        # unit tests
```

Executables are written to `build/bin/`. CMake options:

| Option | Default | Meaning |
|---|---|---|
| `NDA_BUILD_DEVICES` | `AUTO` | build `nda-online-monitor`: `AUTO` (if FairMQ is found), `ON`, `OFF` |
| `NDA_BUILD_LEGACY` | `OFF` | build the original devices (needs FairMQ and NestdaqUnpacker) |
| `NESTDAQ_UNPACKER_SRC_DIR` | (none) | `NestdaqUnpacker/src`, only for `NDA_BUILD_LEGACY` |
| `NESTDAQ_USER_IMPL_DIR` | `external/nestdaq-user-impl` | nestdaq-user-impl sources (data format headers) |
| `NDA_BUILD_TESTS` | `ON` | unit tests |

## Usage

```bash
# Process a file and write the histograms
build/bin/nda-replay --input run000123.dat --output monitor-run000123.root

# Watch a file that is still being written, serve the histograms on a port
build/bin/nda-replay --input run000124.dat --follow --http http:<port> --expect-fem <module IPs>
```

`nda-replay --help` lists all options (sampling, refresh interval, JSON summary, ...).

## Notes

- Include the nestdaq-user-impl headers only through `core/format/NestdaqHeaders.hh`:
  `TimeFrameHeader.h` sets `#pragma pack(2)` without restoring it, which also packs standard library
  classes declared afterwards and crashes at run time.
- The time unit of the raw TDC fields is assumed so that the field spans one heartbeat frame
  (high resolution: 2^-10 ns, low resolution: 1 ns). This is not yet confirmed with real data.

## Original devices

The first version of this repository provided two devices, kept until the new modules replace them:

* **NestdaqTFSlicer**: library for unpacking time frames and slicing data (`TFSlicerUnpacker`, `UnpackTdc`).
* **analyzer**: `OnlineAnalysisSlicer` (reads sliced time frames and creates histograms) and
  `OnlineAnalysisTFBuilder` (online event builder and analysis node).

They need FairMQ, ROOT, Boost and NestdaqUnpacker. Build them with
`cmake -S . -B build -DNDA_BUILD_LEGACY=ON -DNESTDAQ_UNPACKER_SRC_DIR=<path>/NestdaqUnpacker/src`.
Note that `UnpackTdc.h` decodes the heartbeat word with the older field layout
(flags in bits 57:48, frame number in bits 39:24), while the current firmware puts the frame number in bits 23:0
and the flags in bits 55:40 (`AmQStrTdcData.h`, namespace `v1`).
