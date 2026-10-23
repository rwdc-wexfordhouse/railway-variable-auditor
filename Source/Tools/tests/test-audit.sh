#!/bin/sh
# End to end test of railway-variable-audit with a fake CLI (invented data).
# Usage: test-audit.sh AUDIT_PROGRAM FAKE_CLI
set -eu
audit=$1
fake=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
chmod +x "$fake"

RVC_RAILWAY_CLI=$fake "$audit" --out "$work" --policy "$work/none.ini" >"$work/stdout" 2>"$work/stderr"

csv=$(ls "$work"/run-*/audit.csv)
test ! -e "$(dirname "$csv")/run-report.csv"     # the audit is one file
test "$(stat -c %a "$csv")" = 600
head -1 "$csv" | grep -q '^workspace,project,environment,service,variable,value,category,reason'
grep -q 'Alpha' "$csv"
grep -q 'Beta' "$csv"
grep -q '0123456789abcdef0123456789abcdef' "$csv"
grep -q '"tok,with""quote-p-w2"' "$csv"
# No value may reach the terminal.
if grep -q -e '0123456789abcdef' -e 'tok,with' "$work/stdout" "$work/stderr"; then
    echo "a value reached the terminal" >&2
    exit 1
fi
echo "audit test passed"
