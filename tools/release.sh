#!/usr/bin/env bash
# Cut a GitHub release whose tag carries the firmware build number (v<semver>+<count>).
# Usage: tools/release.sh [--dry-run] ["release notes"]
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

DRY_RUN=0
if [ "${1:-}" = "--dry-run" ]; then DRY_RUN=1; shift; fi

SEMVER=$(tr -d '[:space:]' < firmware/p4/version.txt)
BUILD_NUM=$(git rev-list --count HEAD)
VER="v${SEMVER}+${BUILD_NUM}"
TITLE="v${SEMVER} (build ${BUILD_NUM})"
BIN="firmware/p4/build/ajmiddlecar.bin"
NOTES="${1:-Release ${VER}}"

# Only tracked changes matter — the build number comes from committed history; untracked
# build artifacts (host-test binaries, generated dirs) don't change the release commit.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "ERROR: tracked changes present — commit them so the build number matches the release commit"; exit 1
fi
BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$BRANCH" != "main" ]; then echo "ERROR: not on main (on $BRANCH)"; exit 1; fi

if [ "$DRY_RUN" = 1 ]; then
    echo "[dry-run] version : $VER"
    echo "[dry-run] tag     : $VER"
    echo "[dry-run] title   : $TITLE"
    echo "[dry-run] asset   : $BIN"
    echo "[dry-run] notes   : $NOTES"
    echo "[dry-run] would run: idf.py fullclean && idf.py build && gh release create '$VER' '$BIN' --title '$TITLE' --notes '...'"
    exit 0
fi

source tools/env-p4.sh >/dev/null 2>&1
(cd firmware/p4 && idf.py fullclean >/dev/null && idf.py build)
[ -f "$BIN" ] || { echo "ERROR: $BIN not built"; exit 1; }

gh release create "$VER" "$BIN" --title "$TITLE" --notes "$NOTES"
echo "Released $VER"
