#!/usr/bin/env bash
# Start a worker on one dev task: move the issue to Todo and open an Orca worktree with
# Claude in its first terminal, briefed by the worker role in CLAUDE.md.
#
#   tools/pm/launch-worker.sh <change-dir> AJM-N [AJM-M ...]
#
# The worktree is named ajm-N-<capability> from the task's line in tasks.md (the text in
# backticks after "Спека"), so a branch carries the issue id and Linear links the PR.
# Prints one line per worker: issue, worktree path, agent terminal handle — keep the
# handle, `orca terminal wait --for tui-idle` on it is how the PM session learns the
# worker is done.
set -euo pipefail

CHANGE="${1:?usage: launch-worker.sh <change-dir> AJM-N ...}"; shift
[ $# -gt 0 ] || { echo "no issues given" >&2; exit 1; }
cd "$(git rev-parse --show-toplevel)"
REPO_ID="$(orca repo show --repo "path:$PWD" --json | python3 -c 'import json,sys; r=json.load(sys.stdin)["result"]; print(r.get("repo", r)["id"])')"
TASKS="$CHANGE/tasks.md"

for ISSUE in "$@"; do
    LINE="$(grep -E "\($ISSUE\)\s*$" "$TASKS" || true)"
    [ -n "$LINE" ] || { echo "$ISSUE: no line in $TASKS" >&2; continue; }
    NUM="$(printf '%s' "$LINE" | grep -oE '^- \[[ xX]\] [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+$')"
    CAP="$(printf '%s' "$LINE" | grep -oE 'Спека `[^`]+`' | head -1 | sed -E 's/Спека `([^`]+)`/\1/' || true)"
    SLUG="$(printf '%s' "${CAP:-task-$NUM}" | tr '/' '-' | tr -c 'a-z0-9-\n' '-' | sed -E 's/-+/-/g; s/^-|-$//g')"
    N="${ISSUE#*-}"
    NAME="ajm-$N-$SLUG"

    # Linear writes through Orca time out now and then; a hiccup on one issue must not
    # end the batch, so retry, and on a final failure report and move on.
    todo_ok=0
    for attempt in 1 2 3; do
        if orca linear status set "$ISSUE" --to Todo --json 2>/dev/null | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin).get("ok") else 1)' 2>/dev/null; then
            todo_ok=1; break
        fi
        sleep 3
    done
    [ "$todo_ok" = 1 ] || { echo "$ISSUE: could not move to Todo, skipped" >&2; continue; }

    PROMPT="Ты воркер по CLAUDE.md → «Роль „воркер“». Твоя задача — $ISSUE: пункт $NUM в $TASKS. \
Начни с \`orca linear issue --current --full --json\` и переведи задачу в In Progress. Прочитай proposal.md, design.md, tasks.md \
и все spec-дельты в каталоге change ($CHANGE), затем сделай ровно свой пункт — его источники, его проверка, ничего сверх: \
чужие пункты не трогать, соседнее «раз уж тут» не чинить. Код — правда о текущем поведении, дельта спеки — о целевом; \
если пункт меняет поведение, после починки код делает то, что написано в дельте. Тест, который падал бы до починки, — \
в хост-наборе. Расхождение вне scope — отдельный issue через \`orca linear create --parent-current\`, не чинить. \
Проверка: то, что написано в пункте, плюс \`openspec validate $(basename "$CHANGE") --strict\` и \`CONFORMANCE=required tools/test-all.sh\` \
(venv для мока — по подсказке скрипта). Отметь свой пункт [x], закоммить, PR с заголовком \`$ISSUE: ...\`, строкой \
\`Closes $ISSUE\` (и \`Closes\` исходных issues, если пункт их называет), в описании — что и почему, как проверено. \
Заверши по skill orca-linear: attach PR, один итоговый комментарий, статус In Review."

    OUT="$(orca worktree create --repo "id:$REPO_ID" --name "$NAME" --no-parent --linear-issue "$ISSUE" \
        --agent claude --comment "$ISSUE: спека ${CAP:-$NUM}" --prompt "$PROMPT" --json 2>&1 || true)"
    OUT="$OUT" ISSUE="$ISSUE" NAME="$NAME" python3 - <<'PY'
import json, os, sys
issue, name = os.environ["ISSUE"], os.environ["NAME"]
try:
    d = json.loads(os.environ["OUT"])
except json.JSONDecodeError:
    d = {"ok": False, "error": os.environ["OUT"][:300]}
if not d.get("ok"):
    print(issue + ": worktree create failed: " + json.dumps(d.get("error"), ensure_ascii=False), file=sys.stderr)
    sys.exit(0)
r = d["result"]
wt = r.get("worktree", {})
h = r.get("agentTerminalHandle") or (r.get("startupTerminal") or {}).get("handle")
print("\t".join([issue, name, str(wt.get("path") or wt.get("worktreePath")), str(h)]))
PY
done
