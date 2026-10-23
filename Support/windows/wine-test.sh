#!/bin/sh
# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
#
# Usage: wine-test.sh <RailwayVariableAuditor-x.y.z-setup.exe>
# Smoke-tests the Windows installer and the installed programs under Wine in a
# container, with a fake Railway CLI and invented data. A safety net, not a
# substitute for testing on Windows 11. Needs docker, and builds the fake CLI
# with the mingw64 compiler.
set -eu
setup=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
image=railway-variable-auditor-wine-test
command -v docker >/dev/null || { echo "docker not found, skipping"; exit 77; }
docker image inspect $image >/dev/null 2>&1 || docker build -t $image "$here/wine"

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
chmod 755 "$work"
cp "$setup" "$work/setup.exe"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o "$work/fake-railway.exe" "$root/Source/Tools/tests/fake-railway.c"

docker run --rm -e LANG=C.UTF-8 -v "$work:/work:Z" -w /work $image bash -c '
set -u
fail=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fail=1; }
wineboot -u >/dev/null 2>&1
D="C:\\RVA Test"
P="$WINEPREFIX/drive_c/RVA Test"
reg() { wine64 reg query "$@" 2>/dev/null; }

wine64 setup.exe /S "/D=$D"; sleep 2
[ -f "$P/bin/railway-variable-auditor.exe" ] && [ -f "$P/bin/railway-variable-audit.exe" ] && [ -f "$P/uninstall.exe" ] && ok "silent install wrote the programs and uninstall.exe" || bad "install files missing"
[ -f "$P/share/glib-2.0/schemas/gschemas.compiled" ] && [ -f "$P/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ] && ok "GTK runtime data present" || bad "GTK runtime data missing"
[ -f "$P/LICENSE.txt" ] && [ -d "$P/share/licenses" ] && [ -f "$P/THIRD-PARTY-NOTICES.md" ] && ok "licence texts installed" || bad "licences missing"
[ -f "$P/Documentation/Website/index.html" ] && [ -f "$P/Documentation/Website/assets/css/style.css" ] && ok "the website is installed next to the programs" || bad "website missing"
reg "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RailwayVariableAuditor" | grep -q "DisplayName" && ok "Add/Remove Programs entry" || bad "no uninstall entry"
reg "HKCU\\Environment" /v Path | grep -qi "RVA Test.bin" && ok "command line tool on PATH" || bad "PATH not updated"
reg "HKCU\\Software\\Classes\\.emc" | grep -q "HKEY_CURRENT_USER" && bad "a file type was registered" || ok "no file types registered"

wine64 "$P/bin/railway-variable-audit.exe" --help | tr -d "\r" | grep -q "Usage: railway-variable-audit" && ok "the audit command runs" || bad "audit --help failed"

export RVC_RAILWAY_CLI="Z:\\work\\fake-railway.exe"
out=$(wine64 "$P/bin/railway-variable-audit.exe" --out "C:\\audit-out" --policy "C:\\none.ini" 2>"$WINEPREFIX/audit.err" | tr -d "\r"); status=$?
csv=$(ls "$WINEPREFIX"/drive_c/audit-out/run-*/audit.csv 2>/dev/null | head -1)
[ -n "$csv" ] && ok "audit.csv written under Wine" || bad "no audit.csv: $out $(cat "$WINEPREFIX/audit.err" | head -3)"
[ -n "$csv" ] && head -1 "$csv" | tr -d "\r" | grep -q "^workspace,project,environment,service,variable,value,category,reason" && ok "audit.csv header" || bad "audit.csv header"
[ -n "$csv" ] && grep -q "Alpha" "$csv" && grep -q "Beta" "$csv" && grep -q "0123456789abcdef0123456789abcdef" "$csv" && ok "both workspaces and their values are in audit.csv" || bad "audit.csv content"
[ -n "$csv" ] && grep -q "tok,with\"\"quote-p-w2" "$csv" && ok "a value with a comma and a quote came through" || bad "tricky value"
[ -n "$csv" ] && [ ! -e "$(dirname "$csv")/run-report.csv" ] && ok "only one file was written" || bad "extra files"
echo "$out" | grep -q "0123456789abcdef" && bad "a value reached the terminal" || ok "no value on the terminal"

Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &
export DISPLAY=:99; sleep 2
unset RVC_RAILWAY_CLI
wine64 "$P/bin/railway-variable-auditor.exe" >/dev/null 2>"$WINEPREFIX/gui.err" &
sleep 15
if xdotool search --name "Railway Variable Auditor" >/dev/null 2>&1; then ok "the window opens"; else bad "no window: $(head -5 "$WINEPREFIX/gui.err")"; fi
pkill -f railway-variable-auditor.exe; sleep 1

wine64 "$P/uninstall.exe" --uninstall --quiet; sleep 5
[ ! -e "$P" ] && ok "uninstall removed the folder" || bad "folder left: $(ls "$P" | head -3)"
reg "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RailwayVariableAuditor" | grep -q DisplayName && bad "uninstall key left" || ok "uninstall key removed"
reg "HKCU\\Environment" /v Path | grep -qi "RVA Test" && bad "PATH entry left" || ok "PATH entry removed"
exit $fail'
