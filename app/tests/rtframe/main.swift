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

// Every app->car datagram except `hello` carries `seq`, because the car DROPS one that does not
// — goodbye included. A bye without a sequence number is not a quiet nonconformance: the car never
// hears the stop, notices the silence 300 ms later, and retreats along its own breadcrumbs with
// the controls off-screen. There is no other way to build one — `bye` and `command` both take it —
// and this pins that there never is.
for seq in [UInt32(0), 1, 1235, .max] {
    check(RTFrame.bye(seq: seq).contains("\"\(CarContract.seqField)\":\(seq),"),
          "bye carries its seq")
    check(RTFrame.command(seq: seq, t: 0, y: 0).contains("\"\(CarContract.seqField)\":\(seq),"),
          "command carries its seq")
}
// `hello` is the one exemption, and it is the car's, not ours: it is the datagram that resets the
// sequence gate, so it cannot be judged by it.
check(!RTFrame.hello(sid: "7f3a91c2").contains(CarContract.seqField), "hello carries no seq")
// Successive byes are successive numbers, so a repeat cannot be dropped as a replay of the first.
var byeSeq = UInt32(7)
var seen: [UInt32] = []
for _ in 0..<3 { byeSeq = RTFrame.nextSeq(byeSeq); seen.append(byeSeq) }
check(seen == [8, 9, 10], "repeated goodbyes advance the sequence")

// Two decimals with a period, whatever the phone's language does to numbers: `String(format:)`
// with no locale formats in the C locale, and a comma here would desync the car's parser on
// exactly the phones this app is written for.
check(RTFrame.command(seq: 1, t: 0.5, y: 0.25).contains(#""t":0.50"#), "decimal point, not comma")
check(RTFrame.command(seq: 1, t: 0.5, y: 0.25).contains(#""y":0.25"#), "two decimals")

// Every frame the app sends fits the cap the CAR enforces on what it accepts — `max_command`,
// which is smaller than the `max_datagram` receive buffer that sizes the other direction.
check(CarContract.maxCommand <= CarContract.maxDatagram, "the command cap is the tighter one")
for seq in [UInt32(0), 1234, .max] {
    check(RTFrame.command(seq: seq, t: -1, y: -1).utf8.count <= CarContract.maxCommand,
          "command within the command cap")
    check(RTFrame.bye(seq: seq).utf8.count <= CarContract.maxCommand, "bye within the command cap")
}
check(RTFrame.hello(sid: "7f3a91c2").utf8.count <= CarContract.maxCommand, "hello within the cap")

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
// A protocol mismatch is REPORTED, not swallowed: the car and the mock both answer a hello they
// cannot serve so the app can say "this car speaks a protocol I do not" instead of searching
// forever. Returning nil here is what turns a flashed-but-not-updated pair into an endless radar.
check(RTFrame.parse(#"{"proto":2,"hello":"7f3a91c2","device":"ajmiddlecar"}"#)
        == .protoMismatch(sid: "7f3a91c2", theirs: 2),
      "unknown proto is reported by name")
// A hello reply with no proto at all is equally unusable, and equally reportable.
check(RTFrame.parse(#"{"hello":"7f3a91c2","device":"ajmiddlecar"}"#)
        == .protoMismatch(sid: "7f3a91c2", theirs: 0),
      "a missing proto is a mismatch, not a hello")

// Telemetry ordering uses the car's own rule, so a reordered datagram cannot walk uptime, the
// trip count or the calibration flag backwards.
check(RTFrame.seqNewer(89, than: 88), "the next frame is newer")
check(!RTFrame.seqNewer(88, than: 88), "the same frame is not newer")
check(!RTFrame.seqNewer(87, than: 88), "a reordered frame is not newer")
check(RTFrame.seqNewer(0, than: 4294967295), "wraparound reads as newer")
check(!RTFrame.seqNewer(4294967295, than: 0), "and not backwards")

// Telemetry, by the generated field names.
if case .telemetry(let t)? = RTFrame.parse(
    #"{"seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,"calibrated":true,"bus_ok":true,"ctl":"rt"}"#) {
    check(t.rxFps == 10 && t.rssi == -58 && t.uptimeS == 812, "telemetry numbers")
    check(t.calibrated == true && t.busOk == true && t.ctl == CtlOwner.rt, "telemetry flags")
} else {
    check(false, "telemetry parses")
}
// 0 means "unavailable" on the car's side, not "very strong".
check(RTFrame.parse(#"{"uptime_s":1,"rssi":0}"#).flatMap { if case .telemetry(let t) = $0 { return t.rssi } else { return nil } } == nil,
      "rssi 0 is absent")
check(RTFrame.parse("nope") == nil, "non-JSON refused")
check(RTFrame.parse(#"{"foo":1}"#) == nil, "unrecognisable object refused")

if failures == 0 { print("test_rtframe: OK") } else { exit(1) }
