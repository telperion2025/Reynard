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

## Verification (current status at M10 / v1.0.0)

- `ctest --test-dir build` (and `build-asan`, ASan+UBSan+Lsan on Linux):
  12/12 green — harness selftest, 97 conformance `.prg`/`.expected` cases,
  10 DBF round-trip cases, 5 RXP cache cases, lockrace, 24 golden-screen
  scenarios, browse-perf (100k events, <16 ms gate), all 5 spec §11.3
  perf gates, 2 report-emitter hash cases, 2 screen round-trips, 27 CDX
  engine checks, project-manager checks.
- Both build systems (`CMakeLists.txt`, `Makefile.simple`) build with zero
  warnings under `-Wall -Wextra -Wpedantic` in Debug (CI, `-DRX_WERROR=on`)
  and Release; ASan/UBSan clean per commit on ubuntu + macOS, gcc + clang.
- Performance gates (spec §11.3, `reynard-harness perf`): LIST 100k
  141.7 MB/s (gate >60), CDX build 100k 0.66 s (gate <2 s), parse 55 MB/s
  (gate >5), `--version` startup 5.5 ms (gate <20 ms), batch RSS 2.3 MB
  (gate <8 MB). Sanitizer builds skip the wall-clock gates (DEVIATIONS 61).
- Sanitizers: ASan+UBSan per commit (ci.yml build-test, both OS x both CC);
  valgrind memcheck weekly (`.github/workflows/valgrind.yml`);
  MSan monthly (`.github/workflows/msan.yml`); daily soak loop
  (`.github/workflows/soak.yml`, 6 h Actions cap, DEVIATIONS 63).
- Static analysis: cppcheck (`--enable=warning,performance,portability`,
  XML report) and clang-tidy (`run-clang-tidy`) run in the advisory
  `static-analysis` CI job with reports uploaded as artifacts
  (DEVIATIONS 70). Current known advisory findings: gcc `-Wmaybe-uninitialized`
  in `src/lang/cmds_form.c` under `-O2` without `-Werror` was fixed at M10;
  remaining cppcheck notes are style-level (long functions in the VM) and
  tracked as follow-ups.
- Fuzzing: libFuzzer harness `tests/fuzz/fuzz_dbf.c` behind
  `-DENABLE_FUZZ=on` (Clang), seed corpus `testdata/gen/fuzz-seeds/`
  (4 seeds incl. bad-magic/truncated/zero-fields); the CI `fuzz` job runs
  200k iterations under ASan+UBSan and fails on any crash
  (spec §12: every found crash becomes a unit test before the fix).
- Formatting/licensing: `.clang-format` (pinned clang-format 23.1.5) +
  `.editorconfig` enforced by the CI `format` job
  (`scripts/check_editorconfig.py`); `reuse lint` clean (414/414 files,
  BSD-2-Clause, LICENSES/BSD-2-Clause.txt).
- Docs: Doxygen via `make docs` / `cmake --build build --target docs`
  (Doxyfile); CI `docs` job runs `scripts/check_doxygen_public.py`
  (100% of include/reynard/*.h declarations documented), builds HTML and
  publishes to the `docs` branch best-effort + artifact.
- Packaging: CPack DEB/RPM/TGZ/ZIP plus `packaging/` recipes (debian, rpm,
  BSD ports, Homebrew, Alpine/musl, macOS universal2 zip); DEB/TGZ payload
  verified locally, all artifacts built+uploaded by the CI `package` job.
