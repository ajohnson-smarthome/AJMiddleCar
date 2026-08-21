// Host test for the real-time wire. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// The command frame, exactly as the car parses it.
check(RTFrame.command(seq: 1234, t: 0.5, y: -0.25) == #"{"seq":1234,"t":0.50,"y":-0.25}"#,
      "command frame")
check(RTFrame.command(seq: 0, t: 2, y: -2) == #"{"seq":0,"t":1.00,"y":-1.00}"#, "command clamps")
check(RTFrame.hello(sid: "7f3a91c2") == #"{"proto":1,"hello":"7f3a91c2"}"#, "hello frame")
check(RTFrame.bye(seq: 1235) == #"{"seq":1235,"t":0.00,"y":0.00,"bye":1}"#, "bye frame")

// Two decimals with a period, whatever the phone's language does to numbers: `String(format:)`
// with no locale formats in the C locale, and a comma here would desync the car's parser on
// exactly the phones this app is written for.
check(RTFrame.command(seq: 1, t: 0.5, y: 0.25).contains(#""t":0.50"#), "decimal point, not comma")
check(RTFrame.command(seq: 1, t: 0.5, y: 0.25).contains(#""y":0.25"#), "two decimals")

// Every frame fits the contract's datagram cap.
for seq in [UInt32(0), 1234, .max] {
    check(RTFrame.command(seq: seq, t: -1, y: -1).utf8.count <= CarContract.maxDatagram,
          "command within the datagram cap")
    check(RTFrame.bye(seq: seq).utf8.count <= CarContract.maxDatagram, "bye within the cap")
}

// seq is a uint32 the car compares as (int32_t)(seq - last) > 0, so wrapping is correct and
// needs no reset on either side.
check(RTFrame.nextSeq(0) == 1, "seq increments")
check(RTFrame.nextSeq(UInt32.max) == 0, "seq wraps to zero")
check(Int32(bitPattern: RTFrame.nextSeq(UInt32.max) &- UInt32.max) > 0, "wrapped seq reads as newer")
check(RTFrame.command(seq: UInt32.max, t: 0, y: 0) == #"{"seq":4294967295,"t":0.00,"y":0.00}"#,
      "seq at the top of the range is unsigned")

// Session ids are 8 hex characters.
check(RTFrame.sessionID(0x7f3a91c2) == "7f3a91c2", "session id")
check(RTFrame.sessionID(0xf) == "0000000f", "session id is padded to eight")

// The hello reply carries identity; a foreign protocol is refused rather than mis-parsed.
if case .helloReply(let sid, let device, let fw)? =
    RTFrame.parse(#"{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+517"}"#) {
    check(sid == "7f3a91c2" && device == "ajmiddlecar" && fw == "v1.0+517", "hello reply")
} else {
    check(false, "hello reply parses")
}
check(RTFrame.parse(#"{"proto":2,"hello":"7f3a91c2","device":"ajmiddlecar"}"#) == nil,
      "unknown proto refused")

// Telemetry, by the generated field names.
if case .telemetry(let t)? = RTFrame.parse(
    #"{"seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,"calibrated":true,"bus_ok":true,"ctl":"rt"}"#) {
    check(t.rxFps == 10 && t.rssi == -58 && t.uptimeS == 812, "telemetry numbers")
    check(t.calibrated == true && t.busOk == true && t.ctl == "rt", "telemetry flags")
} else {
    check(false, "telemetry parses")
}
// 0 means "unavailable" on the car's side, not "very strong".
check(RTFrame.parse(#"{"uptime_s":1,"rssi":0}"#).flatMap { if case .telemetry(let t) = $0 { return t.rssi } else { return nil } } == nil,
      "rssi 0 is absent")
check(RTFrame.parse("nope") == nil, "non-JSON refused")
check(RTFrame.parse(#"{"foo":1}"#) == nil, "unrecognisable object refused")

if failures == 0 { print("test_rtframe: OK") } else { exit(1) }
