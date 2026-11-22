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
