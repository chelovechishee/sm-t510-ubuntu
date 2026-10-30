#!/usr/bin/env python3
"""Inspect (and optionally talk to) the ACPM firmware of the Exynos 7885/7904.

usage: acpm-probe.py                 list IPC channels (read-only)
       acpm-probe.py get DOMAIN      FREQ_GET on the DVFS channel (5)
       acpm-probe.py set DOMAIN KHZ  FREQ_REQ on the DVFS channel

DVFS domains: 0 mif, 1 int, 2 cpucl0 (A73), 3 cpucl1 (A53), 4 g3d, 5 fsys,
6 cam, 7 disp, 8 aud. Layout as in drivers/firmware/samsung/exynos-acpm.c.
"""
import mmap
import os
import struct
import sys
import time

SRAM, SRAM_SIZE, INITDATA = 0x02052000, 0x1b000, 0x4b80
MBOX, INTGR0 = 0x11c40000, 0x08
DVFS_CHAN = 5
SEQ_SHIFT = 16

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
sram = mmap.mmap(fd, SRAM_SIZE, mmap.MAP_SHARED, offset=SRAM)
mbox = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, offset=MBOX)


def r32(m, off):
    return struct.unpack_from("<I", m, off)[0]


def w32(m, off, val):
    struct.pack_into("<I", m, off, val)


def channels():
    chans, num = r32(sram, INITDATA + 8), r32(sram, INITDATA + 28)
    out = []
    for i in range(num):
        f = struct.unpack_from("<18I", sram, chans + i * 72)
        out.append(dict(id=f[0], rx_rear=f[4], rx_front=f[5], rx_base=f[6],
                        tx_rear=f[10], tx_front=f[11], tx_base=f[12],
                        qlen=f[13], mlen=f[14], poll=f[17]))
    return chans, num, out


def xfer(cmd, want_reply):
    """One message on the DVFS channel. It is a single-slot buffer channel
    (type 2, qlen 1): the APM takes the slot on the doorbell, and answers in
    its TX slot with the same sequence number in bits 21:16 of word 0."""
    c = [x for x in channels()[2] if x["id"] == DVFS_CHAN][0]
    seq = int(time.time() * 1000) % 63 + 1
    cmd = list(cmd)
    cmd[0] |= seq << SEQ_SHIFT
    for i in range(4):
        w32(sram, c["tx_base"] + 4 * i, 0)
    for i, v in enumerate(cmd):
        w32(sram, c["rx_base"] + 4 * i, v)
    w32(mbox, INTGR0, (1 << DVFS_CHAN) << 16)
    if not want_reply:
        return None
    for _ in range(1000):
        msg = [r32(sram, c["tx_base"] + 4 * i) for i in range(4)]
        if (msg[0] >> SEQ_SHIFT) & 0x3f == seq:
            return msg
        time.sleep(0.001)
    sys.exit("no reply; rx %s tx %s" % (
        [hex(r32(sram, c["rx_base"] + 4 * i)) for i in range(4)],
        [hex(r32(sram, c["tx_base"] + 4 * i)) for i in range(4)]))


def main():
    if len(sys.argv) == 1:
        chans, num, out = channels()
        print("initdata chans@%#x num %d" % (chans, num))
        for c in out:
            print(c)
    elif sys.argv[1] == "get":
        msg = xfer([int(sys.argv[2]), 0, 1, 0], True)
        print("reply", [hex(x) for x in msg], "-> %d kHz" % msg[1])
    elif sys.argv[1] == "set":
        xfer([int(sys.argv[2]), int(sys.argv[3]), 0, 0], False)
        print("sent")


if __name__ == "__main__":
    main()
