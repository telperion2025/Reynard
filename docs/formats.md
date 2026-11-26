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

## 4. CDX compound index (§8.4, implemented M6)

500-byte little-endian blocks; block 0..n-1 are the tag directory, then one
B+tree per tag. See src/data/cdx_internal.h for the authoritative layout.

- Directory block: `u32` sig `RCDX` @0, `u16` version (1) @4, `u16` entry
  count @6, `u32` next dir block @8 (0 = last), `u32` first free page @12.
  Tag headers are 236 bytes from offset 24: name[11] @0, key expression[128]
  @12, FOR expression[64] @140, flags `u16` @204 (bit0 descending, bit1
  unique), root page `u32` @206, page count `u32` @210, key count `u64` @222.
- Node block: `u16` type @0 (1 root / 2 internal / 3 leaf), `u16` key count
  @2, `u32` left sibling @4, `u32` right sibling @8, `u32` leftmost child @12
  (internal nodes), entries from offset 16. Leaf entry: `u16` klen, key
  bytes, `u32` recno. Internal entry: `u16` klen, separator key (first key of
  the child), `u32` 0, `u32` child page. A single-leaf tree keeps LEAF type
  at the root page.
- Keys are the canonical sortable encoding (rx_key_encode): type byte then
  payload; character keys are uppercased with trailing spaces stripped
  (MACHINE collation); numerics are sign-flipped IEEE doubles; dates are
  `YYYYMMDD` digits (empty = spaces, sorts first); logicals T/F. Equal keys
  batch by ascending recno; UNIQUE tags drop exact duplicate keys.
- Structural CDX (same stem as the DBF, `.cdx`) opens automatically with USE
  and its first tag is the controlling order; structural writes happen only
  under exclusive open (FoxPro rule). Non-structural CDX and IDX open via
  USE ... INDEX / SET INDEX TO.
- IDX standalone: block 0 is a "RIDX" header (sig @0, version @4, key
  expression[128] @8, FOR[64] @136, flags @200, root page @204, key count
  `u64` @208, page count @216); tree blocks from page 1, same node format.
- NTX/NDX: read-only import of uncompressed dBase III indexes (1024-byte
  pages); compressed variants produce the R_ERR_CDX_VARIANT (303)
  diagnostic; users convert with INDEX ON ... TO new.
- Reynard-written CDX/IDX must open in FoxPro 2.6a and VFP 9; byte-compat
  against reference fixtures is a CI gate at M6+ (no reference fixtures are
  checked into this tree yet — see DEVIATIONS #37). Our own files are
  verified byte-reproducible and by an independent validator (harness `cdx`
  mode: ctest cdx_engine).

## 5. Other formats

- `.RXP` compiled program cache (spec §7.4): magic `RXP1`, version,
  source hash, flags, bytecode blob, string table (M5).
- `.SCR` screen / `.RSO` compiled screen, `.RLB` label, `.RPJ` project /
  `.RPP` packed application (zip container with manifest.json) — text
  formats specified here at their milestones (M7-M9).
- Import/export: SDF, DELIMITED [WITH del|BLANK|TAB], XLS (BIFF5) —
  spec §8.8.


## 6. Screen files: .SCR text and .RSO compiled form (spec \u00a79.2, M7)

Not FoxPro .SCX (binary table) \u2014 Reynard uses its own formats.

**.SCR (text, canonical, one directive per line):**

```
REYNARD SCREEN 1
NAME <NAME>
SIZE <h>,<w>
ITEM SAY <r>,<c> SCHEME <n> "<text>"
ITEM GET <r>,<c> VAR <var> [SAY "<label>"] SIZE <h>,<w> [PICTURE "<p>"] [FUNCTION "<f>"] [WHEN <e>] [VALID <e>] [RANGE <a>[,<b>]] [DEFAULT <e>] SCHEME <n>
ITEM BOX <r1>,<c1> TO <r2>,<c2> SCHEME <n>
ITEM LINE <r1>,<c1> TO <r2>,<c2> SCHEME <n>
ITEM BUTTON|RADIO|CHECK|POPUP <r>,<c> PROMPT "<p>" VAR <var> SCHEME <n>
ITEM SPIN <r>,<c> VAR <var> SCHEME <n>
ITEM INVB <r>,<c> SIZE <h>,<w> VAR <var> SCHEME <n>
END
```

The writer emits exactly this field order, so load\u2192save is
byte-identical (gate: harness `scrcheck`). Strings are double-quoted with
`\\` and `\"` escapes. WHEN/VALID/RANGE/DEFAULT are raw expression text
running to the next clause keyword (canonical order WHEN, VALID, RANGE,
DEFAULT, SCHEME).

**.RSO (binary, byte-reproducible):** header 120 bytes \u2014 u32 LE magic
`RRSO` (0x4F535252), u32 LE version=1, name[64] NUL-padded, u32 h, u32 w,
u32 nitems, u32 item record size (896), sha256[32] of the canonical .SCR
bytes; then nitems fixed 896-byte records, all integers LE: u16 type
(SAY=1 GET=2 BOX=3 LINE=4 BUTTON=5 RADIO=6 CHECK=7 POPUP=8 SPIN=9 INVB=10),
u16 row, u16 col, u16 row2, u16 col2, u16 h, u16 w, u16 scheme, then NUL-padded
strings at fixed offsets: text[256] @16, var[64] @272, pic[64] @336,
func[16] @400, when[128] @416, valid[128] @544, rang[96] @672, deflt[96]
@768.

**GENERATE SCREEN** emits .PRG: `@ r,c SAY "t"`, `@ r,c [SAY "l"] GET var
SIZE h,w [PICTURE..] [WHEN..] [VALID..] [RANGE..] [DEFAULT..]`,
`@ r1,c1 BOX TO r2,c2`, controls as comments (DEVIATIONS #46), and a final
`READ CYCLE`.
