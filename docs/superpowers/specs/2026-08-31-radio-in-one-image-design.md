# One image, two processors — the radio joins the car's firmware

**Status:** design, approved 2026-08-31
**Supersedes:** the radio-pin gate in `tools/release.sh` (`--radio-bumped`)

## The problem

The car is two processors. The P4 runs everything this project writes; the C6 beside it runs
Espressif's `esp_hosted` slave image and is the only radio on the board. They must be a matched
pair — the host library refuses to work properly against a slave of a different version, and a
mismatch costs five seconds of every boot to a timed-out RPC, disables SDIO aggregation, and
leaves `radio.ok` false.

The P4's half updates over the air. The C6's half does not. So a release that moves the
`esp_hosted` pin strands every car in the field until somebody walks up with a USB-serial adapter
and a SH1.0 connector. `tools/release.sh` knows this and refuses to cut such a release without
`--radio-bumped`, which is a warning label rather than a solution: it makes the hazard visible and
leaves it in place.

The goal is one version and one image that updates both processors.

## What is already true

Almost all of it, which is why this is worth doing now.

- **The host can flash the slave over SDIO at runtime.** The API is
  `esp_hosted_cp_ota_begin()` → `esp_hosted_cp_ota_write(buf, len)` in chunks of at most 1536
  bytes → `esp_hosted_cp_ota_end()` → `esp_hosted_cp_ota_activate()`, reached through the
  component's compat header. **This route was used on this hardware on 2026-08-20** to bring the
  radio from its shipped image to 3.0.6. It is not a theory.
- **The capability is compiled in.** `CONFIG_ESP_HOSTED_HOST_FEAT_OTA=y` is pinned explicitly in
  `firmware/p4/sdkconfig.defaults`.
- **The car already detects the mismatch.** `status_api.c`'s `read_radio_version()` reads the
  slave's version over RPC at boot and compares it against `RADIO_EXPECTED_FW`, which is derived
  from the host library's own version macros — so it tracks the component pin with no hand-copied
  string to drift. `/status` reports `radio:{fw,expected,ok}`. Today the mismatch branch logs
  "reflash the C6" and stops there.
- **The slave image is derived from the pin, not stored.** `firmware/c6/flash-radio.sh` builds it
  from `examples/wifi/sta/cp` **inside the pinned component**. There is no vendor binary to check
  in and no second version to keep in step: the pin determines both halves by construction.
- **A failed write is safe.** The slave's OTA lands in its inactive slot; an interrupted or
  corrupt write leaves the running image untouched.
- **There is room.** The app slots are 4 MB each (`partitions.csv`); the car's image is 764 KB.

What is missing is the code that calls those four functions, the image sitting where that code
can reach it, and a release that carries both.

## Design

### Where the image comes from

`tools/release.sh` builds the C6 image before it builds the car's, using the same
`examples/wifi/sta/cp` path `flash-radio.sh` uses, from the component the car's own build fetched.
The result is copied to **`firmware/p4/main/radio_image.bin`**, which is git-ignored: it is a build
product of a pinned dependency, not a source file, and checking it in would create exactly the
second version this design exists to remove.

The car's `main/CMakeLists.txt` embeds that file with `EMBED_FILES`. The name is load-bearing —
`EMBED_FILES` derives the linker symbols from it — so the firmware refers to
`_binary_radio_image_bin_start` / `_binary_radio_image_bin_end`, and renaming the file means
renaming those.

**An everyday `idf.py build` must not require it.** Nobody working on the mixer should have to
build a co-processor image first. When the file is absent, the build embeds a zero-length
placeholder and defines `RADIO_IMAGE_ABSENT`; the firmware then behaves exactly as it does today —
it detects the mismatch, logs it, and tells the reader to reach for `firmware/c6/README.md`. The
one place that must never ship without the image is the release, and `release.sh` verifies the
embedded length is non-zero before it uploads anything.

### When the radio is flashed

Boot, and only boot. There is no session, no client, no armed actuator, and the SDIO link may be
dropped without taking anything with it.

The position within boot is not free. `main.c` marks the app image valid immediately after
`wifi_ap_start()`, and the comment there states the rule the whole tail of boot lives under:
after that line **rollback is waived**, so a panic below it is a permanent boot loop on a car with
no cable. The radio flash goes **after** mark-valid and **before** `rt_link_start()`:

- After mark-valid, because flashing the radio ends in a deliberate reboot. Before mark-valid,
  that reboot would revert a perfectly good app image — the bootloader cannot tell a reboot we
  chose from a crash.
- Before `rt_link_start()` and `http_server_start()`, because from there on the car is serving.

Concretely, between the mark-valid block and `telemetry_start()`:

1. Read the slave's version (the call `status_api.c` already makes; it moves earlier and
   `status_api_start` reads the cached result rather than repeating the RPC).
2. If it matches `RADIO_EXPECTED_FW`, or the build carries no image, continue booting. This is the
   ordinary path and costs one comparison.
3. Otherwise, flash: `begin`, `write` in 1536-byte chunks, `end`, `activate`, then
   `esp_restart()`.

### How failure is bounded

The danger is not a bricked radio — a failed write leaves the running image alone. The danger is a
**loop**: flash, reboot, still mismatched, flash again, forever, with no cable and no way in.

So the attempt count lives in NVS and survives the reboot it causes. Before flashing, the car
increments it; on a boot where the versions match, it clears it. At `RADIO_OTA_MAX_ATTEMPTS`
(three) the car stops trying and boots normally with a mismatched radio — which is precisely
today's behaviour, and today's behaviour is a car that drives. Giving up is the safe direction.

Two failure modes the vendor documents, both already written up in `firmware/c6/README.md`, are
treated as success rather than as errors:

- **`activate()` returns `ESP_FAIL`** against a slave older than v2.6.0 — the old image applies the
  update itself on `end()`. Reboot anyway; the next boot's comparison is the real verdict.
- **The link drops right after `end()`** (`Unrecoverable host sdio state`). That is the radio
  rebooting into its new firmware. Reboot anyway.

In both cases the authority is the same and it is not the return code: **the version read on the
next boot**. That is what clears the attempt counter, and nothing else does.

### The shape of the code

Following the project's own split, the decision is pure and the transport is not.

- **`radio_ota.{c,h}`** — pure, zero ESP-IDF, host-tested with the other pure modules:

  ```c
  /* Whether to flash, given what the radio runs, what this build expects, how many
   * attempts have already been spent, and whether this build carries an image at all. */
  bool radio_ota_should_flash(const char *running, const char *expected,
                              int attempts, int max_attempts, bool have_image);

  /* The attempt counter's next value, given the outcome of this boot's comparison. */
  int radio_ota_next_attempts(bool versions_match, int attempts);
  ```

  The cases worth pinning in the test are the ones that decide whether a car in the field can be
  recovered: a missing image never flashes, an unreadable running version never flashes (an
  unknown is not a mismatch), a spent budget never flashes, and a match always clears the count.

- **`radio_flash.{c,h}`** — the four RPC calls and the chunking, in one function, with no policy
  in it. Kept apart from `radio_ota.c` so the pure half stays pure.

`main.c` calls the decision, then the transport, then `esp_restart()`.

### What the app sees

Nothing new, and that is the point. The app updates the car; the car brings its radio along on the
next boot. The existing `.rebooting` watch in `FirmwareFlow` waits 25 s for the car to come back
with a different version, and reports `.flashed` — "committed, it will confirm itself" — if the
window closes first. A radio flash adds one reboot cycle to the first boot after a pin change, so
that path is exercised more often than before. It already degrades correctly.

`/status`'s `radio` object stays exactly as it is. It becomes far less interesting, because
`ok:false` will now be a transient state during one boot rather than a standing condition — but it
remains the only place a spent attempt budget is visible, which is the case that matters most.

### What the release changes

`tools/release.sh` gains a step and loses a guard:

- **Gains:** build the C6 image before the car's, copy it where the car's build expects it, and
  refuse to upload if the car's image does not actually contain it.
- **Loses:** the `--radio-bumped` gate and the pin-comparison block that feeds it. Its entire
  justification — "OTA cannot deliver the C6 image: every updated car will need a bench reflash" —
  stops being true. The warning it appends to the release notes goes with it.

The car's image grows from 764 KB to roughly 2 MB, well inside the 4 MB slot. Nothing new
enforces that: `idf.py build`'s own `app_check_size` step already fails a build whose image
outgrows the partition, and `release.sh` builds before it uploads — so an image that cannot be
flashed never reaches a release.

## What this costs

**Every car update carries the radio image, whether or not the radio needs it.** Roughly 2 MB
instead of 764 KB, over the adapter's relay, on every single update. This was weighed and
accepted: the alternative — embedding the image only in releases where the pin moved — keeps one
version but produces two different kinds of car image, and "almost always the same" is a worse
property to reason about than "always the same".

**The first boot after a pin change is a double boot.** Detect, flash, restart, boot again. About
twenty seconds, during which the car is not reachable. The app's existing reboot watch covers it.

**The radio's recovery path is unchanged and still physical.** If the C6 is ever left unbootable,
SDIO is dead and the UART header is the only way in. Nothing here makes that more likely — a
failed write lands in the inactive slot — but nothing here removes it either, and a car that
cannot talk to its radio cannot flash its radio.

## What this does not do

- **It does not update the radio from the app directly.** No new endpoint, no second asset, no
  second version. The app's only lever on the radio is updating the car.
- **It does not make the radio image a project artifact.** It is still built from the pinned
  vendor component at release time, and still never authored here.
- **It does not touch the dongle.** The S3 has its own radio built in; there is no co-processor
  and nothing to unify.

## Open, and deliberately left so

**The exact image size is unmeasured.** It is expected around 1.1–1.3 MB, which is what the 2 MB
estimate above rests on. The first build settles it, and the release's own size check is what
would catch a surprise — no decision here depends on the number being at the low end of that
range.
