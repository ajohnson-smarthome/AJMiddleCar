# OTA Fixes — Core (release tooling, firmware, mock, docs) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the non-app findings of the 2026-08-23 OTA-chain audit: a release script that
cannot mis-tag, mis-configure, or silently ship a radio-pin bump; a firmware whose rollback is
visible and whose validation window is small; a mock that reports the uploaded image's real
version; and documents that say what ships.

**Architecture:** release.sh stays one self-contained bash script, hardened in place. Firmware
changes ride the existing seams: `/status` gains two top-level keys (`rollback`, `nvs_wiped`)
that — like `radio` — live OUTSIDE the generated contract; the radio expectation string moves
from a hand-copied `board.h` define to a compile-time derivation from the esp_hosted
component's own version macros. The mock parses `esp_app_desc_t` out of uploaded bytes with
stdlib struct math and mirrors the two new `/status` keys, settable for rehearsal.

**Tech Stack:** bash, C (ESP-IDF 6.0.2), Python 3 stdlib (mock/tests), Markdown.

**Spec:** `docs/superpowers/specs/2026-08-23-ota-fix-decisions.md`

## Global Constraints

- Work ONLY in the worktree `/Users/adamjohnson/VSCode/esp32-p4-car/.claude/worktrees/ota-fixes`
  (branch `ota-fixes`). All paths relative to it.
- `rollback` and `nvs_wiped` deliberately stay OUTSIDE `contract/car-api.json` — they are
  `/status`-only diagnostics like `radio`. Do not touch the schema, the generator, or any
  generated file; `bash tools/check_contract.sh` must stay green, and `docs/protocol.md`'s
  generated endpoints block stays byte-identical.
- `test_state.py`/`test_rtlink.py`/`conformance*.py` stay stdlib-only.
- After every task: `./tools/test-all.sh` green, then commit. Commit style: lowercase
  `fix(...)`/`feat(...)` subject, body says why, trailer:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- Never publish anything: no `gh release create` without `--dry-run` semantics, no tags, no
  pushes. Task 1's verification exercises only offline/dry-run/negative branches.
- Firmware handler/boot changes (Tasks 3–6) are not host-testable; their gate is the suite
  staying green plus Task 9's `idf.py build`.

---

### Task 1: release.sh — push check, sdkconfig purge, argv parsing, radio-pin gate, test gate, version.txt validation

**Files:**
- Modify: `tools/release.sh` (full rewrite)

**Interfaces:**
- Consumes: `firmware/p4/main/idf_component.yml` (the esp_hosted pin), `gh`, `git ls-remote`.
- Produces: the release entrypoint; Task 8's README text references its `--radio-bumped` flag.

- [ ] **Step 1: Replace `tools/release.sh` with:**

```bash
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
        *)              NOTES_ARG="$arg" ;;
    esac
done

# version.txt must be exactly one line: CMake reads only the first, this script strips
# whitespace — a second line would let the tag and the embedded version disagree.
if [ "$(grep -c '' firmware/p4/version.txt)" != 1 ]; then
    echo "ERROR: firmware/p4/version.txt must be exactly one line"; exit 1
fi
SEMVER=$(tr -d '[:space:]' < firmware/p4/version.txt)
BUILD_NUM=$(git rev-list --count HEAD)
VER="v${SEMVER}+${BUILD_NUM}"
TITLE="v${SEMVER} (build ${BUILD_NUM})"
BIN="firmware/p4/build/ajmiddlecar.bin"
NOTES="${NOTES_ARG:-Release ${VER}}"

# The radio half rides OUTSIDE this channel: /ota updates only the P4, so a release whose
# firmware pins a newer esp_hosted strands every OTA'd car on a bench reflash of the C6.
# Detect the pin moving since the previous release and refuse to ship it silently.
radio_pin() { grep -E 'espressif/esp_hosted:' "$1" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1; }
PIN=$(radio_pin firmware/p4/main/idf_component.yml)
PREV_TAG=$(git tag --list 'v*' --sort=-creatordate | head -1 || true)
PIN_MOVED=0
if [ -n "$PREV_TAG" ]; then
    PREV_PIN=$(git show "${PREV_TAG}:firmware/p4/main/idf_component.yml" 2>/dev/null | \
               { grep -E 'espressif/esp_hosted:' || true; } | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
    if [ -n "$PREV_PIN" ] && [ "$PREV_PIN" != "$PIN" ]; then PIN_MOVED=1; fi
fi
if [ "$PIN_MOVED" = 1 ]; then
    NOTES="${NOTES}"$'\n\n'"⚠️ Этот релиз меняет радио-пин (esp_hosted ${PREV_PIN} → ${PIN}): после OTA потребуется стендовая перепрошивка радио C6 (firmware/c6/README.md)."
fi

if [ "$DRY_RUN" = 1 ]; then
    echo "[dry-run] version : $VER"
    echo "[dry-run] tag     : $VER  (target: $(git rev-parse HEAD))"
    echo "[dry-run] title   : $TITLE"
    echo "[dry-run] asset   : $BIN"
    echo "[dry-run] radio   : esp_hosted $PIN$( [ "$PIN_MOVED" = 1 ] && echo "  ← MOVED since $PREV_TAG (needs --radio-bumped)" )"
    echo "[dry-run] notes   : $NOTES"
    echo "[dry-run] would run: test-all && rm sdkconfig && idf.py fullclean && idf.py build && gh release create '$VER' '$BIN' --target <HEAD> ..."
    exit 0
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
REMOTE_HEAD=$(git ls-remote origin refs/heads/main | cut -f1)
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

source tools/env-p4.sh >/dev/null 2>&1
# A stray bench sdkconfig must not configure a release: regenerate purely from defaults.
rm -f firmware/p4/sdkconfig firmware/p4/sdkconfig.old
(cd firmware/p4 && idf.py fullclean >/dev/null && idf.py build)
[ -f "$BIN" ] || { echo "ERROR: $BIN not built"; exit 1; }

gh release create "$VER" "$BIN" --target "$LOCAL_HEAD" --title "$TITLE" --notes "$NOTES"
echo "Released $VER"
```

- [ ] **Step 2: Verify the branches that can run without publishing**

Run each; every command must behave as stated (we are on branch `ota-fixes`, which is exactly
what makes the negative tests safe — the real path can never proceed past the branch check):

```bash
tools/release.sh --dry-run                     # prints the block, exit 0 — even on a dirty tree
tools/release.sh "notes text" --dry-run        # SAME: flag parsed anywhere, notes preserved
tools/release.sh --dry-run "notes text" | grep 'notes   : notes text'
tools/release.sh --no-such-flag; echo "exit=$?" # ERROR: unknown flag, exit 1
printf '1.0\nextra\n' > /tmp/v2.txt && cp firmware/p4/version.txt /tmp/v1.txt \
  && cp /tmp/v2.txt firmware/p4/version.txt \
  && { tools/release.sh --dry-run; echo "exit=$?"; } ; cp /tmp/v1.txt firmware/p4/version.txt
                                               # ERROR: version.txt must be exactly one line, exit 1; then restored
tools/release.sh; echo "exit=$?"               # reaches the branch check -> ERROR: not on main, exit 1
```

Also verify the pin extraction expression against the real file:
`grep -E 'espressif/esp_hosted:' firmware/p4/main/idf_component.yml | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1`
must print `3.0.6`.

- [ ] **Step 3: Full suite and commit**

```bash
./tools/test-all.sh
git add tools/release.sh
git commit -m "fix(release): the tag names the built commit, and nothing ships silently

Push check + --target (gh tags the remote head otherwise), sdkconfig purged
before the release build, --dry-run parsed anywhere and runnable on a dirty
tree, the esp_hosted pin diffed against the previous tag with an explicit
--radio-bumped acknowledgement, tests gate the build, version.txt validated
single-line. The history-rewrite hazard is recorded as deliberately unguarded.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: CMake — version.txt becomes a configure dependency

**Files:**
- Modify: `firmware/p4/CMakeLists.txt`

**Interfaces:** none new. Bench honesty only; the release path already fullcleans.

- [ ] **Step 1: Implement**

In `firmware/p4/CMakeLists.txt`, directly after the `file(STRINGS ...)` line, add:

```cmake
# Re-run configure when version.txt changes. The commit COUNT still refreshes only on
# configure (a git state cannot be a configure dependency), so an incremental bench build
# after new commits reports the count of its last configure — the release path fullcleans,
# which is what makes release binaries exact. Recorded by the 2026-08-23 audit.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/version.txt")
```

- [ ] **Step 2: Suite and commit**

```bash
./tools/test-all.sh
git add firmware/p4/CMakeLists.txt
git commit -m "fix(build): version.txt is a configure dependency

A semver bump without a reconfigure used to leave the old version in the
binary; the count half of the staleness stays (documented) — releases
fullclean, which is what makes them exact.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: /status gains rollback and nvs_wiped

**Files:**
- Modify: `firmware/p4/main/status_api.c`, `firmware/p4/main/status_api.h`, `firmware/p4/main/main.c` (erase path only)

**Interfaces:**
- Consumes: `esp_ota_get_next_update_partition`, `esp_ota_get_state_partition`.
- Produces: `/status` keys `"rollback"` and `"nvs_wiped"` (top-level, booleans, OUTSIDE the
  generated contract, like `radio`); `void status_api_note_nvs_wiped(void)` for main.c.
  Task 7 mirrors the keys in the mock; Task 8 documents them; the app plan consumes them.

- [ ] **Step 1: Implement**

**(a)** `status_api.h` — add beside the existing declaration:

```c
/* main.c calls this when the NVS format-migration path erased the store, BEFORE
 * status_api_start registers the handler — ordering, not a lock, like the radio fields. */
void status_api_note_nvs_wiped(void);
```

**(b)** `status_api.c` — add `#include "esp_ota_ops.h"`; below `s_radio_ok` add:

```c
/* Did the bootloader revert the previous OTA? The other slot is left ESP_OTA_IMG_ABORTED
 * exactly when an update failed its first boot — the one signal a client has that the
 * image it flashed did not survive. Read once at start: the answer cannot change without
 * a reboot. */
static bool s_rollback  = false;
static bool s_nvs_wiped = false;

void status_api_note_nvs_wiped(void) { s_nvs_wiped = true; }

static void read_rollback_state(void) {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    s_rollback = other != NULL &&
                 esp_ota_get_state_partition(other, &st) == ESP_OK &&
                 st == ESP_OTA_IMG_ABORTED;
    if (s_rollback) ESP_LOGW(TAG, "the previous OTA was rolled back by the bootloader");
}
```

**(c)** In `status_get`, enlarge the buffer to `char buf[480];` and extend the snprintf —
after the `%s,` that carries `fields` and before the `radio` object, insert the two keys:

```c
    int n = snprintf(buf, sizeof(buf),
                     "{\"" RT_KEY_DEVICE "\":\"" CAR_DEVICE_ID "\",\"" RT_KEY_FW "\":\"%s\","
                     "\"" RT_KEY_PROTO "\":%d,%s,"
                     "\"rollback\":%s,\"nvs_wiped\":%s,"
                     "\"radio\":{\"" RT_KEY_FW "\":\"%s\",\"expected\":\"" BOARD_RADIO_SLAVE_FW "\",\"ok\":%s}}",
                     fw, RT_PROTO, fields,
                     s_rollback ? "true" : "false", s_nvs_wiped ? "true" : "false",
                     s_radio_fw, s_radio_ok ? "true" : "false");
```

(Task 5 replaces `BOARD_RADIO_SLAVE_FW` in this same line — keep it for now so the tasks
stay independently committable.)

**(d)** In `status_api_start`, call `read_rollback_state();` right after `read_radio_version();`.

**(e)** `main.c` — the NVS branch becomes:

```c
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The migration erases the bench-earned calibration and every config domain.
           There is no snapshot/restore (deferred — 2026-08-23 audit); the flag is how
           the app tells the user the wizard must be re-run, instead of silence. */
        ESP_LOGW(TAG, "NVS format changed — erasing (calibration and config are lost)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
        status_api_note_nvs_wiped();
    }
```

- [ ] **Step 2: Suite and commit**

```bash
./tools/test-all.sh
git add firmware/p4/main/status_api.c firmware/p4/main/status_api.h firmware/p4/main/main.c
git commit -m "feat(fw): /status says when an OTA rolled back and when NVS was wiped

The bootloader reverting an update was invisible — the app's success detector
counted the reboot bounce as success and the forced gate looped forever. The
other slot's ESP_OTA_IMG_ABORTED state is the signal; nvs_wiped is the same
honesty for the format-migration erase that silently costs the calibration.
Both live outside the generated contract, like radio.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: mark-valid moves to right after the AP is up

**Files:**
- Modify: `firmware/p4/main/main.c`

**Interfaces:** consumes nothing new; the boot-order change Task 3's rollback key reports on.

- [ ] **Step 1: Implement**

In `app_main`, DELETE the whole `// OTA rollback: mark this freshly-flashed image valid...`
block (currently after `cfg_api_start()`), and insert immediately after
`ESP_ERROR_CHECK(wifi_ap_start(CAR_AP_SSID, CAR_AP_PASS));`:

```c
    /* OTA rollback: the property rollback protects is "the car is reachable" — the AP is
       up, so mark the image valid NOW, before the API registrations and before
       status_api_start's radio-version RPC (up to 5 s against a mismatched slave). One
       reset inside that window used to silently revert a good update. The tradeoff is
       deliberate: an image that fails beyond this line boot-loops WITHOUT rollback, so
       everything below must tolerate failure — a panic after this point is its own bug. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
```

- [ ] **Step 2: Suite and commit**

```bash
./tools/test-all.sh
git add firmware/p4/main/main.c
git commit -m "fix(fw): mark-valid runs the moment the AP is up, not after the whole bring-up

The window between reboot and validation spanned every API registration plus
a possible 5 s radio RPC; a power blip or an impatient power-cycle inside it
reverted a good image with the app reporting success. Reachability is the
property rollback protects — validate when it is proven.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: the radio expectation is derived, not hand-copied — and FEAT_OTA is pinned

**Files:**
- Modify: `firmware/p4/main/status_api.c`, `firmware/p4/main/board.h`, `firmware/p4/sdkconfig.defaults`

**Interfaces:**
- Consumes: esp_hosted's version macros (`eh_common_fw_version.h`, fetched with the component).
- Produces: `RADIO_EXPECTED_FW` (status_api.c-local); `BOARD_RADIO_SLAVE_FW` ceases to exist.
  Task 8's docs reference the derivation.

- [ ] **Step 1: Fetch the component and verify the macros**

`managed_components/` is absent in this fresh worktree. Fetch it:

```bash
bash -c 'source tools/env-p4.sh && cd firmware/p4 && idf.py reconfigure' 2>&1 | tail -3
ls firmware/p4/managed_components/espressif__esp_hosted/common/eh_common/include/eh_common_fw_version.h
grep -n 'PROJECT_VERSION_' firmware/p4/managed_components/espressif__esp_hosted/common/eh_common/include/eh_common_fw_version.h
```

Expected: the header exists and defines `PROJECT_VERSION_MAJOR_1 3`, `PROJECT_VERSION_MINOR_1 0`,
`PROJECT_VERSION_PATCH_1 6` (numeric). **If the names or the path differ, adapt the code below
to what is actually there and record the difference in your report.** Also verify the header is
reachable from `main`'s include path (it is expected to be exported by the component; if the
trial build in Step 3 says otherwise, the fallback is `#include` via the component's
already-exported umbrella that carries the macros — find it with
`grep -rl PROJECT_VERSION_MAJOR_1 firmware/p4/managed_components/espressif__esp_hosted/*/include/` —
and record which header you used).

- [ ] **Step 2: Implement**

**(a)** `status_api.c` — add the include and the derivation next to the other statics:

```c
#include "eh_common_fw_version.h"   /* esp_hosted's own version macros — the single source */

/* The expected slave version is the HOST library's version: esp_hosted requires the pair
 * matched, and deriving the string from the component's own macros makes drift between
 * idf_component.yml and this check impossible — the hand-copied board.h pin the audit
 * caught could lie in both directions. */
#define RADIO_STR2(x) #x
#define RADIO_STR(x)  RADIO_STR2(x)
#define RADIO_EXPECTED_FW \
    RADIO_STR(PROJECT_VERSION_MAJOR_1) "." RADIO_STR(PROJECT_VERSION_MINOR_1) "." RADIO_STR(PROJECT_VERSION_PATCH_1)
```

Replace every `BOARD_RADIO_SLAVE_FW` in the file with `RADIO_EXPECTED_FW` (three sites: the
strcmp in `read_radio_version`, the mismatch log line, and the `"expected"` field in
`status_get`'s snprintf).

**(b)** `firmware/p4/main/board.h` — replace the `BOARD_RADIO_SLAVE_FW` define and its comment
block with:

```c
// The C6 runs esp_hosted's slave image, delivered out of band (firmware/c6/README.md) —
// over SDIO from the host is the recorded route, the UART header the fallback. The
// EXPECTED slave version is no longer pinned here by hand: status_api derives it at
// compile time from the host component's own version macros (eh_common_fw_version.h),
// so the expectation cannot drift from firmware/p4/main/idf_component.yml.
```

**(c)** Confirm nothing else references the define: `grep -rn BOARD_RADIO_SLAVE_FW firmware/ app/ tools/ docs/ CLAUDE.md`
— expected: only docs hits, which Task 8 rewrites (list them in your report).

**(d)** `firmware/p4/sdkconfig.defaults` — append after the SDIO block:

```
# Host-side OTA of the C6 co-processor (esp_hosted_cp_ota_*). Holds today only via the
# component's promptless Kconfig default; pinned here so a vendor default change cannot
# silently remove the only wireless path to the radio (2026-08-23 audit).
CONFIG_ESP_HOSTED_HOST_FEAT_OTA=y
```

- [ ] **Step 3: Trial-compile the touched translation unit early**

Cheaper than waiting for Task 9: `bash -c 'source tools/env-p4.sh && cd firmware/p4 && idf.py build' 2>&1 | tail -5`
Expected: `Project build complete.` (this also regenerates sdkconfig with the new pin). If the
include fails, apply Step 1's fallback and record it.

- [ ] **Step 4: Suite and commit**

```bash
./tools/test-all.sh
git add firmware/p4/main/status_api.c firmware/p4/main/board.h firmware/p4/sdkconfig.defaults
git commit -m "fix(fw): the radio expectation derives from the component, and FEAT_OTA is pinned

BOARD_RADIO_SLAVE_FW was a hand-copied string tied to nothing: bumped one way
it flagged a matched pair, bumped the other it blessed a real host/slave
mismatch. The component's own version macros are the single source now. The
host-side C6 OTA feature stops depending on a promptless vendor default.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: ota_api — tell esp_ota_begin the real size, and cap it at the slot

**Files:**
- Modify: `firmware/p4/main/ota_api.c`

**Interfaces:** none new.

- [ ] **Step 1: Implement**

**(a)** After the `part == NULL` check, add the slot ceiling:

```c
    if ((uint32_t)req->content_len > part->size) {
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "400 Bad Request", "", "image too large");
    }
```

(The existing `INT_MAX` guard above stays — it protects the `(int)` cast before `part` exists.)

**(b)** Pass the known length instead of erasing the whole slot:

```c
    if (esp_ota_begin(part, req->content_len, &handle) != ESP_OK) {
```

with the comment above it:

```c
    /* The exact length is known from Content-Length: erasing only what the image needs
       instead of OTA_SIZE_UNKNOWN's full 4 MB saves seconds of erase (and flash wear)
       per update — and a too-large image now fails here instead of after the erase. */
```

- [ ] **Step 2: Suite and commit**

```bash
./tools/test-all.sh
git add firmware/p4/main/ota_api.c
git commit -m "fix(fw): /ota erases what the image needs, not the whole slot

OTA_SIZE_UNKNOWN erased all 4 MB before any validation — seconds of wear for
a 763 KB image, or for junk the first write refuses. Content-Length is known;
use it, and refuse an image larger than the slot outright.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: the mock reports the uploaded image's real version, mirrors the new keys, and rehearses rollback

**Files:**
- Modify: `tools/mock_car/state.py` (parse_image_version, end_ota, rollback/nvs_wiped attrs)
- Modify: `tools/mock_car/mock_car.py` (ota handler, status handler, --rollback flag)
- Modify: `tools/conformance.py` (status(): the two new booleans)
- Test: `tools/mock_car/test_state.py`

**Interfaces:**
- Consumes: Task 3's `/status` shape (the mock must agree with the firmware key-for-key).
- Produces: `parse_image_version(data) -> str|None`; `CarState.end_ota(flashed=True, version=None)`
  (backward-compatible — every existing caller keeps working); `CarState.rollback`,
  `CarState.nvs_wiped` (plain settable bools, default False); `mock_car.py --rollback`.

- [ ] **Step 1: Write the failing tests** — append to `tools/mock_car/test_state.py` (a new
  class after `TestTelemetry`; extend the `from state import` line with `parse_image_version`):

```python
def synthetic_image(version=b"v9.9+123", magic=0xABCD5432, first=0xE9, size=4096):
    """The least image parse_image_version accepts: 0xE9 header, esp_app_desc_t at 32
    (image header 24 + segment header 8), magic word first, version[32] at its offset 16."""
    img = bytearray(size)
    img[0] = first
    img[32:36] = magic.to_bytes(4, "little")
    img[48:48 + len(version)] = version
    return bytes(img)


class TestImageVersion(unittest.TestCase):
    def test_a_real_layout_yields_its_version(self):
        self.assertEqual(parse_image_version(synthetic_image()), "v9.9+123")

    def test_garbage_yields_none(self):
        """A 0xE9 blob without the app-desc magic is not an app image — the fallback
        (a synthetic bump) keeps old rehearsal blobs working."""
        self.assertIsNone(parse_image_version(b"\xe9" + b"\x00" * 8191))
        self.assertIsNone(parse_image_version(b"\x00" * 8192))
        self.assertIsNone(parse_image_version(b"\xe9short"))

    def test_end_ota_prefers_the_parsed_version(self):
        car = CarState(now=0.0)
        car.begin_ota(0.0)
        car.end_ota(version="v2.0+700")
        self.assertEqual(car.fw, "v2.0+700")

    def test_end_ota_without_a_version_still_bumps(self):
        car = CarState(now=0.0)
        old = car.fw
        car.begin_ota(0.0)
        car.end_ota()
        self.assertNotEqual(car.fw, old)

    def test_rollback_and_nvs_wiped_default_false(self):
        car = CarState(now=0.0)
        self.assertFalse(car.rollback)
        self.assertFalse(car.nvs_wiped)
```

- [ ] **Step 2: Run to verify they fail**

Run: `python3 tools/mock_car/test_state.py TestImageVersion -v 2>&1 | tail -6`
Expected: ImportError (`parse_image_version` does not exist).

- [ ] **Step 3: Implement**

**(a)** `tools/mock_car/state.py` — next to `parse_frame`:

```python
_APP_DESC_OFFSET = 32          # esp_image_header_t (24 B) + esp_image_segment_header_t (8 B)
_APP_DESC_MAGIC = 0xABCD5432   # esp_app_desc.h ESP_APP_DESC_MAGIC_WORD


def parse_image_version(data):
    """The version a real ESP application image embeds, or None.

    esp_app_desc_t sits at the start of the first segment: image header (24 bytes,
    first byte 0xE9), one segment header (8), then the descriptor — magic_word first,
    version[32] at its offset 16, i.e. absolute offset 48. This is what the car will
    report after flashing these bytes, so the simulator must report the same — a
    synthetic bump hid every asset-vs-tag mismatch from rehearsal (2026-08-23 audit).
    """
    if len(data) < _APP_DESC_OFFSET + 48 or data[0] != 0xE9:
        return None
    magic = int.from_bytes(data[_APP_DESC_OFFSET:_APP_DESC_OFFSET + 4], "little")
    if magic != _APP_DESC_MAGIC:
        return None
    raw = data[_APP_DESC_OFFSET + 16:_APP_DESC_OFFSET + 48]
    ver = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
    return ver or None
```

In `CarState.__init__`, beside `self.fw = fw`:

```python
        self.rollback = False    # the previous "OTA" was rolled back — /status mirrors it
        self.nvs_wiped = False   # one-boot flag after a simulated NVS migration erase
```

`end_ota` becomes:

```python
    def end_ota(self, flashed=True, version=None):
        if flashed:
            self.fw = version or _bump_build(self.fw)
        self._release(CTL_OTA)
```

**(b)** `tools/mock_car/mock_car.py` — in the `ota` handler, after the magic check passes,
capture the previous fw and parse the real one; the tail becomes:

```python
    prev_fw = car.fw
    print(f"ota: {len(data)} bytes — motors stopped, flashing")
    await asyncio.sleep(OTA_SECONDS)
    car.end_ota(version=parse_image_version(data))
    if request.app["rollback_mode"]:
        # Rehearsal: the flashed image "fails its first boot" — the car comes back on
        # the previous firmware with the rollback flag up, exactly what the app's
        # detector must learn to call a FAILURE (decision 5).
        car.fw = prev_fw
        car.rollback = True
        print(f"ota: 'rolled back' — reporting {car.fw}, rollback:true")
    else:
        print(f"ota: done, now running {car.fw} — 'rebooting'")
    link.simulate_reboot(asyncio.get_running_loop().time())
    return web.json_response({"ok": True})
```

Import it (`from state import CarState, parse_image_version`). In `status()`'s dict, after the
`**car.telemetry(...)` splat:

```python
        # Like `radio`: /status-only diagnostics outside the generated contract.
        "rollback": car.rollback,
        "nvs_wiped": car.nvs_wiped,
```

In `build_app(car, link, rollback_mode=False)`: add the parameter, store
`app["rollback_mode"] = rollback_mode`, and pass it from `serve`:
`build_app(car, link, rollback_mode=args.rollback)`. In `main()`'s argparse:

```python
    p.add_argument("--rollback", action="store_true",
                   help="rehearsal: every successful /ota 'fails its first boot' — the mock "
                        "comes back on the old fw with rollback:true in /status")
```

**(c)** `tools/conformance.py` — in `status()`, after the TELEMETRY_FIELDS loop:

```python
        for key in ("rollback", "nvs_wiped"):
            self.check(isinstance(parsed.get(key), bool),
                       f"/status: {key} is {parsed.get(key)!r}, want bool")
```

- [ ] **Step 4: Run the suites**

```bash
python3 tools/mock_car/test_state.py && python3 tools/mock_car/test_rtlink.py
./tools/test-all.sh
```

Expected: OK, OK, `== all green ==` — conformance runs against the mock, which now serves both
keys; the firmware side of the same assertion is Task 3 (already landed) + Task 9's build.

- [ ] **Step 5: Commit**

```bash
git add tools/mock_car/state.py tools/mock_car/mock_car.py tools/conformance.py tools/mock_car/test_state.py
git commit -m "feat(mock): /ota reports the uploaded image's real version, and rollback is rehearsable

The synthetic fw bump could not exhibit an asset-vs-tag mismatch or a
bootloader rollback — the two failure modes the app's detectors hinge on.
parse_image_version reads esp_app_desc_t out of the bytes; --rollback makes
every flash 'fail its first boot'; /status mirrors rollback and nvs_wiped
key-for-key with the firmware, and conformance now asserts both sides.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: documents — protocol.md, CLAUDE.md, c6/README, flash-radio.sh guard

**Files:**
- Modify: `docs/protocol.md` (/status section), `CLAUDE.md` (Status sentence + board.h bullet),
  `firmware/c6/README.md` (FEAT_OTA sentence), `firmware/c6/flash-radio.sh` (pin guard)

**Interfaces:** consumes Tasks 3/5's shipped shapes.

- [ ] **Step 1: protocol.md**

In the `GET /status` example, extend the third line to include the new keys before `radio`:

```json
 "calibrated":true,"bus_ok":true,"ctl":"rt","rollback":false,"nvs_wiped":false,
```

After the `rx_fps` divergence paragraph, add:

```markdown
`rollback` is true when the previous over-the-air update was reverted by the bootloader —
the one signal a client has that the image it flashed did not survive its first boot; treat
"came back on the old version" as a failed update, not a slow one. `nvs_wiped` is true for
the first boot after an NVS format migration erased the saved config: calibration and every
setting are gone, and a client should say so rather than let the car drive on defaults
silently.
```

In the `radio` paragraph, replace `Its image is pinned in `board.h`` with
`` The version it must run is derived from the host's own `esp_hosted` component pin ``
(keep the rest of the sentence).

Verify the generated block survived byte-identical:

```bash
sed -n '/generated:endpoints/,/\/generated:endpoints/p' docs/protocol.md | shasum
bash tools/check_contract.sh
```

Expected: same hash before and after your edit (capture it first), `contract: no drift`.

- [ ] **Step 2: CLAUDE.md**

Line ~78 (`board.h` bullet): replace `expected radio version. Bring-up edits this file` with
`the radio's delivery route (its expected version is derived from the esp_hosted component).
Bring-up edits this file`.

Line ~180 (Status section): replace `the motors are not wired yet, so `board.h`'s I2C pins are
still unverified and a stock build aborts in `pca9685_init` with nothing on the bus` with:

```markdown
the motors are not wired yet, so `board.h`'s I2C pins are still unverified — a stock build
boots anyway with `bus_ok:false` (network and OTA up, motors inert, by design)
```

- [ ] **Step 3: c6/README + flash-radio.sh**

In `firmware/c6/README.md`, replace `the host must be built with
`CONFIG_ESP_HOSTED_HOST_FEAT_OTA=y` (it is).` with `the host must be built with
`CONFIG_ESP_HOSTED_HOST_FEAT_OTA=y` — pinned explicitly in `firmware/p4/sdkconfig.defaults`
(it used to hold only via the component's promptless default).`

In `firmware/c6/flash-radio.sh`, after the `source .../env-p4.sh` line, insert:

```bash
# The image about to be built must be the slave the host pins — after a pin bump with a
# stale managed_components, this script would otherwise flash the OLD slave silently.
PIN=$(grep -E 'espressif/esp_hosted:' "$ROOT/firmware/p4/main/idf_component.yml" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
GOT=$(grep -E '^version:' "$HOSTED/idf_component.yml" 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [ -n "$PIN" ] && [ -n "$GOT" ] && [ "$PIN" != "$GOT" ]; then
    echo "WARNING: fetched esp_hosted is $GOT but the host pins $PIN."
    echo "The image you are about to build is NOT the pinned slave; run"
    echo "  (cd firmware/p4 && source ../../tools/env-p4.sh && idf.py reconfigure)"
    read -r -p "Continue with $GOT anyway? [y/N] " a; [ "$a" = "y" ] || exit 1
fi
```

Verify the field name first: `grep -n '^version:' firmware/p4/managed_components/espressif__esp_hosted/idf_component.yml`
(the component is fetched since Task 5). If the manifest spells it differently, adapt and record.

- [ ] **Step 4: Suite and commit**

```bash
./tools/test-all.sh
git add docs/protocol.md CLAUDE.md firmware/c6/README.md firmware/c6/flash-radio.sh
git commit -m "docs: rollback and nvs_wiped documented; the docs stop describing the old radio pin

protocol.md documents the two new /status keys and the derived expectation;
CLAUDE.md's Status section learns the unwired bus boots on purpose; the c6
README points at the now-explicit FEAT_OTA pin; flash-radio.sh refuses to
silently flash a slave older than the host pins.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: full verification

**Files:** none modified.

- [ ] **Step 1:** `CONFORMANCE=required ./tools/test-all.sh` → `== all green ==` with both
  conformance suites running.
- [ ] **Step 2:** `bash -c 'source tools/env-p4.sh && cd firmware/p4 && idf.py fullclean >/dev/null && idf.py build' 2>&1 | tail -5`
  → `Project build complete.` — this is the compile gate for Tasks 3–6 and proves the Task 5
  derivation resolves. Then `strings firmware/p4/build/ajmiddlecar.bin | grep -m1 'v1\.0+'` —
  record the embedded version in your report (fullclean makes it the current count).
- [ ] **Step 3:** `git status --short` — no tracked changes (untracked build/, sdkconfig,
  managed_components/, dependencies.lock changes? `dependencies.lock` IS tracked: if the
  reconfigure modified it, STOP and report — the pin must not move in this plan).
- [ ] **Step 4:** Report: tasks done, suite counts, build result, embedded version.

---

## Self-review notes (author)

- Spec coverage: decisions 1 (firmware half → T3; mock half → T7; docs → T8), 2 → T4,
  3 → T5, 7 → T7, 8–13 → T1+T2, 19–21 → T8, NVS reduced flag → T3+T7+T8. App-side decisions
  (4–6, 14–18) belong to the sibling app plan. Contract untouched by design.
- Orderings: T3 lands before T5 (both edit status_api.c's snprintf — T5 rewrites the
  `expected` field of the line T3 extended; the plan spells both states). T5 fetches
  managed_components, which T8's flash-radio.sh verification and T9's build reuse.
- Type consistency: `status_api_note_nvs_wiped` spelled identically in T3(a/e);
  `parse_image_version`/`end_ota(version=)`/`rollback`/`nvs_wiped` spelled identically in
  T7's tests, state.py, mock_car.py, and conformance.
- Risk noted for the executor: the exact eh macro names/path and the component manifest's
  `version:` field are verified in-task (T5 Step 1, T8 Step 3) with explicit
  adapt-and-record instructions rather than trusted from this plan.
