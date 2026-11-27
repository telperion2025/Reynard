#!/usr/bin/env python3
"""Generate testdata/gen/help.dbf + help.dbt: the shipped Reynard help database
(spec 9.7: columns TOPIC C(30), KEYWORDS M, BODY M, SEEALSO M). DBF version
0x83 (dBase III + DBT memo), code page mark 0x01 (CP437). DBT: 512-byte blocks,
block 0 header (next-free block u32le), block N at offset 512*N, text
0x1A-terminated. Regenerate: python3 gen_help.py  (writes help.dbf/help.dbt
next to this script)."""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

TOPICS = [
    ("GENERAL", "intro welcome reynard foxpro xbase",
     "Reynard is a cross-platform terminal reimplementation of FoxPro 2.x / xBase.\n"
     "Run a program with: reynard app.prg   (batch) or interactively with no args.\n"
     "Batch test mode: reynard -t. Commands without a program: reynard -c \"? 1+1\".",
     "COMMAND WINDOW, KEYS"),
    ("COMMAND WINDOW", "command window ctrl-f2 type run",
     "Press Ctrl+F2 (or Window > Command) to open the Command window.\n"
     "Type any FoxPro command and press Enter to run it. Ctrl+Up/Down recalls\n"
     "history, Tab completes, Ctrl+Y kills to end of line, Esc aborts (SET ESCAPE).",
     "GENERAL, KEYS"),
    ("BROWSE", "browse grid table edit rows fields",
     "BROWSE opens the grid on the current table. Move with arrows or the mouse,\n"
     "edit cells in place, Ctrl+W or the close box commits. FIELDS, FREEZE,\n"
     "PARTITION and computed columns are supported (spec 9.1).",
     "DATABASE, RECORD"),
    ("MENUS", "menu define pad popup bar activate on selection",
     "DEFINE MENU/PAD/POPUP/BAR build application menus (spec 9.5). ON PAD and\n"
     "ON SELECTION PAD/BAR attach commands; ACTIVATE MENU shows the pad on the\n"
     "menubar, DEACTIVATE/RELEASE remove it.",
     "PROJECTS, GENERAL"),
    ("REPORTS", "report form label frm rlb emitter pdf ps html csv",
     "REPORT FORM x TO FILE out.txt|out.pdf|... runs a report (spec 9.3/7.7);\n"
     "LABEL FORM does labels (9.4). CREATE/MODIFY REPORT opens the designer.\n"
     "Emitters: text, PostScript, PDF, HTML, CSV; TO PREVIEW shows a page window.",
     "FORMS, PROJECTS"),
    ("FORMS", "form screen say get read scr picture valid",
     "@ row,col SAY expr GET var [PICTURE ...] stages a form; READ runs the modal\n"
     "editor (spec 9.2). CREATE/MODIFY SCREEN edits .SCR files; GENERATE SCREEN\n"
     "emits the .prg. VALID/RANGE/WHEN/PICTURE are honored.",
     "REPORTS, BROWSE"),
    ("TRANSACTIONS", "begin end rollback commit journal rjn atomic",
     "BEGIN TRANSACTION journals before-images to <table>.RJN; END TRANSACTION\n"
     "commits, ROLLBACK restores them (spec 8.6). PACK/ZAP are refused inside a\n"
     "transaction. A crash mid-commit leaves a header stamp; the next open\n"
     "completes or rolls back automatically.",
     "DATABASE, GENERAL"),
    ("PROJECTS", "project rpj rpp build run package app launcher",
     "A .RPJ text file lists PRGs, menus, screens, reports, labels and DBFs.\n"
     "BUILD PROJECT app FROM app.rpj compiles it into app.rpp (manifest.json +\n"
     "RXP blobs + resources + data dictionary) and writes an app-app.sh launcher.\n"
     "Run it with: reynard --run app.rpp   (spec 9.6). SET PROCEDURE TO app.rpp\n"
     "loads the packed sources so DO finds procedures inside.",
     "MENUS, --RUN"),
    ("KEYS", "f1 f10 alt help escape keys shortcuts",
     "F1 opens this help (or the ON KEY LABEL F1 command). F10/Alt activates the\n"
     "menubar; arrows navigate; Enter selects; Esc backs out. Ctrl+F2 opens the\n"
     "Command window. In BROWSE: arrows move, Enter edits a cell.",
     "GENERAL, COMMAND WINDOW"),
]

FIELDS = [("TOPIC", "C", 30, 0), ("KEYWORDS", "M", 10, 0), ("BODY", "M", 10, 0),
          ("SEEALSO", "M", 10, 0)]


def build_dbt():
    blocks = [b""]  # block 0 = header, patched later
    recs = []
    for _, kw, body, see in TOPICS:
        for text in (kw, body, see):
            data = text.encode("utf-8") + b"\x1a"
            nblk = (len(data) + 511) // 512
            if nblk == 0:
                nblk = 1
            blocks.append(data.ljust(nblk * 512, b"\x00"))
    next_free = len(blocks)
    hdr = struct.pack("<I", next_free) + b"\x00" * (512 - 4)
    out = hdr + b"".join(blocks[1:])
    # block numbers for each topic's fields: keywords, body, seealso
    nums = []
    b = 1
    for _, kw, body, see in TOPICS:
        row = []
        for text in (kw, body, see):
            data = text.encode("utf-8") + b"\x1a"
            nblk = max(1, (len(data) + 511) // 512)
            row.append(b)
            b += nblk
        nums.append(row)
    return out, nums


def main():
    dbt, nums = build_dbt()
    rec_size = 1 + sum(f[2] for f in FIELDS)
    header_size = 32 + 32 * len(FIELDS) + 1
    nrec = len(TOPICS)
    hdr = bytearray(32)
    hdr[0] = 0x83
    hdr[1] = 100  # frozen test date (M-t): yy=100 mm=1 dd=1
    hdr[2] = 1
    hdr[3] = 1
    struct.pack_into("<I", hdr, 4, nrec)
    struct.pack_into("<H", hdr, 8, header_size)
    struct.pack_into("<H", hdr, 10, rec_size)
    hdr[29] = 0x01  # CP437 language driver mark
    desc = bytearray()
    for name, typ, ln, dec in FIELDS:
        d = bytearray(32)
        d[0:11] = name.encode().ljust(11, b"\x00")
        d[11] = ord(typ)
        d[16] = ln
        d[17] = dec
        desc += d
    desc += b"\x0d"
    recs = bytearray()
    for i, (topic, _, _, _) in enumerate(TOPICS):
        recs += b" "
        recs += topic.encode().ljust(30, b" ")
        for n in nums[i]:
            recs += ("%10d" % n).encode()
    recs += b"\x1a"
    with open(os.path.join(HERE, "help.dbf"), "wb") as f:
        f.write(bytes(hdr) + bytes(desc) + bytes(recs))
    with open(os.path.join(HERE, "help.dbt"), "wb") as f:
        f.write(dbt)
    print("wrote help.dbf (%d recs) + help.dbt (%d bytes)" % (nrec, len(dbt)))


if __name__ == "__main__":
    main()
