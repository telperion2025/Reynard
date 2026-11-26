# Reynard Language Reference (xBase / FoxPro 2.6a procedural)

This document is the authoritative language reference (spec §7). It covers
the full v1.0 target; the **Status** column shows what is implemented at
the current milestone (M3). Every implemented bullet has at least one
`tests/conformance/` case (spec §14.6; index in
[tests/conformance/INDEX.md](../tests/conformance/INDEX.md)).

## 1. Lexical rules

- Case-insensitive keywords and identifiers; the first four letters of any
  keyword are a valid abbreviation (`REPL ALL` = `REPLACE ALL`); ambiguous
  abbreviations are a syntax error.
- `&&` comments to end of line; `*` at line start comments the line.
- Strings in `"..."` or `'...'`; a doubled quote escapes itself.
- Numeric literals: `123`, `3.14`, `.5`.
- Date literals: `{^yyyy-mm-dd}`; `{}` is the empty date.
- Logical literals: `.T. .F. .Y. .N.` (and word forms `AND OR NOT`).
- `;` at end of line continues the command on the next line. In the `-c`
  CLI argument only, `;` splits separate commands (spec §6.4).

## 2. Values and display

| Type | Letter | Display rule |
| --- | --- | --- |
| Character | C | raw text (DBF fields keep trailing padding) |
| Numeric | N | integer-valued prints without decimals; otherwise SET DECIMALS digits (default 2); SET FIXED ON forces decimals |
| Integer | I | integer |
| Currency | Y | 4 decimals |
| Date | D | `MM/DD/YY`, or `MM/DD/YYYY` with SET CENTURY ON; empty date prints `  /  /  ` |
| Logical | L | `T` / `F` / `?` (unset) |
| Memo | M | memo text |

`? e1, e2` prints expressions separated by one space, then a newline;
`??` omits the newline.

## 3. Operators and precedence (spec §7.3)

From lowest to highest: `.OR.` / `OR` — `.AND.` / `AND` — `.NOT.` / `NOT` —
comparison `= == <> != # < > <= >= $` — `+ -` (string `-` shifts trailing
spaces of the left operand) — `* / %` — `** ^` (right associative) — unary
`- +`.

- `=` honors SET EXACT exactly: with EXACT OFF the shorter string is padded
  with spaces; `==` is always exact. `$` is substring, case-sensitive.
- `.AND.` / `.OR.` are lazy: the right operand is not evaluated when the
  result is decided (conformance case 04).
- Date arithmetic: `date ± days`, `date - date` = days, `datetime ±
  seconds`. Out-of-range results raise error 605.
- Function names resolve at runtime (so short-circuiting never touches an
  undefined function).

## 4. Commands

Status legend: ✅ implemented (milestone noted), 🚧 partial, ⏳ planned (milestone).

### Data browsing / positioning

| Command | Status |
| --- | --- |
| `USE [table] [IN n\|alias] [ALIAS a] [AGAIN] [EXCLUSIVE\|SHARED] [NOUPDATE] [INDEX f1[,f2...]] [ORDER n\|TAG t [OF cdx]]` | ✅ M6 (INDEX/ORDER attach + controlling order) |
| `CLOSE [ALL\|DATABASES]` | ✅ |
| `SELECT n\|alias` | ✅ |
| `GO\|GOTO TOP\|BOTTOM\|RECORD n\|expr` | ✅ |
| `SKIP [n]` | ✅ (FoxPro BOF/EOF rules, §7.6) |
| `LIST [scope] [FOR/WHILE] [FIELDS ...] [OFF]` / `DISPLAY [scope] ...` | ✅ M2: scopes, FOR/WHILE, FIELDS, OFF, SET FILTER/HEADINGS/DELETED, deleted `*` marker |
| `BROWSE [FIELDS f[:H="t"][:W=n][:P="p"][:V=e[:E="m"]][:F][:R], ...] [PARTITION n] [LEDIT|REDIT] [FREEZE f] [LOCK n] [WIDTH n] [NOAPPEND] [NODELETE] [NOEDIT|NOMODIFY] [NOWAIT] [TITLE "t"] [FOR e] [WHEN e] [VALID[:F] e[:E="m"]]` / `CHANGE` | ✅ M4 interactive grid (§9.1: nav/edit/:V/FREEZE/PARTITION/LEDIT/REDIT/mouse; raises R_ERR_NOUI 601 in batch); header popover sort/filter ⏳ M6; memo-cell editing ⏳ M7 |
| `LOCATE [scope] FOR e [WHILE e]` / `CONTINUE` | ✅ M2 (FOUND(), `Record = n`); scans in tag order when an index order is active |
| `SEEK e` | ✅ M6 (controlling order; SET EXACT ON = exact match, OFF = first key >= e; miss → EOF, FOUND()=.F.; SET DELETED ON skips deleted entries) |
| `INDEX ON e TO file \| TAG t [OF cdx] [FOR e] [UNIQUE] [DESCENDING] [ADDITIVE]` / `REINDEX` | ✅ M6 (structural CDX tag or standalone .idx; new tag becomes controlling; REINDEX rebuilds all tags) |
| `SET INDEX TO [f1[,f2...]] [ADDITIVE]` / `SET ORDER TO [n \| [TAG] t [OF cdx] \| e]` / `SET UNIQUE ON\|OFF` | ✅ M6 (SET INDEX TO bare closes non-structural files; ORDER 0 = natural order) |
| `APPEND BLANK` / `APPEND FROM f [TYPE] SDF\|DELIMITED [WITH c\|BLANK\|TAB]` (bare APPEND ✅ M4: append-mode BROWSE grid interactively, R_ERR_NOUI 601 in batch) / `REPLACE [scope] [FOR/WHILE] f WITH e, ...` / `DELETE` / `RECALL` / `PACK` / `ZAP` / `COPY TO f [FIELDS..] [scope] [TYPE SDF\|DELIMITED [WITH..]]` / `COPY STRUCTURE TO f [FIELDS..] [EXTENDED]` / `CREATE f FROM struct` / `FLUSH` / `UNLOCK [ALL]` | ✅ M2; SORT TO/TOTAL TO/COUNT/SUM/AVERAGE/CALCULATE/SCATTER/GATHER ⏳ M7+ |
| alias prefixes `alias->field`, `m.var` | ✅ |

### Environment

| Command | Status |
| --- | --- |
| `SET <option> ON\|OFF\|TO v` | 🚧 implemented: TALK, HEADINGS, EXACT, FIXED, CENTURY, DELETED, ESCAPE, CONSOLE, SAFETY, CLOCK, MOUSE, DECIMALS, DEFAULT, PATH, DATE, EXCLUSIVE, MULTILOCKS, REPROCESS, FILTER; all other Appendix B options parse and are accepted (semantics at their milestone) |
| `SET RELATION TO [e INTO alias [, ...]] [ADDITIVE]` / `SET RELATION OFF INTO alias` / `SET SKIP TO [alias [, ...]]` | ✅ M4 (§8.1: driver work area repositions INTO targets on GO/SKIP/LOCATE; without an index the key is numeric = record number; SET SKIP TO holds targets on SKIP; M6 adds index-keyed seeks) |
| `? / ??` | ✅ |
| `var = expr`, `STORE expr TO v1, v2` | ✅ |
| `QUIT` | ✅ |
| `CLEAR [ALL]` | ✅ (no-op at M1) |

### Program flow, I/O, macros (M5)

✅ M5 implemented: IF/ELSEIF/ELSE/ENDIF, DO WHILE/ENDDO, FOR [STEP]/NEXT/
ENDFOR, DO CASE/OTHERWISE/ENDCASE, SCAN [scope] [FOR/WHILE]/ENDSCAN, LOOP,
EXIT (leave the innermost loop), RETURN [expr], RETRY (inside ON ERROR),
CANCEL, DO name [WITH args], PROCEDURE/FUNCTION ... ENDPROC/ENDFUNC
(order-independent, recursive UDFs), PARAMETERS/LPARAMETERS (by value),
PUBLIC/PRIVATE [ALL LIKE/EXCEPT], RELEASE [ALL [LIKE/EXCEPT]|MEMVAR],
DIMENSION/DECLARE arrays a[i,j] + AELEMENT/ASUBSCRIPT/ACOPY/ADEL/AINS/
ASCAN/ASORT/AFIELDS/ADIR/ALEN, & macro (execution-time expansion, depth 16),
ON ERROR DO + ERROR()/MESSAGE(), TEXT...ENDTEXT, INPUT, ACCEPT, WAIT
(non-blocking in -t), .RXP compiled cache (byte-reproducible, sha256-keyed).

\u2705 M7 forms (spec {U}9.2): `@ r,c [SAY e [PICTURE p] [FUNCTION f]] [GET var
[SIZE h,w] [PICTURE p] [FUNCTION f] [WHEN e] [VALID e[:E="m"]] [RANGE a[,b]]
[DEFAULT e] [COLOR SCHEME n]]`, `READ [CYCLE]`, `CLEAR GETS`, `CLEAR READ`,
`@ r,c CLEAR`, `@ r,c BOX [TO r2,c2]`, `TRANSFORM(e, pic[, func])`,
`CREATE|MODIFY|GENERATE SCREEN name [TO f.prg]` (designer + .SCR/.RSO +
.PRG codegen, docs/formats.md {U}6), `DO file.prg` file fallback.

\u23f3 remaining (later milestones): DEFINE WINDOW/MENU/POPUP/BAR,
ACTIVATE/DEACTIVATE/SHOW/HIDE, ON KEY LABEL dispatch, ON ESCAPE,
PUSH/POP, READ EVENTS, CLEAR EVENTS, REPORT FORM,
LABEL FORM, SET PRINTER, SET ALTERNATE, EVALUATE(), GETEXPR, SUSPEND/
RESUME interactive (batch: note only).

## 5. Built-in functions (Appendix A)

✅ implemented at M1 (conformance-tested):

- **String**: LEN, UPPER, LOWER, TRIM, LTRIM, RTRIM, ALLTRIM, LEFT, RIGHT,
  SUBSTR, AT, ATC, RAT, OCCURS, CHR, ASC, SPACE, REPLICATE, PADL, PADR,
  PADC, STR, VAL, STRTRAN, STUFF, LIKE, INLIST, ISALPHA, ISLOWER, ISUPPER,
  ISDIGIT, PROPER, EMPTY, ISBLANK, VALTYPE, TYPE, TRANSFORM (basic),
  ISLEADBYTE (always .F., see DEVIATIONS).
- **Numeric**: ABS, INT, CEILING, FLOOR, EXP, LOG, LOG10, SQRT, SIGN,
  ROUND, MOD, RAND (seed-frozen in `-t`), MAX, MIN, BETWEEN, IIF, FV, PV,
  PAYMENT, PI.
- **Date/time**: DATE, TIME, SECONDS (frozen in `-t`: DATE()=2000-01-01,
  TIME()="00:00:00", SECONDS()=0), YEAR, MONTH, DAY, DOW, CDOW, CMONTH,
  DTOS, DTOC, CTOD, GOMONTH, MDY, DMY, QUARTER, WEEK, HOUR, MINUTE, SEC.
- **Database**: RECNO, RECCOUNT, BOF, EOF, DELETED, ALIAS, SELECT, USED,
  FCOUNT, FIELD, FSIZE, HEADER, RECSIZE, FOUND (M2), LUPDATE (M2),
  FLOCK, RLOCK/LOCK (M2), FILTER (M6). **M6**: ORDER, TAG, TAGCOUNT,
  CDX, MDX, NDX, KEY, KEYCOUNT, TARGET, RELATION.
- **System**: OS, VERSION, GETENV, MEMORY, DISKSPACE, SYS, CAPSLOCK,
  NUMLOCK, INSMODE, INKEY, LASTKEY, READKEY, SET, SCOLS, SROWS, CURDIR,
  FILE; **M5**: ERROR, MESSAGE.
- **Arrays (M5)**: AELEMENT, ASUBSCRIPT, ACOPY, ADEL, AINS, ASCAN, ASORT,
  AFIELDS, ADIR, ALEN.

⏳ remaining Appendix A entries (menu/window WLAST..., low-level FOPEN...
file I/O, OEMTOANSI/ANSITOOEM) land with their milestones (M6+).

## 6. Errors

Format (spec §6.6), printed to stderr in batch:

```
Error in line N col C of FILE: <text>
<source line>
 ^~~~~~
```

Codes are the Appendix D registry (`include/reynard/status.h`): IO 1xx,
DBF 2xx, CDX 3xx, lock 4xx, parse 5xx, runtime 6xx, OS 7xx.

## 7. Test mode (`-t`)

Deterministic batch: session seed 1; `DATE()` → `{^2000-01-01}`,
`TIME()` → `"00:00:00"`, `SECONDS()` → 0, `RAND()` → LCG sequence
(0.51, 0.18, 0.31, ...). Output is byte-stable for golden comparison.
