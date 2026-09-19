// Host test for SearchGuard — the third post-gate guard: a search that has outlasted the
// adapter's join budget is a question for the adapter (its /status), and an adapter that gave
// up (failed / idle, or never given the car's network) gets the network handed over again —
// after the gate by restarting the ladder at the car's rung, under the forced-update screen by
// a POST /wifi. Pure over the elapsed time and the adapter's reply. Run with swiftc.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

func status(ssid: String, state: String) -> DongleStatusReply {
    let json = #"""
    {"proto":1,"usb":{"state":"up"},
     "wifi":{"ssid":"\#(ssid)","configured":\#(!ssid.isEmpty),"state":"\#(state)","rssi_dbm":-50,"channel":1,
             "attempts":{"used":5,"max":5}},
     "relay":{"to_car_hz":0.0,"to_phone_hz":0.0,"udp_sessions":0,"tcp_connections":0,"last_error":null},
     "system":{"uptime_s":1,"free_heap":1,"idf":"v6.0.2"}}
    """#
    return .status(try! JSONDecoder().decode(DongleStatus.self, from: Data(json.utf8)))
}
let car = CarContract.ssid

// The clock: the adapter is not second-guessed while it may still be within its own budget —
// five attempts, ~10 s on the bench. The threshold clears that with a margin.
check(SearchGuard.threshold > 10, "threshold clears the adapter's five-attempt budget (~10 s)")
check(!SearchGuard.due(searchingFor: 0), "just started: not due")
check(!SearchGuard.due(searchingFor: SearchGuard.threshold - 0.1), "under the threshold: not due")
check(SearchGuard.due(searchingFor: SearchGuard.threshold), "at the threshold: due")
check(SearchGuard.due(searchingFor: SearchGuard.threshold * 3), "long after: still due")

// The adapter gave up with the car's network in place: hand it over again (a retry).
check(SearchGuard.verdict(status(ssid: car, state: "failed"), expectedSSID: car) == .handNetwork(.retry),
      "failed with our network: retry")
check(SearchGuard.verdict(status(ssid: car, state: "idle"), expectedSSID: car) == .handNetwork(.retry),
      "idle with our network: retry (IDLE's one exit is a POST /wifi)")
// The adapter does not have the car's network at all — it re-enumerated, or was told another
// one: it will not find the car by itself either, so the network is handed over (configure).
check(SearchGuard.verdict(status(ssid: "", state: "idle"), expectedSSID: car) == .handNetwork(.configure),
      "no network: configure")
check(SearchGuard.verdict(status(ssid: "other", state: "connected"), expectedSSID: car) == .handNetwork(.configure),
      "someone else's network, even connected: configure")

// The adapter is still working, or already there: never restarted out from under it.
check(SearchGuard.verdict(status(ssid: car, state: "searching"), expectedSSID: car) == .wait, "searching: wait")
check(SearchGuard.verdict(status(ssid: car, state: "joining"), expectedSSID: car) == .wait, "joining: wait")
check(SearchGuard.verdict(status(ssid: car, state: "connected"), expectedSSID: car) == .wait,
      "connected: wait — the car is behind a joined adapter; the hello will come or the car is booting")
check(SearchGuard.verdict(status(ssid: car, state: "unknown"), expectedSSID: car) == .wait,
      "a state this build does not know: wait")

// The adapter itself silent, faulty or denied is the wire guards' business, not this one's —
// a transient /status blip must not restart anything.
check(SearchGuard.verdict(.silent, expectedSSID: car) == .wait, "adapter silent: wait")
check(SearchGuard.verdict(.faulty, expectedSSID: car) == .wait, "adapter faulty: wait")
check(SearchGuard.verdict(.denied, expectedSSID: car) == .wait, "adapter denied: wait")

if failures == 0 { print("searchguard: all checks passed") } else { print("searchguard: \(failures) FAILED"); exit(1) }
