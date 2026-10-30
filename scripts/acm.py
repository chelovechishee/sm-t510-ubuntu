#!/usr/bin/env python3
# Run a shell command on the tablet via the ACM serial console and print the output.
# usage: acm.py "<command>" [timeout_seconds]
import os, sys, termios, time, select

dev = os.environ.get("ACM", "/dev/ttyACM0")
cmd = sys.argv[1]
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 5

fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
attrs = termios.tcgetattr(fd)
attrs[0] = 0  # iflag
attrs[1] = 0  # oflag
attrs[3] = 0  # lflag: raw
termios.tcsetattr(fd, termios.TCSANOW, attrs)
termios.tcflush(fd, termios.TCIOFLUSH)

marker = "__END_%d__" % int(time.time())
os.write(fd, ("stty -echo 2>/dev/null; %s; echo %s\n" % (cmd, marker)).encode())

buf = b""
deadline = time.time() + timeout
while time.time() < deadline:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        buf += os.read(fd, 65536)
        if marker.encode() in buf.split(b"echo " + marker.encode())[-1]:
            break
os.close(fd)

out = buf.decode(errors="replace").replace("\r", "")
out = out.split("echo " + marker)[-1]
print(out.split(marker)[0].strip("\n"))
