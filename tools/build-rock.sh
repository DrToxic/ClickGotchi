#!/usr/bin/env bash
# Build clickgotchi.rock for the iPod Video 5.5G (IPOD_VIDEO) against a RockBox release tag.
# Used by hand and by .github/workflows/release.yml.
#   tools/build-rock.sh [output-dir]      (default output: the repo root)
# Needs: git, make, gcc, arm-none-eabi-gcc on PATH.
set -euo pipefail

ROCKBOX_TAG=${ROCKBOX_TAG:-v4.0-final}
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "${1:-$HERE}"
OUT=$(cd "${1:-$HERE}" && pwd)   # absolute: the script changes directory below
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

git clone --depth 1 --branch "$ROCKBOX_TAG" https://github.com/Rockbox/rockbox.git "$WORK/rockbox"
cd "$WORK/rockbox"

# drop the plugin into the RockBox tree and register it, as the README describes
cp "$HERE/apps/plugins/clickgotchi.c" "$HERE/apps/plugins/clickgotchi_art.h" apps/plugins/
cat "$HERE/apps/plugins/SOURCES.add" >> apps/plugins/SOURCES
cat "$HERE/apps/plugins/CATEGORIES.add" >> apps/plugins/CATEGORIES

mkdir "$WORK/build" && cd "$WORK/build"
../rockbox/tools/configure --target=ipodvideo --type=n --compiler-prefix=arm-none-eabi- > configure.log 2>&1 \
  || { cat configure.log; exit 1; }
make "$WORK/build/apps/plugins/clickgotchi.rock"

cp "$WORK/build/apps/plugins/clickgotchi.rock" "$OUT/clickgotchi.rock"
sha256sum "$OUT/clickgotchi.rock"
