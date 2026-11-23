# Deviations Log (spec §15)

Every deliberate deviation from the spec / from FoxPro 2.6a behavior,
with spec section, reason, user-visible change, and date. Reviewed at each
milestone gate. Default answer to "should we deviate?" is no.

| # | Spec § | Deviation | Reason | User-visible | Date |
| --- | --- | --- | --- | --- | --- |
| 1 | App. A | `ISLEADBYTE()` always returns .F. | UTF-8 has no lead bytes | none in practice | 2025-11-22 |
| 2 | App. A | `MEMORY()` returns synthetic 640 (KB), `SYS(12)` same | Unix has no fixed DOS memory map | informational only | 2025-11-22 |
| 3 | §5.5 | `_CLIPTEXT` / OS clipboard deferred past v1.0 | terminal scrollback/clipboard capture out of scope | paste into Reynard uses terminal paste | 2025-11-22 |
| 4 | §7.6 | `TYPE("x")` at M1 receives the evaluated value, so `TYPE()` of an *unknown* variable raises 502 instead of returning "U"; unevaluated-argument TYPE lands with the M5 compiler | runtime name resolution (needed for lazy .AND./.OR.) | error message differs for unknown names | 2025-11-22 |
| 5 | §8.2 | CP852 (mark 0x64) and other exotic marks fall back to no conversion with a one-time warning | table not yet shipped; only 437/850/866/1252 tables in tree | high-byte text passes through raw | 2025-11-22 |
| 6 | §7.4 | Procedure depth limit is 64 frames (documented as deviation from FoxPro's ~32) | spec §7.4 itself sets 64 | deeper recursion allowed | 2025-11-22 |
| 7 | §8.5 | Record locks use offset 100000000 + recno; dBase IV silent-skip SET LOCK mode not implemented | documented in spec | none | 2025-11-22 |
| 8 | §8.6 | VFP data sessions out of scope | spec §8.6 | none | 2025-11-22 |
| 9 | §8 | dBase II (0x02) files read via conversion deferred to v1.1 | spec §8.2 | open of 0x02 file fails with diagnostic | 2025-11-22 |
| 10 | §7.1 | `SELECT ... ` SQL deferred to v1.1 (sql_select.c) | spec §7.1 | SELECT command = workarea switch only | 2025-11-22 |
| 11 | §11.1 | Files that include `<unistd.h>` put POSIX headers before project headers | `<unistd.h>` defines `R_OK` (access(2)); `include/reynard/status.h` owns `R_OK = 0` (Appendix D) | none (build hygiene) | 2025-11-22 |
| 12 | §2.2 | When ncurses development headers are absent, the build produces the script backend only; interactive mode reports and exits 2 | sandbox/ports without ncurses-dev still build and run batch/CI | no full-screen UI until ncurses present (or vendored on macOS) | 2025-11-22 |
| 13 | §7.5 | `ISBLANK()` on numeric values is .F. at M1 (field blank-flag not yet plumbed); character/date/logical blanks are exact | plumbing lands with M2 field writes | ISBLANK(0) = .F. (correct); blank numeric *fields* report .F. until M2 | 2025-11-22 |
| 14 | §6.4 | LIST/DISPLAY `RECORD n` shows a deleted record even with SET DELETED ON (explicit record target) | matches FoxPro explicit-RECORD behavior | none | 2025-11-22 |
| 15 | §8.8 | SDF/DELIMITED write dates as YYYYMMDD and DELIMITED keeps full-width quoted character fields; numeric fields export trimmed | matches the DBF internal date format; self-consistent APPEND FROM round trip; FoxPro-reference-fixture parity check lands with the M6 interop validator | text export layout may differ in spacing from FoxPro for quoted C fields | 2025-11-22 |
| 16 | §8.6 | PACK/ZAP are refused inside a transaction (R_ERR_DBF_UNSUPPORTED 209) | in-place PACK cannot be before-imaged cheaply; FoxPro also restricts these mid-transaction | error message on PACK inside BEGIN/END | 2025-11-22 |
| 17 | §8.3 | DBT reader treats the first 0x1A as the memo terminator; a memo containing a raw 0x1A byte would truncate | classic DBT ambiguity; our writer never emits 0x1A inside payloads | none for normal text | 2025-11-22 |
| 18 | §7.1 | bare `APPEND` and bare `CREATE` raise R_ERR_NOUI 601 in batch (-t); interactive bare APPEND/CREATE land with the M4 BROWSE grid | FoxPro opens full-screen editors for these | batch scripts must use APPEND BLANK / APPEND FROM / CREATE ... FROM | 2025-11-22 |
| 19 | §8.2 | new tables are written as 0x03 (dBase III+) or 0xF5 (FoxPro, when memo fields) with the session code page mark (default 0x01 CP437) | deterministic, FoxPro-compatible defaults | version byte of created files | 2025-11-22 |
| 20 | §5.1 | the term vtable has no cursor API, so the text caret is rendered as an inverse-video cell | keeps the vtable blit-only and both backends simple | caret is a highlighted cell, not the terminal's own cursor | 2025-11-23 |
| 21 | §12 | golden-screen dumps compare cell characters only (trailing spaces stripped); palette attrs are not golden-compared | the script backend dump is a text grid; colors are verified by the selftest's 64-pair palette check | color regressions are not caught by golden tests | 2025-11-23 |
| 22 | §5.3 | Alt+letter is normalized to the uppercase canonical RXK_ALT(c) in the script backend | one canonical form for menu hotkeys regardless of shift state | none | 2025-11-23 |
