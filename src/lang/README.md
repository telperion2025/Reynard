# src/lang — the xBase interpreter (spec §7, M1 core / M5 complete)

lexer.c (tokens with line/col), parser.c (recursive descent, 4-letter
keyword abbreviation, ';' continuation), expr_compile.c (AST -> linear
bytecode, lazy .AND./.OR. jumps), vm.c (typed stack VM + statement
dispatch + function registry), memvar.c, errhand.c (§6.6 diagnostics),
cmds_data.c / cmds_env.c (command handlers), funcs_str/num/date/db/sys.c
(Appendix A functions).
