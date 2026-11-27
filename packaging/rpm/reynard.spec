Name:           reynard
Version:        1.0.0
Release:        1%{?dist}
Summary:        Cross-platform terminal reimplementation of FoxPro 2.x/xBase
License:        BSD-2-Clause
URL:            https://github.com/telperion2025/Reynard
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  cmake >= 3.16, gcc, ncurses-devel
Requires:       ncurses

%description
Reynard is a C17 reimplementation of the FoxPro 2.x/xBase developer
environment for modern POSIX terminals: DBF tables with CDX/IDX indexes,
the xBase language, BROWSE, @ SAY/GET forms, screen builder, reports,
menus, projects and a help browser, with deterministic -t test mode.

%prep
%autosetup

%build
%cmake -DCMAKE_BUILD_TYPE=Release -DRX_WERROR=on
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md CHANGELOG.md
%{_bindir}/reynard
%{_mandir}/man1/reynard.1*

%changelog
* Fri Nov 27 2026 The Reynard contributors <noreply@example.invalid> - 1.0.0-1
- M0-M10 release: full FoxPro 2.x reimplementation milestone set.
