# Changelog

All notable changes per milestone (spec §0.5). Format: Keep a Changelog;
Reynard is pre-1.0, versions are milestone tags M0..M10 then v1.0.0.

## [Unreleased] — M0..M6

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

### M5 — Language core (spec §7.1–§7.5, gate §14)
- Control flow: IF/ELSEIF/ELSE/ENDIF, DO WHILE/ENDDO, FOR/STEP/NEXT/ENDFOR,
  DO CASE/OTHERWISE/ENDCASE, SCAN [scope] [FOR/WHILE] /ENDSCAN with LOOP and
  EXIT (EXIT/LOOP leave the innermost loop only), nested blocks arbitrary.
- Program structure: PROCEDURE/FUNCTION ... ENDPROC/ENDFUNC (registered for
  the whole file before execution, FoxPro order-independence), DO name [WITH
  args], RETURN [expr], user functions callable as UDFs from expressions
  (recursion, depth limit 64), PARAMETERS/LPARAMETERS (by value — DEVIATIONS
  #34), PUBLIC/PRIVATE [ALL LIKE/EXCEPT pat] with per-call frame scopes,
  RELEASE [ALL [LIKE/EXCEPT]] / RELEASE MEMVAR.
- Arrays: DIMENSION/DECLARE a[d1] / a[d1,d2], element get/set a[i,j]
  (1-based, row-major), bare array name in an expression yields element 1;
  AELEMENT, ASUBSCRIPT, ACOPY (creates the target), ADEL, AINS, ASCAN,
  ASORT, AFIELDS, ADIR, ALEN; TYPE("a") = "A".
- Macros: &var expands at execution time per statement (raw source spans are
  kept per statement and re-parsed after expansion, so & sees memvars
  assigned earlier in the program), depth limit 16, && line comments,
  &name as a call argument passes the name (array functions).
- Errors: ON ERROR DO handler (suppresses the default diagnostic, resumes
  after the erroring statement), RETRY re-runs it (storm-guarded at 1000),
  ERROR()/MESSAGE()/MESSAGE(1) introspection inside the handler, ON ERROR
  clears; ON ESCAPE/ON KEY LABEL accepted (batch: no dispatch).
- I/O: WAIT (non-blocking in -t), ACCEPT, INPUT (reads ctx->in = stdin),
  TEXT...ENDTEXT verbatim blocks (lexer skips the raw span), CANCEL, SUSPEND/
  RESUME (batch: note only — DEVIATIONS #35).
- TYPE("name") now resolves memvars/fields ("U" when undefined) per §7.6.
- .RXP compiled cache (spec §7.4): `reynard script.prg` writes script.rxp
  next to the source (fallback $XDG_CACHE_HOME/reynard/rxp/<sha256>.rxp),
  magic RXP1 + sha256 of the source; the serialized AST is little-endian
  fixed-width so the same source yields byte-identical .RXP files; new
  `rxp` harness mode asserts byte-reproducibility + cache-hit equivalence
  (ctest rxp_repro, Makefile.simple test).
- Tests: conformance 60 -> 71 cases (control flow, procs/UDFs, arrays,
  macros, ON ERROR/RETRY, TEXT, SCAN, scopes, WAIT/ACCEPT/INPUT via
  per-case .stdin, & undefined-macro error); all green under -Werror and
  ASan/UBSan, plus Makefile.simple.

### M6 — Index engine and API freeze (spec §8.4, gate §14)
- Data layer (src/data/cdx.c, idx.c, ntx_read.c; internals in
  cdx_internal.h): compound CDX read/write with 500-byte B+tree blocks and a
  chained tag directory; standalone IDX read/write ("RIDX" header block);
  read-only NTX/NDX import with a diagnostic for compressed variants.
  Structural CDX (DBF stem) auto-attaches on USE and its first tag is the
  controlling order; structural writes happen only under exclusive open.
  In-memory tags are sorted lazily (append + qsort on first read), so a
  100k-key build is linear (§11.3 gate: measured 72 ms, gate < 2000 ms).
- Public API include/reynard/*.h frozen at M6: rx_index_attach,
  rx_index_create/clear/insert/delete_rec, rx_index_seek(_tag),
  rx_index_order_set/get, rx_tag_count/info/for_expr, rx_index_file_*,
  rx_index_key_count/recno_at/pos_of, rx_index_flush/close_all/
  close_nonsstructural, rx_key_encode. All public functions Doxygen'd.
- Language (src/lang/cmds_index.c): INDEX ON e TO file | TAG t [OF cdx]
  [FOR e] [UNIQUE] [DESCENDING] [ADDITIVE], SEEK e (EXACT-aware, SET DELETED
  ON skips deleted entries), REINDEX, SET INDEX TO [list] [ADDITIVE],
  SET ORDER TO [n | [TAG] t [OF cdx] | e], SET UNIQUE ON|OFF,
  USE ... INDEX f1[,f2] [ORDER n|TAG t [OF cdx]].
- Ordered navigation: with a controlling order, GO TOP/BOTTOM and SKIP move
  in tag order (BOF/EOF rules preserved); LIST/DISPLAY/LOCATE/CONTINUE/
  SCAN/REPLACE/DELETE iterate in tag order.
- Maintenance: REPLACE / APPEND BLANK / APPEND FROM / COPY TO dbf re-insert
  the record into every tag; PACK rebuilds all tags; ZAP clears them; FLUSH
  and CLOSE write dirty index files; SET RELATION is index-keyed (SEEK in
  the target's order) when the target has an active order.
- Introspection functions: ORDER, TAG, TAGCOUNT, CDX, MDX, NDX, KEY,
  KEYCOUNT, TARGET, RELATION, FILTER (real text now).
- Tests: new `cdx` harness mode (ctest cdx_engine + Makefile.simple test) —
  independent byte-level interop validator (walks dir chain + every B+tree,
  checks node types, sibling links, key ordering incl. DESC, key counts,
  recno ranges), byte-reproducible rebuild check, reopen + seek checks,
  multi-tag + DESC validation, IDX round-trip, 100k perf gate; conformance
  71 -> 81 cases (72–81 cover INDEX ON CDX/IDX, SEEK EXACT, UNIQUE, FOR
  tags, maintenance, PACK/DELETE, order numbers, error 302, index-keyed
  relations); all green under -Werror and ASan/UBSan, plus Makefile.simple.

### M7 \u2014 Forms (\u00a79.2) + aggregates/restructuring (\u00a77.1)
- @ r,c SAY e [PICTURE/FUNCTION] paints on the desktop interactively; in batch
  each @ SAY prints one stdout line (DEVIATIONS #47). @ r,c [SAY e] GET var
  [SIZE h,w] [PICTURE] [FUNCTION] [WHEN] [VALID [:E]] [RANGE lo[,hi]]
  [DEFAULT] [COLOR SCHEME n] registers a pending GET; SAY-side and GET-side
  PICTURE/FUNCTION are distinguished by position relative to GET.
- READ [CYCLE] runs the modal editor loop (src/ui/read.c): full keyboard
  nav (Tab/Shift-Tab/Enter/arrows), mouse click on field hitboxes, whole-field
  highlight on entry (first printable key replaces), Esc aborts (READKEY 27),
  Ctrl+W commits and exits; CLEAR GETS / CLEAR READ [ALL] supported. READ with
  no pending GETs is a no-op; READ with GETs and no UI raises 601.
- Commit semantics: VALID/WHEN evaluate against the *edited* value (staged
  into the field/memvar, restored on rejection); RANGE checked numerically;
  on exit all GETs store back to field (with index maintenance) or memvar.
- PICTURE/TRANSFORM engine (src/lang/picture.c): codes A X 9 # $ * . , L Y
  N ! R I B, @D/@E dates; numeric templates space-fill '9' slots, keep comma
  slots, float $, consume a sign slot for negatives, fill *** on overflow,
  @B left-justifies. Input validation via rx_picture_input_ok.
- Screen builder (spec \u00a79.2): CREATE/MODIFY SCREEN name opens the designer
  (src/ui/screenb.c: arrows move, S say, G get, B box, X delete, mouse click,
  Ctrl+W saves, Esc cancels); GENERATE SCREEN name [TO f.prg] emits @ SAY/GET
  + READ CYCLE .PRG (headless, conformance-tested). Formats in
  docs/formats.md \u00a76: canonical text .SCR (byte-identical load/save
  round-trip) and compiled .RSO (magic RRSO + sha256-of-.SCR + fixed-width LE
  item records, byte-reproducible). DO foo falls back to running foo.prg.
- Aggregates/restructuring (\u00a77.1): COUNT/SUM/AVERAGE/CALCULATE
  (CNT/SUM/AVG/MIN/MAX TO var|ARRAY), SORT TO ... ON f [/A|/D|/C] multi-key,
  TOTAL TO ... ON expr (group sums into a full-structure target),
  SCATTER [FIELDS] [MEMO] TO ARRAY|MEMVAR [BLANK], GATHER FROM ARRAY|MEMVAR.
- Data fix: reading a dirty record buffer now sees pending rx_dbf_put writes
  (was: unconditional disk reload); REPLACE/DELETE/RECALL leave the record
  pointer put (FoxPro semantics; LIST/DISPLAY/SCAN still end at scope end).
- Tests: conformance 82\u201390 (aggregates, SORT/TOTAL, SCATTER/GATHER,
  PICTURE numeric/char, batch @ SAY, READ-no-UI 601, GENERATE SCREEN);
  golden scenarios 17\u201321 (READ field/memvar/VALID/CYCLE, designer,
  build\u2192generate\u2192execute gate); new `scrcheck` harness mode (M7
  builder round-trip gate: .SCR byte-identical re-save, .RSO compile/load,
  codegen snapshot). 90 conformance cases, 21 golden scenarios, 9 ctest
  targets \u2014 all green under -Werror and ASan/UBSan, plus Makefile.simple.

### M8 \u2014 Reports and labels (\u00a79.3/9.4, \u00a77.7)
- Formats (docs/formats.md \u00a77): canonical text .FRM (bands TITLE/
  PAGEHEADER/GROUPHEADER/DETAIL/GROUPFOOTER/PAGEFOOTER/SUMMARY with TEXT/
  FIELD/LINE/BOX objects, PAGE/MARGIN, VAR aggregates) and .RLB (STOCK/SIZE/
  SPACING/MARGINS + detail band). Load/save round-trips byte-identically.
- Engine (src/lang/rpt.c): page breaks, group breaks on group-expr change
  (header/footer), report variables SUM/COUNT/AVG/MIN/MAX reset per
  REPORT/PAGE/GROUP with INITIAL, date + "Page N" header line, PLAIN,
  HEADING, SUMMARY, RANGE n1,n2, scope/FOR/WHILE via the shared iterator;
  labels render one label per page. Deterministic in -t (frozen DATE()).
- Emitters (src/lang/rpt_emit.c): text (rasterized pages, \f separators),
  PostScript (Helvetica, text/lines/rects), PDF (built-in writer, base-14
  Helvetica, no embeds, content-hash /ID \u2014 hash-stable), HTML (tables),
  CSV (detail rows, rtrimmed, quoted). TO FILE picks emitter by extension;
  TO PRINTER spools text to reynard.prn; TO PREVIEW opens the preview
  window (src/ui/preview.c: pages PgUp/PgDn, mouse wheel/click, zoom
  80/100/133/200% with +/-, Esc/Ctrl+W exit).
- Commands: REPORT FORM / LABEL FORM [clauses] (7.7), CREATE/MODIFY
  REPORT/LABEL (interactive designer src/ui/rptb.c; batch CREATE writes an
  empty form, MODIFY re-saves canonically).
- Gates: new `rptcheck` harness mode \u2014 text emitter golden-compared
  byte-exact, ps/pdf/html/csv hash-stable (sha256 goldens) in CI; conformance
  91\u201393 (REPORT FORM clauses, LABEL FORM, CREATE/MODIFY round-trip);
  golden 22 (preview window). 93 conformance cases, 22 goldens, 10 ctest
  targets \u2014 all green under -Werror and ASan/UBSan, plus Makefile.simple.

### M9 - Project manager, menu generator, help browser, transactions (8.6/9.5/9.6/9.7)
- Menu generator (spec 9.5, src/lang/cmds_menu.c): DEFINE MENU/PAD/POPUP/BAR
  with PROMPT/KEY, ON PAD, ON SELECTION PAD/BAR, ACTIVATE/SHOW/DEACTIVATE/
  RELEASE MENU. The model lives in rx_ctx->menus; ACTIVATE pushes the app pad
  onto the menubar through menu_sync_fn (ui/menudef.c) and the menubar's
  run_item executes the ON PAD/ON SELECTION command strings. In batch,
  `ACTIVATE MENU m PAD p` selects pad p immediately and runs its command
  (deterministic gate). Golden 24.
- Transactions (spec 8.6, src/data/journal.c + src/lang/cmds_txn.c):
  BEGIN/END TRANSACTION, ROLLBACK per work area; before-image journal in
  <dbfstem>.RJN covering record writes, header changes, appends and memo
  writes; header byte-30 stamp forces complete-or-rollback on next open
  after a fatal mid-commit error (rx_journal_recover). PACK/ZAP refused
  inside a transaction. Fixed at M9: the 'R' entry journals the on-disk
  before-image (rx_dbf_put has already encoded the new value into recbuf by
  flush time). Conformance 95.
- Project manager (spec 9.6, src/util/rpp.c + src/lang/cmds_proj.c):
  .RPJ text project files (NAME/MAIN/PRG/MENU/SCREEN/REPORT/LABEL/DBF/END),
  BUILD PROJECT name FROM f.rpj compiles to a .RPP - a deterministic
  stored-ZIP (fixed DOS date, CRC32, no compression) with manifest.json,
  src/ raw sources, bin/ RXP1 compiled blobs, res/ screens/reports/labels,
  data/ tables + data/dictionary.json generated from the DBF headers, plus a
  <name>-app.sh launcher. Content-hash (sha256) up-to-date check skips
  rebuilds and yields byte-identical .RPPs; missing dependencies are all
  reported in one diagnostic. `reynard --run app.rpp` runs the packed main
  from its RXP blob (source fallback), registers every packed source so DO
  finds procedures inside, extracts res/ and data/ to a per-app cache dir put on
  SET PATH (so GENERATE SCREEN / REPORT FORM / USE see packed files), and
  SET PROCEDURE TO app.rpp loads packed sources without running them
  (SET PROCEDURE TO also accepts plain .prg procedure files).
- Help browser (spec 9.7, src/lang/help.c + src/ui/helpwin.c): topics in a
  DBF with TOPIC C(30), KEYWORDS M, BODY M, SEEALSO M (shipped fixture
  testdata/gen/help.dbf/.dbt, regenerate with testdata/gen/gen_help.py);
  SET HELP TO targets a custom DBF; F1 opens the browser (ON KEY LABEL F1
  overrides), type-to-filter is a case-insensitive substring over TOPIC +
  KEYWORDS, Enter shows the body, Esc backs out. Falls back to a built-in
  contents page when no help DBF is found. Golden 23.
- Gates: new `projcheck` harness mode (ctest proj_engine) - sample tests/proj
  demo.rpj builds to demo.rpp with byte-stable sha256 golden, launcher
  golden, build/rebuild/run stdout goldens, and `reynard -t --run demo.rpp`
  exercises menus (ON PAD fires), forms (GENERATE SCREEN + DO of the packed
  .scr codegen) and report-to-file plus a committed transaction; conformance
  94-97 (menus batch, txn commit/rollback, BUILD PROJECT + SET PROCEDURE TO
  packed procs, ON KEY LABEL). 97 conformance cases, 24 goldens, 11 ctest
  targets - all green under -Werror and ASan/UBSan, plus Makefile.simple.
