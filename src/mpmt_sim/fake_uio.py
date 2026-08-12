"""A regular file standing in for /dev/uio0.

mpmt_mss.runcontrol.fpga.FPGA opens the UIO device and mmaps 64 KiB of it,
then reads and writes 32-bit registers as plain slices of that mapping. A
regular file of the same size behaves identically under mmap, so the real
FPGA class runs here unmodified — every register decode, bit mask and
rpc_method in it is the production code path, not a reimplementation.

What a file cannot do is change on its own, which is what the animator
thread is for: it advances the counters and telemetry registers that real
firmware would drive, so monitoring snapshots are not frozen.
"""

import mmap
import os
import random
import threading

UIO_SIZE = 0x10000

# Registers, mirroring the constants in mpmt_mss.runcontrol.fpga.FPGA.
REG_STATUS = 3

# One ratemeter per channel, in Hz. FEBManager.getRateAll() reads 8 + ch for
# ch in 0..18, so these nineteen registers are what ends up in the monitoring
# snapshot as fpga.rate_all. Left at zero they make every rate plot a flat
# line at the bottom of its axis.
REG_RATE_FIRST = 8
RATE_CHANNELS = 19

REG_DEADTIME = 27
REG_FIFO_WORD_COUNT = 43
REG_TR32_COUNT = 45
REG_HOUSEKEEPING = 56
REG_FW_DATE_AND_FAULTS = 61
REG_FW_TIME = 62
REG_FW_SHA = 63
REG_PMT_MASK = 103
REG_FW_VERSION = 104

# Register 103 tells FEBManager which channels are PMTs: bit x set means
# channel x+1 is a PMT, clear means it is an LED. FEBManager reads it once,
# in its constructor, so it has to be right before mss starts — a zero here
# silently configures all nineteen channels as LEDs.
#
# The default sets all 19 bits, matching FEBManager.all_channels_mask, i.e.
# a module fully populated with PMTs. Override for a mixed population.
DEFAULT_PMT_MASK = 0x7FFFF

# Status bits the firmware would assert on a healthy board.
STATUS_PLL_LOCKED = 0x00000002
STATUS_CABLE_1_FOUND = 0x00000020
STATUS_CABLE_1_OK = 0x00000080

HEALTHY_STATUS = STATUS_PLL_LOCKED | STATUS_CABLE_1_FOUND | STATUS_CABLE_1_OK

# Plausible constants for the read-only firmware identity registers. These
# are arbitrary but well-formed: they exist so the monitoring payload has
# the right shape, not because they describe any real bitstream.
FW_DATE_AND_FAULTS = 0x20260115
FW_TIME = 0x00120000
FW_SHA = 0x5A1B3C4D
FW_VERSION = 0x00010004


def _write_register(regs, address, value):
    regs[address * 4:(address * 4) + 4] = int(value & 0xFFFFFFFF).to_bytes(
        4, byteorder="little"
    )


def create(path, pmt_mask=DEFAULT_PMT_MASK):
    """Create the backing file and latch the registers read at start-up.

    The static values are written here rather than in the animator thread
    because FEBManager reads the PMT mask inside its constructor: leaving it
    to a background thread is a race that usually loses.
    """
    directory = os.path.dirname(os.path.abspath(path))
    if directory:
        os.makedirs(directory, exist_ok=True)

    with open(path, "a+b") as handle:
        handle.truncate(UIO_SIZE)

    with open(path, "r+b", 0) as handle:
        regs = mmap.mmap(handle.fileno(), UIO_SIZE)
        _write_register(regs, REG_PMT_MASK, pmt_mask)
        _write_register(regs, REG_FW_DATE_AND_FAULTS, FW_DATE_AND_FAULTS)
        _write_register(regs, REG_FW_TIME, FW_TIME)
        _write_register(regs, REG_FW_SHA, FW_SHA)
        _write_register(regs, REG_FW_VERSION, FW_VERSION)
        _write_register(regs, REG_STATUS, HEALTHY_STATUS)
        regs.flush()
        regs.close()

    return path


class Animator:
    """Drives the telemetry registers the way running firmware would.

    Writes go through a second shared mapping of the same file, so the FPGA
    object's own mapping sees them without any coordination between the two.
    """

    def __init__(self, path, period=1.0):
        self.path = path
        self.period = period
        self._stop = threading.Event()
        self._thread = None

    def _write(self, regs, address, value):
        _write_register(regs, address, value)

    def _run(self):
        with open(self.path, "r+b", 0) as handle:
            regs = mmap.mmap(handle.fileno(), UIO_SIZE)

            # Only the registers running firmware would drive are touched
            # here. The identity registers and the PMT mask are latched in
            # create() and must not be overwritten: mss may have been
            # configured from them already.
            tr32 = 0

            # Each channel keeps its own rate so the nineteen ratemeters are
            # not identical, and each wanders from where it was rather than
            # being redrawn every tick — a plot of white noise says nothing
            # about whether a rate is drifting.
            rates = [random.uniform(4000, 6000) for _ in range(RATE_CHANNELS)]

            while not self._stop.is_set():
                tr32 = (tr32 + random.randint(900, 1100)) & 0xFFFFFFFF

                self._write(regs, REG_TR32_COUNT, tr32)
                self._write(regs, REG_DEADTIME, random.randint(1000, 5000))
                self._write(regs, REG_FIFO_WORD_COUNT, random.randint(0, 4096))
                self._write(regs, REG_HOUSEKEEPING, random.randint(0, 0xFFFF))

                for channel in range(RATE_CHANNELS):
                    rates[channel] += random.uniform(-60, 60)
                    rates[channel] = min(9000.0, max(1500.0, rates[channel]))
                    self._write(regs, REG_RATE_FIRST + channel, int(rates[channel]))

                self._stop.wait(self.period)

            regs.close()

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2)
