# Golden-screen UI tests (spec §12, M3 gate)

Each scenario is a pair:

- `<name>.events` — event stream replayed by the script term backend
  (`term_script.c`; grammar in its header comment). The harness runs
  `reynard -t` with `RX_EVENTS`/`RX_DUMP` pointed into a fresh sandbox
  (fixtures from `testdata/gen` copied in, `HOME`/`XDG_CACHE_HOME`
  sandboxed), so output is deterministic (frozen `DATE()`/`TIME()`/`RAND()`).
- `<name>.golden` — expected final cell grid, compared cell-by-cell
  (rows right-stripped, as the dump writes them).

Optional companions: `<name>.size` (`W H` initial geometry, else 80x24 via
`RX_COLS`/`RX_ROWS`) and `<name>.exit` (expected exit code, default 0).

Run: `reynard-harness golden tests/golden ./reynard testdata/gen .test-work`
Regenerate after an intentional UI change (then review the diff by eye):
`... golden tests/golden ./reynard testdata/gen .test-work update`

## Scenario map (spec section coverage)

| scenario | covers |
| --- | --- |
| 01_boot | §6.1 layout: menubar row 0, workspace, Command window, status line |
| 02_system_menu | §6.3 + M3 gate: mouse reaches every System menu action (each traces a line into the Command scrollback) |
| 03_cmdwin | §6.4: Command window input, Enter executes into scrollback, Up recalls history |
| 04_resize | M3 gate: SIGWINCH/resize stability; final 60x20 frame compared cell-by-cell (tolerance note: geometry-resize cases are compared at the final size, §12) |
| 05_error | §6.6: interactive errors land on the message line, session continues |
| 06_window_drag | §6.2 + §5.4: border drag moves a window; release never triggers |
| 07_keyboard_menu | Appendix C: F10/arrows/Enter menubar path, Alt+H pulldown |
| 08_calc | §6.3 System > Calculator: dialog input line evaluates via the VM (`= 14`) |
| 06 (extended) | corner drag resizes the window in the same scenario |
| 09_close_button | §6.2: the title-bar × closes a dialog and hides the Command window |
| 10_text_clipping | §6.2/§6.3: long Command output is clipped at the window border; ASCII Chart dialog fits its 52-cell grid |
| 11_browse_basic | §9.1 M4: BROWSE grid window, arrow/PgDn navigation, status-line record pointer |
| 12_browse_edit | §9.1 M4: in-cell edit + commit, Ctrl+T delete mark, Ctrl+N append |
| 13_browse_partition_freeze | §9.1 M4: PARTITION split panes, FREEZE limits editing to one column |
| 14_browse_mouse | §5.4/§9.1: wheel scroll, click-to-position, header drag reorder, border drag resize |
| 15_browse_relation | §8.1 M4: SET RELATION drives the parent work area from the browse cursor |
| 16_browse_fields | §9.1 M4: FIELDS with :H titles, :W width, computed expression column |
