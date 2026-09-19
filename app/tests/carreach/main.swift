// Host test for CarReach — reaching the car through the adapter: the adapter's network state
// machine with the join budget. Pure over the adapter's /status reply; the POSTs it asks for
// are the caller's to send. Run with swiftc.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

func status(ssid: String, state: String) -> DongleStatusReply {
    let json = #"""
    {"proto":1,"usb":{"state":"up"},
     "wifi":{"ssid":"\#(ssid)","configured":\#(!ssid.isEmpty),"state":"\#(state)","rssi_dbm":-50,"channel":1,
             "attempts":{"used":0,"max":5}},
     "relay":{"to_car_hz":0.0,"to_phone_hz":0.0,"udp_sessions":0,"tcp_connections":0,"last_error":null},
     "system":{"uptime_s":1,"free_heap":1,"idf":"v6.0.2"}}
    """#
    return .status(try! JSONDecoder().decode(DongleStatus.self, from: Data(json.utf8)))
}
let car = CarContract.ssid

// A cold start: not configured → send the credentials, show "sending", spend the one budget.
var r = CarReach()
var out = r.next(status(ssid: "", state: "idle"), expectedSSID: car)
check(out.reach == .hold(.sendingNetwork) && out.ask == .configure, "cold: send credentials")
// The next poll still shows no network: the POST never landed (lost on USB, the adapter
// re-enumerating at that instant). A request the adapter never took is not a spent request —
// it has no network to fail on — so the hand-over repeats, «передаю сеть» again, not «не
// удалось подключиться» about a join that never happened (AJM-126).
out = r.next(status(ssid: "", state: "idle"), expectedSSID: car)
check(out.reach == .hold(.sendingNetwork) && out.ask == .configure, "lost POST: hand the network over again, not join-failed")
out = r.next(status(ssid: "", state: "idle"), expectedSSID: car)
check(out.reach == .hold(.sendingNetwork) && out.ask == .configure, "lost twice: still handing over")
// Once the adapter shows the network, the one request IS spent: the adapter's own budget runs,
// and its `failed` is «не удалось подключиться» with no second request.
check(r.next(status(ssid: car, state: "searching"), expectedSSID: car).reach == .hold(.searching), "taken: searching")
out = r.next(status(ssid: car, state: "failed"), expectedSSID: car)
check(out.reach == .hold(.joinFailed) && out.ask == nil, "taken and failed: join failed, no second request")

// Association steps show without spending anything.
var r2 = CarReach()
check(r2.next(status(ssid: car, state: "searching"), expectedSSID: car).reach == .hold(.searching), "searching")
check(r2.next(status(ssid: car, state: "joining"), expectedSSID: car).reach == .hold(.joining), "joining")
let ok = r2.next(status(ssid: car, state: "connected"), expectedSSID: car)
check(ok.reach == .reached && ok.ask == nil, "connected: reached")
check(r2.attempts == 0 && !r2.gaveUp, "connected resets the budget")

// A radio that reports failed: retry once (still "searching", not "no link"), then hold failed.
var r3 = CarReach()
let retry = r3.next(status(ssid: car, state: "failed"), expectedSSID: car)
check(retry.reach == .hold(.searching) && retry.ask == .retry, "failed: retry, shown as searching")
check(r3.next(status(ssid: car, state: "failed"), expectedSSID: car).reach == .hold(.joinFailed), "failed again: join failed")
r3.retry()
check(r3.next(status(ssid: car, state: "failed"), expectedSSID: car).ask == .retry, "retry() gives a fresh budget")

// The adapter itself gone or refusing is the adapter stage's verdict, not the car's — hand back.
var ra = CarReach()
check(ra.next(.silent, expectedSSID: car).reach == .lost, "adapter silent: lost")
var rb = CarReach()
check(rb.next(.faulty, expectedSSID: car).reach == .lost, "adapter faulty: lost")
var rc = CarReach()
check(rc.next(.denied, expectedSSID: car).reach == .lost, "adapter denied: lost")

if failures == 0 { print("carreach: all checks passed") } else { print("carreach: \(failures) FAILED"); exit(1) }
