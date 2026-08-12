# mpmt-sim

Simulated mPMT hardware, so `mpmt-mss` and `mpmt-daq-interface` can be run,
driven and tested on a laptop — no Zynq board, no I2C bus, no Modbus UART, no
DMA-proxy driver.

It gives you a node that registers with ToolDAQ, exposes the same ~250 slow
control variables a real mPMT does, streams event data to the RBU, and answers
writes with plausible hardware behaviour.

**The production repos are dependencies, never copies.** Nothing in `mpmt-mss`
or `mpmt-daq-interface` is patched or forked; this repo supplies only what sits
underneath them.

## Quick start

### 1. The simulated mss

```bash
docker compose up -d --build
curl -s http://localhost:8000/rpc/methods | python3 -m json.tool
```

Two containers come up — the FEB Modbus server and mss — serving about 120
JSON-RPC methods on `http://localhost:8000/rpc`, the same surface the real mss
exposes.

Try it:

```bash
python3 - <<'EOF'
import json, urllib.request
def rpc(method, params=None):
    body = json.dumps({"jsonrpc":"2.0","id":1,"method":method,
                       "params":params or {}}).encode()
    req = urllib.request.Request("http://localhost:8000/rpc", data=body,
                                 headers={"Content-Type":"application/json"})
    return json.load(urllib.request.urlopen(req))

print(rpc("febmgr.getPMTStatus", {"channel": 1}))
rpc("febmgr.setPMTVoltageSet", {"channel": 2, "value": 1350})
print(rpc("febmgr.getPMTVoltage", {"channel": 2}))
EOF
```

The HV ramps toward the setpoint rather than snapping to it, passing through
the `RUP`/`RDN` status words, so a write produces the sequence real hardware
would.

### 2. The simulated node

Needs `libcurl` and `nlohmann_json` development packages, and the sibling
checkouts `mpmt-daq-interface`, `mpmt-mss` and `libToolDAQ`.

```bash
mkdir build && cd build && cmake .. && make
cd ../config && ../build/sim-producer \
    --daq-config ./InterfaceConfig --data-config ./DataSenderConfig \
    --mss-config ./MSSConfig
```

The node comes up as `MPMT001` in `Ready`. Press **Start** from the WebServer
control UI to begin streaming, or pass `--disable-rc` to start immediately.

Check it registered:

```bash
docker exec WebServer cat /tmp/table_file | grep MPMT
# 5,172.23.237.105,6001,MPMT001,Ready
```

### 3. Options

| Flag | Default | Meaning |
|---|---|---|
| `--daq-config` | `./InterfaceConfig` | DAQInterface config |
| `--data-config` | `./DataSenderConfig` | DataSender config |
| `--mss-config` | `./MSSConfig` | mss client config |
| `--rate` | `10000` | simulated hit rate, Hz |
| `--disable-rc` | off | skip run control, stream immediately |
| `--disable-mss` | off | drop the slow controls and monitoring, and with them libcurl |

| Environment | Default | Meaning |
|---|---|---|
| `MPMT_SIM_FEB_HOST` | `127.0.0.1` | Modbus TCP host of the FEB server |
| `MPMT_SIM_UIO` | `/dev/uio0` | backing file for the FPGA registers |
| `MPMT_SIM_PORT` | `8000` | HTTP port for the RPC service |
| `MPMT_SIM_PMT_MASK` | `0x7FFFF` | FPGA register 103: bit *x* set ⇒ channel *x+1* is a PMT |

> **Do not leave a node streaming with no RBU attached** — see *DataSender disk
> cache* under Notes for upstream. It will fill the disk.

## What it simulates

| Seam | Real hardware | Here |
|---|---|---|
| FEB | Modbus RTU on `/dev/ttyPS1` | Modbus **TCP** server, 19 PMT + 19 LED slaves |
| FPGA | `/dev/uio0` UIO mapping | 64 KiB regular file at the same path |
| Sensors | I2C via `smbus2` | generated readings |
| Event data | DMA-proxy device | synthetic payload in the mPMT data format |

## Layout

```
src/mpmt_sim/       the simulated mss
  main.py           composition root — mirrors mpmt_mss.main line for line
  feb_sim.py        Modbus TCP server modelling PMT and LED boards
  fake_uio.py       backing file + telemetry animator for the FPGA
  fake_sensors.py   SimHouseKeeping, drop-in for the I2C housekeeping
src/payload/        event generator in the mPMT data format
src/producer/       sim-producer, the ToolDAQ node
config/             InterfaceConfig, DataSenderConfig, MSSConfig
tests/              round-trip checks on the payload, and against the RBU decoder
docker/             runtime image, and one that builds the RBU
compose.yml         feb-sim + mss-sim on a private bridge network
```

---

# Details

## How faithful each seam is

The FEB and FPGA seams keep the real code in the loop entirely.

**FEB** is the strongest: `ModbusConfig` already supports `mode="tcp"`, so the
actual Modbus protocol runs register by register, and `ModbusManager`,
`FEBChannel`, `PMTChannel` and `LEDChannel` all execute unmodified.

**FPGA** works because `FPGA` only does `open()` + `mmap()`, and a regular file
behaves identically — so every register decode in that class is the production
one.

**Sensors** are the weak seam. Faking `smbus2` faithfully would mean emulating
BME280 calibration blobs and TLA2024 conversion registers, which tests the
drivers rather than the DAQ. `SimHouseKeeping` fakes one layer higher and
reuses the real `ALIAS_MAP`/`METRIC_MAP`/`OPERATION_MAP`, so labels, units and
derived metrics stay in sync. **Do not use it to validate a sensor driver
change.**

## The event payload

`src/payload/` generates the words the FPGA would deliver over DMA: 3-word PMT
hits (head / subhit / tail) with a 48-bit timestamp split across all three,
plus one 4-word Tr32 event per simulated second carrying ratemeter, humidity,
temperature and dead time. The bit layout is documented field by field at the
top of `mpmt_payload.h`.

`tests/test_payload.cpp` generates a block, decodes it and checks every field.
`tests/test_rbu_decode.cpp` goes further: it decodes the same buffer with
`readouttesting`'s `RAWMPMTHit`/`RAWMPMTTr32`, two implementations written from
the same format sheet without shared code, and fails if they ever disagree.

Two things are deliberately not right yet, both flagged in the code:

- **CRC is always zero.** The 4-bit field exists in both tails; the algorithm
  has not been specified.
- **Event type F does not fit.** The format sheet lists PPS as type `F`, which
  needs four bits, but the field is three bits wide. Tr32 goes out as `0b111`.

## One node is the whole mPMT

With `--mss-config` pointing at the simulated mss, the node publishes **252
slow control variables** on its `sc_port` — 190 PMT channel parameters plus
fpga and run control — and sends an mss monitoring snapshot every
`monitor_period_sec`. `MSSIntegration.cpp` and `MSSSlowControl.cpp` are
compiled from mpmt-daq-interface, so this is the production slow control
surface, not a stand-in.

A write reaches the simulated hardware end to end:

```
slow control :6001 -> sim-producer -> MSSSlowControl -> mss client (HTTP)
  -> mss-sim -> PMTChannel -> Modbus TCP -> feb-sim

pmt2_voltage_set 1350  =>  V=1040 RUP -> V=1200 RUP -> V=1350 UP
```

That is what makes per-object monitoring and control pages possible without
hardware.

### It joins the run lifecycle

Run control in ToolDAQ is **broadcast**, not addressed node by node: the
WebServer's run page sends alerts and whoever subscribed reacts. A node that
does not subscribe simply never starts or stops with the run.

Of the four alerts a run start sends, the framework handles two on its own:

| Alert | Handled by |
|---|---|
| `CacheConfig` | the middleman |
| `LoadConfig` | the framework — fetches this device's resolved config into `m_local_config`, sets `NewConfig` |
| `ChangeConfig` | the framework, calling the `SetChangeConfigFunc` callback, and driving `Config` through `ChangeStart` → `ChangeEnd`/`ChangeFail` |
| `RunStart` | **nobody** — no hook exists, so it is subscribed by hand |

`sim-producer` therefore subscribes to `RunStart` directly and registers
`SetRunStopFunc`, which also clears the cached base/runmode config ids so the
next `ChangeConfig` re-runs rather than being skipped as a no-op.

Verified both ways round — the run alerts and the manual buttons drive the
same state, so they cannot disagree:

```
sendalert RunStart  =>  MPMT001,Running   and blocks start flowing
sendalert RunStop   =>  MPMT001,Stopped   and they stop
```

**`producer.cpp` does not do this yet.** It sets `SetChangeConfigFunc` and
nothing else, so a real mPMT reacts to a configuration change but ignores run
start and stop. That is the piece to port back.

## Running the RBU

`docker/Dockerfile.rbu` builds `readouttesting` from the local checkout — its
own Dockerfile clones from the private GitLab, which needs credentials inside
the build.

```bash
cd ..                                     # the directory holding both repos
docker build -f mpmt-sim/docker/Dockerfile.rbu -t rbu:local .
docker run -d --name rbu --network host rbu:local \
    bash -c 'cd /opt/readouttesting && . ./Setup.sh && ./main configfiles/Dummy/ToolChainConfig'
```

Host networking is required: ServiceDiscovery is multicast, and the RBU has to
see the node's beacon.

Verified with a node streaming at ~10 messages per second:

```
RBU       connected_mpmt => 1
          rec & reply MPMT 107 2000
node      num_data_akn 148 / 150,  num_data_cached 0
```

**What that proves, and what it does not.** The transport, the DAQ header, the
ack/resend path and run control are all exercised. Payload decoding is not: the
active chain is MPMTReceive → MPMTTest, which acks and checks the header is 12
bytes but never looks at the payload.

`MPMTDecode` in `readouttesting` now does decode the mPMT format correctly —
that work is done and cross-checked against this repo's generator — but it is
left out of `ToolsConfig` because nothing in that toolchain drains
`aggrigation_buffer`, so enabling it leaks memory at about 0.44 MiB/s. See the
comments in its `ToolsConfig`.

## Gotchas

**The device name must start with `MPMT`, capitalised.** `MPMTReceive` finds
its senders with `socket_manager.Update("MPMT", data_port)`, and
`DAQUtilities::UpdateConnections` matches that against `device_name` with a
case-sensitive prefix compare. A node called `mPMT001` is invisible to the RBU:
it never connects, and neither side logs anything. The DPB boards follow the
same rule with `DPB001` and `Update("DPB")`.

**The UIO file must be at `/dev/uio0`.** `FEBManager` constructs its own
`FPGA('/dev/uio0')` inside its constructor, with the path hardcoded, so the
backing file has to be at exactly that path for it and the top-level instance
to share registers. Inside a container `/dev` is a writable tmpfs, which is why
this needs no patch and no privileged device mapping.

**The PMT mask must be set before mss starts.** `FEBManager` reads register 103
once, in its constructor. Leave it zero and all nineteen channels are
configured as LEDs — every `getPMT*` call then fails with
`Channel N (LEDChannel) has no ...`. `fake_uio.create()` writes it
synchronously for this reason; the animator thread never touches it.

## Open: more than one node needs a bridge network

Host networking works for a single node and does not scale.

Running several nodes on one host already means giving each its own `sc_port`,
`remote_port`, `alert_*_port`, `data_port`, `UUID_path` and `cache_file`. But
there is a worse problem: `DAQUtilities::UpdateConnections` keys the RBU's
connections by `ip + ":" + port`. Every node sharing the host's network
namespace shares its IP, so two nodes on the same `data_port` collapse to one
key and **the second is skipped silently** — no error on either side, the RBU
just reports one fewer `connected_mpmt` than you started.

A bridge network fixes both at once: each container gets its own IP, so the
*identical* config runs on every node exactly as in the field. The work it
needs:

- move WebServer, middleman and the RBU off `network_mode: host` onto the same
  bridge — they are on host networking today
- confirm ServiceDiscovery's multicast (239.192.1.1) crosses that bridge; Linux
  bridges normally pass it, but IGMP snooping on the docker bridge is worth
  checking rather than assuming
- decide how many nodes are worth running: one simulated mPMT costs about
  145 MB (16 MB producer, 36 MB mss, 93 MB feb), so RAM is the ceiling long
  before CPU — unless the nodes are actually generating data

## Notes for upstream

Found while building this, none fixed here:

- **`mpmt-daq-interface`'s `config/InterfaceConfig` uses key names the code
  never reads.** It spells them `sd_address` / `sd_port`, but both its own
  `src/DAQInterface.cpp` and the upstream libDAQInterface look for
  `service_discovery_address` / `service_discovery_port`. The result is an empty
  multicast address, `inet_addr()` rejecting it, and startup dying with
  `setsockopt mreq: Invalid argument`. This may well be what its README
  describes as end-to-end testing being "blocked by a ServiceDiscovery bug".

- **The DataSender disk cache grows without bound when nothing acks.**
  `DataSender.cpp` recovers a cached message whenever the send queue is empty
  and re-appends it on the next failure, so with no RBU a message cycles
  cache → send → fail → cache forever, appending a fresh copy each time.
  Measured: **7.4 GB written for 250 distinct 32 KB messages** in about two
  minutes. `num_data_cached` hides it because it is a net counter, so it sat
  at ~157 the whole time.

- `FPGA.__init__` calls `self.perror(...)` when the UIO device is missing, but
  the class has no `perror` method — the real error is replaced by an
  `AttributeError` (`mpmt-mss`, `runcontrol/fpga.py:15`).

- `FPGA.startAcquisition` shells out to a hardcoded
  `/opt/mpmt-readout/build/evproducer`. Missing here, which it handles
  gracefully, but it makes the acquisition path untestable without that binary.
