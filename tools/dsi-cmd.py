#!/usr/bin/env python3
"""Send DCS short write packets to the SM-T510 panel through the running DSIM.

usage: dsi-cmd.py BYTE [BYTE]...   (one or two bytes per packet; several packets
                                     separated by "/", e.g. "B0 04 / B8 80")
Prototype of the mainline backlight driver: the bootloader leaves DSIM in video
mode; a header written with packet-go enabled is sent in the blanking period.
"""
import mmap
import os
import struct
import sys
import time

DSIM_BASE = 0x14870000
FIFOCTRL = 0x68
FIFOCTRL_FULL_PH = 1 << 11
FIFOCTRL_EMPTY_PH = 1 << 10
PKTHDR = 0x58
CMD_CONFIG = 0x80
PKT_GO_EN = 1 << 16
DCS_SHORT_WRITE = 0x05
DCS_SHORT_WRITE_PARAM = 0x15


def main():
    packets = [p.split() for p in " ".join(sys.argv[1:]).split("/") if p.split()]
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, offset=DSIM_BASE)

    def rd(off):
        return struct.unpack_from("<I", m, off)[0]

    def wr(off, val):
        struct.pack_into("<I", m, off, val)

    for p in packets:
        b = [int(x, 16) for x in p]
        while rd(FIFOCTRL) & FIFOCTRL_FULL_PH:
            time.sleep(0.001)
        if len(b) == 1:
            hdr = DCS_SHORT_WRITE | b[0] << 8
        else:
            hdr = DCS_SHORT_WRITE_PARAM | b[0] << 8 | b[1] << 16
        wr(CMD_CONFIG, rd(CMD_CONFIG) | PKT_GO_EN)
        wr(PKTHDR, hdr)
        wr(CMD_CONFIG, rd(CMD_CONFIG) & ~PKT_GO_EN)
        for _ in range(100):
            if rd(FIFOCTRL) & FIFOCTRL_EMPTY_PH:
                break
            time.sleep(0.001)
        else:
            print("packet %s not sent, FIFOCTRL %#x" % (" ".join(p), rd(FIFOCTRL)))
    print("CMD_CONFIG %#x FIFOCTRL %#x" % (rd(CMD_CONFIG), rd(FIFOCTRL)))


if __name__ == "__main__":
    main()
