# Changelog

All notable changes per milestone (spec §0.5). Format: Keep a Changelog;
Reynard is pre-1.0, versions are milestone tags M0..M10 then v1.0.0.

## [Unreleased] — M0..M4

### M0 — Skeleton and term layer
- CMake + Makefile.simple builds, zero warnings under -Wall -Wextra
  -Wpedantic (-DRX_WERROR=on in CI); ncurses backend auto-detected,
  script backend always available.
- src/util: strbuf, str, utf8, cp_tables (CP437/850/866/1252), ddatecnv,
  numeric, errlist (Appendix D catalog), log (RX_LOG), cfg.
- src/term: term vtable (term_if.h, keymap.h), term_ncurses.c (SGR-1006
  primary mouse), term_script.c (event replay + golden cell dumps),
  mouse.c (SGR/1000/1015 normalizer).
- src/main.c: full §10.1 CLI, exit codes, config load, batch/test modes.
- tests/harness.c: selftest (M0 gate) + conformance runner; ctest wired.
- CI matrix (.github/workflows/ci.yml): ubuntu/macos x gcc/clang,
  ASan+UBSan conformance, Makefile.simple build.

### M1 — Command skeleton
- Lexer/parser (4-letter keyword abbreviation, ';' continuation, && /*
  comments, {^date} literals), expression compiler to bytecode, VM with
  typed stack, lazy .AND./.OR., FoxPro EXACT/padding comparison rules.
- Read-only DBF engine (versions 0x03/0x04/0x30/0x31/0x32/0x83/0x8B/0xF5),
  code page marks, field decode, work areas 1..255, FoxPro BOF/EOF/SKIP
  semantics; blank field values at EOF.
- Commands: ?/??, =, STORE, USE/CLOSE/SELECT, LIST/DISPLAY (ALL/REST/NEXT/
  RECORD, HEADINGS, DELETED), GO/GOTO, SKIP, SET (M1 subset; full
  Appendix B list parses), QUIT, CLEAR; BROWSE raises R_ERR_NOUI in batch.
- ~60 built-in functions (Appendix A subset); function names resolve at
  runtime.
- §6.6 error format with clang-style caret.
- 28 conformance cases (tests/conformance) green under ASan/UBSan;
  testdata/gen fixtures + fuzz seeds; PROVENANCE.txt policy.
- Docs: README (user documentation), docs/language.md, docs/formats.md,
  docs/DEVIATIONS.md, docs/QUALITY.md, man page, config example.

### M2 — Data engine v1
- DBF write path: rx_dbf_create/_full, append, buffered put + rx_flush,
  delete/recall flag writes, PACK (record + memo compaction), ZAP, real
  reopen; header maintenance with frozen last-update date in -t.
- FPT memos (in-place fit / free-list extend, lazy load, code-page
  conversion) and DBT memos (append-per-write, terminator reader).
- fcntl byte-range locks (file lock at byte 0, record locks at
  100000000+recno) with SET REPROCESS retry policy; FLOCK()/RLOCK()/
  UNLOCK; forked-process lock-race test (tests/harness lockrace).
- Transaction journal <stem>.RJN with header byte-30 stamp and
  complete-or-rollback recovery on open.
- Scopes ALL/REST/NEXT/RECORD + FOR/WHILE + SET FILTER on LIST, DISPLAY,
  DELETE, RECALL, REPLACE, LOCATE, COPY; FoxPro default-scope rule
  (FOR/WHILE without scope = ALL); LIST FIELDS/OFF, deleted `*` marker.
- Commands: APPEND BLANK, APPEND FROM (DBF/SDF/DELIMITED [WITH ...]),
  REPLACE [scope] f WITH e (incl. alias->field), DELETE, RECALL, PACK,
  ZAP, LOCATE/CONTINUE, COPY TO (DBF/SDF/DELIMITED), COPY STRUCTURE
  [EXTENDED], CREATE ... FROM, FLUSH, TYPE, UNLOCK; SET EXCLUSIVE,
  MULTILOCKS, REPROCESS, FILTER.
- COPY/APPEND SDF/DELIMITED serializers in data/copyto.c; work-area
  alias resolution; FOUND()/LUPDATE() real.
- Tests: 48 conformance cases (30-48 new), dbf_roundtrip (byte-identical
  read/write of every fixture + memo FPT determinism + pack), lock_race
  (ctest), all green under ASan/UBSan; Makefile.simple test target runs
  all four suites.

### M3 — Interactive shell
- src/ui: screen.c (rcell canvas, 64-pair palette, damage rects,
  differential blit), window.c (uniform pane stack, border-drag move /
  corner-drag resize, clamping), getsay.c (shared line editor: cursor
  motion, Ctrl+Y, 1000-entry history ring persisted to
  $XDG_CACHE_HOME/reynard/history, Tab command-name completion),
  cmdwin.c (Command window: 5000-line scrollback ring, exec via
  rx_run_source, Ctrl+S pause / Esc abort per SET ESCAPE), menudef.c +
  menubar.c (declarative FoxPro 2.x menubar, pulldowns, mouse click-open
  and drag-sweep, F10/Alt/arrows keyboard path), statusbar.c (message
  line left; CAPS/NUM/clock/workarea+alias/rec N/M/DEL right), dialog.c
  (About, Macros, Filer, Calculator, Calendar/Diary, ASCII Chart),
  helpwin.c, ui.c (rx_ui_run event loop; owns the backend lifecycle).
- term vtable gains size(w,h); script backend honors RX_COLS/RX_ROWS and
  `resize w h` events; ncurses backend uses TIOCGWINSZ (SIGWINCH stable).
- lang: rx_ctx out_fn/msg_line hooks — interpreter output scrolls in the
  Command window and §6.6 diagnostics land on the message line (with
  beep) in interactive mode, stderr in batch; rx_run_source() and
  rx_eval_str() public; rx_keyword_list() for Tab completion.
- main.c: interactive mode dispatches to rx_ui_run; the script backend is
  selected when RX_EVENTS is set (CI golden runs use `reynard -t`).
- Mouse capture: the window that consumes RM_PRESS keeps the drag until
  RM_RELEASE; release never triggers an action (§5.4).
- FoxPro 2.x for DOS color scheme (reference screenshots): attr nibbles are
  FoxPro/CGA palette indices (fg | bg<<4) mapped through one CGA->curses
  table shared by both term backends — blue desktop, silver menubar/
  pulldowns/status (black text, white menubar hotkeys, yellow pulldown
  hotkeys, bright-cyan active title and selected item), bright-cyan
  Command window with white border, silver dialogs with white border and
  blue title bar.
- Fixes: ncurses poll left the event uninitialized on timeout, so the UI
  loop re-handled the previous event (repeating keypresses); and with
  SGR-1002 button tracking, drags arrive as repeated BUTTON1_PRESSED —
  now translated to RM_DRAG, so window/menubar dragging works on real
  terminals, not just in the scripted tests. Every window gets a ×
  close button on its title bar (Command window hides instead).
  Fixed a use-after-free when a captured window closed mid-drag.
- Bug fix: bare expression statements starting with a non-word token
  (e.g. `2+3*4`) previously looped the parser forever; the ST_EXPR
  fallthrough now applies to every leading token kind, with a
  no-progress guard in rx_parse.
- Tests: harness `golden` mode — 8 event-replay scenarios in
  tests/golden (boot layout, mouse-visit of every System menu action,
  Command window exec + history recall, resize, error on message line,
  window border drag, keyboard menubar path, Calculator eval), compared
  cell-by-cell; wired into ctest (golden_screens) and the Makefile.simple
  + CI plain-make paths. All suites green under ASan/UBSan with leak
  detection.
- term_ncurses fixes for real terminals (macOS Terminal.app): mouse mode
  sequences (SGR 1006 + 1002 + 1003) are written with write(2) before
  initscr()/after endwin() — stdio printf interleaved with curses output
  and printed "?1002h"-style garbage on screen; narrow-ncurses builds
  (macOS system 5.7, no ncursesw) draw box borders via the alternate
  charset and transliterate —/×/√/… to -/x/v/. instead of '?'.
- CMake/Makefile.simple: on macOS prefer a Homebrew ncursesw
  (/opt/homebrew/opt/ncurses, /usr/local/opt/ncurses) for native wide
  glyphs; setlocale(LC_ALL, "") at backend init. Golden suite now 9
  scenarios (close button; corner-resize drag in 06).
  MacPorts' /opt/local prefix is searched too (`sudo port install ncurses`).
  Header probing is layout-robust (ncursesw/, ncurses/, flat curses.h under
  each prefix; wide header wins), and term_ncurses.c's include now matches
  the RX_HAVE_NCURSESW_H macro CMake actually sets.
  Flat layout (MacPorts/BSD: include/curses.h + lib/libncursesw.*) is
  treated as wide-capable via new RX_HAVE_NCURSESW_FLAT (ncurses 6 flat
  headers declare the wide API); the /usr system copy stays narrow.
- Palette fix: attr is now the full fg|bg<<4 slot into a 256-entry (16x16)
  palette in both backends, and the ncurses side uses 1-based pairs
  (COLOR_PAIR(attr+1)); previously the 0x3F mask truncated bg to 2 bits and
  an off-by-one shifted every pair, rendering the silver menubar green and
  the cyan Command window black on real terminals (golden dumps are
  text-only, so CI never saw it). Selftest expects 256 slots; DEVIATIONS #25.
- term_ncurses: sync curses' stdscr to the real TIOCGWINSZ geometry at init
  and on KEY_RESIZE (resizeterm) — a stale LINES/COLS clipped blits, leaving
  unpainted black margins where window borders should be. Brute-force mouse:
  all protocol tiers enabled (1000/1002/1003/1015/1006) after the first
  refresh(), and raw ESC-led sequences that terminfo XM doesn't know are
  decoded by mouse.c in nc_poll (with ungetch pushback), so SGR and legacy
  press/drag/release/wheel work regardless of the terminal's XM entry.
  --no-mouse / REYNARD_NO_MOUSE skips enabling entirely.
- Mouse drag fix: mouse.c fill() had the motion mapping inverted — in both
  SGR 1006 and legacy 1002/1003, a button-1 drag is b = 32|0 (b&3 == 0), which
  decoded as RM_MOVE, while no-button motion (b&3 == 3) decoded as a drag;
  legacy X10 release (b == 3) also decoded as a press. Fixed: motion with
  b&3 == 3 is RM_MOVE, otherwise RM_DRAG with button (b&3)+1; release is SGR
  'm' or X10 b == 3. nc_poll's KEY_MOUSE path now also maps
  REPORT_MOUSE_POSITION to RM_DRAG while button 1 is held (any-motion
  terminals report drags as pure motion), and the raw path pushes back any
  bytes past the decoded event and keeps the button-held state in sync.
  Both paths log `RX_LOG=debug` traces ("nc mouse ...") for terminal-side
  diagnosis. Selftest covers SGR/legacy drag, hover and release vectors.
- UI opacity + border color: window and pulldown interiors are now filled
  with the window's own background (Command window cyan, dialogs/pulldowns
  silver), so windows are opaque instead of letting the desktop/borders
  show through empty cells; window borders and the × close button are black
  on the window background (RXA_CMD_BORDER black/bcyan, RXA_DLG_BORDER
  black/white). Golden 08 regenerated — it had encoded the see-through
  dialog (Command window border showing through the Calculator).
- Text clipping: new scr_textn_clip() stops drawing at a column limit; the
  Command window scrollback, dialog lines, the getsay input line, window
  titles and the status-line message are all clipped to their container, so
  long lines can no longer spill over a window border. ASCII Chart dialog
  widened to 56x21 and Calendar/Diary to 34x13 so their own content fits.
  New golden 10_text_clipping locks this in (200-char output clipped at the
  Command border; ASCII grid fully inside its dialog).

### M4 — Browse v1 (spec §9.1, gate §14)
- src/ui/browse.c: BROWSE/CHANGE grid as a full-workspace window on the
  uniform pane stack, bound to one work area (multiple browse windows on
  different areas supported). Columns default to all fields; BROWSE
  FIELDS supports :H/:W/:P/:V[:E]/:F/:R plus computed expression columns.
  FREEZE, PARTITION (fixed left pane + scrollable right), LEDIT/REDIT,
  LOCK, WIDTH, NOAPPEND/NODELETE/NOEDIT/NOMODIFY, NOWAIT, TITLE, FOR, WHEN,
  record-level VALID[:E] all parsed (parser.c) and honored.
- Editing: in-cell typing replaces the cell (FoxPro), Enter/Tab/Up/Down
  commit + move, arrows edit within the text, Esc cancels; commits parse per
  field type (C/N/F/I/D via CTOD/L), take an fcntl record lock in shared
  mode (§8.5 shared locking), evaluate :V then record VALID, revert on
  failure with the :E message on the message line. Ctrl+T toggles the
  delete mark, Ctrl+N appends (bare APPEND opens append-mode browse).
  Memo/G/P cells read-only (DEVIATIONS #27). Mouse: click cell, drag
  selection range, wheel scroll, header drag reorder, border drag resize,
  double-click border size-to-widest (§5.4).
- lang: rx_ctx gains the browse_fn hook (NULL in batch -> R_ERR_NOUI 601);
  SET RELATION TO/OFF INTO/ADDITIVE and SET SKIP TO implemented
  (rx_rel_apply fires on GO/SKIP/LOCATE via rx_nav_goto/rx_nav_skip;
  recno-keyed without indexes, DEVIATIONS #30). menudef Database > Browse
  and Record > Append now run BROWSE/APPEND.
- ui: status bar right side shows work area + alias + rec N/M again
  (USED() is logical; statusbar now reads it as such). RX_PERF=<file>
  times each UI event's handle+render for the perf gate.
- Tests: conformance 49 -> 60 cases (relations, SET SKIP, relation
  errors, BROWSE clause parse errors, bare-APPEND NOUI); golden screens
  10 -> 16 (browse basic/edit/partition-freeze/mouse/relation/fields);
  new `browseperf` harness mode = §11.3 gate: 100k-record DBF, 300 PgDn
  scripted browse, asserts avg handle+render <= 16ms (measured ~0.1ms);
  wired into ctest (browse_perf) and Makefile.simple test, so CI enforces
  it on every matrix job.
