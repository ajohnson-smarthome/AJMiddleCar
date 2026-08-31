// Host test for DongleLink.next(reply:latestTag:expectedSSID:rollbackAcknowledged:) and
// DongleReply.of(_:) — the pure decisions behind "find the dongle, update it, point it at the
// car, then drive". Run with swiftc; no XCTest, no simulator.
//
// `sources` lists DongleLink.swift, DongleStatus.swift, UpdateRules.swift AND CarError.swift:
// DongleLink calls UpdateRules.mustUpdate directly (its declared Task 4 dependency), and
// DongleReply.of classifies the transport's own error vocabulary, so both have to be on the
// compile line for this to link at all.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// `DongleReply` carries a `DongleStatus`, which is not Equatable (it is a decoded document, not
// a value to compare), so the classification checks below ask which case it is rather than
// comparing whole replies.
extension DongleReply {
    var isFaulty: Bool { if case .faulty = self { return true }; return false }
    var isSilent: Bool { if case .silent = self { return true }; return false }
    var isDenied: Bool { if case .denied = self { return true }; return false }
}

/// Builds a `/status` body, decodes it and wraps it as the reply `next` takes — varying only
/// what each test cares about. Every fixture is a full, valid document, because
/// `DongleStatus.parse` throws on a short one and this file is not testing that
/// (donglestatus/main.swift already does). `device` defaults to the generated contract's own
/// value, so only the identity test below has to name one.
func reply(fw: String, rollback: Bool, ssid: String, state: String, rssi: Int = -50,
           device: String = DongleContract.device) -> DongleReply {
    let json = #"""
    {"device":"\#(device)","fw":"\#(fw)","idf":"v6.0.2","usb":"\#(DongleUsbState.up)",
     "rollback":\#(rollback),
     "net":{"ssid":"\#(ssid)","state":"\#(state)","rssi":\#(rssi)}}
    """#
    return .status(try! DongleStatus.parse(Data(json.utf8)))
}

let behind = "v1.0+100"   // running build
let latest = "v1.0+200"   // what GitHub says is newest
let current = latest      // a dongle already on the newest build
// The generated contract's real SSID, not a hand-spelled literal that happens to match it — a
// hardcoded stand-in is exactly what let an earlier credential sweep miss a stale string
// elsewhere in this app (wrongCar.hint), so this file uses the constant throughout.
let carSSID = CarContract.ssid

// -- no dongle answering at all: nil, not an error --------------------------------------
// A wrong implementation that throws, force-unwraps, or otherwise cannot take `reply: .silent`
// never compiles far enough to run this file at all; one that maps nil to any DongleStep
// other than plugIn (e.g. treats "no answer" as though it were a fresh, unconfigured dongle
// and offers .sendCredentials) fails right here.
check(DongleLink.next(reply: .silent, latestTag: latest, expectedSSID: carSSID) == .plugIn,
      "no dongle answering maps to plugIn, not an error state or any configured step")

// -- something answered, and it was not usable: not "plug one in" -----------------------
// The distinction this branch exists to make. A dongle that returns HTTP 500, or a body this
// build cannot decode, is plugged in and talking — telling the user to plug one in is then the
// one sentence on screen that is definitely false, and it points a bench operator at the cable
// while the fault is in the firmware. An implementation that maps every non-answer to .plugIn
// (which is what `try?` produced) fails here.
check(DongleLink.next(reply: .faulty, latestTag: latest, expectedSSID: carSSID) == .faulty,
      "a dongle that answered with an error is not the plug-it-in step")
check(DongleLink.next(reply: .denied, latestTag: latest, expectedSSID: carSSID) == .accessDenied,
      "local-network denial is its own step, not plugIn and not a wait")
check(DongleLink.next(reply: .silent, latestTag: latest, expectedSSID: carSSID) !=
      DongleLink.next(reply: .faulty, latestTag: latest, expectedSSID: carSSID),
      "silence and a bad answer do not land on the same screen")

// -- classifying what the client threw ---------------------------------------------------
// `DongleReply.of` is the rule the flow applies before asking `next` anything, and it is here
// rather than in the flow so it is tested. The two halves that matter: bytes that arrived are
// a device answering, and everything else is not evidence of one.
check(DongleReply.of(CarError.http(status: 500, body: Data())).isFaulty,
      "an HTTP status from the dongle is an answer, not silence")
check(DongleReply.of(CarError.truncated(got: 3, want: 99)).isFaulty,
      "a truncated body is an answer, not silence")
check(DongleReply.of(DecodingError.dataCorrupted(.init(codingPath: [], debugDescription: "x"))).isFaulty,
      "a body that would not decode is an answer, not silence")
check(DongleReply.of(CarError.denied).isDenied, "a denial is a denial, not silence")
check(DongleReply.of(CarError.timeout(3)).isSilent, "a timeout is silence")
check(DongleReply.of(CarError.refused).isSilent, "a refused connection is silence")
check(DongleReply.of(CarError.noDongle(.notAvailable)).isSilent, "an unsatisfied path is silence")
check(DongleReply.of(CarError.malformed("no parseable response head")).isSilent,
      "a connection that closed without a head is silence, not proof something answered")
check(DongleReply.of(CancellationError()).isSilent, "a cancelled read says nothing about the dongle")

// -- a foreign adapter answering: its own step, not a dongle to drive ---------------------
// The spec's stated purpose for `status.device`. Deliberately paired with a document that is
// otherwise perfect — current firmware, the car's own SSID, connected — so an implementation
// that never compares the field would answer .readyForCar and drive the car through somebody
// else's hardware.
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: carSSID,
                                   state: DongleNetState.connected, device: "some-other-adapter"),
                      latestTag: latest, expectedSSID: carSSID) == .wrongDongle(device: "some-other-adapter"),
      "an adapter that is not ours is named, not driven")
// And it is answered BEFORE anything in the document is acted on: a foreign device's fw and
// rollback flag describe a device this app knows nothing about, so neither an update nor a
// rollback report may be reached through them.
check(DongleLink.next(reply: reply(fw: behind, rollback: true, ssid: "", state: DongleNetState.idle,
                                   device: "some-other-adapter"),
                      latestTag: latest, expectedSSID: carSSID) == .wrongDongle(device: "some-other-adapter"),
      "identity is checked before the update, the rollback and the credentials")

// -- firmware behind latest: update the dongle before anything else ---------------------
// Deliberately paired with a net that is already `connected` with the car's own stored SSID —
// proving the update check runs BEFORE the join questions, not only when the dongle also
// happens to be unconfigured. An implementation that checks `configured`/`net.state` first
// would return .readyForCar here instead, which is exactly the ordering bug the spec calls
// out: "The dongle updates before the car... Settle the pipe before pushing the long transfer
// down it."
check(DongleLink.next(reply: reply(fw: behind, rollback: false, ssid: carSSID, state: DongleNetState.connected),
                      latestTag: latest, expectedSSID: carSSID) == .updating,
      "a dongle behind the latest release updates first, even if its net is already connected")

// -- current and never configured: send the car's credentials ---------------------------
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: "", state: DongleNetState.idle),
                      latestTag: latest, expectedSSID: carSSID) == .sendCredentials,
      "current firmware, no stored SSID, sends the car's credentials")

// -- current but pointed at the WRONG network: still send the car's credentials ---------
// A non-empty SSID that disagrees with the car's own is not "configured" from the app's seat —
// it is a dongle left joined to a stale bench network, or a previous car. A wrong
// implementation that checks only `!ssid.isEmpty` would answer .waiting or .readyForCar here,
// leaving a mis-pointed dongle stuck forever with no way to re-point it (this is where a
// mis-pointed dongle used to dead-end).
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: "someOtherNetwork", state: DongleNetState.connected),
                      latestTag: latest, expectedSSID: carSSID) == .sendCredentials,
      "a dongle connected to the wrong network is re-pointed, not treated as ready")

// -- configured, net joining: wait, do not re-POST ---------------------------------------
// The radio is working through its own bounded budget; nothing has failed yet, so a re-POST
// would only restart a budget that is already running.
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: carSSID, state: DongleNetState.joining),
                      latestTag: latest, expectedSSID: carSSID) == .waiting,
      "configured and joining waits, not sendCredentials and not retryJoin")

// -- configured, net idle: ask again — waiting here waits forever ------------------------
// `idle` on a dongle whose stored SSID is the car's own is not "never configured" (that path
// returns .sendCredentials above, and the fixture below proves the difference: same state,
// different ssid). It is the edge wifi_sta.c documents: esp_wifi_set_config failed, so
// wifi_sta_join returned early and left the state machine in IDLE while GET /net already
// reports the network it was told. IDLE's only exit is WIFI_EV_CONFIGURED (wifi_state.c),
// which only a POST /net raises — so an implementation that answers .waiting here (as this
// one did) parks the app on a "connecting" screen that nothing on the dongle will ever end.
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: carSSID, state: DongleNetState.idle),
                      latestTag: latest, expectedSSID: carSSID) == .retryJoin,
      "configured but idle asks the radio again — nothing else can leave IDLE")
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: "", state: DongleNetState.idle),
                      latestTag: latest, expectedSSID: carSSID) == .sendCredentials,
      "an idle dongle with no stored SSID is still the configure step, not the retry step")

// -- net failed: the retry step, not the configure step ----------------------------------
// This is where U2 lands: the stored credentials are already correct, so the fix is asking
// the radio to try again, not re-sending the same SSID/password as though nothing were saved.
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: carSSID, state: DongleNetState.failed),
                      latestTag: latest, expectedSSID: carSSID) == .retryJoin,
      "a failed join retries the join, it does not re-send credentials")

// -- net connected: hand off to the car's existing gate -----------------------------------
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: carSSID, state: DongleNetState.connected),
                      latestTag: latest, expectedSSID: carSSID) == .readyForCar,
      "current, configured for the car's own network, connected hands off to the car")

// -- rollback true: say so, do not re-offer the same update forever ----------------------
// The realistic case: the dongle reverted TO `behind` and `latest` is still the release that
// failed, so mustUpdate is also true here — a version of `next` that checked mustUpdate first
// and rollback second would answer .updating, which is precisely the "offering the same
// update again forever" loop this field exists to break.
check(DongleLink.next(reply: reply(fw: behind, rollback: true, ssid: carSSID, state: DongleNetState.idle),
                      latestTag: latest, expectedSSID: carSSID) == .rolledBack,
      "rollback true reports the rollback instead of re-offering the same update")
// Rollback is reported even where a naive implementation might reach .readyForCar first —
// checked here with a net that is otherwise fully connected, to prove rollback is not merely
// a modifier tucked inside the update branch.
check(DongleLink.next(reply: reply(fw: behind, rollback: true, ssid: carSSID, state: DongleNetState.connected),
                      latestTag: latest, expectedSSID: carSSID) == .rolledBack,
      "rollback true is reported even when the net looks fully connected")

// -- rollback acknowledged: the app is not permanently bricked ---------------------------
// The bootloader's rollback flag is sticky across every reboot until a LATER OTA to that slot
// succeeds (status_api.c reads it once at boot from the other partition's state) — so without
// an acknowledgement path, one failed OTA would report .rolledBack forever and never let the
// flow reach the car again, on the SAME firmware it was happily running before the update was
// ever offered. Once the user has been told and chooses to proceed, `next` must stop reporting
// it and must not silently re-offer the failed update either.
check(DongleLink.next(reply: reply(fw: behind, rollback: true, ssid: carSSID, state: DongleNetState.connected),
                      latestTag: latest, expectedSSID: carSSID, rollbackAcknowledged: true) == .readyForCar,
      "an acknowledged rollback proceeds to the car instead of reporting rollback forever")
check(DongleLink.next(reply: reply(fw: behind, rollback: true, ssid: "", state: DongleNetState.idle),
                      latestTag: latest, expectedSSID: carSSID, rollbackAcknowledged: true) == .sendCredentials,
      "an acknowledged rollback still needs credentials sent if it never got any")
check(DongleLink.next(reply: reply(fw: behind, rollback: true, ssid: carSSID, state: DongleNetState.connected),
                      latestTag: latest, expectedSSID: carSSID, rollbackAcknowledged: false) == .rolledBack,
      "rollbackAcknowledged defaults to false — a caller that forgets to pass it still sees rolledBack")

// -- an unknown net state: not-ready, never treated as connected -------------------------
// "rebooting" is deliberately not one of the contract's four states — this is the one fixture
// in the file that must not come from the generated vocabulary, because it is testing what
// happens when the wire outgrows it. A wrong implementation that defaults an unrecognised
// state to "must be fine, treat it as connected" would drive on a dongle that never actually
// finished joining.
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: carSSID, state: "rebooting"),
                      latestTag: latest, expectedSSID: carSSID) == .waiting,
      "an unknown net state is treated as not-ready, not as connected")

// -- the function reads its arguments, not a fixed answer --------------------------------
// Same status, two different latestTags either side of the running build: the output must
// differ. Catches an implementation that hardcodes .waiting/.readyForCar and ignores
// latestTag entirely.
let steadyStatus = reply(fw: current, rollback: false, ssid: carSSID, state: DongleNetState.connected)
check(DongleLink.next(reply: steadyStatus, latestTag: latest, expectedSSID: carSSID) !=
      DongleLink.next(reply: steadyStatus, latestTag: "v1.0+900", expectedSSID: carSSID),
      "a newer latestTag changes the answer for the same status")
// No latestTag at all (offline, nothing cached either — GateRule already covers the offline
// fallback that fills this in) must not force an update: mustUpdate(_, latestTag: nil) is
// false by construction, so this dongle proceeds rather than being stuck offering an update
// it has no version to compare against.
check(DongleLink.next(reply: reply(fw: current, rollback: false, ssid: "", state: DongleNetState.idle),
                      latestTag: nil, expectedSSID: carSSID) == .sendCredentials,
      "no latestTag at all does not force .updating")
// Same status, two different expectedSSIDs either side of the stored one: the output must
// differ. Catches an implementation that ignores expectedSSID entirely (Important 4's own
// case above already proves the mismatch path exists; this proves the parameter is what
// drives it, not the fixture's absolute string).
check(DongleLink.next(reply: steadyStatus, latestTag: latest, expectedSSID: carSSID) !=
      DongleLink.next(reply: steadyStatus, latestTag: latest, expectedSSID: "someOtherNetwork"),
      "a different expectedSSID changes the answer for the same status")

if failures == 0 { print("test_donglelink: OK") } else { exit(1) }
