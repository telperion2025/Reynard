# Reynard

**Reynard — the clever fox of terminals.** A cross-platform, terminal-based
reimplementation of the FoxPro 2.x / xBase environment for Linux, BSD, and
macOS, written in C17 with no runtime dependencies beyond the terminal
library.

It recreates the working feel of FoxPro 2.x for DOS on modern Unix-family
terminals: a System menu bar with pulldowns, a persistent Command window,
full-screen BROWSE, screen and report builders, and batch mode for shell
scripts — all file-based, reading and writing real dBase III+/FoxPro/VFP9
DBF files (including memos and CDX compound indexes).

> Status: milestones **M0** (skeleton + terminal layer) and the **M1 core**
> (lexer, parser, VM, read-only DBF engine, batch/test modes, 28-case
> conformance suite) are implemented. Interactive UI lands at M3; see
> [docs/DEVIATIONS.md](docs/DEVIATIONS.md) for the deviation log and
> [CHANGELOG.md](CHANGELOG.md) for progress. The full specification this
> project implements is summarized in [docs/language.md](docs/language.md)
> and [docs/formats.md](docs/formats.md).

## Features (v1.0 target)

- Fidelity of workflow: System menu bar, Command window, full-screen Browse,
  FoxPro function-key conventions.
- Portability: one codebase for Linux (glibc/musl), FreeBSD, NetBSD,
  OpenBSD, and macOS (arm64 + x86_64).
- Real xBase data compatibility: dBase III+/IV, FoxPro, and VFP9 DBF files,
  FPT/DBT memos, CDX compound indexes.
- Scriptability: batch mode (`reynard prog.prg`, `reynard -c "..."`,
  `reynard -`) and a deterministic test mode (`-t`) for CI.
- Open source core under the 2-clause BSD license (see
  [LICENSE](LICENSE), `reynard --dump-license`).

## Building

Requirements: a C17 compiler (GCC 11+ or Clang 14+), CMake >= 3.16, and
ncurses/ncursesw >= 6.1 for the interactive backend (on macOS a vendored
ncurses 6.4+ is used automatically; without ncurses headers the build
still works — only the headless script backend and batch mode are
available).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # selftest + conformance suite
```

Or with plain make (no CMake), as BSD ports trees prefer:

```sh
make -f Makefile.simple
make -f Makefile.simple test
make -f Makefile.simple install DESTDIR=/tmp/stage
```

CMake options: `-DUSE_SYSTEM_NCURSES=on|off` (default on; off builds the
vendored ncurses 6.4+, used on macOS), `-DENABLE_FUZZ=on` (libFuzzer
harnesses), `-DRX_WERROR=on` (warnings as errors, used in CI).

## Usage

```
reynard [options] [script.prg]
  -c "cmd; cmd"   run commands then exit (batch; ';' splits commands here)
  -t              test mode: deterministic batch (DATE/TIME/RAND frozen)
  -o <file>       batch stdout redirect
  --run <app.rpp> run a packaged application (M9)
  --cfg <file>    alternate config file
  --no-mouse      disable all mouse backends
  --mouse=<m>     force mouse backend: auto|sgr|u1000|1002|1015|gpm|sysmouse
  --utf8=on|off   force wide-char mode
  --version, -V   print version
  --help, -h      help
  --dump-license  print the license
```

Exit codes: `0` success, `1` script/runtime error, `2` usage/environment
error, `3` lock conflict, `4` internal assertion (writes
`reynard-crash.log`).

### Quick start (batch mode)

```sh
$ reynard -c '? "Hello from Reynard:", 6 * 7'
Hello from Reynard: 42

$ cat demo.prg
USE people                && open a DBF (searched in cwd, SET DEFAULT, SET PATH)
? RECCOUNT(), ALIAS()
LIST NEXT 3
GO BOTTOM
? RECNO(), NAME
$ reynard -t demo.prg       # -t freezes DATE()/TIME()/RAND() for reproducible runs
```

### Interactive mode

Running `reynard` with no arguments opens the full-screen environment
(System menu bar, Command window, status line; requires the ncurses
backend). This lands at milestone M3; until then batch mode is the
supported interface.

## Configuration

Config file: `$XDG_CONFIG_HOME/reynard/reynard.cfg` (override with
`--cfg` or the `REYNARD_CFG` environment variable). One statement per
line; `#` comments; parse errors print a warning and are skipped:

```
# comment
COLOR SCHEME 3 = "W+/B, W+/BG, ..."
F12 = "DO mymacro"
HISTORY = 1000
CODEPAGE_DEFAULT = 1252
MOUSE = auto
```

Environment variables: `REYNARD_CFG`, `REYNARD_DATAPATH`,
`REYNARD_CODEPAGE`, `REYNARD_NOUPDATE` (open everything read-only),
`RX_LOG` (`debug|info|warn|error` tracing), plus standard `TERM`,
`COLORTERM`, `LANG` (only `*.UTF-8` enables UTF-8 mode), `TMPDIR`,
`TMUX`, `TERM_PROGRAM`. See [config_examples/reynard.cfg](config_examples/reynard.cfg).

`SET DEFAULT` and `SET PATH` follow FoxPro's search semantics: current
directory first, then the path list.

## The language

Reynard speaks procedural FoxPro 2.6a xBase: `USE`/`SELECT`/`LIST`/
`DISPLAY`/`GO`/`SKIP`/`LOCATE`, `REPLACE`, `? expr`, full expressions with
FoxPro precedence and `SET EXACT` semantics, memvars (`m.var`), and ~230
built-in functions. The complete command and function reference is
[docs/language.md](docs/language.md); binary file layouts are in
[docs/formats.md](docs/formats.md). Man page: [docs/man/reynard.1](docs/man/reynard.1)
(install with `make install`).

## Testing

```sh
ctest --test-dir build --output-on-failure
# or directly:
./build/reynard-harness selftest                 # term/mouse backend self-test
./build/reynard-harness conformance tests/conformance ./build/reynard testdata/gen /tmp/work
```

- `tests/harness.c` — the purpose-built harness: sandboxed workdir per
  case, fixture copy-in, golden output diff (spec §12).
- `tests/conformance/` — `.prg` + `.expected` pairs run in `-t` mode;
  error cases use `.expected.err` and require exit code 1. The mapping
  index is [tests/conformance/INDEX.md](tests/conformance/INDEX.md).
- `tests/golden/` — golden-screen tests via the `term_script` event
  replay backend (M3+).
- `testdata/gen/gen_fixtures.py` — regenerates the synthetic DBF fixtures;
  `testdata/reference/` holds real-tool reference files with
  `PROVENANCE.txt` (spec §8.9).

## Repository layout

```
include/reynard/   installed public headers (dbf.h, status.h, rval.h)
src/main.c         CLI, mode dispatch, config load
src/term/          terminal backends (ncurses, script replay), mouse
src/ui/            screen, windows, menubar, command window, browse (M3+)
src/lang/          lexer, parser, expression compiler, VM, functions
src/data/          DBF/FPT/DBT/CDX engine, work areas, scopes, locking
src/tools/         screen/report/label builders, project manager (M7+)
src/util/          strbuf, UTF-8, code pages, numeric, config, logging
tests/             harness, conformance, golden, unit, fuzz
docs/              language.md, formats.md, QUALITY.md, DEVIATIONS.md, man/
```

Dependency direction is strict: `util <- term <- ui <- lang <- data`.

## License

2-clause BSD — see [LICENSE](LICENSE). Vendored ncurses (macOS builds
only) is under its own MIT-style license; see [REUSE.toml](REUSE.toml).
