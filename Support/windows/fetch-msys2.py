#!/usr/bin/env python3
# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
"""Builds the Windows sysroot from pinned MSYS2 UCRT64 packages.

EPEL 10 has no MinGW build of GTK 4 or json-glib, so the cross build
uses the prebuilt MSYS2 packages as its headers, import libraries and runtime
DLLs. Every package is pinned by file name and SHA-256 in Support/windows/msys2.lock.

  fetch-msys2.py <sysroot-dir> [--update-lock]

Without --update-lock only the pinned files are downloaded, and each download
must match its recorded hash. --update-lock re-resolves the newest versions
from the MSYS2 repository and rewrites the lock file; review the diff.
"""
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import urllib.request

REPO = "https://repo.msys2.org/mingw/ucrt64/"
PREFIX = "mingw-w64-ucrt-x86_64-"
ROOTS = ["gtk4", "json-glib", "zlib", "adwaita-icon-theme", "librsvg", "hicolor-icon-theme"]
HERE = os.path.dirname(os.path.abspath(__file__))
LOCK = os.path.join(HERE, "msys2.lock")


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def read_db():
    raw = fetch(REPO + "ucrt64.db")
    tar = subprocess.run(["zstd", "-dc"], input=raw, capture_output=True, check=True).stdout \
        if raw[:4] == b"\x28\xb5\x2f\xfd" else raw
    pkgs = {}
    with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
        for m in tf.getmembers():
            if not m.name.endswith("/desc"):
                continue
            fields, key = {}, None
            for line in tf.extractfile(m).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line.strip("%")
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            pkgs[fields["NAME"][0]] = fields
    return pkgs


def resolve(pkgs):
    provides = {}
    for name, f in pkgs.items():
        for p in f.get("PROVIDES", []):
            provides.setdefault(re.split(r"[<>=]", p)[0], name)
    chosen, todo = {}, [PREFIX + r for r in ROOTS]
    while todo:
        name = todo.pop()
        name = name if name in pkgs else provides.get(name)
        if not name or name in chosen:
            continue
        chosen[name] = pkgs[name]
        for d in pkgs[name].get("DEPENDS", []):
            todo.append(re.split(r"[<>=]", d)[0])
    return chosen


def write_lock(chosen):
    with open(LOCK, "w") as fh:
        fh.write("# Pinned MSYS2 UCRT64 packages. Regenerate with: Support/fetch-msys2.py <dir> --update-lock\n")
        fh.write("# name filename sha256\n")
        for name in sorted(chosen):
            f = chosen[name]
            fh.write("%s %s %s\n" % (name, f["FILENAME"][0], f["SHA256SUM"][0]))


def read_lock():
    out = []
    for line in open(LOCK):
        if line.strip() and not line.startswith("#"):
            out.append(line.split())
    return out


def fix_pkgconfig(dest):
    """The OpenSSL .pc files contain 'prefix=${prefix}', which pkgconf resolves to
    nothing when a sysroot is used. Drop that line so the earlier prefix stays."""
    pcdir = os.path.join(dest, "ucrt64", "lib", "pkgconfig")
    for name in os.listdir(pcdir):
        path = os.path.join(pcdir, name)
        text = open(path).read()
        fixed = "\n".join(l for l in text.split("\n") if l.strip() != "prefix=${prefix}")
        if fixed != text:
            open(path, "w").write(fixed)


def main():
    dest = sys.argv[1]
    if "--update-lock" in sys.argv or not os.path.exists(LOCK):
        print("resolving packages from", REPO)
        write_lock(resolve(read_db()))
    cache = os.path.join(dest, "_downloads")
    os.makedirs(cache, exist_ok=True)
    entries = read_lock()
    for i, (name, filename, sha) in enumerate(entries, 1):
        path = os.path.join(cache, filename)
        if not os.path.exists(path) or hashlib.sha256(open(path, "rb").read()).hexdigest() != sha:
            print("[%d/%d] downloading %s" % (i, len(entries), filename))
            data = fetch(REPO + filename)
            if hashlib.sha256(data).hexdigest() != sha:
                sys.exit("hash mismatch for %s: refusing to use it" % filename)
            open(path, "wb").write(data)
    print("extracting", len(entries), "packages")
    for _, filename, _ in entries:
        z = subprocess.Popen(["zstd", "-dc", os.path.join(cache, filename)], stdout=subprocess.PIPE)
        subprocess.run(["tar", "-x", "-C", dest, "--exclude=.PKGINFO", "--exclude=.BUILDINFO",
                        "--exclude=.MTREE", "--exclude=.INSTALL"], stdin=z.stdout, check=True)
        z.wait()
    fix_pkgconfig(dest)
    open(os.path.join(dest, ".complete"), "w").write("ok\n")


if __name__ == "__main__":
    main()
