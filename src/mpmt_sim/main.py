"""Simulated mss: the real service, wired to simulated hardware.

This mirrors mpmt_mss.main one line at a time. The RPCRuntime, the service
names, the FPGA class and the whole FEBManager stack are the production
objects; only what sits underneath them is simulated:

    real mss                        simulated here
    ------------------------------  ------------------------------
    FPGA('/dev/uio0')               FPGA(<64 KiB regular file>)
    FEBManager(rtu, /dev/ttyPS1)    FEBManager(tcp, feb_sim server)
    HouseKeeping()  [I2C]           SimHouseKeeping()

Because the service names are unchanged, mpmt-daq-interface needs no
modification at all: point mss_url in its MSSConfig at this process.

Environment:
    MPMT_SIM_UIO        backing file for the FPGA  (default /tmp/mpmt-sim/uio0)
    MPMT_SIM_FEB_HOST   Modbus TCP host of feb_sim (default 127.0.0.1)
    MPMT_SIM_PORT       HTTP port to serve on      (default 8000)
"""

import os

from mpmt_mss.rpc import RPCRuntime, create_app
from mpmt_mss.runcontrol.fpga import FPGA
from mpmt_mss.feb import FEBManager, ModbusConfig

from mpmt_sim import fake_uio
from mpmt_sim.fake_sensors import SimHouseKeeping

# Defaults to the real device path on purpose. FEBManager constructs its own
# FPGA('/dev/uio0') internally, so the backing file has to live at exactly
# that path for both it and the instance below to map the same registers.
# Inside a container /dev is a writable tmpfs, so a regular file can simply
# be created there — no patch to mpmt-mss, no privileged device mapping.
UIO_PATH = os.environ.get("MPMT_SIM_UIO", "/dev/uio0")
FEB_HOST = os.environ.get("MPMT_SIM_FEB_HOST", "127.0.0.1")
HTTP_PORT = int(os.environ.get("MPMT_SIM_PORT", "8000"))
PMT_MASK = int(os.environ.get("MPMT_SIM_PMT_MASK", str(fake_uio.DEFAULT_PMT_MASK)), 0)

# Must come first: FEBManager builds an FPGA in its own constructor, and that
# open() fails before anything else gets a chance to run.
fake_uio.create(UIO_PATH, pmt_mask=PMT_MASK)
_animator = fake_uio.Animator(UIO_PATH).start()

# The FEB link has to be up before FEBManager is constructed: it connects in
# __init__ and raises if the host is unreachable.
febmgr = FEBManager(ModbusConfig(mode="tcp", host=FEB_HOST))

fpga = FPGA(UIO_PATH)
hk = SimHouseKeeping()

runtime = RPCRuntime()

runtime.register_service("fpga", fpga)
runtime.register_service("sensors", hk)
runtime.register_service("febmgr", febmgr)

app = create_app(runtime)


def start():
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=HTTP_PORT)


if __name__ == "__main__":
    start()
