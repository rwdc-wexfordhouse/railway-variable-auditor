# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
#
# A throwaway Wine environment for smoke-testing the Windows build and the
# installer from Linux. It is not a substitute for testing on Windows 11.
FROM fedora:latest
RUN dnf install -y --setopt=install_weak_deps=False wine-core wine-common xorg-x11-server-Xvfb procps-ng dejavu-sans-fonts xdotool xwd netpbm-progs \
    && dnf clean all
ENV WINEDEBUG=-all WINEARCH=win64 WINEPREFIX=/wineprefix WINEDLLOVERRIDES="mscoree,mshtml="
RUN useradd -m tester && mkdir /wineprefix && chown tester /wineprefix
USER tester
WORKDIR /home/tester

# Wine cannot use variable fonts, and a fresh prefix has none, so register a static font.
RUN wineboot -u >/dev/null 2>&1; \
    F=$WINEPREFIX/drive_c/windows/Fonts; mkdir -p $F; \
    cp /usr/share/fonts/dejavu*/DejaVuSans.ttf /usr/share/fonts/dejavu*/DejaVuSans-Bold.ttf $F/; \
    wine64 reg add "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts" /v "DejaVu Sans (TrueType)" /d DejaVuSans.ttf /f >/dev/null 2>&1; \
    wine64 reg add "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts" /v "DejaVu Sans Bold (TrueType)" /d DejaVuSans-Bold.ttf /f >/dev/null 2>&1; \
    for n in "Segoe UI" "MS Shell Dlg" "MS Shell Dlg 2" "Tahoma" "Arial" "Sans"; do \
        wine64 reg add "HKCU\\Software\\Wine\\Fonts\\Replacements" /v "$n" /d "DejaVu Sans" /f >/dev/null 2>&1; done; \
    wineserver -k; true
