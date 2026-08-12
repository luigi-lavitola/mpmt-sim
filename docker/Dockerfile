# Runtime for the simulated mss and the FEB server.
#
# Neither mpmt-sim nor mpmt-mss is copied in: both are mounted at run time
# (see compose.yml) so edits take effect on restart without a rebuild, and
# so the simulator always runs against the working tree of the real repo
# rather than a snapshot of it.

FROM python:3.12-slim

# mpmt-mss's own dependencies, plus what the simulator adds. smbus2 is
# needed even though no I2C bus exists here: mpmt_mss.sensors.housekeeping
# imports it at module level, and fake_sensors reuses that module's metric
# tables.
RUN pip install --no-cache-dir \
      "fastapi>=0.137.1" \
      "pydantic>=2.13.4" \
      "pymodbus==3.9.2" \
      "pyserial>=3.5" \
      "requests>=2.34.2" \
      "smbus2>=0.6.1" \
      "uvicorn>=0.49.0" \
      "cmd2>=4.1.2"

ENV PYTHONPATH=/opt/mpmt-sim/src:/opt/mpmt-mss/src \
    PYTHONUNBUFFERED=1

WORKDIR /opt/mpmt-sim

CMD ["python", "-m", "mpmt_sim.main"]
