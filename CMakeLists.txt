cmake_minimum_required(VERSION 3.16)
project(reynard VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(USE_SYSTEM_NCURSES "Use system ncurses/ncursesw instead of vendored" ON)
option(ENABLE_FUZZ "Build libFuzzer harnesses" OFF)
option(RX_WERROR "Treat compiler warnings as errors (CI)" OFF)

add_compile_definitions(_POSIX_C_SOURCE=200809L)

set(RX_WARNINGS -Wall -Wextra -Wpedantic)
if(RX_WERROR)
    list(APPEND RX_WARNINGS -Werror)
endif()

# Embed LICENSE for --dump-license.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" RX_LICENSE_TEXT)
string(REGEX REPLACE "([\\\"])" "\\\\\\1" RX_LICENSE_TEXT_ESC "${RX_LICENSE_TEXT}")
string(REGEX REPLACE "\n" "\\\\n\"\n\"" RX_LICENSE_TEXT_C "${RX_LICENSE_TEXT_ESC}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/license_text.c"
     "const char rx_license_text[] =\n\"${RX_LICENSE_TEXT_C}\";\n")

set(RX_CORE_SOURCES
    ${CMAKE_CURRENT_BINARY_DIR}/license_text.c
    src/util/strbuf.c
    src/util/str.c
    src/util/utf8.c
    src/util/cp_tables.c
    src/util/ddatecnv.c
    src/util/numeric.c
    src/util/errlist.c
    src/util/log.c
    src/util/cfg.c
    src/util/sha256.c
    src/term/term_script.c
    src/term/mouse.c
    src/ui/screen.c
    src/ui/window.c
    src/ui/getsay.c
    src/ui/cmdwin.c
    src/ui/menudef.c
    src/ui/menubar.c
    src/ui/statusbar.c
    src/ui/dialog.c
    src/ui/helpwin.c
    src/ui/browse.c
    src/ui/ui.c
    src/data/dbf.c
    src/data/dbf_create.c
    src/data/field.c
    src/data/memo_fpt.c
    src/data/memo_dbt.c
    src/data/codepage.c
    src/data/workarea.c
    src/data/rlock.c
    src/data/journal.c
    src/data/scope.c
    src/data/copyto.c
    src/lang/lexer.c
    src/lang/parser.c
    src/lang/expr_compile.c
    src/lang/vm.c
    src/lang/cmds_data.c
    src/lang/cmds_env.c
    src/lang/memvar.c
    src/lang/errhand.c
    src/lang/rxp.c
    src/lang/funcs_str.c
    src/lang/funcs_num.c
    src/lang/funcs_date.c
    src/lang/funcs_db.c
    src/lang/funcs_sys.c
    src/lang/funcs_array.c
)

# ncurses production backend: built when headers are available. On hosts
# without ncurses development files (and without a vendored build) the
# script backend remains fully usable for CI and batch work.
set(RX_HAVE_NCURSES OFF)
if(USE_SYSTEM_NCURSES)
    # macOS: the system ncurses is narrow-only (5.7); prefer a package-
    # manager ncurses when installed so wide glyphs render natively —
    # Homebrew (/opt/homebrew, /usr/local) or MacPorts (/opt/local).
    if(APPLE)
        list(APPEND CMAKE_PREFIX_PATH
            /opt/homebrew/opt/ncurses /usr/local/opt/ncurses /opt/local)
    endif()
    # Header layouts differ by packager: <ncursesw/curses.h> (Homebrew,
    # most Linux), <ncurses/curses.h>, or flat <curses.h> (MacPorts,
    # BSD). Probe each prefix in order, wide first.
    set(RX_PREFIXES ${CMAKE_PREFIX_PATH} /usr /usr/local /opt/local)
    foreach(_p ${RX_PREFIXES})
        if(EXISTS "${_p}/include/ncursesw/curses.h")
            set(RX_NCURSES_INC "${_p}/include")
            set(RX_NCURSES_LIBHINT "${_p}/lib")
            set(RX_NCURSES_WIDE ON)
            set(RX_HAVE_NCURSES ON)
            break()
        endif()
    endforeach()
    if(NOT RX_HAVE_NCURSES)
        foreach(_p ${RX_PREFIXES})
            if(EXISTS "${_p}/include/ncurses/curses.h")
                set(RX_NCURSES_INC "${_p}/include")
                set(RX_NCURSES_LIBHINT "${_p}/lib")
                set(RX_NCURSES_NCDIR ON)
                set(RX_HAVE_NCURSES ON)
                break()
            elseif(EXISTS "${_p}/include/curses.h")
                set(RX_NCURSES_INC "${_p}/include")
                set(RX_NCURSES_LIBHINT "${_p}/lib")
                set(RX_HAVE_NCURSES ON)
                # flat layout (MacPorts, BSD): ncurses 6's curses.h declares
                # the wide API, so treat it as wide when the wide lib is there
                # (never for the /usr system copy, which is narrow 5.7 on macOS)
                if(NOT "${_p}" STREQUAL "/usr")
                    file(GLOB _wlib "${_p}/lib/libncursesw.*")
                    if(_wlib)
                        set(RX_NCURSESW_FLAT ON)
                    endif()
                endif()
                break()
            endif()
        endforeach()
    endif()
    if(RX_HAVE_NCURSES)
        find_library(RX_NCURSESW_LIB ncursesw PATHS "${RX_NCURSES_LIBHINT}" NO_DEFAULT_PATH)
        if(NOT RX_NCURSESW_LIB)
            find_library(RX_NCURSESW_LIB ncursesw)
        endif()
        find_library(RX_NCURSES_LIB ncurses PATHS "${RX_NCURSES_LIBHINT}" NO_DEFAULT_PATH)
        if(NOT RX_NCURSES_LIB)
            find_library(RX_NCURSES_LIB ncurses)
        endif()
        # flat wide header is only usable if the wide lib actually links
        if(RX_NCURSESW_FLAT AND NOT RX_NCURSESW_LIB)
            set(RX_NCURSESW_FLAT OFF)
        endif()
    endif()
endif()

# term_ncurses.c is always compiled; it self-guards with RX_HAVE_NCURSES
# and returns a NULL backend when ncurses is unavailable.
list(APPEND RX_CORE_SOURCES src/term/term_ncurses.c)

add_library(reynard_core STATIC ${RX_CORE_SOURCES})
if(NOT WIN32)
    target_link_libraries(reynard_core PUBLIC m)
endif()
target_include_directories(reynard_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(reynard_core PRIVATE ${RX_WARNINGS})
if(RX_HAVE_NCURSES)
    target_compile_definitions(reynard_core PRIVATE RX_HAVE_NCURSES=1)
    if(RX_NCURSES_WIDE)
        target_compile_definitions(reynard_core PRIVATE RX_HAVE_NCURSESW_H=1)
    elseif(RX_NCURSESW_FLAT)
        target_compile_definitions(reynard_core PRIVATE RX_HAVE_NCURSESW_FLAT=1)
    elseif(RX_NCURSES_NCDIR)
        target_compile_definitions(reynard_core PRIVATE RX_HAVE_NCURSES_H=1)
    endif()
    target_include_directories(reynard_core PRIVATE ${RX_NCURSES_INC})
    if(RX_NCURSESW_LIB)
        target_link_libraries(reynard_core PUBLIC ${RX_NCURSESW_LIB})
    else()
        target_link_libraries(reynard_core PUBLIC ${RX_NCURSES_LIB})
    endif()
endif()

add_executable(reynard src/main.c)
target_link_libraries(reynard PRIVATE reynard_core)
target_compile_options(reynard PRIVATE ${RX_WARNINGS})

enable_testing()
add_executable(reynard-harness tests/harness.c)
target_link_libraries(reynard-harness PRIVATE reynard_core)
target_compile_options(reynard-harness PRIVATE ${RX_WARNINGS})

add_test(NAME harness_selftest
    COMMAND reynard-harness selftest)
add_test(NAME conformance
    COMMAND reynard-harness conformance
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance
        $<TARGET_FILE:reynard>
        ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gen
        ${CMAKE_CURRENT_BINARY_DIR}/test-work)
add_test(NAME dbf_roundtrip
    COMMAND reynard-harness roundtrip
        ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gen
        ${CMAKE_CURRENT_BINARY_DIR}/test-work)
add_test(NAME rxp_repro
    COMMAND reynard-harness rxp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/rxp
        $<TARGET_FILE:reynard>
        ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gen
        ${CMAKE_CURRENT_BINARY_DIR}/test-work-rxp)
add_test(NAME lock_race
    COMMAND reynard-harness lockrace
        ${CMAKE_CURRENT_BINARY_DIR}/test-work)
add_test(NAME golden_screens
    COMMAND reynard-harness golden
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden
        $<TARGET_FILE:reynard>
        ${CMAKE_CURRENT_SOURCE_DIR}/testdata/gen
        ${CMAKE_CURRENT_BINARY_DIR}/test-work-golden)
add_test(NAME browse_perf
    COMMAND reynard-harness browseperf
        ${CMAKE_CURRENT_BINARY_DIR}/test-work-perf
        $<TARGET_FILE:reynard>)

if(ENABLE_FUZZ)
    add_subdirectory(tests/fuzz)
endif()
