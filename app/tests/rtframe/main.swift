// Host test for the real-time wire. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// The three datagrams the app sends, exactly as the car parses them.
check(RTFrame.hello(sid: "7f3a91c2") == #"{"proto":2,"type":"hello","session":"7f3a91c2"}"#, "hello")
check(RTFrame.command(seq: 1234, throttle: 0.5, turn: -0.25)
        == #"{"proto":2,"type":"drive","seq":1234,"throttle":0.50,"turn":-0.25}"#, "drive")
check(RTFrame.command(seq: 0, throttle: 2, turn: -2)
        == #"{"proto":2,"type":"drive","seq":0,"throttle":1.00,"turn":-1.00}"#, "drive clamps")
check(RTFrame.command(seq: 1, throttle: .nan, turn: .infinity)
        == #"{"proto":2,"type":"drive","seq":1,"throttle":0.00,"turn":0.00}"#, "non-finite is a stop")
check(RTFrame.bye(seq: 1235) == #"{"proto":2,"type":"bye","seq":1235}"#, "bye")

// The widest drive the app can emit stays under the car's command cap.
check(RTFrame.command(seq: .max, throttle: -1, turn: -1).utf8.count <= CarContract.maxCommand,
      "the widest drive fits max_command")

// Every app->car datagram except hello carries seq — a goodbye included.
for seq in [UInt32(0), 1, 1235, .max] {
    check(RTFrame.bye(seq: seq).contains("\"\(CarContract.seqField)\":\(seq)}"), "bye carries its seq")
    check(RTFrame.command(seq: seq, throttle: 0, turn: 0).contains("\"\(CarContract.seqField)\":\(seq),"),
          "drive carries its seq")
}
check(!RTFrame.hello(sid: "7f3a91c2").contains(CarContract.seqField), "hello carries no seq")
var byeSeq = UInt32(7)
var seen: [UInt32] = []
for _ in 0..<3 { byeSeq = RTFrame.nextSeq(byeSeq); seen.append(byeSeq) }
check(seen == [8, 9, 10], "repeated goodbyes advance the sequence")

// Decimal point, two decimals, whatever the phone's locale.
check(RTFrame.command(seq: 1, throttle: 0.5, turn: 0.25).contains(#""throttle":0.50"#), "decimal point")
check(RTFrame.command(seq: 1, throttle: 0.5, turn: 0.25).contains(#""turn":0.25"#), "two decimals")

// -- inbound ----------------------------------------------------------------------
let ack = #"{"proto":2,"type":"hello_ack","session":"7f3a91c2","device":{"id":"ajmiddlecar","fw":"v1.0+784","build":784,"rolled_back":false}}"#
if case .helloReply(let sid, let device)? = RTFrame.parse(ack) {
    check(sid == "7f3a91c2", "ack sid")
    check(device == DeviceInfo(id: "ajmiddlecar", fw: "v1.0+784", build: 784, rolled_back: false), "ack device")
} else { check(false, "hello_ack parses") }

check(RTFrame.parse(#"{"proto":3,"type":"hello_ack","session":"7f3a91c2","device":{"id":"ajmiddlecar","fw":"v9","build":9,"rolled_back":false}}"#)
        == .protoMismatch(sid: "7f3a91c2", theirs: 3), "a foreign proto is reported by name")
check(RTFrame.parse(#"{"type":"hello_ack","session":"7f3a91c2"}"#) == .protoMismatch(sid: "7f3a91c2", theirs: 0),
      "no proto reads as proto 0")
check(RTFrame.parse(#"{"proto":2,"type":"hello_ack","session":"7f3a91c2"}"#) == nil,
      "an ack without its device group is not an identity")
// The v1 reply, as an old car would send it: not an ack at all — it has no type.
check(RTFrame.parse(#"{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+784"}"#) == nil,
      "a v1 reply is ignored")

let tele = #"{"proto":2,"type":"telemetry","seq":88,"link":{"rx_hz":10,"rssi_dbm":-58,"timeouts":0},"motors":{"bus":"ok","calibrated":true,"owner":"remote"},"system":{"uptime_s":812,"free_heap":200000},"video":{"state":"idle","fps":0,"kbps":0,"dropped":0}}"#
if case .telemetry(let t)? = RTFrame.parse(tele) {
    check(t.seq == 88 && t.link.rx_hz == 10 && t.link.rssi_dbm == -58 && t.link.timeouts == 0, "telemetry link")
    check(t.motors.bus == .ok && t.motors.calibrated && t.motors.owner == .remote, "telemetry motors")
    check(t.system.uptime_s == 812 && t.system.free_heap == 200000, "telemetry system")
} else { check(false, "telemetry parses") }
if case .telemetry(let t)? = RTFrame.parse(#"{"proto":2,"type":"telemetry","seq":1,"link":{"rx_hz":0,"rssi_dbm":null,"timeouts":3},"motors":{"bus":"down","calibrated":false,"owner":"hover"},"system":{"uptime_s":1,"free_heap":1},"video":{"state":"idle","fps":0,"kbps":0,"dropped":0}}"#) {
    check(t.link.rssi_dbm == nil, "null rssi is nil")
    check(t.motors.bus == .down, "bus down")
    check(t.motors.owner == .unknown("hover"), "an owner word this build does not know is kept, not dropped")
} else { check(false, "telemetry with nulls parses") }
check(RTFrame.parse(#"{"proto":1,"type":"telemetry","seq":1,"link":{"rx_hz":0,"rssi_dbm":null,"timeouts":0},"motors":{"bus":"ok","calibrated":true,"owner":"idle"},"system":{"uptime_s":1,"free_heap":1}}"#) == nil,
      "telemetry in a foreign proto is dropped")
check(RTFrame.parse(#"{"proto":2,"type":"telemetry","seq":1}"#) == nil, "telemetry without its groups is dropped")
check(RTFrame.parse(#"{"proto":2,"type":"drive","seq":1,"throttle":0,"turn":0}"#) == nil, "our own datagram types are not inbound")
check(RTFrame.parse("not json") == nil, "junk")

// The car's telemetry counter wraps like ours.
check(RTFrame.seqNewer(1, than: 0) && !RTFrame.seqNewer(0, than: 1) && RTFrame.seqNewer(0, than: Int(UInt32.max)),
      "telemetry seq ordering wraps")
check(RTFrame.sessionID(0xdeadbeef) == "deadbeef" && RTFrame.sessionID(1) == "00000001", "session id is 8 hex chars")

if failures == 0 { print("test_rtframe: OK") } else { exit(1) }
