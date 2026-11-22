# src/util — bottom layer (spec §3.1)

strbuf (growable buffer), str (r_str helpers), utf8 (codec), cp_tables
(CP437/850/866/1252 <-> UTF-8, generated from CLDR mappings), ddatecnv
(civil<->julian), numeric (FoxPro SET DECIMALS/FIXED display), errlist
(Appendix D catalog), log (RX_LOG tracing), cfg (reynard.cfg parsing).
Nothing in util includes project headers above it.
