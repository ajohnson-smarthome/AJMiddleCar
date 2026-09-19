#!/usr/bin/env python3
"""Создаёт issues в Linear из tasks.md OpenSpec-change и дописывает (KOT-N) к пунктам.

Шаг 3 роли «PM-сессия» из CLAUDE.md. Идемпотентен: пункты, у которых id уже
проставлен, пропускаются, поэтому прерванный запуск можно повторить.

Пункты с пометкой «(стенд)» — работа с платой на столе — и «(PM)» — шаги самой
PM-сессии (триаж, сведение) — в Linear не уходят: их не делает воркер в worktree
(см. openspec/config.yaml и CLAUDE.md).

Использование:
  tools/pm/tasks-to-linear.py <change-dir> (--project <id> | --parent AJM-N)
                              [--workspace <id>] [--team AJM] [--dry-run] [--limit N]

  <change-dir>  каталог change, например openspec/changes/ajm-5-lidar
  --project     issues уходят в Project (большой change)
  --parent      issues становятся sub-issues PM-задачи (маленький change)
  --workspace   id воркспейса из `orca linear team list --json`; нужен, если их несколько

Ходит в Linear через `orca linear create`, поэтому нужен запущенный Orca.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HEADING = re.compile(r"^## (\d+)\. (.+)$")
TASK = re.compile(r"^- \[( |x|X)\] (\d+\.\d+) (.+)$")
LINKED = re.compile(r"\([A-Z]+-\d+\)\s*$")
BENCH = re.compile(r"\((стенд|PM)\)")
TITLE_MAX = 110


def parse_args():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("change_dir")
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument("--project")
    target.add_argument("--parent")
    p.add_argument("--workspace")
    p.add_argument("--team", default="AJM")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--limit", type=int, default=None)
    return p.parse_args()


def make_title(num: str, text: str) -> str:
    # заголовок — номер пункта и текст до фразы проверки; полный текст уходит в body
    head = re.split(r";\s*проверить", text, maxsplit=1)[0].strip().rstrip(";")
    title = f"{num} {head}"
    return title if len(title) <= TITLE_MAX else title[: TITLE_MAX - 3].rstrip() + "…"


def make_body(change_rel: str, group: str, num: str, text: str) -> str:
    return (
        f"OpenSpec change: `{change_rel}/`\n"
        f"Группа: {group}\n"
        f"Пункт {num} из `tasks.md`:\n\n"
        f"{text}\n\n"
        "Перед началом прочитать `proposal.md`, `design.md`, `specs/` и `tasks.md` в каталоге change. "
        "Делать только этот пункт; по завершении отметить его `[x]` в `tasks.md`."
    )


def create_issue(args, title: str, body: str) -> str:
    cmd = ["orca", "linear", "create", "--team", args.team, "--state", "Backlog",
           "--title", title, "--body-file", "-", "--json"]
    if args.project:
        cmd += ["--project", args.project]
    else:
        cmd += ["--parent", args.parent]
    if args.workspace:
        cmd += ["--workspace", args.workspace]
    res = subprocess.run(cmd, input=body, capture_output=True, text=True)
    try:
        out = json.loads(res.stdout)
    except json.JSONDecodeError:
        sys.exit(f"orca linear create: не JSON\n{res.stdout}\n{res.stderr}")
    if not out.get("ok"):
        sys.exit(f"orca linear create: {json.dumps(out.get('error'), ensure_ascii=False)}")
    issue = out["result"].get("issue", out["result"])
    return issue["identifier"]


def main():
    args = parse_args()
    change_dir = Path(args.change_dir)
    tasks_path = change_dir / "tasks.md"
    if not tasks_path.exists():
        sys.exit(f"нет файла {tasks_path}")
    change_rel = change_dir.as_posix().rstrip("/")

    lines = tasks_path.read_text(encoding="utf-8").split("\n")
    group = "(без группы)"
    created = 0
    for idx, line in enumerate(lines):
        if m := HEADING.match(line):
            group = f"{m.group(1)}. {m.group(2)}"
            continue
        m = TASK.match(line)
        if not m or LINKED.search(m.group(3)) or BENCH.search(m.group(3)):
            continue
        if args.limit is not None and created >= args.limit:
            break
        mark, num, text = m.groups()
        title = make_title(num, text)
        if args.dry_run:
            print(f"[dry] {title}")
            created += 1
            continue
        ident = create_issue(args, title, make_body(change_rel, group, num, text))
        lines[idx] = f"- [{mark}] {num} {text} ({ident})"
        # пишем после каждого создания, чтобы прерванный запуск не потерял связи
        tasks_path.write_text("\n".join(lines), encoding="utf-8")
        print(f"{ident}  {title}")
        created += 1

    verb = "would be created" if args.dry_run else "created"
    print(f"done: {created} issue(s) {verb}")


if __name__ == "__main__":
    main()
