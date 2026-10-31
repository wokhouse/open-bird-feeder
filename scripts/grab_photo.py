#!/usr/bin/env python3
"""Grab one JPEG photo frame from the birdcam serial console and save it.

Usage: python3 grab_photo.py <serial_port> <out.jpg> [timeout_s]

Opens the port with raw fds (no DTR assert) so the ESP32-S3 app keeps
running while we read -- pyserial's open() pulls DTR/RTS and holds the
native USB-Serial/JTAG device in download mode, giving empty captures.
Pure stdlib; run with plain system python3.
"""
import base64
import os
import re
import select
import sys
import time

port, outpath = sys.argv[1], sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0

fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
buf = b""
deadline = time.time() + timeout
try:
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 1.0)
        if not r:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            chunk = b""
        if not chunk:
            continue
        sys.stderr.buffer.write(chunk)
        sys.stderr.flush()
        buf += chunk
        if b"--PHOTO-END--" in buf:
            break
finally:
    os.close(fd)

m = re.search(rb"--PHOTO-BEGIN--[^\n]*\n(.*?)\n--PHOTO-END--", buf, re.S)
if not m:
    print("NO PHOTO FOUND in", len(buf), "bytes of serial output", file=sys.stderr)
    sys.exit(1)

jpeg = base64.b64decode(m.group(1), validate=False)
with open(outpath, "wb") as f:
    f.write(jpeg)
hdr = re.search(rb"--PHOTO-BEGIN-- (.*)", buf)
print(f"saved {outpath}  {len(jpeg)} bytes  ({hdr.group(1).decode() if hdr else '?'})")
