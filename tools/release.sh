#!/usr/bin/env bash
# Cut a GitHub release whose tag carries the firmware build number (v<semver>+<count>).
# Usage: tools/release.sh [--dry-run] [--radio-bumped] ["release notes"]
#
# Known hazard, deliberately unguarded: the build number is `git rev-list --count HEAD`,
# so a rewrite of main's history can cut a release every fielded car considers itself
# ahead of. Do not rewrite main's history; there is no in-band defense (2026-08-23 audit).
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

DRY_RUN=0; RADIO_BUMPED=0; NOTES_ARG=""
for arg in "$@"; do
    case "$arg" in
        --dry-run)      DRY_RUN=1 ;;
        --radio-bumped) RADIO_BUMPED=1 ;;
        --*)            echo "ERROR: unknown flag $arg"; exit 1 ;;
        *)              [ -z "$NOTES_ARG" ] || { echo "ERROR: multiple notes arguments"; exit 1; }
                        NOTES_ARG="$arg" ;;
    esac
done

# version.txt must be exactly one line: CMake reads only the first, this script strips
# whitespace — a second line would let the tag and the embedded version disagree.
if [ "$(grep -c '' firmware/p4/version.txt)" != 1 ]; then
    echo "ERROR: firmware/p4/version.txt must be exactly one line"; exit 1
fi
SEMVER=$(tr -d '[:space:]' < firmware/p4/version.txt)
[[ "$SEMVER" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "ERROR: version.txt must contain a bare semver (got: $SEMVER)"; exit 1; }
BUILD_NUM=$(git rev-list --count HEAD)
VER="v${SEMVER}+${BUILD_NUM}"
TITLE="v${SEMVER} (build ${BUILD_NUM})"
BIN="firmware/p4/build/ajmiddlecar.bin"
NOTES="${NOTES_ARG:-Release ${VER}}"

# Radio pin: read from the current tree only (no network), so this fires even under
# --dry-run and before any fetch. The `|| true` keeps a missing/malformed pin line from
# being swallowed by `set -e` on a failing command substitution — under set -euo pipefail,
# a bare `VAR=$(cmd)` assignment from a failing substitution kills the script with no
# output, which would make the guard below unreachable. The explicit guard turns that into
# a clear error instead.
radio_pin() { grep -E 'espressif/esp_hosted:' "$1" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1; }
PIN=$(radio_pin firmware/p4/main/idf_component.yml) || true
[ -n "$PIN" ] || { echo "ERROR: no esp_hosted pin found in firmware/p4/main/idf_component.yml"; exit 1; }
PIN_MOVED=0
PREV_PIN=""

if [ "$DRY_RUN" = 1 ]; then
    echo "[dry-run] version : $VER"
    echo "[dry-run] tag     : $VER  (target: $(git rev-parse HEAD))"
    echo "[dry-run] title   : $TITLE"
    echo "[dry-run] asset   : $BIN"
    echo "[dry-run] radio   : esp_hosted $PIN"
    echo "[dry-run] notes   : $NOTES"
    echo "[dry-run] would run: test-all && rm sdkconfig && idf.py fullclean && idf.py build && gh release create '$VER' '$BIN' --target <HEAD> ..."
    exit 0
fi

# The radio half rides OUTSIDE this channel: /ota updates only the P4, so a release whose
# firmware pins a newer esp_hosted strands every OTA'd car on a bench reflash of the C6.
# Detect the pin moving since the previous release and refuse to ship it silently.
#
# The previous release's tag may exist only on origin (gh-created release tags are pushed,
# not necessarily fetched into every clone) — fetch quietly first, tolerating offline. The
# repo also carries a stray bare `v1.0` tag that is not a release tag; match the release
# pattern (v<semver>+<build>) so it can never be picked up here.
#
# This whole block runs only on the real (non-dry-run) path: it is the only network call
# release.sh makes, and a dry run must make zero network calls.
git fetch --tags origin >/dev/null 2>&1 || true
PREV_TAG=$(git tag --list 'v*+*' --sort=-creatordate | head -1 || true)
if [ -z "$PREV_TAG" ]; then
    echo "NOTE: no previous release tag found — radio-pin gate is a no-op for this release."
else
    PREV_PIN=$(git show "${PREV_TAG}:firmware/p4/main/idf_component.yml" 2>/dev/null | \
               { grep -E 'espressif/esp_hosted:' || true; } | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1) || true
    if [ -z "$PREV_PIN" ]; then
        echo "NOTE: ${PREV_TAG} predates the esp_hosted pin line — radio-pin gate is a no-op for this release."
    elif [ "$PREV_PIN" != "$PIN" ]; then
        PIN_MOVED=1
    fi
fi
if [ "$PIN_MOVED" = 1 ]; then
    NOTES="${NOTES}"$'\n\n'"⚠️ Этот релиз меняет радио-пин (esp_hosted ${PREV_PIN} → ${PIN}): после OTA потребуется стендовая перепрошивка радио C6 (firmware/c6/README.md)."
fi

# Only tracked changes matter — the build number comes from committed history; untracked
# build artifacts don't change the release commit. (The one untracked file that COULD —
# firmware/p4/sdkconfig — is deleted below so the build regenerates it from defaults.)
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "ERROR: tracked changes present — commit them so the build number matches the release commit"; exit 1
fi
BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$BRANCH" != "main" ]; then echo "ERROR: not on main (on $BRANCH)"; exit 1; fi

# The tag gh creates must name the commit the binary was built from. gh tags the REMOTE
# default-branch head when the tag does not exist, so an unpushed main would ship a binary
# whose tag points at the wrong commit — and could make /releases/latest ambiguous.
LOCAL_HEAD=$(git rev-parse HEAD)
REMOTE_HEAD=$(git ls-remote origin refs/heads/main | cut -f1) || { echo "ERROR: cannot reach origin to verify main is pushed"; exit 1; }
[ -n "$REMOTE_HEAD" ] || { echo "ERROR: origin has no refs/heads/main — is the default branch renamed?"; exit 1; }
if [ "$LOCAL_HEAD" != "$REMOTE_HEAD" ]; then
    echo "ERROR: local main ($LOCAL_HEAD) is not origin/main ($REMOTE_HEAD) — push first"; exit 1
fi

if [ "$PIN_MOVED" = 1 ] && [ "$RADIO_BUMPED" != 1 ]; then
    echo "ERROR: the esp_hosted pin moved since $PREV_TAG ($PREV_PIN → $PIN)."
    echo "       OTA cannot deliver the C6 image: every updated car will need a bench"
    echo "       reflash. Pass --radio-bumped to acknowledge and ship anyway."; exit 1
fi

echo "Running the test suite before building..."
./tools/test-all.sh

set +e
source tools/env-p4.sh >/dev/null 2>&1
RC=$?
set -e
[ "$RC" = 0 ] || { echo "ERROR: failed to source tools/env-p4.sh"; exit 1; }
# A stray bench sdkconfig must not configure a release: regenerate purely from defaults.
rm -f firmware/p4/sdkconfig firmware/p4/sdkconfig.old
(cd firmware/p4 && idf.py fullclean >/dev/null && idf.py build)
[ -f "$BIN" ] || { echo "ERROR: $BIN not built"; exit 1; }

gh release create "$VER" "$BIN" --target "$LOCAL_HEAD" --title "$TITLE" --notes "$NOTES"
echo "Released $VER"
