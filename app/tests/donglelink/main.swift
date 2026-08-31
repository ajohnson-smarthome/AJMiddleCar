// Host test for DongleLink.next(status:latestTag:) — the pure decision behind "find the
// dongle, update it, point it at the car, then drive". Run with swiftc; no XCTest, no simulator.
//
// `sources` lists DongleLink.swift, DongleStatus.swift AND UpdateRules.swift: DongleLink calls
// UpdateRules.mustUpdate directly (it is the module's declared Task 4 dependency, per the task
// brief's own Interfaces line), so it has to be on the compile line for this to link at all.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

/// Builds a `/status` body and decodes it, varying only what each test cares about — every
/// fixture is a full, valid document, because `DongleStatus.parse` throws on a short one and
/// this file is not testing that (donglestatus/main.swift already does).
func status(fw: String, rollback: Bool, ssid: String, state: String, rssi: Int = -50) -> DongleStatus {
    let json = #"""
    {"device":"\#(DongleContract.device)","fw":"\#(fw)","idf":"v6.0.2","usb":"\#(DongleUsbState.up)",
     "rollback":\#(rollback),
     "net":{"ssid":"\#(ssid)","state":"\#(state)","rssi":\#(rssi)}}
    """#
    return try! DongleStatus.parse(Data(json.utf8))
}

let behind = "v1.0+100"   // running build
let latest = "v1.0+200"   // what GitHub says is newest
let current = latest      // a dongle already on the newest build

// -- no dongle answering at all: nil, not an error --------------------------------------
// A wrong implementation that throws, force-unwraps, or otherwise cannot take `status: nil`
// never compiles far enough to run this file at all; one that maps nil to any DongleStep
// other than plugIn (e.g. treats "no answer" as though it were a fresh, unconfigured dongle
// and offers .sendCredentials) fails right here.
check(DongleLink.next(status: nil, latestTag: latest) == .plugIn,
      "no dongle answering maps to plugIn, not an error state or any configured step")

// -- firmware behind latest: update the dongle before anything else ---------------------
// Deliberately paired with a net that is already `connected` with a stored SSID — proving the
// update check runs BEFORE the join questions, not only when the dongle also happens to be
// unconfigured. An implementation that checks `configured`/`net.state` first would return
// .readyForCar here instead, which is exactly the ordering bug the spec calls out: "The dongle
// updates before the car... Settle the pipe before pushing the long transfer down it."
check(DongleLink.next(status: status(fw: behind, rollback: false, ssid: "AJMiddleCar", state: DongleNetState.connected),
                      latestTag: latest) == .updating,
      "a dongle behind the latest release updates first, even if its net is already connected")

// -- current and never configured: send the car's credentials ---------------------------
check(DongleLink.next(status: status(fw: current, rollback: false, ssid: "", state: DongleNetState.idle),
                      latestTag: latest) == .sendCredentials,
      "current firmware, no stored SSID, sends the car's credentials")

// -- configured, net idle or joining: wait, do not re-POST ------------------------------
// `idle` here is the documented edge case (wifi_sta.c: esp_wifi_set_config failed, so the
// state machine never stepped to `.joining`) rather than "never configured" — that path
// already returned above because ssid is non-empty here. A wrong implementation that treats
// any `idle` as "unconfigured" would send credentials again instead of waiting.
check(DongleLink.next(status: status(fw: current, rollback: false, ssid: "AJMiddleCar", state: DongleNetState.idle),
                      latestTag: latest) == .waiting,
      "configured but idle waits rather than re-sending credentials")
check(DongleLink.next(status: status(fw: current, rollback: false, ssid: "AJMiddleCar", state: DongleNetState.joining),
                      latestTag: latest) == .waiting,
      "configured and joining waits, not sendCredentials and not retryJoin")

// -- net failed: the retry step, not the configure step ----------------------------------
// This is where U2 lands: the stored credentials are already correct, so the fix is asking
// the radio to try again, not re-sending the same SSID/password as though nothing were saved.
check(DongleLink.next(status: status(fw: current, rollback: false, ssid: "AJMiddleCar", state: DongleNetState.failed),
                      latestTag: latest) == .retryJoin,
      "a failed join retries the join, it does not re-send credentials")

// -- net connected: hand off to the car's existing gate -----------------------------------
check(DongleLink.next(status: status(fw: current, rollback: false, ssid: "AJMiddleCar", state: DongleNetState.connected),
                      latestTag: latest) == .readyForCar,
      "current, configured, connected hands off to the car")

// -- rollback true: say so, do not re-offer the same update forever ----------------------
// The realistic case: the dongle reverted TO `behind` and `latest` is still the release that
// failed, so mustUpdate is also true here — a version of `next` that checked mustUpdate first
// and rollback second would answer .updating, which is precisely the "offering the same
// update again forever" loop this field exists to break.
check(DongleLink.next(status: status(fw: behind, rollback: true, ssid: "AJMiddleCar", state: DongleNetState.idle),
                      latestTag: latest) == .rolledBack,
      "rollback true reports the rollback instead of re-offering the same update")
// Rollback is reported even where a naive implementation might reach .readyForCar first —
// checked here with a net that is otherwise fully connected, to prove rollback is not merely
// a modifier tucked inside the update branch.
check(DongleLink.next(status: status(fw: behind, rollback: true, ssid: "AJMiddleCar", state: DongleNetState.connected),
                      latestTag: latest) == .rolledBack,
      "rollback true is reported even when the net looks fully connected")

// -- an unknown net state: not-ready, never treated as connected -------------------------
// "rebooting" is deliberately not one of the contract's four states — this is the one fixture
// in the file that must not come from the generated vocabulary, because it is testing what
// happens when the wire outgrows it. A wrong implementation that defaults an unrecognised
// state to "must be fine, treat it as connected" would drive on a dongle that never actually
// finished joining.
check(DongleLink.next(status: status(fw: current, rollback: false, ssid: "AJMiddleCar", state: "rebooting"),
                      latestTag: latest) == .waiting,
      "an unknown net state is treated as not-ready, not as connected")

// -- the function reads its arguments, not a fixed answer --------------------------------
// Same status, two different latestTags either side of the running build: the output must
// differ. Catches an implementation that hardcodes .waiting/.readyForCar and ignores
// latestTag entirely.
let steadyStatus = status(fw: current, rollback: false, ssid: "AJMiddleCar", state: DongleNetState.connected)
check(DongleLink.next(status: steadyStatus, latestTag: latest) != DongleLink.next(status: steadyStatus, latestTag: "v1.0+900"),
      "a newer latestTag changes the answer for the same status")
// No latestTag at all (offline, nothing cached either — GateRule already covers the offline
// fallback that fills this in) must not force an update: mustUpdate(_, latestTag: nil) is
// false by construction, so this dongle proceeds rather than being stuck offering an update
// it has no version to compare against.
check(DongleLink.next(status: status(fw: current, rollback: false, ssid: "", state: DongleNetState.idle),
                      latestTag: nil) == .sendCredentials,
      "no latestTag at all does not force .updating")

if failures == 0 { print("test_donglelink: OK") } else { exit(1) }
