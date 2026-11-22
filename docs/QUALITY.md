# Quality Plan and Status (spec §11/§12)

## Binding standards

- C17, zero compiler warnings under `-Wall -Wextra -Wpedantic`
  (`-DRX_WERROR=on` in CI), sanitizer-clean (ASan+UBSan per commit on
  Linux/macOS in CI; valgrind weekly; MSan monthly).
- 4-space indent, K&R braces, snake_case, 100-column limit;
  `.editorconfig` + `.clang-format` checked in and enforced in CI.
- Every public function has a doxygen comment; every module has a
  README one-pager; every milestone updates CHANGELOG.md.
- License header in every file; REUSE.toml checked in CI.
- No `#if 0`; feature flags live in flags.h with an issue number.

## Verification (current status at M1)

- `ctest`: harness selftest (term script backend event replay + blit +
  golden cell dump, mouse protocol decoder unit checks) and the
  conformance suite (28 `.prg`/`.expected` cases run in `-t` mode under
  ASan/UBSan) — green.
- Both build systems (`CMakeLists.txt`, `Makefile.simple`) build with
  zero warnings.
- Performance gates (spec §11.3) are measured in CI from M4 onward
  (browse relayout < 16 ms, LIST 100k > 60 MB/s, CDX build < 2 s,
  parse > 5 MB/s, `--version` < 20 ms, RSS < 8 MB batch / < 48 MB
  interactive).
- Static analysis: clang-tidy + cppcheck at the configured preset run in
  CI; results documented here (to be populated at M2+).
- Fuzzing: libFuzzer harnesses behind `-DENABLE_FUZZ=on` on
  `rx_dbf_open` with seed corpus `testdata/gen/fuzz-seeds/` (M2+).
- DBF/CDX round-trip byte-identity against provenance fixtures and the
  independent interop validator land at M2/M6 (spec §12).
