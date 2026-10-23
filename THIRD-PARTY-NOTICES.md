# Third-party notices

Railway Variable Auditor is released under the GNU General Public Licence, version 3 (see `LICENSE`).

## Windows installer

The Windows build bundles the libraries below, taken unmodified from the MSYS2 UCRT64 repository (https://www.msys2.org/). The exact package versions and their SHA-256 hashes are pinned in `Support/windows/msys2.lock`. The licence text of every bundled package is installed in `share/licenses/<package>/` next to the program, and is the authority. The summary here is a guide only.

| Component | Used for | Licence (see `share/licenses`) |
|---|---|---|
| GTK 4, GLib, GIO, GdkPixbuf | The window, files, processes | LGPL-2.1-or-later |
| json-glib | Reading Railway's answers | LGPL-2.1-or-later |
| Cairo, Pango, HarfBuzz, Graphene, Fribidi, Fontconfig, FreeType | Drawing text and graphics | LGPL, MPL, MIT, FreeType licence (per package) |
| librsvg | SVG icons | LGPL-2.1-or-later |
| Adwaita icon theme, hicolor icon theme | Icons | LGPL-3.0 / CC-BY-SA-3.0, GPL-2.0 (per package) |
| zlib | Compression in the installer and the program | zlib licence |
| libpng, libjpeg-turbo, libtiff, and other image and text libraries | Image loading | Per package |
| GCC runtime (libgcc, libstdc++, winpthreads) | C runtime | GPL-3.0 with the GCC Runtime Library Exception |

The Windows installer (`Source/windows/installer`) is a small hand-written Win32 wizard. Its payload is compressed with zlib.

## Railway CLI

The program runs the Railway CLI (`railway`) as a separate program. The CLI is not part of this package and is not bundled. It has its own licence from Railway.

## Not bundled, loaded if present

libpq (the PostgreSQL client library) is loaded at run time by the rotation code that was set aside. The audit does not use it, and no package bundles it.
