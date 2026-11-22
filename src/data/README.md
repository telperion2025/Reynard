# src/data — data engine (spec §8, M1/M2 read-only, M6 indexing)

Public API in include/reynard/dbf.h (frozen at M6). dbf.c: DBF open/parse
with diagnostics, positioning (FoxPro BOF/EOF), read; field.c: field
decode; codepage.c: language-driver marks; workarea.c: the 1..255 work-area
table. Writes, FPT/DBT memos, scopes/FOR/WHILE, locking, journaling, CDX
land at M2/M6.
