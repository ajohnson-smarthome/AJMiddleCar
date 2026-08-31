# Radio in One Image — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Embed the C6 radio's image in the car's firmware so one OTA updates both processors, and the `esp_hosted` pin can move without a bench visit.

**Architecture:** The release builds the slave image from the pinned component and drops it at `firmware/p4/main/radio_image.bin`, which the car's build embeds with `EMBED_FILES`. At boot — after the app image is marked valid, before the car starts serving — the car compares the radio's running version against `RADIO_EXPECTED_FW`; on a mismatch it pushes the embedded image over SDIO and restarts. An attempt counter in NVS bounds the loop that would otherwise be unrecoverable.

**Tech Stack:** ESP-IDF 6.0.2, `esp_hosted` (pinned), NVS, plain `cc` for the pure module's host tests.

**Spec:** `docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md`

## Global Constraints

- **Pure modules have zero ESP-IDF dependencies** and are host-tested with `cc -Wall -Wextra -Werror -std=c11`. `radio_ota.c` is a pure module; `radio_flash.c` is not.
- **Nothing below `esp_ota_mark_app_valid_cancel_rollback()` in `main.c` may panic.** Rollback is waived at that line; a panic after it is a permanent boot loop on a car with no cable. Every new call site there logs and continues.
- **The chunk size for `esp_hosted_cp_ota_write` is at most 1536 bytes.**
- **`RADIO_OTA_MAX_ATTEMPTS` is 3.**
- **Two vendor behaviours are successes, not errors:** `esp_hosted_cp_ota_activate()` returning `ESP_FAIL`, and the SDIO link dropping right after `esp_hosted_cp_ota_end()`. The authority is the version read on the next boot, never a return code.
- **`firmware/p4/main/radio_image.bin` is git-ignored.** It is a build product of a pinned dependency; checking it in would create the second version this work exists to remove.
- **An everyday `idf.py build` must succeed without the radio image present.**
- Run `tools/test-all.sh` before every commit; it must stay green.
- **Read symbols with `riscv32-esp-elf-nm`, never the system `nm`.** `ajmiddlecar.elf` is a
  RISC-V ELF and macOS's `nm` reads Mach-O; the toolchain's is on PATH after
  `source tools/env-p4.sh`.

---

### Task 1: `radio_ota` — the pure decision

**Files:**
- Create: `firmware/p4/main/radio_ota.h`
- Create: `firmware/p4/main/radio_ota.c`
- Create: `firmware/p4/test/test_radio_ota.c`
- Modify: `firmware/p4/test/Makefile`

**Interfaces:**
- Consumes: nothing.
- Produces: `bool radio_ota_should_flash(const char *running, const char *expected, int attempts, int max_attempts, bool have_image)` and `int radio_ota_next_attempts(bool versions_match, int attempts)`, both declared in `radio_ota.h`. Task 4 calls both.

- [ ] **Step 1: Write the failing test**

Create `firmware/p4/test/test_radio_ota.c`:

```c
/* The decision that keeps a car recoverable. Every case here is a way the car could end up
 * unable to boot into a working state with no cable in reach, so the test is about refusals
 * as much as about the one path that flashes. */
#include <stdio.h>
#include <string.h>
#include "radio_ota.h"

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

int main(void) {
    const int MAX = 3;

    /* The one case that acts. */
    check(radio_ota_should_flash("2.11.7", "3.0.6", 0, MAX, true),
          "a mismatch with budget left and an image on board flashes");
    check(radio_ota_should_flash("2.11.7", "3.0.6", 2, MAX, true),
          "the last attempt in the budget still flashes");

    /* A build with no image can do nothing about a mismatch, and must not pretend. */
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 0, MAX, false),
          "no embedded image never flashes");

    /* An unknown is not a mismatch. read_radio_version leaves "unavailable" when the RPC
     * fails, and flashing on that would mean flashing a radio we could not talk to. */
    check(!radio_ota_should_flash("unavailable", "3.0.6", 0, MAX, true),
          "an unreadable running version never flashes");
    check(!radio_ota_should_flash(NULL, "3.0.6", 0, MAX, true),
          "a null running version never flashes");
    check(!radio_ota_should_flash("2.11.7", NULL, 0, MAX, true),
          "a null expected version never flashes");

    /* Matching is the ordinary path and must cost nothing. */
    check(!radio_ota_should_flash("3.0.6", "3.0.6", 0, MAX, true),
          "a matched pair does not flash");
    check(!radio_ota_should_flash("3.0.6", "3.0.6", 2, MAX, true),
          "a matched pair does not flash even with attempts on the clock");

    /* The budget is what stops a boot loop nobody can reach with a cable. */
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 3, MAX, true),
          "a spent budget stops trying");
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 99, MAX, true),
          "a counter past the budget stops trying");

    /* The counter: a match is the only thing that clears it, and it is cleared even when the
     * car never flashed anything — a radio matched by a bench reflash must not leave the
     * budget spent for the next genuine mismatch. */
    check(radio_ota_next_attempts(true, 0) == 0, "a match keeps a clean counter clean");
    check(radio_ota_next_attempts(true, 3) == 0, "a match clears a spent counter");
    check(radio_ota_next_attempts(false, 0) == 1, "a mismatch charges the first attempt");
    check(radio_ota_next_attempts(false, 2) == 3, "a mismatch charges the last attempt");
    check(radio_ota_next_attempts(false, 3) == 3,
          "the counter saturates rather than wrapping — it is stored as one byte");

    if (!failures) printf("test_radio_ota: all passed\n");
    return failures ? 1 : 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && make -C firmware/p4/test test_radio_ota
```
Expected: FAIL — no rule to make target `test_radio_ota` (the Makefile has no entry yet).

- [ ] **Step 3: Write the header**

Create `firmware/p4/main/radio_ota.h`:

```c
#pragma once
#include <stdbool.h>

/* Whether to push the embedded radio image at the co-processor, and how the attempt counter
 * moves. Pure on purpose: this is the decision that determines whether a car in the field can
 * be recovered without a cable, and it is worth being able to check by reading rather than by
 * reproducing on hardware.
 *
 * The counter is NOT about the write succeeding. A failed write is harmless — the slave's OTA
 * lands in its inactive slot — so what is bounded here is the LOOP: flash, reboot, still
 * mismatched, flash again. The authority on success is the version read after the reboot, and
 * nothing else.
 */

/* Three tries, then live with a mismatched radio — which is exactly what the car does today,
 * and today's behaviour is a car that drives. Giving up is the safe direction. */
#define RADIO_OTA_MAX_ATTEMPTS 3

/* Refuses on every unknown. An unreadable running version is not a mismatch, a build with no
 * image cannot act on one, and a spent budget must stop rather than loop. */
bool radio_ota_should_flash(const char *running, const char *expected,
                            int attempts, int max_attempts, bool have_image);

/* The counter's next value. A match clears it — including when the match came from a bench
 * reflash rather than from anything this car did, so the budget is fresh for the next real
 * mismatch. A mismatch charges one and saturates at RADIO_OTA_MAX_ATTEMPTS, because it is
 * stored as a single NVS byte and wrapping would silently re-arm the loop. */
int radio_ota_next_attempts(bool versions_match, int attempts);
```

- [ ] **Step 4: Write the implementation**

Create `firmware/p4/main/radio_ota.c`:

```c
#include "radio_ota.h"

#include <string.h>

/* What the version read leaves in place when the RPC fails. Compared here rather than guarded
 * at the call site, so the refusal is part of the tested decision and not of the wiring. */
#define RADIO_FW_UNAVAILABLE "unavailable"

bool radio_ota_should_flash(const char *running, const char *expected,
                            int attempts, int max_attempts, bool have_image)
{
    if (!have_image)                                   return false;
    if (running == NULL || expected == NULL)           return false;
    if (strcmp(running, RADIO_FW_UNAVAILABLE) == 0)    return false;
    if (strcmp(running, expected) == 0)                return false;
    return attempts < max_attempts;
}

int radio_ota_next_attempts(bool versions_match, int attempts)
{
    if (versions_match)                        return 0;
    if (attempts >= RADIO_OTA_MAX_ATTEMPTS)    return RADIO_OTA_MAX_ATTEMPTS;
    return attempts + 1;
}
```

- [ ] **Step 5: Add the test to the Makefile**

In `firmware/p4/test/Makefile`, append `test_radio_ota` to the `all:` target's list, and add
the rule and the run line. The rule:

```make
test_radio_ota: test_radio_ota.c ../main/radio_ota.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
```

Add `./test_radio_ota` to the `run:` target beside the other `./test_*` lines, and add
`test_radio_ota` to the `rm -f` list in `clean:`.

- [ ] **Step 6: Run the test to verify it passes**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && make -C firmware/p4/test run
```
Expected: `test_radio_ota: all passed`, and every other test still passing.

- [ ] **Step 7: Commit**

```bash
git add firmware/p4/main/radio_ota.h firmware/p4/main/radio_ota.c \
        firmware/p4/test/test_radio_ota.c firmware/p4/test/Makefile
git commit -m "feat(fw): the pure decision behind flashing the radio from the car"
```

---

### Task 2: Embed the image, and build without it

**Files:**
- Modify: `firmware/p4/main/CMakeLists.txt`
- Modify: `.gitignore`

**Interfaces:**
- Consumes: nothing.
- Produces: the linker symbols `_binary_radio_image_bin_start` and `_binary_radio_image_bin_end`, always defined. Task 3 reads both. A build with no image supplied produces a zero-length region, which is how "this build carries no radio image" is expressed — there is no separate flag.

- [ ] **Step 1: Make the image optional at configure time**

In `firmware/p4/main/CMakeLists.txt`, above `idf_component_register(...)`, add:

```cmake
# The C6's image rides inside the car's image, so one OTA updates both processors
# (docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md). tools/release.sh builds it
# from the pinned esp_hosted component and drops it here; it is git-ignored, because it is a
# build product of a pinned dependency and checking it in would create the second version this
# whole design exists to remove.
#
# An everyday build must not require it — nobody working on the mixer should have to build a
# co-processor image first — and EMBED_FILES fails outright on a missing file. So an absent
# image becomes an empty one, and the firmware reads the embedded length to decide whether it
# has anything to offer the radio. There is no separate flag: zero bytes IS the statement.
set(RADIO_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/radio_image.bin")
if(NOT EXISTS "${RADIO_IMAGE}")
    file(WRITE "${RADIO_IMAGE}" "")
    message(STATUS "no radio_image.bin — this build carries no co-processor image")
endif()
```

- [ ] **Step 2: Embed it**

In the same file, replace the `idf_component_register(...)` call with this one. It adds
`radio_ota.c` (Task 1) to `SRCS` and the `EMBED_FILES` line; `radio_flash.c` joins `SRCS` in
Task 3, when the file exists:

```cmake
idf_component_register(
    SRCS "ota_api.c" "main.c" "pca9685.c" "mixer.c" "motors.c" "car.c" "calibration.c" "wifi_ap.c" "http_server.c" "control_proto.c" "rt_link.c" "recovery.c" "calib_api.c" "status_api.c" "ramp.c" "link.c" "cfg_api.c" "telemetry.c" "wheel.c" "dims.c" "cfg_json.c" "api_util.c" "radio_ota.c"
    INCLUDE_DIRS "."
    EMBED_FILES "radio_image.bin"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer heap esp_app_format app_update lwip
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_driver_uart esp_driver_i2c
)
```

- [ ] **Step 3: Ignore the image**

In `.gitignore`, beside the other firmware build products, add:

```
firmware/p4/main/radio_image.bin
```

- [ ] **Step 4: Verify a clean build still works with no image**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && rm -f firmware/p4/main/radio_image.bin && \
  source tools/env-p4.sh && (cd firmware/p4 && idf.py build 2>&1 | tail -5)
```
Expected: BUILD SUCCEEDS, and the configure output contains
`no radio_image.bin — this build carries no co-processor image`.

- [ ] **Step 5: Verify a build with an image embeds it**

The stand-in is the project's own bootloader image — a real ESP binary of a known size that the
build has already produced, so this proves the embed against the kind of file the field will
actually carry rather than against noise.

The check reads the assembly `EMBED_FILES` generates, **not** the linked ELF. The symbols do not
reach `ajmiddlecar.elf` until something references them, because the linker discards unreferenced
sections — and the first reference arrives in Task 3. Looking for them in the ELF here fails on a
perfectly correct build, which is a check that teaches an implementer to delete checks.

Run:
```bash
source tools/env-p4.sh && \
  cp firmware/p4/build/bootloader/bootloader.bin firmware/p4/main/radio_image.bin && \
  (cd firmware/p4 && idf.py build 2>&1 | tail -3) && \
  grep -c "_binary_radio_image_bin_start\|_binary_radio_image_bin_end" firmware/p4/build/radio_image.bin.S
```
Expected: the build succeeds and the grep prints `2` — both symbols defined in the generated
assembly. Then remove the stand-in: `rm -f firmware/p4/main/radio_image.bin`.

- [ ] **Step 6: Commit**

```bash
git add firmware/p4/main/CMakeLists.txt .gitignore
git commit -m "build(fw): the car's image can carry the radio's, and builds fine without it"
```

---

### Task 3: `radio_flash` — the four RPCs

**Files:**
- Create: `firmware/p4/main/radio_flash.h`
- Create: `firmware/p4/main/radio_flash.c`
- Modify: `firmware/p4/main/CMakeLists.txt` (add `radio_flash.c` to `SRCS`)

**Interfaces:**
- Consumes: the linker symbols from Task 2.
- Produces, declared in `radio_flash.h`:
  - `const char *radio_flash_version(void)` — the co-processor's running version, read once and
    cached; `"unavailable"` when the RPC failed.
  - `size_t radio_flash_image_size(void)` — the embedded image's length, zero when absent.
  - `void radio_flash_apply(void)` — pushes the embedded image and never returns normally
    (it restarts the car).

  Task 4 calls all three; Task 4 also repoints `status_api.c` at `radio_flash_version()`.

- [ ] **Step 1: Write the header**

Create `firmware/p4/main/radio_flash.h`:

```c
#pragma once
#include <stddef.h>

/* Pushing the embedded co-processor image over the SDIO link that already exists, and reading
 * back what the radio runs. No policy lives here — whether to do it at all is radio_ota.h's
 * decision, and that one is pure and tested.
 *
 * The route is the vendor's: esp_hosted_cp_ota_begin/write/end/activate, reached through the
 * component's compat header, and proven on this board on 2026-08-20 (firmware/c6/README.md).
 */

/* The co-processor's running version, e.g. "3.0.6". Read once over RPC on the first call and
 * cached: against a mismatched slave that call costs up to five seconds, and the answer cannot
 * change without a reboot. Returns "unavailable" when the RPC failed. */
const char *radio_flash_version(void);

/* How many bytes of radio image this build carries. Zero means it carries none — an ordinary
 * developer build — and nothing should be attempted. */
size_t radio_flash_image_size(void);

/* Push the embedded image at the co-processor and restart the car. Does not return.
 *
 * Two documented vendor behaviours are treated as success rather than as errors: activate()
 * returning ESP_FAIL against a slave older than v2.6.0 (the old image applies the update itself
 * on end()), and the SDIO link dropping right after end() (that is the radio rebooting into its
 * new firmware). In both cases the verdict belongs to the version read on the next boot, not to
 * a return code — so this restarts either way, and the caller has already charged the attempt.
 */
void radio_flash_apply(void);
```

- [ ] **Step 2: Write the implementation**

Create `firmware/p4/main/radio_flash.c`:

```c
#include "radio_flash.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"

static const char *TAG = "radio_flash";

/* EMBED_FILES derives these from the file's name; renaming radio_image.bin renames them. */
extern const uint8_t radio_image_start[] asm("_binary_radio_image_bin_start");
extern const uint8_t radio_image_end[]   asm("_binary_radio_image_bin_end");

/* The vendor's cap. Larger writes are rejected by the RPC, not split for us. */
#define RADIO_CHUNK 1536

size_t radio_flash_image_size(void)
{
    return (size_t)(radio_image_end - radio_image_start);
}

const char *radio_flash_version(void)
{
    static char cached[24];
    static bool read;
    if (read) return cached;
    read = true;
    esp_hosted_coprocessor_fwver_t v;
    if (esp_hosted_get_coprocessor_fwversion(&v) != 0) {
        snprintf(cached, sizeof(cached), "unavailable");
        ESP_LOGW(TAG, "could not read radio firmware version");
        return cached;
    }
    snprintf(cached, sizeof(cached), "%u.%u.%u",
             (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
    return cached;
}

void radio_flash_apply(void)
{
    const size_t total = radio_flash_image_size();
    ESP_LOGW(TAG, "flashing the radio: %u bytes over SDIO", (unsigned)total);

    if (esp_hosted_cp_ota_begin() != ESP_OK) {
        /* Nothing was written, so nothing is at risk. Restart anyway: the attempt is already
         * charged, and the next boot re-reads the version and decides again. */
        ESP_LOGE(TAG, "ota begin refused — restarting without writing");
        esp_restart();
    }

    size_t sent = 0;
    while (sent < total) {
        const size_t n = (total - sent) < RADIO_CHUNK ? (total - sent) : RADIO_CHUNK;
        if (esp_hosted_cp_ota_write(radio_image_start + sent, n) != ESP_OK) {
            /* Safe: an interrupted write leaves the slave's running image alone — the update
             * lands in its inactive slot. */
            ESP_LOGE(TAG, "ota write failed at %u/%u — restarting",
                     (unsigned)sent, (unsigned)total);
            esp_restart();
        }
        sent += n;
    }

    /* From here the two known vendor behaviours are expected, and neither is an error: end()
     * may take the SDIO link down as the radio reboots, and activate() may time out against an
     * old slave that already applied the image itself. Log and restart. */
    if (esp_hosted_cp_ota_end() != ESP_OK) {
        ESP_LOGW(TAG, "ota end reported a failure — expected when the link drops as the radio reboots");
    }
    if (esp_hosted_cp_ota_activate() != ESP_OK) {
        ESP_LOGW(TAG, "ota activate reported a failure — expected against a slave older than 2.6.0");
    }
    ESP_LOGW(TAG, "radio image delivered — restarting; the next boot's version read is the verdict");
    esp_restart();
}
```

- [ ] **Step 3: Add it to the build**

In `firmware/p4/main/CMakeLists.txt`, add `"radio_flash.c"` to `SRCS`, after `"radio_ota.c"`.

- [ ] **Step 4: Verify it builds**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && source tools/env-p4.sh && \
  (cd firmware/p4 && idf.py build 2>&1 | tail -5)
```
Expected: BUILD SUCCEEDS. If `esp_hosted.h` or `esp_hosted_ota.h` is not found, add
`espressif__esp_hosted` to `REQUIRES` in the same `idf_component_register(...)` and rebuild.

Then confirm the embedded symbols now reach the linked image — this file is their first
reference, and until it existed the linker discarded them as unreferenced:

```bash
source tools/env-p4.sh && \
  cp firmware/p4/build/bootloader/bootloader.bin firmware/p4/main/radio_image.bin && \
  (cd firmware/p4 && idf.py build >/dev/null 2>&1) && \
  riscv32-esp-elf-nm firmware/p4/build/ajmiddlecar.elf | grep radio_image_bin
```
Expected: both `_binary_radio_image_bin_start` and `_binary_radio_image_bin_end` are listed. Then
remove the stand-in: `rm -f firmware/p4/main/radio_image.bin`.

- [ ] **Step 5: Commit**

```bash
git add firmware/p4/main/radio_flash.h firmware/p4/main/radio_flash.c firmware/p4/main/CMakeLists.txt
git commit -m "feat(fw): push the embedded radio image over SDIO"
```

---

### Task 4: The boot gate and the attempt counter

**Files:**
- Modify: `firmware/p4/main/main.c` (between the mark-valid block and `telemetry_start()`)
- Modify: `firmware/p4/main/status_api.c` (`read_radio_version`, around line 58)

**Interfaces:**
- Consumes: `radio_ota_should_flash`, `radio_ota_next_attempts`, `RADIO_OTA_MAX_ATTEMPTS`
  (Task 1); `radio_flash_version`, `radio_flash_image_size`, `radio_flash_apply` (Task 3).
- Produces: nothing further tasks call.

- [ ] **Step 1: Point `status_api.c` at the cached version**

In `firmware/p4/main/status_api.c`, replace the body of `read_radio_version()` so it uses the
cached read instead of making a second RPC — against a mismatched slave that call costs up to
five seconds, and the boot gate has already paid it:

```c
static void read_radio_version(void) {
    snprintf(s_radio_fw, sizeof(s_radio_fw), "%s", radio_flash_version());
    s_radio_ok = (strcmp(s_radio_fw, RADIO_EXPECTED_FW) == 0);
    if (s_radio_ok) {
        ESP_LOGI(TAG, "radio firmware %s", s_radio_fw);
    } else {
        /* No longer an instruction to fetch a cable: main.c's boot gate offers the embedded
         * image first, and only a spent attempt budget or a build with no image reaches here
         * still mismatched. */
        ESP_LOGW(TAG, "radio firmware %s, expected %s — the car could not correct it",
                 s_radio_fw, RADIO_EXPECTED_FW);
    }
}
```

Add `#include "radio_flash.h"` to the includes at the top of the file. Remove the now-unused
`esp_hosted` include if nothing else in the file uses it; leave it if anything does.

- [ ] **Step 2: Share the expected-version macro**

`RADIO_EXPECTED_FW` is defined privately inside `status_api.c`, and `main.c` now needs the same
value. Two copies of that derivation would be exactly the hand-copied pin an earlier audit
caught, so move it. Create `firmware/p4/main/radio_expected.h`:

```c
#pragma once
/* The expected slave version is the HOST library's version: esp_hosted requires the pair
 * matched, and deriving the string from the component's own macros makes drift between
 * idf_component.yml and this check impossible. Shared between status_api.c, which reports it,
 * and main.c's boot gate, which acts on it. */
#define RADIO_STR2(x) #x
#define RADIO_STR(x)  RADIO_STR2(x)
#define RADIO_EXPECTED_FW \
    RADIO_STR(PROJECT_VERSION_MAJOR_1) "." RADIO_STR(PROJECT_VERSION_MINOR_1) "." RADIO_STR(PROJECT_VERSION_PATCH_1)
```

Delete those same five lines (the two `RADIO_STR*` macros and `RADIO_EXPECTED_FW`) from
`status_api.c`, and add `#include "radio_expected.h"` there instead.

- [ ] **Step 3: Write the boot gate in `main.c`**

Add these includes to `firmware/p4/main/main.c`: `"nvs.h"`, `"radio_ota.h"`, `"radio_flash.h"`,
`"radio_expected.h"`. Then, above `app_main`, add:

```c
/* The attempt counter. Its own namespace and a single byte: this is not configuration, nothing
   reads it over the API, and the JSON-per-domain shape the config domains use would be
   ceremony around one number. */
#define RADIO_NVS_NS   "radio"
#define RADIO_NVS_KEY  "ota_tries"

static uint8_t radio_attempts_load(void) {
    nvs_handle_t h;
    if (nvs_open(RADIO_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    if (nvs_get_u8(h, RADIO_NVS_KEY, &n) != ESP_OK) n = 0;
    nvs_close(h);
    return n;
}

static void radio_attempts_store(uint8_t n) {
    nvs_handle_t h;
    if (nvs_open(RADIO_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS for the radio attempt counter");
        return;
    }
    if (nvs_set_u8(h, RADIO_NVS_KEY, n) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void radio_gate(void) {
    const char *running = radio_flash_version();
    const bool have_image = radio_flash_image_size() > 0;
    const bool match = (strcmp(running, RADIO_EXPECTED_FW) == 0);
    const uint8_t attempts = radio_attempts_load();

    /* Charged BEFORE the flash, deliberately: radio_flash_apply() restarts the car, so a
       counter written after it would never be written at all. */
    const uint8_t next = (uint8_t)radio_ota_next_attempts(match, attempts);
    if (next != attempts) radio_attempts_store(next);

    if (!radio_ota_should_flash(running, RADIO_EXPECTED_FW,
                                attempts, RADIO_OTA_MAX_ATTEMPTS, have_image)) {
        if (!match && have_image) {
            ESP_LOGW(TAG, "radio %s vs expected %s: %d attempts spent, giving up and booting on",
                     running, RADIO_EXPECTED_FW, (int)attempts);
        }
        return;
    }
    ESP_LOGW(TAG, "radio %s, expected %s — flashing (attempt %d of %d)",
             running, RADIO_EXPECTED_FW, (int)next, RADIO_OTA_MAX_ATTEMPTS);
    radio_flash_apply();   /* does not return */
}
```

- [ ] **Step 4: Call it from the right place in boot**

In `app_main`, immediately after the `esp_ota_mark_app_valid_cancel_rollback()` block and
before `telemetry_start();`, insert:

```c
    /* The radio's image rides inside this one, so a pin bump no longer means a bench visit
       (docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md).

       The position is not free. AFTER mark-valid, because a successful flash ends in a restart
       we chose and the bootloader cannot tell that from a crash — before it, that restart would
       revert a perfectly good app image. BEFORE rt_link_start/http_server_start, because from
       there on the car is serving and the SDIO link is not ours to drop. */
    radio_gate();
```

- [ ] **Step 5: Verify it builds and the tests stay green**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && tools/test-all.sh 2>&1 | tail -3 && \
  source tools/env-p4.sh && (cd firmware/p4 && idf.py build 2>&1 | tail -3)
```
Expected: `== all green ==` and BUILD SUCCEEDS.

- [ ] **Step 6: Verify the ordinary build is inert**

With no `radio_image.bin` present, the embedded region is empty and the gate must do nothing.
Confirm the symbols exist and are equal — which is what `radio_flash_image_size()` returns zero
from:

```bash
rm -f firmware/p4/main/radio_image.bin && \
  source tools/env-p4.sh && (cd firmware/p4 && idf.py build >/dev/null 2>&1) && \
  riscv32-esp-elf-nm firmware/p4/build/ajmiddlecar.elf | grep radio_image_bin
```
Expected: two lines whose addresses are identical — start and end at the same place, so the
image is zero bytes and `radio_ota_should_flash` refuses on `have_image == false`.

- [ ] **Step 7: Commit**

```bash
git add firmware/p4/main/main.c firmware/p4/main/status_api.c firmware/p4/main/radio_expected.h
git commit -m "feat(fw): the car offers its embedded image to a mismatched radio at boot"
```

---

### Task 5: The release carries both

**Files:**
- Modify: `tools/release.sh`

**Interfaces:**
- Consumes: `firmware/p4/main/radio_image.bin` as the path the car's build embeds (Task 2).
- Produces: nothing further tasks call.

- [ ] **Step 1: Build the radio image before the car's**

In `tools/release.sh`, immediately before the line
`(cd firmware/p4 && idf.py fullclean >/dev/null && idf.py build)`, insert:

```bash
# The C6's image rides inside the car's, so build it first and put it where the car's build
# embeds it. Same source flash-radio.sh uses — the pinned component's own example — so the pin
# determines both halves and there is no second version to keep in step.
HOSTED="firmware/p4/managed_components/espressif__esp_hosted"
CP="$HOSTED/examples/wifi/sta/cp"
if [ ! -d "$CP" ]; then
    echo "ERROR: esp_hosted is not fetched — run (cd firmware/p4 && idf.py reconfigure) first"; exit 1
fi
python "$HOSTED/tools/eh.py" patch-idf --idf-path "$IDF_PATH" >/dev/null
(cd "$CP" && { [ -d build ] || idf.py set-target esp32c6 >/dev/null; } && idf.py build >/dev/null)
CP_BIN=$(ls "$CP"/build/*.bin 2>/dev/null | grep -v -E 'bootloader|partition-table|ota_data' | head -1)
[ -n "$CP_BIN" ] || { echo "ERROR: the co-processor build produced no image"; exit 1; }
cp "$CP_BIN" firmware/p4/main/radio_image.bin
echo "radio image: $(basename "$CP_BIN"), $(wc -c < firmware/p4/main/radio_image.bin) bytes"
```

- [ ] **Step 2: Refuse to ship a car image without it**

After the existing `[ -f "$BIN_CAR" ] || { ... }` check, add:

```bash
# A release whose car image does not actually contain the radio's is the silent version of the
# bug this whole change removes: it would look current and strand a pin bump anyway.
if ! riscv32-esp-elf-nm firmware/p4/build/ajmiddlecar.elf | grep -q _binary_radio_image_bin_start; then
    echo "ERROR: the car image does not embed a radio image"; exit 1
fi
EMBEDDED=$(wc -c < firmware/p4/main/radio_image.bin)
[ "$EMBEDDED" -gt 4096 ] || { echo "ERROR: the embedded radio image is $EMBEDDED bytes — not an image"; exit 1; }
```

- [ ] **Step 3: Remove the radio-pin gate**

Delete the whole `PIN_MOVED` machinery — its justification is gone. Remove:
- the `--radio-bumped` flag parsing in the argument loop, and `RADIO_BUMPED=0`;
- the `radio_pin()` function, `PIN`, `PIN_MOVED`, `PREV_PIN`, `PREV_TAG` and the
  `git fetch --tags` block that feeds them;
- the `if [ "$PIN_MOVED" = 1 ]` note-appending block;
- the `if [ "$PIN_MOVED" = 1 ] && [ "$RADIO_BUMPED" != 1 ]` error block;
- `[dry-run] radio   : esp_hosted $PIN` from the dry-run output, replaced by
  `echo "[dry-run] radio   : built from the pinned esp_hosted and embedded in $BIN_CAR"`.

Update the usage comment at the top of the file: `# Usage: tools/release.sh [--dry-run] ["release notes"]`.

- [ ] **Step 4: Verify the dry run still works and no longer mentions the gate**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && tools/release.sh --dry-run
```
Expected: the version, tag, title, both assets and the new radio line print; nothing mentions
`--radio-bumped`; exit code 0; and no network call was made.

- [ ] **Step 5: Commit**

```bash
git add tools/release.sh
git commit -m "feat(release): build the radio's image into the car's, and retire the pin gate"
```

---

### Task 6: The documents that now say something untrue

**Files:**
- Modify: `CLAUDE.md` (the Hardware table's radio row, and the Build section)
- Modify: `firmware/c6/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing.

- [ ] **Step 1: Correct `CLAUDE.md`**

In the paragraph beginning "**The C6 is a modem, not a brain.**", replace the sentence
"The radio's image is built by `firmware/c6/flash-radio.sh` and can be delivered either over the
C6's UART header or — as was actually done — over the SDIO link itself, from the host" with:

```markdown
The radio's image is built into the car's own firmware and delivered over SDIO by the car
itself: a mismatch at boot makes the car push the embedded image at the C6 and restart, so one
OTA updates both processors and an `esp_hosted` pin bump no longer means a bench visit
(`docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md`). The UART header remains the
recovery path, and `firmware/c6/flash-radio.sh` still builds the image standalone.
```

- [ ] **Step 2: Correct `firmware/c6/README.md`**

At the top of the "Flashing over SDIO, from the host" section, add:

```markdown
> **This is now automatic.** The car carries the C6's image inside its own and offers it to a
> mismatched radio at boot — see `docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md`.
> What follows is the mechanism that runs underneath, and the manual route for a car that has
> spent its three attempts.
```

- [ ] **Step 3: Verify nothing else claims OTA cannot reach the radio**

Run:
```bash
cd /Users/adamjohnson/VSCode/esp32-p4-car && \
  grep -rn "radio-bumped\|bench reflash\|reflash the C6" --include="*.md" --include="*.sh" --include="*.c" . | grep -v managed_components
```
Expected: only `firmware/c6/README.md`'s recovery section, which is still true. Fix any other hit.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md firmware/c6/README.md
git commit -m "docs: the radio updates itself now, so stop sending people for a cable"
```

---

## Bench procedure

None of this can be proven in the simulator or on the host. When hardware is next in reach:

1. **Cut a release** (`tools/release.sh "…"`) and confirm the console prints a radio image of
   roughly 1.1–1.3 MB and that the car's image is around 2 MB.
2. **Flash the car by cable** from that build, with the radio already matched. Expected: the boot
   log has no `radio_flash` line at all — the ordinary path costs one comparison.
3. **Force a mismatch.** Flash the C6 by hand with an older image
   (`firmware/c6/flash-radio.sh /dev/cu.usbserial-XXXX` from a checkout at an older pin), then
   reset the car. Expected: `flashing the radio: N bytes over SDIO`, then a restart, then a boot
   whose `status_api` line reads `radio firmware <expected>` — matched, and the counter cleared.
4. **Prove the budget.** With the radio deliberately unable to take the image, confirm the car
   gives up after three boots and carries on driving with `radio.ok:false`, rather than looping.
   Recovering from that state is the UART header, as it always was.
