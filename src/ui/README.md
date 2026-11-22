# src/ui — screen environment and window system (spec §6, M3)

Screen layout (menubar row 0, workspace, status line), uniform pane stack
for all windows, declarative menubar (menudef.c), Command window with
history, shared getsay.c editor core. Lands at milestone M3; batch mode
raises R_ERR_NOUI for UI-only commands until then.
