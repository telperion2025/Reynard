# Reynard Binary Formats (spec §8)

Reynard reads and writes standard xBase files byte-compatibly. This
document is the authoritative layout reference; `testdata/reference/`
holds real-tool fixtures with `PROVENANCE.txt` (spec §8.9).

## 1. DBF table file (§8.2)

All integers little-endian.

**Header (32 bytes):**

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | version byte: 0x02 dBase II (v1.1), 0x03 dBase III+/Clipper, 0x04 dBase IV, 0x30 FoxPro, 0x31/0x32 VFP, 0x83/0x8B/0xF5 with memo |
| 1 | 3 | last update YYMMDD (binary) |
| 4 | 4 | record count |
| 8 | 2 | header size (bytes) |
| 10 | 2 | record size (bytes) |
| 12 | 20 | reserved (byte 28 = transaction flag, byte 29 = language driver / code page mark) |

**Field descriptor (32 bytes each, starting at offset 32, terminated by 0x0D):**
name[11] (NUL-padded), type char at 11, data address at 12 (4, ignored on
read), length at 16, decimal count at 17, rest reserved.

**Record:** 1 delete flag (0x20 intact, 0x2A deleted) + raw field bytes,
contiguous. File ends with 0x1A.

**Field encodings:**

| Type | Storage |
| --- | --- |
| C | space-padded ASCII/code-page bytes, len <= 254 |
| N/F | right-aligned ASCII, len 1-20, dec 0-15; all-blank = 0 |
| D | 8 ASCII `YYYYMMDD`; all spaces = empty date |
| L | 1 byte: `YyTt` true, `NnFf` false, else unset |
| M/G/P | 10 ASCII digits: FPT/DBT block number |
| I | 4-byte LE int32 |
| Y | 8-byte LE int64 scaled by 10000 |
| B | 8-byte IEEE double |
| T | 4-byte LE julian day + 4-byte LE milliseconds since midnight |

**Code page marks (byte 29):** 0x00 none, 0x01 CP437, 0x02 CP850,
0x03/0x57/0xC8 CP1252, 0x64 CP852 (table pending — no conversion +
warning, see DEVIATIONS), 0x65 CP866. Unknown marks: no conversion, one
warning. `SET CODEPAGE` / `REYNARD_CODEPAGE` override session conversion.
Character data converts at the DBF boundary; UTF-8 internally.

## 2. FPT memo (§8.3)

512-byte header block: bytes 0-3 next-free block number (BE), bytes 6-7
block size (BE, power of two 32-65536, default 64), rest reserved. Block
number n occupies bytes [n*bs, (n+1)*bs); the header occupies blocks
0..ceil(512/bs)-1, so the first memo block is ceil(512/bs).
Each block: 4-byte BE type signature (0 text, 1 picture, 2 object),
4-byte BE payload length, payload, zero padding to the block boundary.
Writes update in place when the new memo fits the old allocation, else
extend from the free list (next-free bumped in the header). PACK rewrites
the sidecar from scratch with only live memos. Memo text is stored in the
DBF code page and converted to UTF-8 on read (lazy, on first access).

## 3. DBT memo (§8.3)

512-byte blocks; block 0 is the header with a 4-byte LE next-free block
at offset 0. Reader: payload runs from blk*512 to the first 0x1A
terminator (trailing NUL/0x1A padding stripped when absent). Writer:
append-per-write for compatibility — data + 0x1A, zero-padded to the
512-byte boundary, next-free updated. PACK rewrites the sidecar.

## 3a. RJN transaction journal (§8.6)

`<dbfstem>.RJN`, created by BEGIN TRANSACTION: magic `RJN1`, then entries
of 1 tag byte + payload: `R` u32le recno + rec_size raw bytes (record
before-image), `H` 32 raw header bytes, `A` u32le recno (appended record),
`M` u32le blk + u32le has_payload + u32le oldn + oldn bytes + i64le old
memo file size, `C` commit marker. DBF header byte 30 is the
incomplete-transaction stamp: set at BEGIN, cleared at COMMIT; on open,
a set stamp with an existing sidecar triggers automatic rollback
(complete-or-rollback, §8.6). PACK/ZAP are refused inside a transaction.

## 5a. SDF / DELIMITED text export (§8.8)

SDF: fixed-width fields concatenated in field order, CRLF record
terminator; memo/G/B/P fields skipped. DELIMITED: separator `,` (default),
custom char with `WITH c`, space with `WITH BLANK` (unquoted, trailing
blanks trimmed), tab with `WITH TAB`; character fields are double-quoted
with embedded quotes doubled (except WITH BLANK); numeric fields export
trimmed; dates as `YYYYMMDD`; logicals as `T`/`F`; memo fields skipped.
Files are written in the DBF code page. Default extension: `.txt`.
APPEND FROM parses the same layouts back; default extension lookup tries
the name as given, then `.txt` (text) / `.dbf` (tables).

## 4. CDX compound index (§8.4)

500-byte B+tree nodes (FoxPro page size). Structural CDX opens
automatically with the DBF (same stem); tag headers chain in the root
page storing key expression, FOR filter, flags (descending/unique), page
counts. Character keys have trailing spaces stripped. Reynard-written
CDX must open in FoxPro 2.6a and VFP 9 (CI-verified against reference
fixtures at M6). IDX read/write; NTX/NDX read-only.

## 5. Other formats

- `.RXP` compiled program cache (spec §7.4): magic `RXP1`, version,
  source hash, flags, bytecode blob, string table (M5).
- `.SCR` screen / `.RSO` compiled screen, `.RLB` label, `.RPJ` project /
  `.RPP` packed application (zip container with manifest.json) — text
  formats specified here at their milestones (M7-M9).
- Import/export: SDF, DELIMITED [WITH del|BLANK|TAB], XLS (BIFF5) —
  spec §8.8.
