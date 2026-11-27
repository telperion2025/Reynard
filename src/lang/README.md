# src/lang — the xBase interpreter (spec §7, M1 core / M5 complete)

lexer.c (tokens with line/col), parser.c (recursive descent, 4-letter
keyword abbreviation, ';' continuation), expr_compile.c (AST -> linear
bytecode, lazy .AND./.OR. jumps), vm.c (typed stack VM + statement
dispatch + function registry), memvar.c, errhand.c (§6.6 diagnostics),
cmds_data.c / cmds_env.c (command handlers), funcs_str/num/date/db/sys.c
(Appendix A functions).

M6: cmds_index.c implements INDEX ON ... TO/TAG, SEEK, REINDEX, SET INDEX TO,
SET ORDER TO (parser clauses IDX/ORD_* in parser.c), tag-ordered navigation
and scope iteration (cmds_data.c), index maintenance on REPLACE/APPEND/PACK/
ZAP, and index-keyed SET RELATION; funcs_db.c gained ORDER/TAG/TAGCOUNT/CDX/
MDX/NDX/KEY/KEYCOUNT/TARGET/RELATION/FILTER. Tag key expressions are stored as
text in CDX headers and re-compiled once per session (rx_idxexpr registry in
rx_ctx) for REINDEX and per-record maintenance.
