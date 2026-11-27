#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2025 The Reynard contributors
"""Enforce the .editorconfig subset in CI (spec 11.1).

Checks every tracked text file for: LF endings, no trailing whitespace,
final newline, max_line = 100, 4-space indentation in .c/.h (tabs are
only allowed in Makefile.simple, which .editorconfig marks tab-indented).
Run from the repo root:  python3 scripts/check_editorconfig.py
"""
import subprocess
import sys

MAX_LINE = 100
TEXT_EXTS = {
    ".c", ".h", ".py", ".md", ".1", ".toml", ".yml", ".yaml", ".cfg",
    ".txt", ".cmake", ".sh", ".rb", ".spec", ".in", ".plist", "APKBUILD",
}
TAB_OK = {"Makefile.simple", "packaging/debian/rules", "packaging/ports/Makefile"}

def tracked_files():
    out = subprocess.run(
        ["git", "ls-files"], capture_output=True, text=True, check=True
    ).stdout
    return [f for f in out.splitlines() if f]

def is_text(path):
    if path in TAB_OK:
        return True
    if "/" in path:
        ext = "." + path.rsplit(".", 1)[-1] if "." in path.rsplit("/", 1)[-1] else ""
    else:
        ext = "." + path.rsplit(".", 1)[-1] if "." in path else ""
    if ext in TEXT_EXTS:
        return True
    base = path.rsplit("/", 1)[-1]
    return base in TEXT_EXTS or base in {"Makefile", "Doxyfile", "LICENSE", "control",
                                          "changelog", "copyright", "distinfo",
                                          "pkg-plist"}

CODE_EXTS = (".c", ".h", ".py")

def check(path):
    """Check one file: LF endings, no trailing whitespace, final newline,
    max_line for code files, and no tab indentation in code (the exact
    indentation style is enforced by the clang-format CI job; Makefile.simple
    and the packaging makefiles are tab-indented per .editorconfig)."""
    errs = []
    with open(path, "rb") as f:
        data = f.read()
    if b"\r" in data:
        errs.append("CR bytes (end_of_line = lf)")
    if data and not data.endswith(b"\n"):
        errs.append("missing final newline (insert_final_newline = true)")
    lines = data.split(b"\n")
    code = path.endswith(CODE_EXTS) and path not in TAB_OK
    for i, ln in enumerate(lines, 1):
        if ln != ln.rstrip(b" \t"):
            errs.append(f"line {i}: trailing whitespace")
        if code and len(ln) > MAX_LINE:
            errs.append(f"line {i}: {len(ln)} cols > {MAX_LINE} (max_line = 100)")
        if code and ln.startswith(b"\t"):
            errs.append(f"line {i}: tab indentation in code (indent_style = space)")
        if len(errs) > 4:
            break
    return errs

def main():
    bad = 0
    for path in tracked_files():
        if not is_text(path):
            continue
        errs = check(path)
        if errs:
            bad += 1
            print(f"FAIL {path}:")
            for e in errs[:5]:
                print(f"  {e}")
    if bad:
        print(f"editorconfig: {bad} file(s) failed")
        return 1
    print("editorconfig: all tracked text files OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
