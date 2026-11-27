# Reynard packaging (spec §13, M10)

Release artifacts are produced from the repo root with CPack (CMake) or with
the per-distribution recipes in this tree. All recipes build from a signed
release tag (`v1.0.0`); CI produces and uploads them automatically (see
`.github/workflows/ci.yml` job `package`).

| Target | Recipe | Output |
|---|---|---|
| Debian/Ubuntu `.deb` | CPack `DEB` generator (metadata below) or `packaging/debian/` with `dpkg-buildpackage` | `reynard_1.0.0-1_amd64.deb` |
| Fedora/RHEL `.rpm` | CPack `RPM` generator or `packaging/rpm/reynard.spec` with `rpmbuild` | `reynard-1.0.0-1.x86_64.rpm` |
| BSD ports | `packaging/ports/Makefile` (FreeBSD; OpenBSD/NetBSD adapt `pkg-plist`) | `reynard-1.0.0.pkg` |
| Homebrew (macOS/Linux) | `packaging/homebrew/reynard.rb` formula | bottle |
| Alpine (musl, static) | `packaging/alpine/APKBUILD` | `reynard-1.0.0-r0.apk` |
| macOS universal zip | `packaging/macos/make-universal2-zip.sh` (x86_64 + arm64 via `lipo`) | `reynard-1.0.0-macos-universal2.zip` |

Dependencies at runtime: a POSIX system, `libc`, and ncurses (wide build
preferred: `libncursesw6`; narrow `libncurses6` works). Batch/`-t` mode and
the script terminal backend need no ncurses at all. When no system ncurses is
available, build with `-DUSE_SYSTEM_NCURSES=off` to compile the pinned
vendored ncurses (see `vendor/fetch-ncurses.sh`, ncurses 6.5, sha256-pinned).

## CPack metadata (also set in CMakeLists.txt)

- name `reynard`, version from `project(... VERSION ...)`, vendor
  "The Reynard contributors"
- Debian: section `devel`, depends `libc6, libncursesw6 | libncurses6`
- RPM: license `BSD-2-Clause`, group `Development/Tools`, requires `ncurses`
- Installs: `bin/reynard`, `share/man/man1/reynard.1`,
  `share/doc/reynard/{README.md,CHANGELOG.md,LICENSE,licenses/}`
