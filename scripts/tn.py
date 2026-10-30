#!/usr/bin/env python3
# Run a shell command in the tablet's rescue/install shell (busybox telnetd on
# 172.16.42.1, no login), print its output and exit with its exit status.
# usage: tn.py "<command>" [timeout_seconds]
import select
import socket
import sys
import time

cmd = sys.argv[1]
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 10
s = socket.create_connection(("172.16.42.1", 23), timeout=5)


def strip_iac(data):
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 255 and i + 1 < len(data):
            if data[i + 1] in (251, 252, 253, 254) and i + 2 < len(data):
                # refuse every option
                verb = {251: 254, 253: 252}.get(data[i + 1])
                if verb:
                    s.sendall(bytes([255, verb, data[i + 2]]))
                i += 3
                continue
            i += 2
            continue
        out.append(data[i])
        i += 1
    return bytes(out)


# the markers are put together by the remote shell, so the echo of the command
# line itself never contains them; one line, so no prompt gets in between the
# output and the status (eval keeps a trailing & of the command working)
tag = "%d" % int(time.time() * 1000)
begin, end = "T510BEGIN" + tag, "T510END" + tag
quoted = "'" + cmd.replace("'", "'\\''") + "'"
time.sleep(0.5)
s.sendall(('stty -echo 2>/dev/null; echo T510""BEGIN%s; eval %s; echo "T510""END%s $?"\n'
           % (tag, quoted, tag)).encode())

buf = b""
deadline = time.time() + timeout
while time.time() < deadline:
    tail = buf.split(end.encode())
    if len(tail) > 1 and b"\n" in tail[-1]:
        break
    r, _, _ = select.select([s], [], [], 0.2)
    if r:
        d = s.recv(65536)
        if not d:
            break
        buf += strip_iac(d)
s.close()

out = buf.decode(errors="replace").replace("\r", "")
body = out.split(begin + "\n")[-1]
if end not in body:
    sys.stdout.write(body)
    sys.exit(124)
body, status = body.split(end, 1)
sys.stdout.write(body)
sys.exit(int(status.split()[0]) if status.split() else 1)
