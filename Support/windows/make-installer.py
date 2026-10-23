#!/usr/bin/env python3
# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
"""Appends the bundle to the installer stub and writes the finished setup.exe.

  make-installer.py <stub.exe> <bundle-dir> <output.exe>

Payload layout is described in Source/windows/installer/payload.h.
"""
import hashlib
import os
import struct
import sys
import zlib

MAGIC = b"RVAUDITOR-PKG-01"


def main():
    stub, bundle, out = sys.argv[1:4]
    stub_data = open(stub, "rb").read()
    files = []
    for root, _, names in os.walk(bundle):
        for n in names:
            full = os.path.join(root, n)
            files.append((os.path.relpath(full, bundle).replace(os.sep, "/"), full))
    files.sort(key=lambda f: (f[0] != "LICENSE.txt", f[0]))  # the licence first, so Setup finds it fast

    body = bytearray()
    total = 0
    for rel, full in files:
        data = open(full, "rb").read()
        packed = zlib.compress(data, 9)
        path = rel.encode("utf-8")
        body += struct.pack("<I", len(path)) + path
        body += struct.pack("<QQI", len(data), len(packed), zlib.crc32(data) & 0xFFFFFFFF)
        body += packed
        total += len(data)

    digest = hashlib.sha256(bytes(body)).digest()
    trailer = MAGIC + struct.pack("<QQIIQ", len(stub_data), len(body), len(files), 0, total) + digest
    assert len(trailer) == 80
    with open(out, "wb") as fh:
        fh.write(stub_data)
        fh.write(body)
        fh.write(trailer)
    print("%s: %d files, %.1f MB unpacked, %.1f MB installer" % (out, len(files), total / 1e6, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
