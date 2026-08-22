// Host test for the transport's pure session decisions. Run with swiftc; no XCTest.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// -- handshake filtering: a reply for another sid is a leftover from a previous socket. ----
let sid = "7f3a91c2"
let ourReply = RTFrame.parse(#"{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+517"}"#)
check(SessionPolicy.handshakeOutcome(ourReply, sid: sid)
        == .identity(device: "ajmiddlecar", fw: "v1.0+517"), "our sid's reply is the identity")

let staleReply = RTFrame.parse(#"{"proto":1,"hello":"deadbeef","device":"ajmiddlecar","fw":"v1.0+517"}"#)
check(SessionPolicy.handshakeOutcome(staleReply, sid: sid) == .ignore,
      "another sid's reply is ignored — ownership is not resumable")

let mismatch = RTFrame.parse(#"{"proto":2,"hello":"7f3a91c2"}"#)
check(SessionPolicy.handshakeOutcome(mismatch, sid: sid) == .protoMismatch(theirs: 2),
      "a proto mismatch for our sid is reported, not ignored")
let staleMismatch = RTFrame.parse(#"{"proto":2,"hello":"deadbeef"}"#)
check(SessionPolicy.handshakeOutcome(staleMismatch, sid: sid) == .ignore,
      "a proto mismatch for another sid is a leftover too")

var telemetry = Telemetry(); telemetry.uptimeS = 5
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

// -- the identity hold is long enough to read, and one place owns the number. --------------
check(SessionPolicy.identityHoldSeconds == 10, "the wrong-car hold is ten seconds")

if failures == 0 { print("test_sessionpolicy: OK") } else { exit(1) }
