# vendor/

`fetch-ncurses.sh` downloads the pinned ncurses 6.5 tarball (sha256-pinned)
into `vendor/ncurses/` for builds without a system ncurses
(`cmake -DUSE_SYSTEM_NCURSES=off`). The vendored sources are fetched at build
time and are NOT committed to this repository; when vendored they are
distributed under the ncurses MIT-style license (see the upstream tarball).
