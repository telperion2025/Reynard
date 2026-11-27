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

> Status: milestones **M0** (skeleton + terminal layer), **M1** (lexer,
> parser, VM, read-only DBF engine, batch/test modes), **M2** (data
> engine v1: DBF create/read/write, FPT/DBT memos, scopes + FOR/WHILE +
> SET FILTER, LOCATE, locking with a forked-process race test, COPY/APPEND
> SDF/DELIMITED, 48-case conformance suite + byte-identical round-trip
> gate), **M3** (interactive shell: menubar + pulldowns with mouse and
> keyboard, window stack, Command window with persistent history, status
> line, System-menu dialogs; 10 golden-screen tests) and **M4** (Browse v1:
> full-screen grid window with in-cell editing, FREEZE/PARTITION/LEDIT,
> mouse reorder/resize, Ctrl+T/Ctrl+N, SET RELATION/SET SKIP, shared
> locking on edits; 60-case conformance suite incl. scripted-BROWSE
> goldens; §11.3 browse perf gate in CI) and **M5** (language core:
> IF/DO WHILE/FOR/DO CASE/SCAN, PROCEDURE/FUNCTION UDFs with recursion,
> PUBLIC/PRIVATE scopes, DIMENSION arrays + array functions, & macros at
> execution time, ON ERROR/RETRY with ERROR()/MESSAGE(), WAIT/ACCEPT/INPUT,
> TEXT...ENDTEXT, .RXP compiled-program cache with a byte-reproducibility
> gate; 71-case conformance suite) and **M6** (index engine §8.4: CDX
read/write with structural tags, standalone IDX, read-only NTX import,
INDEX ON/SEEK/REINDEX/SET INDEX/SET ORDER/SET UNIQUE, tag-ordered
navigation and scopes, automatic index maintenance on write, ORDER/TAG/
TAGCOUNT/CDX/MDX/NDX/KEY/KEYCOUNT/TARGET/RELATION functions; include/reynard
API frozen; interop validator + byte-reproducibility + 100k-key perf gate in
CI; 81-case conformance suite), **M7** (forms §9.2: @ SAY/GET/READ with WHEN/VALID/RANGE/PICTURE/SIZE/DEFAULT, CYCLE, mouse, golden-tested; PICTURE/TRANSFORM engine; screen builder CREATE/MODIFY/GENERATE SCREEN with .SCR/.RSO formats and the build→generate→execute round-trip gate; COUNT/SUM/AVERAGE/CALCULATE, SORT TO, TOTAL TO, SCATTER/GATHER; 90-case conformance suite, 21 golden scenarios), **M8** (reports/labels §9.3-9.4: .FRM/.RLB formats, band engine with groups/variables, 5 deterministic emitters (text/PS/PDF/HTML/CSV) with the CI hash gate, preview window with zoom, CREATE/MODIFY REPORT/LABEL designer; 93-case conformance suite, 22 golden scenarios), **M9** (menu generator \u00a79.5 with the menubar app pad; transactions \u00a78.6 with .RJN before-image journal and crash recovery; project manager \u00a79.6: .RPJ -> BUILD PROJECT -> byte-deterministic .RPP + launcher, `reynard --run app.rpp`, SET PROCEDURE TO packed sources; help browser \u00a79.7 backed by a help DBF with SET HELP TO and ON KEY LABEL F1; projcheck gate + conformance 94-97 + goldens 23-24; 97-case conformance suite, 24 golden scenarios) are implemented. See
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
ncurses/ncursesw >= 6.1 for the interactive backend. On macOS the system
ncurses is narrow-only (5.7): `brew install ncurses` or
`sudo port install ncurses` (MacPorts, /opt/local) gives ncursesw and is
picked up automatically (CMake prefix path and Makefile.simple probe);
without ncursesw the UI falls back to ACS line-drawing and ASCII
transliteration. Without ncurses headers the build
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
(requires the ncurses backend): menubar on row 0 (F10 or Alt activates,
arrows navigate, Enter selects, Esc backs out; the mouse clicks titles to
open and drags across them to sweep), a floating Command window at the
bottom (type a command, Enter runs it into the window's scrollback;
Ctrl+Up/Down walk the persisted history, Tab completes command names,
Ctrl+Y kills to end of line, Ctrl+S pauses, Esc aborts per SET ESCAPE).
Every window has a × close button on its title bar; drag a window's
border to move it and a corner to resize it (the Command window hides
on close — Ctrl+F2 or Window > Command brings it back)., and a status line showing
the last message plus work area/alias/record pointer. System-menu dialogs
(About, Filer, Calculator, Calendar/Diary, ASCII Chart, Help) are on the
same window stack. QUIT or File > Quit exits with code 0. The color
scheme is the FoxPro 2.x for DOS look: blue desktop, silver menubar,
status line and dialogs, bright-cyan Command window and active menu
items (16-color CGA palette).

`reynard -t` with no script is the same interactive shell with
DATE()/TIME()/RAND() frozen — the harness drives it through the script
backend (`RX_EVENTS`/`RX_DUMP`) for the golden-screen tests.

### Terminal notes (macOS Terminal.app)

Mouse reporting is negotiated per spec §5.4: SGR 1006 is enabled first, with
1002 (button drag) / 1003 (motion) alongside; legacy 1000 bursts are
auto-detected by the ncurses parser. For native wide glyphs on macOS use
`brew install ncurses` or `sudo port install ncurses` (MacPorts, /opt/local).
Both build files probe `ncursesw/curses.h`, `ncurses/curses.h` and flat
`curses.h` under those prefixes. If detection misses, check
`port contents ncurses | grep curses.h` and reconfigure from a clean
build dir (`rm -rf build`) — CMake caches the probe results. The mode sequences are written with
`write(2)` before `initscr()` / after `endwin()` so they never interleave
with curses output. If your terminal swallows mouse events entirely, run with
`--no-mouse` (or `SET MOUSE OFF`) — every menu action is also reachable by
keyboard (F10/Alt + arrows + Enter). Box borders and the × close button use
the alternate charset when only narrow ncurses is available.

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
- `tests/golden/` — golden-screen tests: event-replay scenarios run
  through the `term_script` backend and compared cell-by-cell
  (`tests/golden/README.md` maps each scenario to its spec section).
- `testdata/gen/gen_fixtures.py` — regenerates the synthetic DBF fixtures;
  `testdata/reference/` holds real-tool reference files with
  `PROVENANCE.txt` (spec §8.9).

## Repository layout

```
include/reynard/   installed public headers (dbf.h, status.h, rval.h)
src/main.c         CLI, mode dispatch, config load
src/term/          terminal backends (ncurses, script replay), mouse
src/ui/            screen, windows, menubar, command window, dialogs (M3); browse (M4+)
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
