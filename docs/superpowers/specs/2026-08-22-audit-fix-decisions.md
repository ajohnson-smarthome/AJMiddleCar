# Audit-fix decisions — the cross-cutting contract

> **For agentic workers:** this spec records the *decisions* behind the 2026-08-22 audit-fix
> plans (`docs/superpowers/plans/2026-08-22-audit-fixes-*.md`). Where a decision changes wire
> or session behavior it MUST land identically on every side that implements it: firmware
> (`firmware/p4/main/`), mock (`tools/mock_car/`), and the contract documents. The audit that
> produced these findings: 51 confirmed defects, each verified against
> `link-layer-cutover @ 3fa2c0a`.

## Wire & session semantics (firmware ⇄ mock ⇄ docs; app unaffected unless noted)

1. **The sequence gate survives a watchdog trip.** `rt_session_trip` clears only `armed`;
   `have_seq`/`last_seq` persist. The gate resets only on adopt and on bye. Closes the
   post-trip window where one network-delayed pre-dropout duplicate drove the car and
   aborted a retreat. The mock test pinning the old behavior
   (`test_a_trip_clears_the_sequence_gate`) is rewritten to pin the new one. Rationale for
   the old behavior (read the comment at the clearing site before deleting it) is superseded:
   a resuming same-session stream has monotonically newer seqs and passes the kept gate.

2. **A goodbye during a sticky hold (OTA/CALIB) neither steals nor releases the sticky
   grant.** In `on_bye`: when the arbiter's owner is OTA or CALIB, skip the
   `car_stop(LINK_SRC_SAFE)`/`link_release(LINK_SRC_SAFE)` pair entirely — motors are already
   stopped (OTA) or under the wizard's pulse (CALIB). Still: clear breadcrumbs (that is what
   suppresses the retreat), disarm the watchdog, end the session, log one line. The mock's
   bye path gets the same guard; its "a goodbye during an OTA does not wedge the mock" test
   keeps the no-wedge property and additionally asserts OTA's hold survives the goodbye.

3. **Dead-sid memory.** The car remembers the sids of recently ended sessions (ended by
   bye, by eviction, or by idle expiry — rule 4) in a small ring (capacity 4). A hello
   carrying a remembered dead sid is answered but NOT adopted **while a live session
   exists** (classify → reply, like a live-session repeat); when no session is live, any
   hello adopts — refusing there would wedge a client whose session idled out
   mid-handshake, and adopting a stale duplicate displaces nobody (the phantom session
   dies by rule 4). Closes the stale-duplicate-hello eviction of a live driver. The app
   always generates a fresh sid per session, so it never hits this. Mock mirrors both the
   rule and the refinement.

4. **Sessions are mortal.** While the control watchdog is NOT armed (post-trip, or a
   session that never commanded after its handshake), the session ends when strictly more
   than `RT_SESSION_IDLE_MS` = **10000 ms** have passed since its last activity — the last
   accepted command, or the adoption itself when none was ever accepted. (Anchor is last
   activity, NOT the trip: the same clock the watchdog feeds; at exactly 10000 ms the
   session is still alive, strictly greater ends it.) On expiry: ownership cleared,
   telemetry push stops, the sid is recorded dead (rule 3), and the breadcrumb path is
   forgotten — which also aborts a retreat still in flight, exactly as the firmware's
   rt_glue_idle does (a dead driver's path must never keep replaying). A resuming stream must
   re-hello — the app already does after its 3 s stall. The constant joins
   `contract/car-api.json`'s `rt` section as `session_idle_ms` and is generated into all
   four artifacts; no implementation writes 10000 as a literal.

5. **Duplicate top-level keys ⇒ the datagram is dropped.** Firmware detects a second
   top-level occurrence of any key it reads; the mock rejects via
   `json.loads(..., object_pairs_hook=...)`.

6. **Command-frame grammar is JSON, on both sides.** Firmware's scanner matches keys only at
   brace depth 1 and tightens number tokens to JSON grammar: no leading `+`, no bare `.`,
   no leading zeros (`0123`), and `seq`/`proto` must be integer tokens consumed entirely —
   `proto:1.5` is malformed (dropped, no reply; an *integer* proto ≠ ours still gets the
   answered-by-name reply). **Shared pinned frames** — both firmware host tests and mock
   tests must each pin ALL of these, with these exact outcomes:
   - `{"seq":5,"junk":{"t":0.9},"y":0.5}` → dropped (no top-level `t`)
   - `{"seq":7,"t":.5,"y":0}` → dropped (bare `.` mantissa)
   - `{"seq":8,"t":+1,"y":0}` → dropped (leading `+`)
   - `{"seq":9,"t":0.5,"y":0,"t":0.9}` → dropped (duplicate key)
   - `{"proto":1.5,"hello":"abcd1234"}` → dropped, no reply, no adoption
   - `{"seq":10,"t":0.50,"y":-0.25}` → accepted (control)
   - `{"proto":1,"hello":"7f3a91c2"}` → adopted and answered
   - `{"proto":2,"hello":"7f3a91c2"}` → answered by name, not adopted

7. **Config values must be integral.** `cfg_post`, `calib_spin`, `calib_save` reject a
   fractional JSON number with 400 (`valuedouble != valueint` check). The generated
   validator already rejects; the firmware catches up. Exact message strings stay
   implementation-local; the `{"error","field"}` envelope is the contract.

8. **JSON envelope everywhere.** `/calib/spin`, `/calib/save`, `/ota` answer
   `{"ok":true}` on success and `{"error":"…","field":"…"}` (field `""` when the fault is
   body-wide) on errors, `Content-Type: application/json` — replacing plain-text `ok` and
   `httpd_resp_send_err` bodies. The mock already does this. conformance.py starts asserting
   bodies, not just statuses, for these endpoints.

9. **`GET /` answers `"<device> <fw>\n"`** on both implementations (the mock already does);
   the firmware drops its advisory-only line. conformance asserts the device-id prefix.

10. **Hello is answered on every receipt** (repeats included) — the code was right, the
    cutover plan's "once per adopted session" wording is fixed. Acceptor sid grammar
    (1–15 alphanumerics) is documented next to the producer's 8-hex rule.

11. **Eviction notice — deliberately deferred.** Last-hello-wins hijack/oscillation between
    two phones stays as designed; a mock test pins the oscillation, `docs/IDEAS.md` gains
    the eviction-notice idea, the cutover plan records the deferral.

12. **`/status` `rx_fps` semantics = firmware's** per-consumer delta (0 on the first poll
    and after a ≥10 s gap). The mock mirrors `fps_now`'s accumulator design.

## Firmware-local (no wire impact)

- Two-pass PCA9685 writes: falling channels first, then rising; a pair's rise is skipped
  while its mate's fall has not succeeded (shoot-through window).
- Bus-recovery paced by wall time (once per second of continuous failure), not tick count.
- RT grant hold = `RT_WATCHDOG_MS + LINK_TICK_MS` so the trip always precedes the lapse.
- `link_release` results checked at every call site (retry once with a longer take, then
  ESP_LOGE).
- The retreat consumes history: snapshot at trip, then `recovery_forget()` before replay
  begins; per-sample replay duration capped at 250 ms so dead air is never credited.
- Comment contracts in `car.h`/`link.h` rewritten to the real rule: the watchdog is fed by
  every parsed in-session command, accepted or refused; only the breadcrumb is gated.
- `volatile` on `telemetry.c:s_push_seq` and `calibration.c:s_valid`; comment on the radio
  pair's registration-order safety.
- `ramp_set_ms`/save paths return success; `cfg_post` answers 500 when set/save fails.
- `status_get` returns 500 on snprintf overflow (mirror the hello-reply guard);
  `cfg_get` reserves the closing-brace byte.
- `parse_mix` rejects non-finite input (`isfinite`).
- Shared `read_body` (loop until Content-Length) used by cfg_api AND calib_api.
- A host-test seam for `rt_link.c`'s impure glue (adopt / on_bye / check_silence orderings),
  with stubs recording call order — the same rules test_state.py pins on the mock.

## App-local

- Scene-phase lifecycle serialized (single reconciler; start awaits/cancels a pending stop;
  no stop enqueued on the `.inactive` step of a foregrounding); scene restart gated by the
  same `flow.phase` set the root uses.
- `connect()` cancels the socket on the `.failed`/`.waiting` resume paths.
- Launch gate falls through to `.awaitingCar` when GitHub is unreachable but a cached
  firmware exists (`hasCachedFile` + `cachedBuild`).
- Session lifecycle events can never be lost to telemetry backlog (latched session state or
  a lossless lifecycle channel).
- `fetchRadio` retries (3 attempts, 1/2/4 s) and refetches when FirmwareView appears.
- `retryAfterWrongCar` pokes the transport (`retryNow()`) so the 10 s hold aborts.
- `clamp` maps non-finite to 0 in RTFrame AND ControlModel.
- `recompute()`/telemetry assignments guarded by equality before publishing.
- Session-policy decisions extracted into a pure, swiftc-testable module.

## Mock-local

- `/calib/spin`: sleep `CALIB_HOLD_MS`, release the grant, then answer — firmware's order.
- `/ota`: `begin_ota` goes through the arbiter (`_take`) and can answer 500 "actuator
  busy"; the grant is taken BEFORE reading the body; the app-lock is held for the whole
  handler; the image must start with `0xE9`; after `end_ota` the mock simulates the reboot —
  drops the RT owner and suppresses telemetry for 4 s (> the app's 3 s stall) so the
  reconnect delivers the bumped fw in a hello reply.
- `save_calibration` rejects str/bool-typed `pair`/`sign` (mirror `cJSON_IsNumber`).

## Safety net

- `test_gen_contract.py` range guard asserts BOTH bounds with word-boundary regexes.
- New `tools/conformance_rt.py`: real UDP datagrams (hello/repeat/adopt, seq order, seq-less
  goodbye, oversized datagram, the rule-6 pinned frames, telemetry shape) against a given
  host:port; wired into `tools/test-all.sh` against the mock; runnable by hand against
  `192.168.4.1:4210`.

## Documents

- `docs/protocol.md` rewritten to the UDP wire (source: the cutover plan + shipped code;
  the generated endpoints block stays generated).
- `CLAUDE.md`: module list (− ws_control, + rt_link + link, four `*_api`), gotcha 1 reworded.
- Rearchitecture spec's `link_src_t` sketch and the wifi-pinned spec stamped superseded.
- Cutover plan: hello-reply wording, sid grammar, rules 1–4 and 11 recorded.
