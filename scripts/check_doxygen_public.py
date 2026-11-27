#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2025 The Reynard contributors
"""Public-API documentation gate (spec 11.1, M6 API-freeze gate, enforced at M10):
every top-level declaration in include/reynard/*.h must carry a doxygen
comment (block comment on the preceding lines, or a trailing /**< ... */).
Run from the repo root:  python3 scripts/check_doxygen_public.py
"""
import glob
import re
import sys

PROTO = re.compile(r"^[A-Za-z_].*\([^;]*\)\s*;\s*$")          # function prototype
VAR = re.compile(r"^(?:const\s+|static\s+|extern\s+)*[A-Za-z_][\w\s\*]*\w+\s*;")  # variable
MACRO = re.compile(r"^#define\s+RX_[A-Z0-9_]+")
TYPEDEF = re.compile(r"^typedef\b")

def is_decl(stripped):
    if not stripped or stripped.startswith(("#", "/*", "*", "//")):
        return False
    if stripped.endswith(";") and ("(" in stripped or VAR.match(stripped)):
        return True
    return bool(MACRO.match(stripped) or TYPEDEF.match(stripped))

def main():
    bad = 0
    for path in sorted(glob.glob("include/reynard/*.h")):
        text = open(path, encoding="utf-8").read()
        lines = text.splitlines()
        undoc = []
        depth = 0
        in_comment = False
        for i, ln in enumerate(lines):
            stripped = ln.strip()
            if depth == 0 and is_decl(stripped) and "/**<" not in ln:
                # look back past blank lines for a doxygen block
                j = i - 1
                while j >= 0 and lines[j].strip() == "":
                    j -= 1
                if j < 0 or not lines[j].strip().endswith("*/") or "/**" not in "\n".join(lines[max(0, j - 40):j + 1]):
                    undoc.append(f"{path}:{i + 1}: {stripped[:70]}")
            # rough lexer for brace depth / comment state
            j = 0
            while j < len(ln):
                two = ln[j:j + 2]
                if in_comment:
                    if two == "*/":
                        in_comment = False
                        j += 2
                        continue
                    j += 1
                    continue
                if two == "/*":
                    in_comment = True
                    j += 2
                    continue
                if two == "//":
                    break
                if ln[j] == "{":
                    depth += 1
                elif ln[j] == "}":
                    depth -= 1
                j += 1
        for u in undoc:
            print("UNDOCUMENTED " + u)
        bad += len(undoc)
    if bad:
        print(f"doxygen public API: {bad} undocumented declaration(s)")
        return 1
    print("doxygen public API: all include/reynard/*.h declarations documented")
    return 0

if __name__ == "__main__":
    sys.exit(main())
