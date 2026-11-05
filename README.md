# Nestdaq Analyzer

Online analysis tools for the NestDAQ data acquisition system.

> **Status: under development.** The generic part (data format, DAQ health, per-channel monitor, file replay)
> and the E88 monitor build and are tested with generated data. The FairMQ device also builds and receives synthetic time frames over an isolated IPC channel. Live DAQ inputs remain untested.

## What it does

NestDAQ assembles the data of the front-end modules (FEMs, for example HR-TDC and LR-TDC boards) into
time frames. The online analyzer reads these time frames and fills histograms that a shift member can watch
in a browser (THttpServer / JSROOT), together with warnings:

| Page | Contents |
|---|---|
| Summary | status per check (DAQ, channels, detectors, trigger, analyzer), list of warnings, time frames seen by the analyzer |
| DAQ | time frame flags, words per module and type, heartbeat delimiter flags, missing or repeated heartbeat frames, data rate per module |
| Channels/&lt;module&gt; | hit pattern, rate, ToT and time within the heartbeat frame for every channel; hot channels and channels that went silent |
| E88/&lt;detector&gt; | (E88 only) hit pattern per segment and side, ToT, two-ended coincidences and their time difference, time relative to the nearest SC mean time |
| E88/Trigger | (E88 only) T1 and B2TT record counts and rates, T1 - SC mean time, B2TT record time - T1, event id steps |

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
├── experiments/
│   └── e88/           E88 monitor: parameter files, B2TT records, E88Module
├── apps/              nda-replay (reads files, no FairMQ)
├── devices/           nda-online-monitor (FairMQ device, built only when FairMQ is found)
├── test/              unit tests
├── docs/              online-input.md
├── external/
│   └── nestdaq-user-impl/   data format headers (git submodule, pinned)
├── NestdaqTFSlicer/, analyzer/   original devices (see below)
└── README.md
```

Experiment-specific modules live in `experiments/<experiment>/`. They are only active when their parameters are
given; their parameter files (detector definition, channel map, settings) are kept outside this repository.

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

### Multi-word records

Some boards put records of several 64-bit words into their stream: a header word with its own 6-bit type and a
word count in bits 49:44, followed by payload words whose upper bits are arbitrary. Give their type with
`--record-type <type>` so that the payload is not read as TDC or delimiter words (the E88 monitor registers the
type of its B2TT records itself).

### E88 monitor

```bash
build/bin/nda-replay --input run000123.dat --param <param dir>/index.txt --http http:<port>
# or give the files one by one
build/bin/nda-replay --input run000123.dat --detector-def detectors.txt --channel-map channel-map.txt --settings settings.txt
```

File formats (whitespace separated text, `#` starts a comment, `#!` lines are directives):

| File | Header | Columns |
|---|---|---|
| detector definition | `#! format detector_def v1` | `detector module layer n_segment n_side edges [description]` |
| channel map | `#! format channel_map v1`, `#! valid_runs <first> <last>` | `fem_ip ch detector module layer segment side name` |
| settings | `#! format settings v1` | `key value [value ...]` |
| parameter index | `#! format param_index v1` | `kind first_run last_run required file` (kinds `detector_def`, `channel_map`, `settings`) |

Settings used by the monitor: `tdc_lsb_ns`, `b2tt_data_type`, `sc_pair_window`, `mrpc_pair_window`,
`t1_search`, `b2tt_search` and `window.<detector>` (range of the time relative to the SC mean time).
The channel named `T1` is the E88 trigger. Times are formed within one time frame, so coincidences that
straddle two time frames are lost (T1 and B2TT records are matched across time frames).

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
