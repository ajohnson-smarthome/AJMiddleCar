"""Host tests for the release scripts' publish step. Stdlib only.

`gh` is the one thing here that cannot run for real — it would publish to GitHub — so a
fake `gh` sits first on PATH. It keeps a tiny release table, logs every call, and refuses
to upload a file that is not there, the way the real one does. Everything else is the
real script: the order it drives `gh` in, and what it leaves behind when a step fails.
"""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
RELEASE = ROOT / "tools" / "release.sh"
PUBLISH = ROOT / "tools" / "release-publish.sh"

TAG, TARGET, TITLE, NOTES = "v9.9+999", "0" * 40, "v9.9 (build 999)", "Release v9.9+999"
CAR, DONGLE = "ajmiddlecar.bin", "ajdongle.bin"

# The fake `gh`. State is one JSON file: {tag: {"draft": bool, "target": sha, "assets": [...]}}.
# FAKE_GH_LOSE_ASSET names a file the "server" silently drops on upload — the failure the
# name check after upload exists to catch.
FAKE_GH = r'''#!/usr/bin/env python3
import json, os, sys
log, state_path = os.environ["FAKE_GH_LOG"], os.environ["FAKE_GH_STATE"]
with open(log, "a") as f:
    f.write(json.dumps(sys.argv[1:]) + "\n")
state = json.load(open(state_path)) if os.path.exists(state_path) else {}
args = sys.argv[1:]
if args[:2] == ["release", "create"]:
    tag = args[2]
    if tag in state:
        sys.exit("release %s already exists" % tag)
    # Anything that is neither a flag nor a flag's value is a file to upload on create —
    # the old order this test exists to keep out.
    files, skip = [], False
    for a in args[3:]:
        if skip:
            skip = False
        elif a in ("--target", "--title", "--notes"):
            skip = True
        elif not a.startswith("--"):
            files.append(a)
    state[tag] = {"draft": "--draft" in args, "target": args[args.index("--target") + 1],
                  "assets": [os.path.basename(f) for f in files]}
elif args[:2] == ["release", "upload"]:
    tag = args[2]
    if tag not in state:
        sys.exit("release not found")
    for f in args[3:]:
        if not os.path.isfile(f):
            sys.exit("open %s: no such file or directory" % f)
        if os.path.basename(f) != os.environ.get("FAKE_GH_LOSE_ASSET"):
            state[tag]["assets"].append(os.path.basename(f))
elif args[:2] == ["release", "view"]:
    tag = args[2]
    if tag not in state:
        sys.exit("release not found")
    if args[3:] != ["--json", "assets", "--jq", ".assets[].name"]:
        sys.exit("fake gh: unsupported view flags %r" % args[3:])
    for name in state[tag]["assets"]:
        print(name)
elif args[:2] == ["release", "edit"]:
    tag = args[2]
    if tag not in state:
        sys.exit("release not found")
    if "--draft=false" in args:
        state[tag]["draft"] = False
else:
    sys.exit("fake gh: unexpected %r" % args)
json.dump(state, open(state_path, "w"))
'''


class FakeGh:
    def __init__(self, tmp):
        self.tmp = pathlib.Path(tmp)
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        gh = bin_dir / "gh"
        gh.write_text(FAKE_GH)
        gh.chmod(0o755)
        self.log = self.tmp / "gh.log"
        self.state = self.tmp / "gh.json"
        self.env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}",
                        FAKE_GH_LOG=str(self.log), FAKE_GH_STATE=str(self.state))

    def calls(self):
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def releases(self):
        return json.loads(self.state.read_text()) if self.state.exists() else {}


def run(argv, env, cwd=ROOT):
    return subprocess.run(argv, env=env, cwd=cwd, capture_output=True, text=True)


class TestDryRun(unittest.TestCase):
    def test_prints_draft_upload_check_publish_in_that_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            gh = FakeGh(tmp)
            r = run([str(RELEASE), "--dry-run"], gh.env)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = r.stdout
            for step in ("--draft", "release upload", "--draft=false"):
                self.assertIn(step, out)
            self.assertLess(out.index("--draft"), out.index("release upload"))
            self.assertLess(out.index("release upload"), out.index("--draft=false"))
            # The files are no longer arguments of `create` — that was the half-release.
            self.assertNotRegex(out, r"release create '[^']*' 'firmware/")
            # A rehearsal talks to nobody.
            self.assertEqual(gh.calls(), [])

    def test_release_sh_publishes_only_through_the_helper(self):
        # release.sh itself never calls gh: every `gh release` it mentions is a
        # `[dry-run]` line. The one real publish path is the helper the tests below drive.
        lines = RELEASE.read_text().splitlines()
        for line in lines:
            if "gh release" in line and not line.lstrip().startswith("#"):
                self.assertIn("[dry-run]", line, line)
        self.assertTrue(any(line.startswith('tools/release-publish.sh "$VER"') for line in lines))


class TestPublish(unittest.TestCase):
    def publish(self, gh, files):
        return run([str(PUBLISH), TAG, TARGET, TITLE, NOTES, *files], gh.env)

    def images(self, tmp, *names):
        paths = []
        for name in names:
            p = pathlib.Path(tmp) / name
            p.write_bytes(b"\xe9" + bytes(63))
            paths.append(str(p))
        return paths

    def test_draft_then_upload_then_names_then_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            gh = FakeGh(tmp)
            r = self.publish(gh, self.images(tmp, CAR, DONGLE))
            self.assertEqual(r.returncode, 0, r.stderr)
            verbs = [c[:2] for c in gh.calls()]
            self.assertEqual(verbs, [["release", "create"], ["release", "upload"],
                                     ["release", "view"], ["release", "edit"]])
            create = gh.calls()[0]
            self.assertIn("--draft", create)
            self.assertEqual(create[create.index("--target") + 1], TARGET)
            self.assertEqual(create[create.index("--title") + 1], TITLE)
            self.assertEqual(create[create.index("--notes") + 1], NOTES)
            self.assertIn("--draft=false", gh.calls()[3])
            rel = gh.releases()[TAG]
            self.assertFalse(rel["draft"])
            self.assertEqual(sorted(rel["assets"]), sorted([CAR, DONGLE]))

    def test_second_file_unavailable_leaves_a_draft(self):
        with tempfile.TemporaryDirectory() as tmp:
            gh = FakeGh(tmp)
            (car,) = self.images(tmp, CAR)
            r = self.publish(gh, [car, str(pathlib.Path(tmp) / DONGLE)])
            self.assertNotEqual(r.returncode, 0)
            self.assertNotIn(["release", "edit"], [c[:2] for c in gh.calls()])
            rel = gh.releases()[TAG]
            self.assertTrue(rel["draft"], "the half-release must stay invisible to /releases/latest")
            self.assertIn("draft", r.stderr.lower())
            self.assertIn(TAG, r.stderr)

    def test_asset_missing_after_upload_is_not_published(self):
        with tempfile.TemporaryDirectory() as tmp:
            gh = FakeGh(tmp)
            gh.env["FAKE_GH_LOSE_ASSET"] = DONGLE
            r = self.publish(gh, self.images(tmp, CAR, DONGLE))
            self.assertNotEqual(r.returncode, 0)
            self.assertNotIn(["release", "edit"], [c[:2] for c in gh.calls()])
            self.assertTrue(gh.releases()[TAG]["draft"])
            self.assertIn(DONGLE, r.stderr)

    def test_nothing_is_created_without_files_to_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            gh = FakeGh(tmp)
            r = run([str(PUBLISH), TAG, TARGET, TITLE, NOTES], gh.env)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(gh.calls(), [])


if __name__ == "__main__":
    unittest.main()
