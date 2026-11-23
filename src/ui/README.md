# src/ui — the full-screen UI (spec §6, milestones M3–M4)

Screen layout: menubar row 0, workspace with the uniform pane stack,
status line on the last row (§6.1). Layering: `ui` includes only `lang`,
`term`, `util` headers — never upward. `lang` talks back through the
`rx_ctx` hooks (`out_fn`, `msg_line`), so there is no include cycle.

| file | role |
| --- | --- |
| ui.h | public entry: `rx_ui_run(ctx, term_backend)` (called from main) |
| ui_internal.h | shared structs: `rx_ui`, `rx_win`, `rx_screen`, palette attrs |
| ui.c | event loop `rx_ui_run`: poll → route key/mouse/resize → render → blit; owns backend lifecycle |
| screen.c | virtual canvas: rcell grid, 64-pair palette, damage rects, differential blit (§5.2) |
| window.c | uniform pane stack: push/remove/top/hit-test, border-drag zones, clamp (§6.2) |
| getsay.c | shared single-line editor core: cursor motion, Ctrl+Y, history ring persisted to `$XDG_CACHE_HOME/reynard/history` (1000 entries), Tab keyword completion (§6.4) |
| cmdwin.c | the Command window: scrollback ring (5000 lines), input line, exec via `rx_run_source`, border move / corner resize |
| menudef.c | declarative FoxPro 2.x menu set with action/disabled/checked hooks (§6.3) |
| menubar.c | row-0 rendering, pulldowns, mouse click-open/drag-sweep, F10/Alt/arrows keyboard path (§6.3, §5.4, App. C) |
| statusbar.c | status line: message left; CAPS/NUM/clock/workarea+alias/rec N/M/DEL right (§6.5) |
| dialog.c | modal dialogs on the pane stack: About, Macros, Filer, Calculator, Calendar/Diary, ASCII Chart |
| helpwin.c | Help contents window |
| browse.c | M4 §9.1 BROWSE/CHANGE grid: full-workspace window on the pane stack, in-cell editing via getsay, FREEZE/PARTITION/LEDIT/REDIT, mouse reorder/resize/scroll, Ctrl+T/Ctrl+N, record locks on edit in shared mode |

Mouse capture: a window that consumes an RM_PRESS keeps receiving drags
until RM_RELEASE, which never triggers an action (§5.4). Resize
(EV_RESIZE / SIGWINCH) re-lays out and full-repaints; golden scenario
`04_resize` covers it.

Color scheme: FoxPro 2.x for DOS look (blue desktop, silver menubar and
status, bright-cyan Command window and active menu items, silver dialogs
with blue title bars); palette indices are FoxPro/CGA numeric order.

CI determinism: `tests/golden/*.events` are replayed through the script
backend (`term_script.c`); see `tests/golden/README.md`.
