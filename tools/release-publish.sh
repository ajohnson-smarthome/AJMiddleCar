#!/usr/bin/env bash
# The publish step of tools/release.sh, on its own so a host test can drive it against a fake gh.
# Usage: tools/release-publish.sh <tag> <target-sha> <title> <notes> <file>...
#
# Order matters here. `gh release create <tag> <files>` publishes FIRST and uploads AFTER, so a
# second upload that broke off left a one-file release at /releases/latest — and every pult in
# the field on "no release for this board", since the app looks for its image by exact name and
# treats a release without it as no release at all (AJM-139). So: create the release as a draft,
# upload every file, check that the release holds exactly the names we meant to ship, and only
# then flip the draft to published. A draft has no tag and is invisible to /releases/latest, so
# whatever breaks after the create leaves a draft behind, never a half-release.
set -euo pipefail

[ "$#" -ge 5 ] || { echo "usage: $0 <tag> <target-sha> <title> <notes> <file>..." >&2; exit 1; }
TAG="$1"; TARGET="$2"; TITLE="$3"; NOTES="$4"; shift 4
FILES=("$@")

DRAFTED=0
on_exit() {
    local rc=$?
    if [ "$rc" -ne 0 ] && [ "$DRAFTED" = 1 ]; then
        echo "ERROR: $TAG is left as a DRAFT — /releases/latest does not see it. Finish it by hand" >&2
        echo "       (gh release upload '$TAG' <file>... && gh release edit '$TAG' --draft=false)" >&2
        echo "       or discard it (gh release delete '$TAG' --yes) and run the release again." >&2
    fi
    exit "$rc"
}
trap on_exit EXIT

gh release create "$TAG" --draft --target "$TARGET" --title "$TITLE" --notes "$NOTES"
DRAFTED=1
gh release upload "$TAG" "${FILES[@]}"

# Exactly the names we uploaded, no more and no fewer: the app finds an image by its file name,
# so a renamed or lost asset is as good as no release for that board.
WANT=$(printf '%s\n' "${FILES[@]##*/}" | sort)
GOT=$(gh release view "$TAG" --json assets --jq '.assets[].name' | sort)
if [ "$WANT" != "$GOT" ]; then
    echo "ERROR: $TAG does not hold the files it should — not publishing." >&2
    echo "       expected: $(echo "$WANT" | tr '\n' ' ')" >&2
    echo "       found   : $(echo "$GOT" | tr '\n' ' ')" >&2
    exit 1
fi

gh release edit "$TAG" --draft=false
echo "published $TAG with $(echo "$GOT" | tr '\n' ' ')"
