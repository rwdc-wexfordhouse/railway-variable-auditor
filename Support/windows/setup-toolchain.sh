#!/usr/bin/env bash
# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
#
# Installs what is needed to cross-compile the Windows build and its installer
# on Rocky Linux 10 (or another EL10 derivative with EPEL enabled).
# Run once, with a user that can sudo:   Support/windows/setup-toolchain.sh
set -euo pipefail

command -v dnf >/dev/null 2>&1 || { echo "error: this script expects dnf" >&2; exit 1; }

sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb || true

sudo dnf install -y \
    mingw64-gcc mingw64-binutils \
    meson pkgconf-pkg-config \
    zstd tar python3 python3-pillow \
    librsvg2-tools glib2-devel

# Optional: 'make windows-test' runs the Windows build under Wine inside a
# container. It needs podman or docker (podman is in the Rocky repositories).
sudo dnf install -y podman || true

echo
echo "Toolchain installed:"
x86_64-w64-mingw32-gcc --version | head -1
