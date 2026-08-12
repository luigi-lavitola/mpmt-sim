"""Housekeeping sensors without an I2C bus.

Unlike the FEB and FPGA seams, this one cannot reuse the production code
path. The real HouseKeeping.discovery() probes an SMBus and instantiates
per-chip driver classes whose readAll() walks device registers; standing
that up faithfully would mean emulating BME280 calibration blobs and
TLA2024 conversion registers, which tests the drivers rather than anything
the DAQ cares about.

So this fakes one layer higher: it produces the same readings structure
HouseKeeping.read() returns, reusing the real ALIAS_MAP / METRIC_MAP /
OPERATION_MAP so labels, units, rounding and derived metrics stay in sync
with the production tables. The chip register decode is the part that is
not exercised here — worth remembering before trusting this to validate a
sensor driver change.
"""

import math
import random
import time

from mpmt_mss.rpc import rpc_service, rpc_method
from mpmt_mss.sensors.housekeeping import (
    ALIAS_MAP,
    METRIC_MAP,
    OPERATION_MAP,
)

# Nominal value per metric label, used as the centre of the wander.
NOMINAL = {
    "+5.0V": 5.0,
    "+3.3V": 3.3,
    "IpoeA": 0.35,
    "IpoeB": 0.32,
    "T": 24.0,
    "P": 1013.0,
    "H": 45.0,
    "X": 0.0,
    "Y": 0.0,
    "Z": 1.0,
}

# Fractional wander applied to the nominal value.
NOISE = {
    "+5.0V": 0.01,
    "+3.3V": 0.01,
    "IpoeA": 0.05,
    "IpoeB": 0.05,
    "T": 0.02,
    "P": 0.002,
    "H": 0.05,
    "X": 0.5,
    "Y": 0.5,
    "Z": 0.05,
}


@rpc_service(name="HouseKeeping")
class SimHouseKeeping:
    """Drop-in replacement for mpmt_mss.sensors.HouseKeeping.

    Registered under the same "sensors" service name, so callers — including
    MSSIntegration's monitoring poll — cannot tell the difference from the
    RPC surface.
    """

    def __init__(self, addresses=None):
        # Default to every device the real DEVICE_MAP knows about, i.e. an
        # mPMT where discovery found the full sensor complement.
        self.addresses = list(addresses if addresses is not None else METRIC_MAP.keys())
        self.readings = {}
        self._t0 = time.monotonic()

    def _value(self, label):
        nominal = NOMINAL.get(label, 1.0)
        noise = NOISE.get(label, 0.05)

        # A slow sine keeps successive samples correlated, so plots look like
        # a drifting quantity rather than white noise.
        phase = (time.monotonic() - self._t0) / 120.0
        drift = math.sin(phase + hash(label) % 7) * noise
        jitter = random.uniform(-noise, noise) * 0.3

        if nominal == 0.0:
            return drift + jitter

        return nominal * (1.0 + drift + jitter)

    @rpc_method
    def read(self):
        self.readings = {}

        for address in self.addresses:
            name = ALIAS_MAP.get(address, "")
            self.readings[name] = []
            converted = []

            for metric in METRIC_MAP[address]:
                value = round(self._value(metric.label), 2)
                self.readings[name].append(
                    {"label": metric.label, "unit": metric.unit, "value": value}
                )
                converted.append(value)

            for operation in OPERATION_MAP.get(address, []):
                self.readings[name].append({
                    "label": operation.label,
                    "unit": operation.unit,
                    "value": round(
                        operation.op(converted[operation.index1],
                                     converted[operation.index2]), 2
                    ),
                })

        return self.readings
