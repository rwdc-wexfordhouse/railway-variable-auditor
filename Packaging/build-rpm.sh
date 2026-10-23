#!/bin/bash
# Build the RPM (and the source RPM) for this machine.
#
#   Packaging/build-rpm.sh
#
# Needs: rpm-build, meson, gcc, glib2-devel, json-glib-devel, gtk4-devel, desktop-file-utils.
# Builds in a temporary folder, runs the unit tests as part of the build, and puts the
# results in Packaging/dist/.
#
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
# Copyright (c) 2026, Ross Cameron <ross@wexfordhouse.co.za>
# Licensed under the GNU General Public License, version 3.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
spec="$root/Packaging/railway-variable-auditor.spec"
name="railway-variable-auditor"
version="$(sed -n 's/^Version:[[:space:]]*//p' "$spec")"
dist="$root/Packaging/dist"

for tool in rpmbuild meson gcc; do
    command -v "$tool" >/dev/null || { echo "$tool is needed (sudo dnf install rpm-build meson gcc)" >&2; exit 2; }
done
grep -q "version : '$version'" "$root/meson.build" \
    || { echo "The version in the spec ($version) does not match meson.build" >&2; exit 2; }

top="$(mktemp -d)"
trap 'rm -rf "$top"' EXIT
mkdir -p "$top"/{SOURCES,SPECS,BUILD,RPMS,SRPMS,BUILDROOT}

# A source tarball of the tree, without build folders or earlier packages.
( cd "$root" && tar --exclude='./build*' --exclude='./Packaging/dist' --exclude='./.git' --exclude='*.rpm' \
      --transform "s,^\./,$name-$version/," -czf "$top/SOURCES/$name-$version.tar.gz" . )
cp "$spec" "$top/SPECS/"

rpmbuild --define "_topdir $top" -ba "$top/SPECS/$name.spec"

mkdir -p "$dist"
rm -f "$dist"/*.rpm
cp "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm "$dist/"
echo
echo "Built:"
ls -1 "$dist"
echo
main="$(ls "$dist"/$name-$version-*.x86_64.rpm | grep -v -e debuginfo -e debugsource | head -1)"
echo "Install with:  sudo dnf install $main"
