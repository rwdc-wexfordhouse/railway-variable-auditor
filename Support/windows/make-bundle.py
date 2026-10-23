#!/usr/bin/env python3
# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
"""Assembles the Windows runtime tree that the installer ships.

  make-bundle.py --sysroot build/msys2 --exe-dir build/windows --out build/dist

Layout (GTK on Windows finds its data relative to the DLLs, so this mirrors MSYS2):
  bin/                railway-variable-auditor.exe, railway-variable-audit.exe, every DLL they need,
                      and GLib's two gspawn helper programs (needed to run the Railway CLI)
  lib/gdk-pixbuf-2.0/ image loaders and loaders.cache (SVG icons need them)
  share/glib-2.0/     compiled GSettings schemas
  share/icons/        Adwaita and hicolor icon themes
  share/licenses/     licence text of every bundled MSYS2 package
  Documentation/      the website (Documentation/Website)
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBJDUMP = "x86_64-w64-mingw32-objdump"


def imports(path):
    out = subprocess.run([OBJDUMP, "-p", path], capture_output=True, text=True).stdout
    return [l.split(":", 1)[1].strip() for l in out.splitlines() if "DLL Name" in l]


def closure(roots, bin_dir):
    available = {n.lower(): n for n in os.listdir(bin_dir) if n.lower().endswith(".dll")}
    todo, seen = list(roots), set()
    while todo:
        path = todo.pop()
        for dll in imports(path):
            real = available.get(dll.lower())
            if real and real not in seen:
                seen.add(real)
                todo.append(os.path.join(bin_dir, real))
    return sorted(seen)


def copy_tree(src, dst, ignore=None):
    if os.path.isdir(src):
        shutil.copytree(src, dst, dirs_exist_ok=True, ignore=ignore)


def make_loader_cache(out):
    """Runs gdk-pixbuf-query-loaders.exe under Wine (in a container) to write loaders.cache.
    Falls back to the committed copy, which matches the pinned package versions."""
    cache = os.path.join(out, "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders.cache")
    tool = os.path.join(args.sysroot, "ucrt64", "bin", "gdk-pixbuf-query-loaders.exe")
    shutil.copy(tool, os.path.join(out, "bin"))
    try:
        res = subprocess.run(
            ["docker", "run", "--rm", "-v", out + ":/work:Z", "-w", "/work/bin", "railway-variable-auditor-wine-test", "bash", "-c",
             "wineboot -u >/dev/null 2>&1; wine64 gdk-pixbuf-query-loaders.exe"],
            capture_output=True, text=True, timeout=300)
        if res.returncode == 0 and "pixbufloader" in res.stdout:
            open(cache, "w").write(res.stdout)
            print("loaders.cache generated under Wine")
        else:
            raise RuntimeError("no output")
    except Exception:
        shutil.copy(os.path.join(ROOT, "Support", "windows", "loaders.cache"), cache)
        print("loaders.cache copied from Support/ (no Wine container available)")
    os.remove(os.path.join(out, "bin", "gdk-pixbuf-query-loaders.exe"))


def main():
    sysroot = os.path.join(args.sysroot, "ucrt64")
    sbin = os.path.join(sysroot, "bin")
    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    for d in ("bin", "lib/gdk-pixbuf-2.0/2.10.0/loaders", "share/glib-2.0/schemas", "share/icons"):
        os.makedirs(os.path.join(out, d))

    exes = []
    for name in ("railway-variable-auditor.exe", "railway-variable-audit.exe"):
        found = [os.path.join(r, name) for r, _, fs in os.walk(args.exe_dir) if name in fs]
        if not found:
            sys.exit("%s not found under %s: build the Windows programs first" % (name, args.exe_dir))
        exes.append(found[0])
    for e in exes:
        shutil.copy(e, os.path.join(out, "bin"))

    loader_src = os.path.join(sysroot, "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders")
    loaders = [os.path.join(loader_src, n) for n in os.listdir(loader_src) if n.endswith(".dll")]
    for l in loaders:
        shutil.copy(l, os.path.join(out, "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders"))

    dlls = closure(exes + loaders, sbin)
    for d in dlls:
        shutil.copy(os.path.join(sbin, d), os.path.join(out, "bin"))
    print("%d DLLs bundled" % len(dlls))

    # GLib on Windows starts every child process (the Railway CLI) through these two helper
    # programs, which sit next to glib. They are not DLLs, so the closure above misses them.
    # Without them the program cannot run the Railway CLI at all.
    for helper in ("gspawn-win64-helper.exe", "gspawn-win64-helper-console.exe"):
        shutil.copy(os.path.join(sbin, helper), os.path.join(out, "bin"))

    make_loader_cache(out)

    schemas = os.path.join(sysroot, "share", "glib-2.0", "schemas")
    dst = os.path.join(out, "share", "glib-2.0", "schemas")
    for n in os.listdir(schemas):
        if n.endswith(".gschema.xml") and "Demo" not in n:
            shutil.copy(os.path.join(schemas, n), dst)
    subprocess.run(["glib-compile-schemas", dst], check=True)
    for n in os.listdir(dst):
        if n.endswith(".xml"):
            os.remove(os.path.join(dst, n))

    for theme in ("Adwaita", "hicolor"):
        copy_tree(os.path.join(sysroot, "share", "icons", theme), os.path.join(out, "share", "icons", theme))
    copy_tree(os.path.join(sysroot, "etc", "fonts"), os.path.join(out, "etc", "fonts"))
    copy_tree(os.path.join(sysroot, "share", "licenses"), os.path.join(out, "share", "licenses"))
    copy_tree(os.path.join(ROOT, "Documentation", "Website"), os.path.join(out, "Documentation", "Website"))
    shutil.copy(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE.txt"))
    for n in ("THIRD-PARTY-NOTICES.md", "README.md"):
        shutil.copy(os.path.join(ROOT, n), os.path.join(out, n))

    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(out) for f in fs)
    print("bundle ready: %s (%.1f MB)" % (out, total / 1e6))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--exe-dir", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    sys.exit(main())
