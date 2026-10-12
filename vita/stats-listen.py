#!/usr/bin/env python3
"""Receives the Vita app's once-a-second debug lines (UDP 18999) from any sender."""
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 18999))
out = open(sys.argv[1], "a", buffering=1)
while True:
    data, _ = s.recvfrom(2048)
    out.write(data.decode("utf-8", "replace"))
