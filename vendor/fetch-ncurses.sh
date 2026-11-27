#!/bin/sh
# Fetch the pinned vendored ncurses 6.5 (spec 13) into vendor/ncurses/.
# Used when building without system ncurses: cmake -DUSE_SYSTEM_NCURSES=off
# (macOS CI runners without MacPorts/Homebrew ncurses, minimal containers).
set -eu
VER=6.5
SHA256=136aa573e9062ca929a9c0e9e45d5bcaed6e2bf2d895fb6ee4d7831c17cd9ef2 # ncurses-6.5.tar.gz
URL="https://ftp.gnu.org/gnu/ncurses/ncurses-$VER.tar.gz"
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/ncurses"
if [ -d "$DEST" ]; then
    echo "vendor/ncurses already present"
    exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
DL=""
if command -v curl >/dev/null 2>&1; then
    DL="curl -fsSLO"
elif command -v wget >/dev/null 2>&1; then
    DL="wget -q"
else
    echo "need curl or wget to fetch ncurses" >&2
    exit 1
fi
# shellcheck disable=SC2086
$DL "$URL"
echo "$SHA256  ncurses-$VER.tar.gz" | sha256sum -c -
tar xzf "ncurses-$VER.tar.gz"
mv "ncurses-$VER" "$DEST"
echo "vendored ncurses $VER -> $DEST"
