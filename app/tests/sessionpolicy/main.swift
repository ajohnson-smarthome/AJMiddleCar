// Host test for the transport's pure session decisions. Run with swiftc; no XCTest.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// -- handshake filtering: a reply for another sid is a leftover from a previous socket. ----
let sid = "7f3a91c2"
let me = DeviceInfo(id: "ajmiddlecar", fw: "v1.0+517", build: 517, rolled_back: false)
func ack(_ sid: String, proto: Int = 2) -> String {
    #"{"proto":\#(proto),"type":"hello_ack","session":"\#(sid)","device":{"id":"ajmiddlecar","fw":"v1.0+517","build":517,"rolled_back":false}}"#
}
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack(sid)), sid: sid) == .identity(me),
      "our sid's reply is the identity")
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack("deadbeef")), sid: sid) == .ignore,
      "another sid's reply is ignored — ownership is not resumable")
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack(sid, proto: 3)), sid: sid)
        == .identity(DeviceInfo(id: "ajmiddlecar", fw: "v1.0+517", build: 517, rolled_back: false)),
      "a foreign proto is still our car by identity — proto is not judged here")
check(SessionPolicy.handshakeOutcome(RTFrame.parse(ack("deadbeef", proto: 3)), sid: sid) == .ignore,
      "another sid's reply is a leftover regardless of its proto")
let telemetry = Telemetry(proto: 2, seq: 1,
                          link: LinkInfo(rx_hz: 0, rssi_dbm: nil, timeouts: 0),
                          motors: MotorsInfo(bus: .ok, calibrated: true, owner: .idle),
                          system: SystemInfo(uptime_s: 5, free_heap: 1),
                          video: VideoInfo(state: .idle, fps: 0, kbps: 0, dropped: 0),
                          battery: BatteryInfo(voltage_mv: nil, current_ma: nil, power_mw: nil,
                                               soc_pct: nil, state: .absent))
check(SessionPolicy.handshakeOutcome(.telemetry(telemetry), sid: sid) == .ignore,
      "telemetry during the handshake is not an answer")
check(SessionPolicy.handshakeOutcome(nil, sid: sid) == .ignore, "garbage is ignored")

// -- backoff: exponential, capped, and capped LOWER before any car ever answered. ----------
check(SessionPolicy.backoffBase(attempt: 0, pathBlocked: false, everAdopted: true) == 0.1,
      "attempt 0 is the base")
check(SessionPolicy.backoffBase(attempt: 1, pathBlocked: false, everAdopted: true) == 0.1,
      "attempt 1 is the base")
check(SessionPolicy.backoffBase(attempt: 2, pathBlocked: false, everAdopted: true) == 0.2,
      "attempt 2 doubles")
check(SessionPolicy.backoffBase(attempt: 10, pathBlocked: false, everAdopted: true) == 5.0,
      "the cap holds after adoption")
check(SessionPolicy.backoffBase(attempt: 10, pathBlocked: false, everAdopted: false) == 1.0,
      "discovery is capped at one second: a car switched on late is found within a second")
check(SessionPolicy.backoffBase(attempt: 10, pathBlocked: true, everAdopted: false) == 5.0,
      "a blocked path earns the full cap — waiting there costs nothing")

if failures == 0 { print("test_sessionpolicy: OK") } else { exit(1) }
