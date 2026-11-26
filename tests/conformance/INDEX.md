# Conformance index (spec §14.6)

Each `.prg` case maps to the spec section it covers. Regenerated/checked
by CI (`.github/workflows/ci.yml` conformance-index job).

| Case | Covers |
| --- | --- |
| 01_print_basic | §7 ?/??, comma separation, arithmetic display, SET DECIMALS default |
| 02_string_funcs | App. A string functions, string +/- (§7.3) |
| 03_numeric_funcs | App. A numeric functions, STR/VAL |
| 04_compare_exact | §7.6 SET EXACT, ==, lazy .AND./.OR. |
| 05_dates | App. A date funcs, {^} literals, SET CENTURY, EMPTY/ISBLANK, frozen DATE() in -t (§3.3) |
| 06_set_decimals | §7.4 SET DECIMALS / SET FIXED / SET() query (App. B) |
| 07_memvar | §7.5 assignment, STORE, m.var |
| 08_use_list | §7.1 USE/LIST, §8.1 introspection funcs, LIST format §9.1 |
| 09_navigation | §8 GO/SKIP, §7.6 BOF/EOF boundary rules |
| 10_list_scopes | §8.7 scopes ALL/REST/NEXT/RECORD, SET HEADINGS |
| 11_deleted | SET DELETED, DELETED(), delete flag §8.2 |
| 12_workareas | §8.1 work areas, SELECT, USED/ALIAS, alias->field |
| 13_codepage | §8.2 code page mark conversion (CP437) |
| 14_empty_table | §7.6 RECCOUNT()=0, BOF/EOF on empty table |
| 15_fp30_table | §8.2 version byte 0x30 acceptance |
| 16_err_undef_var | App. D 502, §6.6 error format |
| 17_err_syntax | App. D 501, §6.6 caret |
| 18_err_divzero | App. D 505 |
| 19_err_nofile | App. D 107, §10.3 search semantics |
| 20_err_notable | App. D 210 no table in use |
| 21_err_browse_noui | App. D 601 NOUI in batch (§3.3) |
| 22_err_undef_func | App. D 507 runtime resolution §7.3 |
| 23_quit | QUIT semantics §7.1 |
| 24_rand_frozen | §3.3 -t determinism (RAND/TIME/SECONDS) |
| 25_type_empty | §7.6 TYPE/VALTYPE/EMPTY/ISBLANK/INLIST |
| 26_string_compare_detail | §7.6 padding rules, EXACT |
| 27_date_arith | §7.3 date arithmetic, SET CENTURY |
| 28_set_path_default | §10.3 SET PATH/DEFAULT, SET() query |
| 29_use_path | §7.1 USE with path-like names (./people, people.dbf) |
| 30_append_replace | §7.1 APPEND BLANK, REPLACE ... WITH, §8.2 write path |
| 31_delete_recall_pack | §7.1 DELETE/RECALL/PACK, delete flag, memo-safe PACK |
| 32_locate_continue | §7.1 LOCATE/CONTINUE, FOUND(), §8.7 REST/ALL scopes |
| 33_set_filter | §8.7 SET FILTER TO, FIELDS clause, LIST NEXT |
| 34_copy_sdf_type | §8.8 COPY TO ... SDF, TYPE, CRLF fixed-width layout |
| 35_append_from_sdf | §8.8 APPEND FROM ... SDF round trip |
| 36_copy_delimited | §8.8 COPY TO/APPEND FROM DELIMITED (quoted C, trimmed N) |
| 37_delimited_with | §8.8 DELIMITED WITH <char> custom separator |
| 38_for_while | §8.7 FOR/WHILE on LIST/DELETE, default scope = ALL with FOR |
| 39_memo_fpt | §8.3 FPT memo read/rewrite, multi-block, persistence |
| 40_memo_dbt | §8.3 DBT memo read (0x83) |
| 41_workarea_write | §8.1 work areas, alias->field REPLACE, USED/ALIAS/SELECT |
| 42_zap | §7.1 ZAP, APPEND FROM <dbf>, empty-table BOF/EOF |
| 43_copy_structure | §8.8 COPY STRUCTURE [EXTENDED], CREATE ... FROM |
| 44_err_readonly | App. D 108, USE NOUPDATE read-only writes |
| 45_err_continue | App. D 606 CONTINUE without LOCATE |
| 46_locks | §8.5 FLOCK()/RLOCK()/UNLOCK, SET EXCLUSIVE, USE SHARED |
| 47_copy_to_dbf | §8.8 COPY TO <dbf> record copy, SAFETY overwrite guard |
| 48_err_append_nofile | App. D 107 APPEND FROM missing file |
| 49_memo_sdf_demo | §8.3 FPT REPLACE persistence across CLOSE/USE + §8.8 COPY TO SDF → APPEND FROM (.txt default ext) + LOCATE/CONTINUE after append |
| 50_rel_basic | §8.1 M4 SET RELATION TO RECNO() INTO alias: parent follows GO/SKIP in the driver area |
| 51_rel_skip | §8.1 M4 SET SKIP TO: skip-listed target is not repositioned on SKIP (one-to-many) |
| 52_rel_off_clear | §8.1 M4 SET RELATION OFF INTO alias + non-ADDITIVE re-issue clears |
| 53_rel_additive | §8.1 M4 multiple relations with ADDITIVE across three work areas |
| 54_rel_alias_err | App. D 503 SET RELATION INTO unknown alias |
| 55_browse_parse_w | §9.1 M4 BROWSE FIELDS :W without '=' is a parse error (501) |
| 56_browse_parse_partition | §9.1 M4 BROWSE PARTITION without a number is a parse error (501) |
| 57_err_append_noui | App. D 601 bare APPEND in batch mode |
| 58_rel_offset | §8.1 M4 relation key expression (RECNO()+1) |
| 59_rel_bottom | §8.1 M4 GO BOTTOM drives the parent to the last record |
| 60_skip_clear | §8.1 M4 SET SKIP TO with no aliases clears the skip list |
| 61_control_flow | §7.1 M5 IF/ELSEIF/ELSE, DO WHILE, FOR/STEP/NEXT, DO CASE/OTHERWISE, LOOP/EXIT |
| 62_procs_udf | §7.5 M5 DO ... WITH, PROCEDURE/FUNCTION, recursion, RETURN value, PRIVATE hiding |
| 63_arrays | §7.5 M5 DIMENSION, a[i,j] get/set, TYPE "A", AELEMENT/ASUBSCRIPT/ALEN, ACOPY, ASORT |
| 64_macros | §7.2 M5 & macro substitution at execution time (&cmd = 99, USE &tbl, && comment) |
| 65_on_error | §7.4 M5 ON ERROR DO handler, ERROR()/MESSAGE() introspection, resume after error |
| 66_text | §7.1 M5 TEXT...ENDTEXT verbatim output |
| 67_scan | §7.1 M5 SCAN [FOR/WHILE] ... ENDSCAN with LOOP/EXIT, EOF after scan |
| 68_scopes | §7.5 M5 PUBLIC/PRIVATE/RELEASE, RELEASE ALL LIKE pattern, TYPE "U" |
| 69_io | §7.1 M5 WAIT (non-blocking in -t), ACCEPT, INPUT (per-case .stdin) |
| 70_err_macro | App. D 502 & undefined macro variable |
| 71_retry | §7.4 M5 RETRY re-runs the erroring statement after the handler fixes it |
| 72_index_cdx_basic | §8.4 M6 INDEX ON ... TAG (structural CDX), ORDER()/TAGCOUNT()/KEY()/KEYCOUNT(), ordered LIST, SEEK, SET ORDER TO 0/TAG, GO TOP/BOTTOM + SKIP in tag order, BOF/EOF |
| 73_idx_standalone | §8.4 M6 INDEX ON ... TO standalone .idx, NDX(), USE ... INDEX, SET INDEX TO (clear) |
| 74_seek_exact | §8.4 M6 SEEK with SET EXACT ON (exact) vs OFF (first >=), FOUND()/EOF() |
| 75_unique_for | §8.4 M6 SET UNIQUE ON (UNIQUE tag), INDEX ON ... FOR filter (KEYCOUNT, KEY(2)) |
| 76_replace_maintain | §8.4 M6 REPLACE/APPEND BLANK keep tags current (SEEK after write), REINDEX |
| 77_delete_pack | §8.4 M6 DELETE keeps entries, SET DELETED ON SEEK skips deleted, PACK rebuilds (RECCOUNT/KEYCOUNT) |
| 78_for_tag | §8.4 M6 FOR-filtered tag: ordered LIST shows only matching records |
| 79_order_numbers | §8.4 M6 SET ORDER TO n / TAG, SET INDEX TO ... ADDITIVE, TAG(n), multi-file order numbering |
| 80_err_seek_noorder | App. D 302 SEEK without an active index order |
| 81_rel_index | §8.1 M6 index-keyed SET RELATION (SEEK in target order), RELATION(), TARGET() |
| 82_aggr_count_sum | §7.1 M7 COUNT [scope] [FOR] [TO var], SUM/AVERAGE expr-list TO var|ARRAY, CALCULATE CNT/SUM/AVG/MIN/MAX TO ARRAY |
| 83_sort_total | §7.1 M7 SORT TO ... ON f /D (+ multi-key, FIELDS), TOTAL TO ... ON expr (group sums, full target structure) |
| 84_scatter_gather | §7.1 M7 SCATTER TO ARRAY/MEMVAR [FIELDS] [BLANK], GATHER FROM ARRAY/MEMVAR (index maintenance via SEEK after write; memvar shadows field) |
| 85_aggr_scope | §7.1 M7 scope+FUR on aggregates: COUNT FOR, SUM FOR, COUNT/AVERAGE NEXT n |
| 86_picture_numeric | §9.2 M7 TRANSFORM/PICTURE numeric templates: 9/# fill+space-pad, comma slots, $, neg sign slot, *** overflow, @B left, decimals |
| 87_picture_char | §9.2 M7 char/logical/date templates: !!!, XXX, @!, @R literals, L, @D/@E dates |
| 88_say_batch | §9.2 M7 @ SAY in batch prints one line per SAY (DEVIATIONS #47), SAY-side PICTURE |
| 89_read_noui |
| 90_generate_screen |
| 91_report_form | §9.3 M8 REPORT FORM TO FILE (text emitter: title/page header/groups/vars), PLAIN+HEADING+FOR+RANGE, SUMMARY |
| 92_label_form | §9.4 M8 LABEL FORM TO FILE (one label per page), FOR clause |
| 93_create_modify | §9.3 M8 CREATE/MODIFY REPORT+LABEL in batch (empty form write + canonical re-save round-trip), TYPE snapshot | §9.2 M7 GENERATE SCREEN case TO out.prg (codegen from .SCR sibling), TYPE out.prg snapshot, MODIFY SCREEN recompiles .RSO (builder round-trip) | §9.2 M7 READ with pending GETs and no UI → error 601 (App. D) |
