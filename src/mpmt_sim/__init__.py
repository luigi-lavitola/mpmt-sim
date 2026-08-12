"""Simulated mPMT hardware for mpmt-daq-interface and mpmt-mss.

The production repos are dependencies, never copies: this package supplies
only the backends that would otherwise need a Zynq board, an I2C bus, a
Modbus UART and a DMA-proxy driver.
"""

__version__ = "0.1.0"
