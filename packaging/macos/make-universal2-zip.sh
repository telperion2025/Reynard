#!/bin/sh
# Build the macOS universal2 (x86_64 + arm64) zip for Reynard (spec 13).
# Run on an Apple Silicon mac with Xcode CLT and Homebrew ncurses installed:
#   ./packaging/macos/make-universal2-zip.sh v1.0.0
# Produces: reynard-<ver>-macos-universal2.zip with bin/reynard (universal),
# share/man/man1/reynard.1 and share/doc/reynard/*.
set -eu
VER="${1:-1.0.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/reynard-$VER-macos-universal2.zip"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for ARCH in x86_64 arm64; do
    B="$WORK/build-$ARCH"
    cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=Release -DRX_WERROR=on \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/ncurses;/usr/local/opt/ncurses"
    cmake --build "$B" --parallel
    DESTDIR="$WORK/root-$ARCH" cmake --install "$B"
done

mkdir -p "$WORK/pkg/bin" "$WORK/pkg/share"
lipo -create "$WORK/root-x86_64/usr/local/bin/reynard" \
              "$WORK/root-arm64/usr/local/bin/reynard" \
    -output "$WORK/pkg/bin/reynard"
cp -R "$WORK/root-arm64/usr/local/share/man" "$WORK/pkg/share/man"
mkdir -p "$WORK/pkg/share/doc/reynard"
cp "$ROOT/README.md" "$ROOT/CHANGELOG.md" "$ROOT/LICENSE" "$WORK/pkg/share/doc/reynard/"
mkdir -p "$WORK/pkg/share/doc/reynard/licenses"
cp "$ROOT/LICENSES/BSD-2-Clause.txt" "$WORK/pkg/share/doc/reynard/licenses/"

cd "$WORK/pkg"
zip -r "$OUT" bin share
cd "$ROOT"
echo "wrote $OUT"
