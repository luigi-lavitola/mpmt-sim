# mpmt-sim

Simulated mPMT hardware, so `mpmt-mss` and `mpmt-daq-interface` can be run,
driven and tested on a laptop without a Zynq board, an I2C bus, a Modbus
UART or a DMA-proxy driver.

**The production repos are dependencies, never copies.** Nothing in
`mpmt-mss` or `mpmt-daq-interface` is patched or forked. This repo supplies
only what sits underneath them, so their code cannot drift away from what
is being tested.

## Status

| Part | State |
|---|---|
| FEB / Modbus | working, verified |
| FPGA registers | working, verified |
| Housekeeping sensors | working, verified |
| Event payload generator | working, verified |
| `sim-producer` (ToolDAQ node + data path) | working, verified |
| mss slow controls + monitoring on the node | working, verified |

A simulated mPMT registers with ServiceDiscovery as `mPMT001`, exposes
Start/Stop/Status run control and streams DAQHeader-wrapped blocks at ~10
messages per second:

```
1,172.23.237.105,6001,mPMT001,Ready
```

## What is simulated, and how faithfully

| Seam | Real | Simulated | Production code still exercised |
|---|---|---|---|
| FEB | Modbus RTU on `/dev/ttyPS1` | Modbus **TCP** server | `ModbusManager`, `FEBChannel`, `PMTChannel`, `LEDChannel` — all of it |
| FPGA | `/dev/uio0` UIO mapping | 64 KiB regular file at the same path | the whole `FPGA` class, every register decode |
| Sensors | I2C via `smbus2` | generated readings | metric tables only — **chip drivers are not exercised** |

The first two seams keep the real code in the loop entirely. The FEB one is
the strongest: `ModbusConfig` already supports `mode="tcp"`, so the actual
Modbus protocol runs, register by register.

The sensor seam is the weak one. Faking `smbus2` faithfully would mean
emulating BME280 calibration blobs and TLA2024 conversion registers — that
tests the drivers, not the DAQ. So `SimHouseKeeping` fakes one layer higher
and reuses the real `ALIAS_MAP`/`METRIC_MAP`/`OPERATION_MAP` so labels,
units and derived metrics stay in sync. **Do not use this to validate a
sensor driver change.**

## Running it

```bash
docker compose up -d --build
curl -s http://localhost:8000/rpc/methods | python3 -m json.tool
```

That serves 120 JSON-RPC methods — 27 `fpga.*`, 92 `febmgr.*`, 1 `sensors.*`
— on `http://localhost:8000/rpc`, the same surface the real mss exposes.

Point `mpmt-daq-interface` at it by setting `mss_url` in its `MSSConfig`:

```
mss_url http://localhost:8000/rpc
```

No other change is needed on that side.

### Example

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

The HV ramps toward the setpoint at 40 V per step rather than snapping to
it, passing through the `RUP`/`RDN` status words, so a slow-control write
produces the same observable sequence real hardware would.

## Configuration

| Variable | Default | Meaning |
|---|---|---|
| `MPMT_SIM_FEB_HOST` | `127.0.0.1` | Modbus TCP host of the FEB server |
| `MPMT_SIM_UIO` | `/dev/uio0` | backing file for the FPGA registers |
| `MPMT_SIM_PORT` | `8000` | HTTP port for the RPC service |
| `MPMT_SIM_PMT_MASK` | `0x7FFFF` | FPGA register 103: bit *x* set ⇒ channel *x+1* is a PMT, clear ⇒ LED |

### Two things that will bite you

**The UIO file must be at `/dev/uio0`.** `FEBManager` constructs its own
`FPGA('/dev/uio0')` inside its constructor, with the path hardcoded, so the
backing file has to be at exactly that path for it and the top-level
instance to share registers. Inside a container `/dev` is a writable tmpfs,
which is why this needs no patch and no privileged device mapping.

**The PMT mask must be set before mss starts.** `FEBManager` reads register
103 once, in its constructor. Leave it zero and all nineteen channels are
configured as LEDs — every `getPMT*` call then fails with
`Channel N (LEDChannel) has no ...`. `fake_uio.create()` writes it
synchronously for this reason; the animator thread deliberately never
touches it.

## Layout

```
src/mpmt_sim/
  main.py           composition root — mirrors mpmt_mss.main line for line
  feb_sim.py        Modbus TCP server modelling PMT and LED boards
  fake_uio.py       backing file + telemetry animator for the FPGA
  fake_sensors.py   SimHouseKeeping, drop-in for the I2C housekeeping
docker/Dockerfile   runtime; mounts both repos rather than baking them in
compose.yml         feb-sim + mss-sim on a private bridge network
```

The bridge network is not incidental: `ModbusManager` hardcodes port 502,
so the FEB server cannot be moved off it, and a private network avoids
colliding with anything else on the host.

## The event payload

`src/payload/` generates the words the FPGA would deliver over DMA, in the
mPMT data format: 3-word PMT hits (head / subhit / tail) with a 48-bit
timestamp split across all three, plus one 4-word Tr32 event per simulated
second carrying ratemeter, humidity, temperature and dead time. The bit
layout is documented field by field at the top of `mpmt_payload.h`.

`tests/test_payload.cpp` generates a full block, decodes it back and checks
every field — framing tags, channel coverage, timestamp reassembly, ADC
range and Tr32 cadence.

Two things are deliberately not right yet, both flagged in the code:

- **CRC is always zero.** The 4-bit field exists in both tails; the
  algorithm has not been specified.
- **Event type F does not fit.** The format sheet lists PPS as type `F`,
  which needs four bits, but the event type field is three bits wide in
  both the PMT hit and the Tr32 head. The Tr32 event currently goes out as
  `0b111`.

## Running a simulated node

```bash
mkdir build && cd build && cmake .. && make
cd ../config && ../build/sim-producer \
    --daq-config ./InterfaceConfig --data-config ./DataSenderConfig \
    --mss-config ./MSSConfig
```

Needs `libcurl` and `nlohmann_json` development packages, which the mss
client links. `--disable-mss` drops both and builds the data path alone.

### One node, the whole mPMT

With `--mss-config` pointing at the simulated mss the node is a complete
mPMT as far as the rest of ToolDAQ is concerned: it publishes **252 slow
control variables** on its `sc_port` — 190 PMT channel parameters plus the
fpga controls and run control — and sends an mss monitoring snapshot every
`monitor_period_sec`. `MSSIntegration.cpp` and `MSSSlowControl.cpp` are
compiled from mpmt-daq-interface, so this is the production slow control
surface, not a stand-in.

Writing through it reaches the simulated hardware end to end:

```
slow control :6001 -> sim-producer -> MSSSlowControl -> mss client (HTTP)
  -> mss-sim -> PMTChannel -> Modbus TCP -> feb-sim

pmt2_voltage_set 1350  =>  V=1040 RUP -> V=1200 RUP -> V=1350 UP
```

That is what makes per-object monitoring and control pages possible without
hardware: every variable a real mPMT exposes is here, with plausible values
that respond to being written.

Press Start from the WebServer control UI to move it from `Ready` to
`Running` and begin streaming. `--disable-rc` starts sending immediately.

Running several nodes on one host means giving each its own `sc_port`,
`remote_port`, `alert_*_port`, `data_port`, `UUID_path` **and `cache_file`**.
On real hardware none of this applies: each mPMT is its own machine, so the
identical config works everywhere and only `device_name` changes.

### Open: more than one node needs a bridge network

Host networking works for a single node and does not scale, for a reason
beyond the port edits above.

`DAQUtilities::UpdateConnections` keys the RBU's connections by
`ip + ":" + port`. Every node sharing the host's network namespace also
shares its IP, so two nodes on the same `data_port` collapse to one key and
**the second is skipped silently** — no error on either side, the RBU simply
reports one fewer `connected_mpmt` than you started. Giving each node its own
`data_port` avoids it, but that is one more way the test setup diverges from
the field, where every mPMT has its own IP and they all use 8888.

A bridge network fixes both problems at once: each container gets its own IP,
so the *identical* config runs on every node exactly as it would on real
hardware, and nothing collides. The work it needs:

- move WebServer, middleman and the RBU off `network_mode: host` onto the
  same bridge — they are on host networking today
- confirm ServiceDiscovery's multicast (239.192.1.1) crosses that bridge;
  Linux bridges normally pass it, but IGMP snooping on the docker bridge is
  worth checking rather than assuming
- decide how many nodes are worth running: measured here, one simulated mPMT
  costs about 145 MB (16 MB producer, 36 MB mss, 93 MB feb), so the ceiling
  is RAM long before it is CPU, unless the nodes are actually generating data

> **Do not leave a node streaming with no RBU attached.** See the disk cache
> note below — it will fill the disk.

### The device name must start with `MPMT`, capitalised

`readouttesting`'s MPMTReceive finds its senders with
`socket_manager.Update("MPMT", data_port)`, and
`DAQUtilities::UpdateConnections` matches that against `device_name` with a
case-sensitive prefix compare. A node named `mPMT001` is invisible to the
RBU: it never connects, and neither side logs anything. The DPB boards
follow the same rule with `DPB001` and `Update("DPB")`.

## Running the RBU

`docker/Dockerfile.rbu` builds `readouttesting` from the local checkout —
its own Dockerfile clones from the private GitLab, which needs credentials
inside the build.

```bash
cd ..                                     # the directory holding both repos
docker build -f mpmt-sim/docker/Dockerfile.rbu -t rbu:local .
docker run -d --name rbu --network host rbu:local \
    bash -c 'cd /opt/readouttesting && . ./Setup.sh && ./main configfiles/Dummy/ToolChainConfig'
```

Host networking is required: ServiceDiscovery is multicast, and the RBU has
to see the node's beacon to connect to it.

Verified end to end with a node streaming at ~10 messages per second:

```
RBU       connected_mpmt => 1
          rec & reply MPMT 107 2000
node      num_data_akn 148 / 150,  num_data_cached 0
```

**What this does not prove.** The active chain is MPMTReceive → MPMTTest,
which acks and checks the header is 12 bytes but never looks at the
payload. `MPMTDecode` is not in `ToolsConfig`, and would not work if it
were: it is a byte-for-byte copy of `DPBDecode` (two `printf` strings
apart — the mPMT one still prints `"received DPB"`) and decodes the IDOD
format. The word tags collide rather than overlap, so it would misparse
silently instead of failing:

| tag | IDOD | mPMT |
|---|---|---|
| `0b11` | hit / ped object | tail |
| `0b10` | sync object | head |

So the transport, the DAQ header, the ack/resend path and run control are
all exercised. Payload decoding on the RBU side is not.

## Notes for upstream

Four things found while building this, none fixed here:

- **`config/InterfaceConfig` uses key names the code never reads.** It
  spells them `sd_address` / `sd_port`, but both `mpmt-daq-interface`'s own
  `src/DAQInterface.cpp` and the upstream libDAQInterface look for
  `service_discovery_address` / `service_discovery_port`. The result is an
  empty multicast address, `inet_addr()` rejecting it, and startup dying
  with `setsockopt mreq: Invalid argument`. This may well be what the README
  describes as end-to-end testing being "blocked by a ServiceDiscovery bug".

- **The DataSender disk cache grows without bound when nothing acks.**
  `DataSender.cpp` recovers a cached message whenever the send queue is
  empty, and re-appends it on the next failure. With no RBU the message
  cycles cache → send → fail → cache forever, appending a fresh copy each
  time. Measured here: **7.4 GB written for 250 distinct 32 KB messages**,
  roughly a thousand rewrites each, in about two minutes. `num_data_cached`
  hides it because it is a net counter — incremented on write, decremented
  on recovery — so it sat at ~157 the whole time.

- `FPGA.__init__` calls `self.perror(...)` when the UIO device is missing,
  but the class has no `perror` method — the real error is replaced by an
  `AttributeError` (`mpmt-mss`, `runcontrol/fpga.py:15`).
- `FPGA.startAcquisition` shells out to a hardcoded
  `/opt/mpmt-readout/build/evproducer`. Missing here, which it handles
  gracefully, but it makes the acquisition path untestable without that
  binary in place.
