# libFuzzer harnesses (spec §12), built with -DENABLE_FUZZ=on.
# libFuzzer requires Clang; with GCC the targets are skipped gracefully.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(STATUS "ENABLE_FUZZ: libFuzzer needs Clang; skipping fuzz targets")
    return()
endif()

add_executable(fuzz_dbf fuzz_dbf.c)
target_link_libraries(fuzz_dbf PRIVATE reynard_core)
target_compile_options(fuzz_dbf PRIVATE -fsanitize=fuzzer,undefined -g)
target_link_options(fuzz_dbf PRIVATE -fsanitize=fuzzer,undefined)

add_custom_target(fuzz-corpus
    COMMAND ${CMAKE_COMMAND} -E make_directory fuzz-corpus
    COMMAND fuzz_dbf ${CMAKE_CURRENT_SOURCE_DIR}/../../testdata/gen/fuzz-seeds
            -runs=100000 -artifact_prefix=fuzz-corpus/
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Run fuzz_dbf against the seed corpus (CI fuzzing job)")
