# Reynard Language Reference (xBase / FoxPro 2.6a procedural)

This document is the authoritative language reference (spec §7). It covers
the full v1.0 target; the **Status** column shows what is implemented at
the current milestone (M1). Every implemented bullet has at least one
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

Status legend: ✅ implemented at M1, 🚧 partial, ⏳ planned (milestone).

### Data browsing / positioning

| Command | Status |
| --- | --- |
| `USE [table] [IN n\|alias] [ALIAS a] [AGAIN] [EXCLUSIVE\|SHARED] [NOUPDATE] [INDEX ...] [ORDER ...]` | ✅ (INDEX/ORDER accepted, applied at M6) |
| `CLOSE [ALL\|DATABASES]` | ✅ |
| `SELECT n\|alias` | ✅ |
| `GO\|GOTO TOP\|BOTTOM\|RECORD n\|expr` | ✅ |
| `SKIP [n]` | ✅ (FoxPro BOF/EOF rules, §7.6) |
| `LIST [scope] [OFF]` / `DISPLAY [scope]` | ✅ scopes ALL/REST/NEXT n/RECORD n; SET HEADINGS/DELETED honored |
| `BROWSE` | ⏳ M4 (raises R_ERR_NOUI 601 in batch) |
| `LOCATE/CONTINUE/SEEK/SET ORDER/SET INDEX` | ⏳ M2/M6 |
| `APPEND/APPEND BLANK/APPEND FROM/REPLACE/DELETE/RECALL/PACK/ZAP/REINDEX/INDEX ON/COPY TO/SORT TO/TOTAL TO/COUNT/SUM/AVERAGE/CALCULATE/SCATTER/GATHER/FLUSH` | ⏳ M2+ |
| alias prefixes `alias->field`, `m.var` | ✅ |

### Environment

| Command | Status |
| --- | --- |
| `SET <option> ON\|OFF\|TO v` | 🚧 implemented: TALK, HEADINGS, EXACT, FIXED, CENTURY, DELETED, ESCAPE, CONSOLE, SAFETY, CLOCK, MOUSE, DECIMALS, DEFAULT, PATH, DATE; all other Appendix B options parse and are accepted (semantics at their milestone) |
| `? / ??` | ✅ |
| `var = expr`, `STORE expr TO v1, v2` | ✅ |
| `QUIT` | ✅ |
| `CLEAR [ALL]` | ✅ (no-op at M1) |

### Program flow, I/O, macros (M5)

⏳ IF/ELSE/ENDIF, DO WHILE, FOR/ENDFOR, DO CASE, SCAN/ENDSCAN, LOOP,
EXIT, RETURN, RETRY, CANCEL, SUSPEND, RESUME, PARAMETERS/LPARAMETERS,
DO [WITH], PROCEDURE/FUNCTION, DEFINE WINDOW/MENU/POPUP/BAR,
ACTIVATE/DEACTIVATE/SHOW/HIDE, ON KEY LABEL, ON ERROR, ON ESCAPE,
PUSH/POP, READ EVENTS, CLEAR EVENTS, @ SAY/GET, READ, TEXT...ENDTEXT,
INPUT, ACCEPT, WAIT, TYPE, REPORT FORM, LABEL FORM, SET PRINTER,
SET ALTERNATE, & macro, EVALUATE(), GETEXPR.

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
  FCOUNT, FIELD, FSIZE, HEADER, RECSIZE, FOUND, LUPDATE, FILTER, ORDER, TAG.
- **System**: OS, VERSION, GETENV, MEMORY, DISKSPACE, SYS, CAPSLOCK,
  NUMLOCK, INSMODE, INKEY, LASTKEY, READKEY, SET, SCOLS, SROWS, CURDIR,
  FILE.

⏳ remaining Appendix A entries (arrays ACOPY..., menu/window WLAST...,
low-level FOPEN... file I/O, OEMTOANSI/ANSITOOEM) land with their
milestones (M2/M5).

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
