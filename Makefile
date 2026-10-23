# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>

NAME    := railway-variable-auditor
VERSION := $(shell sed -n "s/^ *version : '\(.*\)',/\1/p" meson.build | head -1)

.PHONY: all test rpm clean windows-sysroot windows installer windows-test

all:
	meson setup build/native --reconfigure >/dev/null 2>&1 || meson setup build/native
	meson compile -C build/native

test: all
	meson test -C build/native

# Builds the RPM into Packaging/dist.
rpm:
	Packaging/build-rpm.sh

# ---------------------------------------------------------------------------
# Windows 11 (x86_64): cross-compiled on Linux, packaged as a per-user setup.exe.
# One-time setup: Support/windows/setup-toolchain.sh
# ---------------------------------------------------------------------------
WIN_DIR     := build/windows
SYSROOT     := $(CURDIR)/build/msys2
DIST        := build/dist
STUB        := build/installer-stub.exe
SETUP       := build/RailwayVariableAuditor-$(VERSION)-setup.exe
WIN_CC      := x86_64-w64-mingw32-gcc
WIN_WINDRES := x86_64-w64-mingw32-windres
INST_SRC    := Source/windows/installer
WIN_OPTIONS := -Dtests=false -Dwindows_icon=$(CURDIR)/build/logo.ico

windows-sysroot: $(SYSROOT)/.complete
$(SYSROOT)/.complete: Support/windows/fetch-msys2.py Support/windows/msys2.lock
	mkdir -p $(SYSROOT)
	python3 Support/windows/fetch-msys2.py $(SYSROOT)

build/logo.ico: Documentation/Website/assets/img/favicon.svg Support/windows/generate-icon.py
	mkdir -p build
	rsvg-convert -w 512 -h 512 $< -o build/logo.png
	python3 Support/windows/generate-icon.py build/logo.png $@

build/mingw-ucrt64.cross: Support/windows/mingw-ucrt64.cross.in
	mkdir -p build
	sed "s|@SYSROOT@|$(SYSROOT)|g" $< > $@

windows: $(SYSROOT)/.complete build/logo.ico build/mingw-ucrt64.cross
	@command -v $(WIN_CC) >/dev/null || { echo "$(WIN_CC) not found: run Support/windows/setup-toolchain.sh"; exit 1; }
	test -f $(WIN_DIR)/build.ninja || meson setup $(WIN_DIR) . --cross-file build/mingw-ucrt64.cross $(WIN_OPTIONS)
	meson compile -C $(WIN_DIR)

$(DIST): windows
	python3 Support/windows/make-bundle.py --sysroot $(SYSROOT) --exe-dir $(WIN_DIR) --out $(DIST)

build/installer-setup.rc: $(INST_SRC)/setup.rc.in build/logo.ico
	sed -e "s|@MANIFEST@|$(CURDIR)/$(INST_SRC)/setup.manifest|" -e "s|@ICON@|$(CURDIR)/build/logo.ico|" \
	    -e "s|@VERSION@|$(VERSION)|g" -e "s|@MAJOR@|$(word 1,$(subst ., ,$(VERSION)))|" \
	    -e "s|@MINOR@|$(word 2,$(subst ., ,$(VERSION)))|" -e "s|@PATCH@|$(word 3,$(subst ., ,$(VERSION)))|" $< > $@

$(STUB): $(wildcard $(INST_SRC)/*.c $(INST_SRC)/*.h) build/installer-setup.rc $(SYSROOT)/.complete
	mkdir -p build/win-inc
	cp $(SYSROOT)/ucrt64/include/zlib.h $(SYSROOT)/ucrt64/include/zconf.h build/win-inc/
	$(WIN_WINDRES) -I$(INST_SRC) build/installer-setup.rc -O coff -o build/installer-setup.res.o
	$(WIN_CC) -municode -mwindows -O2 -Wall -Wextra -std=c11 -static-libgcc \
	    -D_WIN32_WINNT=0x0A00 -DRVA_VERSION_STRING='"$(VERSION)"' -Ibuild/win-inc -I$(INST_SRC) \
	    -o $@ $(INST_SRC)/setup.c $(INST_SRC)/install.c $(INST_SRC)/payload.c build/installer-setup.res.o \
	    $(SYSROOT)/ucrt64/lib/libz.a -lcomctl32 -lole32 -lshell32 -ladvapi32 -luser32 -lbcrypt -luuid

installer: $(DIST) $(STUB)
	python3 Support/windows/make-installer.py $(STUB) $(DIST) $(SETUP)
	@echo
	@echo "Installer: $(SETUP)"

# Smoke tests of the Windows build under Wine in a container (not a replacement for Windows 11).
windows-test: installer
	Support/windows/wine-test.sh $(SETUP)

clean:
	rm -rf build/native build/windows build/dist build/installer-stub.exe build/win-inc build/*.ico build/*.png
