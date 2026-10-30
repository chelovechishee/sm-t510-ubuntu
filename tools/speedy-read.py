#!/usr/bin/env python3
"""Read registers of a SPEEDY device (as seen through the i2c-exynos-speedy adapter).

usage: speedy-read.py BUS DEVICE_ID FIRST_REG [COUNT]
e.g.   speedy-read.py 0 0x01 0x00      # S2MPU08 PMIC id
       speedy-read.py 0 0x02 0x00 27   # S2MPU08 RTC registers
Read-only: one "write register number, read COUNT bytes" I2C_RDWR transfer.
"""
import ctypes
import fcntl
import sys

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001


class I2cMsg(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint16), ("flags", ctypes.c_uint16),
                ("len", ctypes.c_uint16), ("buf", ctypes.POINTER(ctypes.c_uint8))]


class I2cRdwr(ctypes.Structure):
    _fields_ = [("msgs", ctypes.POINTER(I2cMsg)), ("nmsgs", ctypes.c_uint32)]


def main():
    bus, dev, reg = (int(a, 0) for a in sys.argv[1:4])
    count = int(sys.argv[4], 0) if len(sys.argv) > 4 else 1
    wbuf = (ctypes.c_uint8 * 1)(reg)
    rbuf = (ctypes.c_uint8 * count)()
    msgs = (I2cMsg * 2)(I2cMsg(dev, 0, 1, wbuf), I2cMsg(dev, I2C_M_RD, count, rbuf))
    with open(f"/dev/i2c-{bus}", "rb+", buffering=0) as f:
        fcntl.ioctl(f, I2C_RDWR, I2cRdwr(msgs, 2))
    for i, b in enumerate(rbuf):
        print(f"{reg + i:02x}: {b:02x}")


if __name__ == "__main__":
    main()
