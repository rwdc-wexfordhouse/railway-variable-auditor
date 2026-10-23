#!/bin/bash
# Take the screenshots for the website from the real window, on invented demonstration data.
#
#   Source/Tools/make-screenshots.sh [BUILD_DIR] [OUTPUT_DIR]
#
# It builds the application with -Dautomation=true, starts a private headless Wayland
# compositor (mutter --headless), so nothing appears on your screen, and runs the
# application with --screenshots. The application drives every screen itself and saves
# a PNG of each. It never uses the Railway CLI, your policy file or your account.
#
# Needs: meson, ninja, mutter and dbus-run-session. Run it again whenever the window changes.
#
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
# Copyright (c) 2026, Ross Cameron <ross@wexfordhouse.co.za>
# Licensed under the GNU General Public License, version 3.
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-${TMPDIR:-/tmp}/rvc-automation-build}"
out="${2:-$root/Documentation/Website/assets/img/app}"

# Everything below runs in its own D-Bus session so the compositor stays private.
if [ -z "${RVC_IN_SESSION:-}" ]; then
    exec env RVC_IN_SESSION=1 dbus-run-session -- "$0" "$build" "$out"
fi

for tool in meson ninja mutter; do
    command -v "$tool" >/dev/null || { echo "$tool is needed" >&2; exit 2; }
done

if [ ! -f "$build/build.ninja" ]; then
    meson setup "$build" "$root" -Dautomation=true >/dev/null
else
    meson configure "$build" -Dautomation=true >/dev/null
fi
meson compile -C "$build" >/dev/null

work="$(mktemp -d)"
trap 'kill "${compositor:-0}" 2>/dev/null || true; rm -rf "$work"' EXIT
mkdir -p "$out"
rm -f "$out"/*.png

display="rvc-headless-$$"
mutter --headless --wayland --no-x11 --virtual-monitor 1100x800 \
       --wayland-display="$display" >"$work/mutter.log" 2>&1 &
compositor=$!
for _ in $(seq 1 40); do
    [ -S "$XDG_RUNTIME_DIR/$display" ] && break
    sleep 0.25
done
[ -S "$XDG_RUNTIME_DIR/$display" ] || { echo "the headless compositor did not start" >&2; cat "$work/mutter.log" >&2; exit 2; }

# The software renderer, because the headless compositor has no screen to draw to.
WAYLAND_DISPLAY="$display" GDK_BACKEND=wayland GSK_RENDERER=cairo \
    timeout 180 "$build/Source/App/railway-variable-auditor" --screenshots "$out"

echo "Screenshots are in $out"
