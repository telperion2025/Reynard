# Changelog

All notable changes per milestone (spec §0.5). Format: Keep a Changelog;
Reynard is pre-1.0, versions are milestone tags M0..M10 then v1.0.0.

## [Unreleased] — M0 + M1 + M2 in progress

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
