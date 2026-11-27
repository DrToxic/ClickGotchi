#!/usr/bin/env bash
# Build clickgotchi.rock for the iPod Video 5.5G (IPOD_VIDEO) against a RockBox release tag.
# Used by hand and by .github/workflows/release.yml.
#   tools/build-rock.sh [output-dir]      (default output: the repo root)
# ROCKBOX_REF picks the RockBox tree: a tag (default v4.0-final), a branch, or a full commit SHA.
#   ROCKBOX_REF=6fc65ab2f4c8c420dd95437c7ce10d970324316d tools/build-rock.sh dist-api287
# Needs: git, make, gcc, arm-none-eabi-gcc on PATH.
set -euo pipefail

ROCKBOX_REF=${ROCKBOX_REF:-${ROCKBOX_TAG:-v4.0-final}}
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "${1:-$HERE}"
OUT=$(cd "${1:-$HERE}" && pwd)   # absolute: the script changes directory below
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# fetch just the one ref (a tag, branch or commit SHA), not the whole history
mkdir "$WORK/rockbox" && cd "$WORK/rockbox"
git init -q
git remote add origin https://github.com/Rockbox/rockbox.git
git fetch -q --depth 1 origin "$ROCKBOX_REF"
git checkout -q FETCH_HEAD
echo "RockBox $(git rev-parse --short HEAD) plugin API $(sed -n 's/^#define PLUGIN_API_VERSION[[:space:]]*//p' apps/plugin.h)"

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
