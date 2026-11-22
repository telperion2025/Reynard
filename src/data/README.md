# src/data — data engine (spec §8, M1/M2 read-only, M6 indexing)

Public API in include/reynard/dbf.h (frozen at M6). dbf.c: DBF open/parse
with diagnostics, positioning (FoxPro BOF/EOF), read AND the M2 write path
(append/put/flush/delete/pack/zap, header maintenance); dbf_create.c:
table creation; field.c: field decode/encode; memo_fpt.c / memo_dbt.c:
memo sidecars (lazy, code-page converted); codepage.c: language-driver
marks; workarea.c: the 1..255 work-area table; rlock.c: fcntl byte-range
locks (§8.5); journal.c: .RJN before-image transactions (§8.6);
scope.c: ALL/REST/NEXT/RECORD ranges (§8.7); copyto.c: SDF/DELIMITED
serializers (§8.8). CDX/IDX/NTX land at M6.
