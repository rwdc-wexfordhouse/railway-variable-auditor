# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
# Copyright (c) 2026, Ross Cameron <ross@wexfordhouse.co.za>
# Licensed under the GNU General Public License, version 3.

Name:           railway-variable-auditor
Version:        0.1.0
Release:        8%{?dist}
Summary:        List every Railway environment variable, with its value, in one CSV file
License:        GPL-3.0-only
URL:            https://wexfordhouse.co.za/
Source0:        %{name}-%{version}.tar.gz

# The program was called railway-variable-cycler until 0.1.0-3.
Obsoletes:      railway-variable-cycler < 0.1.0-4

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  pkgconfig(glib-2.0) >= 2.70
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(gtk4) >= 4.10
BuildRequires:  desktop-file-utils

# The Railway CLI is also needed at run time, but it has no RPM. Install it with
# npm i -g @railway/cli, or from https://docs.railway.com/cli, and sign in with
# railway login. It is not listed here because there is nothing to depend on.

%description
Railway Variable Auditor helps you respond to Railway's security disclosure of
21 October 2026. It signs in to Railway through the Railway CLI, finds every
workspace you can access, reads every environment variable of every service and
environment, and writes them, with their current values, to one CSV file for you
to work through by hand. It only reads. It never changes anything in Railway.

The package contains the GTK window (railway-variable-auditor), the same audit as a command (railway-variable-audit), the man pages, and
the documentation website.

The CSV holds live secrets. It is readable by you only. Delete it when you have
finished.

It needs the Railway CLI at run time. The CLI is not packaged, so install it
yourself (npm i -g @railway/cli) and sign in with railway login. The program
never sees your Railway token.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%check
# The live PostgreSQL test needs Docker, so a package build leaves it out.
%meson_test --no-suite live
desktop-file-validate %{buildroot}%{_datadir}/applications/za.co.wexfordhouse.RailwayVariableAuditor.desktop

%files
%license LICENSE COPYRIGHT
%doc README.md
%{_bindir}/railway-variable-auditor
%{_bindir}/railway-variable-audit
%{_mandir}/man1/railway-variable-auditor.1*
%{_mandir}/man1/railway-variable-audit.1*
%{_mandir}/man3/railway.3*
%{_mandir}/man3/rvcore.3*
%{_mandir}/man5/railway-variable-auditor-csv.5*
%{_docdir}/%{name}/Website/
%{_datadir}/applications/za.co.wexfordhouse.RailwayVariableAuditor.desktop
%{_datadir}/icons/hicolor/scalable/apps/za.co.wexfordhouse.RailwayVariableAuditor.svg

%changelog
* Fri Oct 23 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-8
- The Help button and F1 are gone. WebKitGTK is no longer a build dependency.

* Thu Oct 22 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-7
- Windows 11 support added to the source (cross-compiled setup program, built
  with make installer). No change to the Linux programs.

* Thu Oct 22 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-6
- Help (button and F1) opens the installed website in a window with an HTML
  viewer (WebKitGTK 6), at the page for the current screen.

* Thu Oct 22 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-5
- The audit writes one file, audit.csv. run-report.csv is gone: anything that
  could not be read is listed in the window and on the terminal.
- The run folder goes on the Desktop by default (home if there is no Desktop).

* Thu Oct 22 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-4
- Renamed to Railway Variable Auditor (package railway-variable-auditor,
  replaces railway-variable-cycler).
- The program is now an audit: the window and railway-variable-audit write every
  variable, with its value, to one owner only CSV. Rotation, staging, deploying,
  undo and database passwords are set aside and not reachable.
- The read only scan command is no longer installed.
- A policy file in the old railway-variable-cycler config folder is still read.

* Thu Oct 22 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-3
- New railway-variable-audit command: every variable of every workspace, with
  its value, in one owner only CSV. Read only.

* Thu Oct 22 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-2
- Rotate the passwords of Postgres application roles through a secure tunnel,
  with a check before and after, a rollback, undo and a live PostgreSQL test.
- Window: database roles in the review, results and undo screens.

* Thu Oct 22 2026 Ross Cameron <ross@wexfordhouse.co.za> - 0.1.0-1
- First package: the GTK application, the read only scan command, the engine
  and SDK documentation, and the website.
