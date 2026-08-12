"""Modbus TCP server standing in for the mPMT front-end boards.

This is the highest-fidelity seam in the whole simulator: mpmt-mss talks to
the FEBs over Modbus, and ModbusConfig already supports mode="tcp", so the
real ModbusManager, FEBChannel, PMTChannel and LEDChannel code runs
unmodified against this server. Nothing in mpmt-mss is patched — only the
transport is pointed here instead of /dev/ttyPS1.

Register layout follows what the real channel classes read and write:

  PMT (slaves 1..19, see FEBManager.configure)
    6         status word          0=UP 1=DOWN 2=RUP 3=RDN 4=TUP 5=TDN 6=TRIP
    0x07      temperature          high byte = degrees, low byte = 1/1000 ths
    0x23      rate ramp up
    0x24      rate ramp down
    0x26      HV setpoint (volts)  writable, validated 25..1500 by mss
    0x28/0x29 measured current      INT32 across two registers, milli-units
    0x2A/0x2B measured voltage      INT32 across two registers, milli-units

  LED (slaves 21..39)
    40001     trigger source
    40002     channel mask
    40003     bias DAC level        writable
    40004     bias ADC readback

The 32-bit values are stored low word first: the reader does
`rr.registers.reverse()` before converting as a big-endian INT32.
"""

import argparse
import logging
import random
import threading
import time

from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusServerContext,
    ModbusSlaveContext,
)
from pymodbus.server import StartTcpServer

log = logging.getLogger("feb_sim")

PMT_SLAVES = range(1, 20)     # channels 1..19
LED_SLAVES = range(21, 40)    # channels 1..19 offset by 20

PMT_REGISTER_COUNT = 0x40
LED_REGISTER_COUNT = 40016

# PMT registers
REG_STATUS = 6
REG_TEMPERATURE = 0x07
REG_RAMP_UP = 0x23
REG_RAMP_DOWN = 0x24
REG_HV_SETPOINT = 0x26
REG_CURRENT = 0x28
REG_VOLTAGE = 0x2A

# PMT status words, mirroring PMTChannel.STATUS_MAP
STATUS_UP = 0
STATUS_DOWN = 1
STATUS_RAMPING_UP = 2
STATUS_RAMPING_DOWN = 3

# LED registers
REG_LED_TRIGGER_SOURCE = 40001
REG_LED_CHANNEL_MASK = 40002
REG_LED_BIAS_DAC = 40003
REG_LED_BIAS_ADC = 40004

DEFAULT_SETPOINT = 1000       # volts, within the 25..1500 range mss enforces
RAMP_VOLTS_PER_STEP = 40      # how fast simulated HV chases its setpoint


def encode_temperature(celsius):
    """Pack a temperature the way PMTChannel.convertTemperature unpacks it."""
    whole = int(celsius)
    thousandths = int(round((celsius - whole) * 1000))
    return ((whole & 0xFF) << 8) | (thousandths & 0xFF)


def encode_int32(value):
    """Split a signed 32-bit value into [low word, high word].

    The reader reverses the pair before decoding it as a big-endian INT32,
    so the low word has to sit first.
    """
    raw = int(value) & 0xFFFFFFFF
    return [raw & 0xFFFF, (raw >> 16) & 0xFFFF]


class PMTModel:
    """Per-channel HV behaviour: the voltage chases the setpoint instead of
    snapping to it, so a slow-control write produces a visible ramp and the
    status word passes through RUP/RDN the way real hardware does."""

    def __init__(self, channel):
        self.channel = channel
        self.setpoint = DEFAULT_SETPOINT
        self.voltage = 0.0
        # A little spread so the channels are not identical in the UI.
        self.base_temperature = 24.0 + 0.15 * channel

    def step(self):
        target = float(self.setpoint)
        delta = target - self.voltage

        if abs(delta) <= RAMP_VOLTS_PER_STEP:
            self.voltage = target
            status = STATUS_UP if self.voltage > 0 else STATUS_DOWN
        elif delta > 0:
            self.voltage += RAMP_VOLTS_PER_STEP
            status = STATUS_RAMPING_UP
        else:
            self.voltage -= RAMP_VOLTS_PER_STEP
            status = STATUS_RAMPING_DOWN

        # Current tracks voltage with a bit of noise; roughly microamps.
        current = self.voltage * 0.6 + random.uniform(-2.0, 2.0)
        temperature = self.base_temperature + random.uniform(-0.4, 0.4)

        return status, self.voltage, current, temperature


def build_context():
    """Create the datastore.

    pymodbus 3.9 dropped zero_mode and ModbusSlaveContext.getValues/setValues
    add one to every address internally. Starting each data block at address
    1 cancels that out, so register N really is register N — both for the
    animator below and for what mss reads off the wire. Get this wrong and
    every value is silently shifted by one register.
    """
    slaves = {}
    models = {}

    for slave in PMT_SLAVES:
        block = ModbusSequentialDataBlock(1, [0] * PMT_REGISTER_COUNT)
        slaves[slave] = ModbusSlaveContext(hr=block)
        models[slave] = PMTModel(slave)

        slaves[slave].setValues(3, REG_HV_SETPOINT, [DEFAULT_SETPOINT])
        slaves[slave].setValues(3, REG_STATUS, [STATUS_DOWN])
        slaves[slave].setValues(3, REG_RAMP_UP, [50, 0])
        slaves[slave].setValues(3, REG_RAMP_DOWN, [50, 0])

    for slave in LED_SLAVES:
        block = ModbusSequentialDataBlock(1, [0] * LED_REGISTER_COUNT)
        slaves[slave] = ModbusSlaveContext(hr=block)

        slaves[slave].setValues(3, REG_LED_TRIGGER_SOURCE, [0])
        slaves[slave].setValues(3, REG_LED_CHANNEL_MASK, [0])
        slaves[slave].setValues(3, REG_LED_BIAS_DAC, [0])
        slaves[slave].setValues(3, REG_LED_BIAS_ADC, [0])

    return ModbusServerContext(slaves=slaves, single=False), models


def animate(context, models, stop_event, period=1.0):
    """Advance the simulated hardware once per period."""
    while not stop_event.is_set():
        for slave, model in models.items():
            ctx = context[slave]

            # A setpoint written by mss is picked up here on the next tick.
            model.setpoint = ctx.getValues(3, REG_HV_SETPOINT, 1)[0]

            status, voltage, current, temperature = model.step()

            ctx.setValues(3, REG_STATUS, [status])
            ctx.setValues(3, REG_TEMPERATURE, [encode_temperature(temperature)])
            ctx.setValues(3, REG_VOLTAGE, encode_int32(voltage * 1000))
            ctx.setValues(3, REG_CURRENT, encode_int32(current * 1000))

        for slave in LED_SLAVES:
            ctx = context[slave]
            # The bias readback follows the commanded DAC level.
            dac = ctx.getValues(3, REG_LED_BIAS_DAC, 1)[0]
            ctx.setValues(3, REG_LED_BIAS_ADC, [min(0xFFFF, int(dac * 0.98))])

        stop_event.wait(period)


def main():
    parser = argparse.ArgumentParser(description="Simulated mPMT FEB Modbus TCP server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=502,
                        help="ModbusManager connects to 502 and is not configurable")
    parser.add_argument("--period", type=float, default=1.0,
                        help="seconds between simulation steps")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

    context, models = build_context()

    stop_event = threading.Event()
    thread = threading.Thread(
        target=animate, args=(context, models, stop_event, args.period), daemon=True
    )
    thread.start()

    log.info("FEB simulator listening on %s:%d — %d PMT slaves, %d LED slaves",
             args.host, args.port, len(PMT_SLAVES), len(LED_SLAVES))

    try:
        StartTcpServer(context=context, address=(args.host, args.port))
    finally:
        stop_event.set()


if __name__ == "__main__":
    main()
