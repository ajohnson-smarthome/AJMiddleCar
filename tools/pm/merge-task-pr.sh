#!/usr/bin/env bash
# Merge a worker's PR into main the way the PM session does it by hand, minus the hand.
#
#   tools/pm/merge-task-pr.sh <pr-number>
#
# Every task PR ticks its own line of tasks.md, and neighbouring lines conflict in git
# even though the intent never does. This merges origin/<branch> into main with --no-ff;
# a conflict in tasks.md is resolved as "main's file plus this PR's tick" (the tick is
# the line carrying the PR's issue id, taken from the PR title `AJM-N: ...`); a conflict
# in any other file aborts, because that one needs a human. Then it pushes main (GitHub
# marks the PR merged once main contains its head), deletes the remote branch, drops the
# Orca worktree that was on it, and moves the Linear issue to Done.
#
# Needs: gh, orca, a clean main checkout as the current directory's repo.
set -euo pipefail

PR="${1:?usage: merge-task-pr.sh <pr-number>}"
cd "$(git rev-parse --show-toplevel)"

[ "$(git branch --show-current)" = "main" ] || { echo "not on main" >&2; exit 1; }
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "main is dirty" >&2; exit 1; }

BRANCH="$(gh pr view "$PR" --json headRefName --jq .headRefName)"
TITLE="$(gh pr view "$PR" --json title --jq .title)"
ISSUE="$(printf '%s' "$TITLE" | grep -oE '^[A-Z]+-[0-9]+' || true)"
[ -n "$ISSUE" ] || { echo "PR #$PR title does not start with an issue id: $TITLE" >&2; exit 1; }

git fetch -q origin main "$BRANCH"
git pull -q --ff-only origin main

echo "== merge #$PR ($ISSUE) from $BRANCH =="
if ! git merge --no-ff --no-edit -m "Merge PR #$PR: $TITLE" "origin/$BRANCH" >/dev/null 2>&1; then
    conflicted="$(git diff --name-only --diff-filter=U)"
    others="$(printf '%s\n' "$conflicted" | grep -v '/tasks\.md$' || true)"
    if [ -n "$others" ]; then
        echo "conflicts outside tasks.md — aborting merge, resolve by hand:" >&2
        printf '  %s\n' "$others" >&2
        git merge --abort
        exit 1
    fi
    for f in $conflicted; do
        # main's version, then this PR's own tick — the only line the PR is allowed to change
        git checkout --ours -- "$f"
        python3 - "$f" "$ISSUE" <<'EOF'
import re, sys
path, issue = sys.argv[1], sys.argv[2]
s = open(path, encoding="utf-8").read()
pat = re.compile(r"^- \[ \] (.*\(" + re.escape(issue) + r"\)\s*)$", re.M)
s2, n = pat.subn(lambda m: "- [x] " + m.group(1), s)
if n != 1:
    sys.exit(f"{path}: expected exactly one open line for {issue}, found {n}")
open(path, "w", encoding="utf-8").write(s2)
EOF
        git add -- "$f"
    done
    git commit -q --no-edit -m "Merge PR #$PR: $TITLE"
    echo "tasks.md conflict resolved: main + tick for $ISSUE"
fi

git push -q origin main
git push -q origin --delete "$BRANCH" 2>/dev/null || true
echo "main: $(git log --oneline -1)"

# The worker's worktree is done with; find it by branch and let Orca drop it.
WT_ID="$(orca worktree list --json 2>/dev/null | python3 -c '
import json, sys
branch = sys.argv[1]
d = json.load(sys.stdin)
wts = d.get("result", {}).get("worktrees", [])
for w in wts:
    b = (w.get("branch") or "").removeprefix("refs/heads/")
    if b == branch:
        print(w["id"]); break
' "$BRANCH")"
if [ -n "$WT_ID" ]; then
    orca worktree rm --worktree "id:$WT_ID" --force --json >/dev/null && echo "worktree removed: $WT_ID"
fi
git worktree prune

orca linear status set "$ISSUE" --to Done --json >/dev/null && echo "$ISSUE → Done"
