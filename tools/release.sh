#!/usr/bin/env bash
# Cut a GitHub release whose tag carries the firmware build number (v<semver>+<count>).
# Usage: tools/release.sh [--dry-run] ["release notes"]
#
# Known hazard, deliberately unguarded: the build number is `git rev-list --count HEAD`,
# so a rewrite of main's history can cut a release every fielded car considers itself
# ahead of. Do not rewrite main's history; there is no in-band defense (2026-08-23 audit).
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

DRY_RUN=0; NOTES_ARG=""
for arg in "$@"; do
    case "$arg" in
        --dry-run)      DRY_RUN=1 ;;
        --*)            echo "ERROR: unknown flag $arg"; exit 1 ;;
        *)              [ -z "$NOTES_ARG" ] || { echo "ERROR: multiple notes arguments"; exit 1; }
                        NOTES_ARG="$arg" ;;
    esac
done

# version.txt must be exactly one line: CMake reads only the first, this script strips
# whitespace — a second line would let the tag and the embedded version disagree. It lives at
# the repo root because both firmwares read it: one release, one version, two images.
if [ "$(grep -c '' version.txt)" != 1 ]; then
    echo "ERROR: version.txt must be exactly one line"; exit 1
fi
SEMVER=$(tr -d '[:space:]' < version.txt)
[[ "$SEMVER" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "ERROR: version.txt must contain a bare semver (got: $SEMVER)"; exit 1; }
BUILD_NUM=$(git rev-list --count HEAD)
VER="v${SEMVER}+${BUILD_NUM}"
TITLE="v${SEMVER} (build ${BUILD_NUM})"
BIN_CAR="firmware/p4/build/ajmiddlecar.bin"
BIN_DONGLE="firmware/s3/build/ajdongle.bin"
NOTES="${NOTES_ARG:-Release ${VER}}"

if [ "$DRY_RUN" = 1 ]; then
    echo "[dry-run] version : $VER"
    echo "[dry-run] tag     : $VER  (target: $(git rev-parse HEAD))"
    echo "[dry-run] title   : $TITLE"
    echo "[dry-run] assets  : $BIN_CAR"
    echo "[dry-run]         : $BIN_DONGLE"
    echo "[dry-run] radio   : built from the pinned esp_hosted and embedded in $BIN_CAR"
    echo "[dry-run] notes   : $NOTES"
    echo "[dry-run] would run: test-all && rm sdkconfig && idf.py fullclean && idf.py build (p4, s3) && gh release create '$VER' '$BIN_CAR' '$BIN_DONGLE' --target <HEAD> ..."
    exit 0
fi

# Only tracked changes matter — the build number comes from committed history; untracked
# build artifacts don't change the release commit. (The two untracked files that COULD —
# firmware/p4/sdkconfig and firmware/s3/sdkconfig — are deleted below so the build
# regenerates both from defaults.)
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

echo "Running the test suite before building..."
./tools/test-all.sh

set +e
source tools/env-p4.sh >/dev/null 2>&1
RC=$?
set -e
[ "$RC" = 0 ] || { echo "ERROR: failed to source tools/env-p4.sh"; exit 1; }
# A stray bench sdkconfig must not configure a release: regenerate purely from defaults. This
# matters more for the dongle than for the car — bench work on the S3 has run with local
# overrides before, and a release built from one would ship them.
rm -f firmware/p4/sdkconfig firmware/p4/sdkconfig.old
rm -f firmware/s3/sdkconfig firmware/s3/sdkconfig.old
# The C6's image rides inside the car's, so build it first and put it where the car's build
# embeds it. Same source flash-radio.sh uses — the pinned component's own example — so the pin
# determines both halves and there is no second version to keep in step.
HOSTED="firmware/p4/managed_components/espressif__esp_hosted"
CP="$HOSTED/examples/wifi/sta/cp"
if [ ! -d "$CP" ]; then
    echo "ERROR: esp_hosted is not fetched — run (cd firmware/p4 && idf.py reconfigure) first"; exit 1
fi
# The co-processor's SDIO datapath sends frames larger than stock ESP-IDF allows. This is the
# vendor's own patch, it is idempotent, and without it the build stops with an explicit error —
# the same line firmware/c6/flash-radio.sh runs for the same reason.
python "$HOSTED/tools/eh.py" patch-idf --idf-path "$IDF_PATH" >/dev/null
(cd "$CP" && { [ -d build ] || idf.py set-target esp32c6 >/dev/null; } && idf.py build >/dev/null)
# `|| true` is load-bearing under `set -euo pipefail`: pipefail reports the pipeline's rightmost
# non-zero status, so an unexpanded glob or a grep that filters everything makes this assignment
# fail — and a bare VAR=$(...) failing under `set -e` kills the script with no output at all,
# leaving the explicit error on the next line unreachable. The radio-pin block deleted from this
# script carried a comment warning about exactly this.
CP_BIN=$(ls "$CP"/build/*.bin 2>/dev/null | grep -v -E 'bootloader|partition-table|ota_data' | head -1) || true
[ -n "$CP_BIN" ] || { echo "ERROR: the co-processor build produced no image"; exit 1; }
cp "$CP_BIN" firmware/p4/main/radio_image.bin
echo "radio image: $(basename "$CP_BIN"), $(wc -c < firmware/p4/main/radio_image.bin) bytes"

(cd firmware/p4 && idf.py fullclean >/dev/null && idf.py build)
[ -f "$BIN_CAR" ] || { echo "ERROR: $BIN_CAR not built"; exit 1; }
# A release whose car image does not actually contain the radio's is the silent version of the
# bug this whole change removes: it would look current and strand a pin bump anyway.
if ! riscv32-esp-elf-nm firmware/p4/build/ajmiddlecar.elf | grep -q _binary_radio_image_bin_start; then
    echo "ERROR: the car image does not embed a radio image"; exit 1
fi
EMBEDDED=$(wc -c < firmware/p4/main/radio_image.bin)
[ "$EMBEDDED" -gt 4096 ] || { echo "ERROR: the embedded radio image is $EMBEDDED bytes — not an image"; exit 1; }
# The dongle is an Xtensa target; the car and its radio are both RISC-V, so an ESP-IDF installed
# for the car alone has no compiler for it. Say so rather than letting a toolchain error look
# like a firmware problem.
if ! (cd firmware/s3 && idf.py fullclean >/dev/null && idf.py build); then
    echo "ERROR: the dongle build failed. If this is a fresh ESP-IDF install, it has no Xtensa"
    echo "       toolchain yet: ~/esp/esp-idf-v6.0.2/install.sh esp32s3"; exit 1
fi
[ -f "$BIN_DONGLE" ] || { echo "ERROR: $BIN_DONGLE not built"; exit 1; }

gh release create "$VER" "$BIN_CAR" "$BIN_DONGLE" --target "$LOCAL_HEAD" --title "$TITLE" --notes "$NOTES"
echo "Released $VER"
