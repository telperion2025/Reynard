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
