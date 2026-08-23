# OTA-chain fix decisions

> **For agentic workers:** decisions behind the 2026-08-23 OTA-fix plans
> (`docs/superpowers/plans/2026-08-23-ota-fixes-*.md`). They close the ~30 confirmed findings
> of the 2026-08-23 OTA-chain audit (report noted in the repo's session memory). Where a
> decision touches the wire or crosses sides it MUST land identically everywhere it applies.

## Cross-cutting

1. **Rollback becomes visible.** `/status` gains a top-level `"rollback": true|false` —
   true when the *other* OTA slot's state is `ESP_OTA_IMG_ABORTED` (the last update was
   reverted by the bootloader). The mock mirrors it (settable for rehearsal); protocol.md
   documents it beside `/status`. The app treats `rollback:true` — or same-fw-after-bounce —
   as a FAILED flash with its own message, never as success.

2. **`mark-valid` moves earlier.** `esp_ota_mark_app_valid_cancel_rollback()` runs
   immediately after `wifi_ap_start()` succeeds — the property rollback protects is "the
   car is reachable" — before the API registrations and before `status_api_start()`'s
   radio-version RPC (which can cost 5 s). Everything after that point must tolerate
   failure without panicking or is a bug on its own.

3. **The radio expected-version is derived, not hand-copied.** The expectation string is
   built at compile time from the esp_hosted component's own version macros
   (`eh_common_fw_version.h`: `PROJECT_VERSION_MAJOR_1/MINOR_1/PATCH_1`), so drift between
   `idf_component.yml` and the check is impossible. `board.h`'s `BOARD_RADIO_SLAVE_FW`
   entry is replaced by a comment pointing at the derivation (board.h stays the place that
   *documents* the board's radio; the value lives with the library that defines it).
   `CONFIG_ESP_HOSTED_HOST_FEAT_OTA=y` is pinned explicitly in `sdkconfig.defaults` with a
   comment (today it holds only via a promptless vendor default).

4. **Forced-update works offline-first.** (a) `AppFlow.offlineFallback` seeds `latestTag`
   from `UpdateClient.cachedTag` when a cached image exists, so `mustUpdate` compares
   against the last-known release without network. (b) `FirmwareView.check()` falls back to
   the cache when `latestRelease()` fails: if `cachedBuild > car build`, offer to flash
   `cachedBinURL` directly (phase flow reuses `.available`→`.downloaded` with the cached
   file; copy may say «из кэша»). (c) `AppFlow.carIdentified` may CLEAR `.updateRequired`
   when `mustUpdate` turns false (the guard keeps its other cases).

5. **An acknowledged upload is a committed flash.** After `upload()` returns true, the
   25 s window confirms; on expiry the phase is a new `.flashed` ("прошито — переподключись
   к машинке для проверки") — success-with-caveat, not `.failed`. `rollback:true` or
   same-fw-after-bounce → `.failed` with the rollback message. Forced mode gains an escape:
   after a failed flash, a «продолжить без обновления» button calls `updateFinished()`.

6. **Downloads are validated.** `download()` checks HTTP status == 200, first byte 0xE9,
   and size ≥ 4096 before moving the file into the cache or recording it; failures return
   nil (no cache mutation). The cache moves from `Caches/` to Application Support
   (excluded from backup), with a one-time migration of an existing cached file.

7. **The mock reports the uploaded image's real version.** After a successful /ota the mock
   parses the `esp_app_desc_t` version string out of the uploaded bytes (fixed offset in
   the ESP image header; verify against IDF layout) and serves THAT as its fw — so
   asset-vs-tag mismatches and rollback rehearsal (`--rollback` flag: after "reboot",
   report the previous fw and rollback:true) become testable in the simulator.

## release.sh

8. Refuse to publish unless `git rev-parse HEAD` equals the head of `origin/main`
   (and pass `--target "$(git rev-parse HEAD)"` anyway, belt-and-braces).
9. Delete `firmware/p4/sdkconfig`/`sdkconfig.old` before the release build (regenerate
   purely from defaults).
10. Parse `--dry-run` anywhere in the argv; run the dry-run branch BEFORE the clean-tree
    check (a dry run is read-only and must work on a dirty tree).
11. Diff the radio pin (`idf_component.yml` esp_hosted version) against the previous
    release tag; if it moved, require an explicit `--radio-bumped` flag and auto-append a
    warning line to the notes ("этот релиз требует стендовой перепрошивки радио C6").
12. Run `./tools/test-all.sh` before building; abort on failure.
13. `version.txt` must be exactly one line (validate); CMake configure re-reads it and the
    commit count on every build (`CMAKE_CONFIGURE_DEPENDS` on version.txt + a build-time
    count check or documented fullclean-only accuracy — the release path already fullcleans;
    the fix targets bench honesty).

## App-side smaller rules

14. `upload()` surfaces the car's error envelope text; FirmwareView shows it in `.failed`.
15. Upload deadline drops to 45 s with a visible cancel; a cancelled/failed upload releases
    nothing app-side (the car's grant self-releases on its own error paths).
16. `AppFlow.startupCheck`'s download-failure path also goes through `offlineFallback`.
17. A missing/malformed `radio.ok` is treated as NOT ok (unknown ≠ healthy); when all radio
    fetch attempts fail, FirmwareView shows «радио: нет данных» instead of hiding the line.
18. The radio-mismatch string drops the repo-README reference; the debug gallery gains a
    phase rendering it (so the string is reachable in rehearsal).

## Docs

19. protocol.md: `/status` gains the `rollback` key with one explanatory sentence.
20. CLAUDE.md Status section: the unwired-bus sentence rewritten to the shipped behavior
    (boots with bus_ok:false, network and OTA up, motors inert); board.h's "wire-flashed
    once" comment corrected (SDIO reflash is the recorded route).
21. firmware/c6/README.md: FEAT_OTA sentence points at the new explicit pin in
    sdkconfig.defaults; flash-radio.sh gains a built-version-vs-pin check (warn+confirm).

## Deliberately NOT in scope

- Monotonic-build-number hardening beyond the push check (history rewrites stay a known
  operational hazard, recorded in release.sh's header comment).
- NVS snapshot/restore across format bumps: reduced to a boot flag — when the erase path
  runs, a one-boot `"nvs_wiped": true` appears in `/status` and the app shows a notice that
  calibration was lost (full snapshot/restore deferred; ledger the deferral).
- The reply-flush-before-restart risk (mitigated by decision 5's app semantics).
- First-run-offline UX text; IDF provenance recording.
