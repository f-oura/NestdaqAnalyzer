# Where the online monitor gets its data

The monitor modules are the same in all cases; only the input differs.
Three inputs are prepared. They have to be compared on the real DAQ before one is chosen.

| Input | Source | What the monitor sees | If the monitor is slow | Status |
|---|---|---|---|---|
| T1 | `dqm` channel of the TimeFrameBuilder | a copy of every complete time frame | the TimeFrameBuilder only sends when the monitor can receive, so time frames are dropped on the monitor side | **not tested** |
| T2 | `decimator` channel of the TimeFrameBuilder | one time frame out of `decimation-factor` | not checked yet | **not tested** |
| T5 | the file written by the FileSink | every time frame written to disk | the monitor falls behind; the DAQ is not affected | tested with a file written in chunks |

Two other outputs exist but are not used: the `dqm` channel of the STFBuilder blocks when the receiver is slow
(it could stall the DAQ), and the output of the trigger slicers is only available when a slicer runs in the DAQ.

## T5: follow the file written by the FileSink

No connection to the DAQ is needed, so this is the first thing to try on a running system.

```bash
nda-replay --input <file being written> --follow --http http:<port> --expect-fem <module IPs>
```

`--follow` waits for more data at the end of the file and stops at the FileSink trailer
(or after `--idle-timeout` seconds without new data). Time frames that are only partly written are waited for.

## T1 and T2: FairMQ device `nda-online-monitor`

The device reads multipart time frames from its input channel (default name `in`) and runs the same modules.
Options:

| Option | Default | Meaning |
|---|---|---|
| `--in-chan-name` | `in` | input channel |
| `--http` | (none) | THttpServer engine, e.g. `http:<port>` |
| `--update-sec` | `2` | refresh interval of rates and warnings |
| `--sample` | `1` | process one time frame out of N |
| `--expect-fem` | (none) | comma-separated module IPs that must send data |
| `--output-dir` | (none) | directory for the histogram file written at the end of each run |
| `--monitor-run-number` | `0` | run number used in the output file name and to select the E88 parameters |
| `--record-type` | (none) | comma-separated 6-bit types of multi-word records |
| `--e88-param` | (none) | E88 parameter index (enables the E88 monitor) |
| `--e88-detector-def`, `--e88-channel-map`, `--e88-settings` | (none) | E88 parameter files, override the index |

The NestDAQ topology is stored in redis as endpoints and links (see `script/topology-amq-stb.sh` in
nestdaq-user-impl). A sketch for T1, **not yet tested**:

```bash
# endpoint <service> <channel> <parameters>
endpoint  TimeFrameBuilder   dqm  type push method bind
endpoint  OnlineMonitor      in   type pull method connect
# link <service> <channel> <service> <channel>
link      TimeFrameBuilder   dqm  OnlineMonitor   in
```

For T2 use the `decimator` channel and set `decimation-factor` of the TimeFrameBuilder.
How the device is registered as a NestDAQ service, and how it learns the run number, still has to be checked.

Connecting a device to a running DAQ changes the DAQ topology. Agree on it with the people running the DAQ first.
