#!/usr/bin/env bash
# Fail if any generated artefact differs from a fresh run of the generator.
# The contract lives in contract/car-api.json; nothing it produces is hand-edited.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 "$ROOT/tools/gen_contract.py" --out-dir "$TMP"

status=0
# The artefact list comes from the generator: it was duplicated here, and a fifth
# artefact meant editing two files in step or silently checking only some of them.
while IFS= read -r rel; do
    if ! diff -u "$ROOT/$rel" "$TMP/$rel" > /dev/null 2>&1; then
        echo "DRIFT: $rel differs from a fresh generation" >&2
        diff -u "$ROOT/$rel" "$TMP/$rel" >&2 || true
        status=1
    fi
done < <(python3 "$ROOT/tools/gen_contract.py" --list-artifacts)

# docs/protocol.md is spliced into hand-written prose, so compare only the region.
region() { sed -n '/generated:endpoints/,/\/generated:endpoints/p' "$1"; }
if ! diff -u <(region "$ROOT/docs/protocol.md") <(region "$TMP/docs/protocol.md") > /dev/null 2>&1; then
    echo "DRIFT: docs/protocol.md generated region differs" >&2
    diff -u <(region "$ROOT/docs/protocol.md") <(region "$TMP/docs/protocol.md") >&2 || true
    status=1
fi

if [ "$status" -eq 0 ]; then echo "contract: no drift"; fi
exit "$status"
