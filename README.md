# Railway Variable Auditor

A cross platform desktop tool that signs in to Railway, finds every workspace you can access, and writes every environment variable, with its current value, to one CSV file. You then work your way through it.

On 21 October 2026 Railway disclosed that an unauthorised third party "may have accessed encrypted environment variables and, for some of them, obtained the decrypted contents" ([Railway's disclosure](https://blog.railway.com/p/security-disclosure-wednesday-october-21st)). Railway says the impact may be limited to a subset of users, and recommends that everyone acts. This tool gives you the full list, so that nothing is missed.

Railway's advice is to roll all environment variables, including API keys and any private registry credentials, and to review logs for unusual activity. Railway also paused all deployments while it investigated (incident QH8L8B3E at status.railway.com). Check that page for the latest.

**What this tool covers.** It reads and lists. It never writes to Railway and never redeploys anything. It cannot rotate your keys: rotate each one with its provider, set the new value in Railway, and redeploy. It cannot rotate private registry credentials, which are not variables, and it cannot review your logs. Do both by hand. The website page "Checklist" lists every manual step with links to Railway's own recommendation posts. Railway invalidated all API tokens on 22 October 2026, so sign in again with `railway login`.

Sign in with the Railway CLI (`railway login`), a browser sign-in as yourself that reaches every workspace you can access. The tool never sees your token. A workspace, project or environment that cannot be read is reported with the reason, and the rest carries on.

> **Not official.** This is an independent tool by Wexford House. It is not made, endorsed or supported by Railway. It comes with no warranty (see `LICENSE`). It has not yet been run by its author against a live account, and the Windows build has not been run on Windows 11. `audit.csv` holds live secrets: keep it safe and delete it when you have finished.

## Status

| Part | State |
|---|---|
| `railway-variable-auditor`: the audit window (GTK 4) | Builds and runs: driven end to end on demonstration data in a headless compositor. Not yet used on a real screen with a real account |
| `railway-variable-audit`: the same audit as a command | Built and tested against a fake Railway CLI (file, permissions, no value on the terminal). Not yet run on a live account by the author |
| Railway SDK (`Source/SDK`) | Built and tested: 31 unit tests, no network |
| Engine (`Source/Core`): discover, read, classify, write CSV | Built and tested against a fake Railway, valgrind clean |
| Rotating, staging, deploying, undo and database passwords | **Set aside.** The code is still in `Source/Core` and its tests still run, but no program uses it |
| Windows 11 installer (`make installer`) | Cross-compiled on Linux. Smoke-tested under Wine only (see the Windows section). Not tested on Windows 11. Not code signed |
| macOS | Not tried |

## How to use it

1. `railway login` (or press Sign in with Railway in the window).
2. Press **Audit all my workspaces**, or run `railway-variable-audit --out FOLDER`. This only reads.
3. Check the screen (or the terminal) for anything that could not be read. It is listed with the reason.
4. Open `audit.csv` and work through it. Follow the checklist on the website: databases, buckets, GitHub and registry credentials, third party keys, logs.
5. Set the new values in Railway and redeploy every service that uses them. Run the audit again to check, then delete both audit files.

## Output files

Each audit makes a folder `run-YYYYMMDD-HHMMSS`, owner only, on your Desktop (or where you choose).

The folder holds one file:

- **audit.csv**: workspace, project, environment, service (or `shared`), variable, value, category and reason. **Live secrets**, owner only. Keep it safe and delete it when you have finished.

Values are shown as stored. A reference such as `${{Postgres.DATABASE_URL}}` appears as written. A variable that Railway hides from the API (a sealed variable) appears with an empty value.

The category column is the tool's opinion, to help you sort the list. Nothing is changed because of it: `ini-config`, `database-role`, `database-credential`, `internal-hostname`, `reference`, `unshared`, `shared-config`, `shared-manual` or `shared`. The meanings are on the website page "How it works".

`policy.ini` in your configuration folder (`railway-variable-auditor/`) holds workspaces to skip and extra names. A file in the old `railway-variable-cycler/` folder is still read.

## Railway CLI and limits

The tool needs the Railway CLI. Install it with `brew install railway`, `scoop install railway` or `npm i -g @railway/cli`, then run `railway login`. It was tested with version 5.52.1. The tool sends its requests through `railway api`.

Railway limits API requests per hour (1,000 on Hobby, 10,000 on Pro). For a large account the tool tells you how many requests it needs. The CLI does not report how much quota is left, so the tool counts its own requests.

## Requirements

- A C11 compiler
- Meson and Ninja
- GTK 4.10 or newer
- GLib, GIO and json-glib
- The Railway CLI (needed at run time)
- A Railway account

## Building

Everything builds and the tests pass on Red Hat Enterprise Linux.

```
meson setup build
meson compile -C build
meson test -C build
meson install -C build
```

`meson install` puts the two programs, the man pages and a copy of the website.

### RPM (Rocky Linux 10 and other RHEL 10 systems)

```
Packaging/build-rpm.sh
sudo dnf install Packaging/dist/railway-variable-auditor-0.1.0-8.el10.x86_64.rpm
```

The package holds the window, the audit command, the man pages, the website, a menu entry and an icon. It replaces the old `railway-variable-cycler` package. The unit tests run during the package build. The Railway CLI has no RPM, so install it separately (`npm i -g @railway/cli`) and run `railway login`.

To retake the website screenshots, run `Source/Tools/make-screenshots.sh`. It builds with `-Dautomation=true`, runs the window on invented demonstration data in a private headless compositor (nothing appears on your screen), and saves a picture of every screen. A build made with that option also accepts `--demo` to try the window by hand on the invented data.

### Windows installer (cross-compiled on Rocky Linux 10)

```
Support/windows/setup-toolchain.sh     # once: the mingw64 compiler, meson, podman
make installer                         # build/RailwayVariableAuditor-0.1.0-setup.exe
make windows-test                      # smoke test under Wine in a container (needs docker)
```

`make installer` downloads the pinned MSYS2 UCRT64 packages (GTK 4, json-glib and what they need) into `build/msys2`, checks each against the SHA-256 hash in `Support/windows/msys2.lock`, cross-compiles both programs, and appends the runtime bundle to a small hand-written Win32 setup program. Setup installs for the current user only, with no administrator rights. Silent switches: `/S`, `/D=C:\folder`, `--no-path`, `--no-menu`, `--desktop`. The installer is not code signed, so SmartScreen will warn. The Railway CLI is not bundled: install it with `npm i -g @railway/cli` or `scoop install railway`.

### Red Hat Enterprise Linux

```
sudo dnf install gcc meson ninja-build gtk4-devel json-glib-devel
```

### macOS

```
brew install meson gtk4 json-glib railway
```

### Microsoft Windows (building on Windows itself)

Not tried. In principle, install MSYS2, then in the UCRT64 shell:

```
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-gtk4 mingw-w64-ucrt-x86_64-json-glib
```

The package names above have not been checked against each platform's package manager yet. The libraries themselves are confirmed to build and link on Red Hat Enterprise Linux.

## Documentation

- Website: open `Documentation/Website/index.html` in a browser. The site covers how it works, a checklist of what to do by hand (with links to Railway's own posts), the SDK and engine reference, building, and security.
- Man pages in `Documentation/Man Pages/`:
  - `railway-variable-auditor.1`: the window
  - `railway-variable-audit.1`: the same audit as a command
  - `railway-variable-auditor-csv.5`: the files
  - `railway.3`: the SDK
  - `rvcore.3`: the engine
  - `THIRD-PARTY-NOTICES.md` lists what the Windows installer bundles

  Read one with `man -l "Documentation/Man Pages/railway.3"`. The man page of the old scan command is kept in `Documentation/Set Aside/`.

## Reporting a problem

See `SECURITY.md` for security problems. For anything else, open an issue on GitHub.

## Licence

Released under the GNU General Public Licence, version 3. See `LICENSE`.

Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
