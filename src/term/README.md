# src/term — terminal and mouse layer (spec §5, M0)

term_if.h defines the backend vtable (rcell grid, damage rects, palette,
poll). term_ncurses.c is the production backend (SGR 1006 mouse primary,
1000/1002/1003/1015 fallbacks; built when curses headers exist).
term_script.c replays .events files and dumps cell grids for golden-screen
CI. mouse.c normalizes all mouse wire protocols into rmouse. keymap.h has
the RXK_* logical key symbols.
