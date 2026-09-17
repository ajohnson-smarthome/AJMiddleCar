// Host test for DeviceVersion and VersionReply — the frozen /version document and how a read
// of it is classified. Run with swiftc; no XCTest, no simulator.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

extension VersionReply {
    var isAbsent: Bool { if case .absent = self { return true }; return false }
    var isSilent: Bool { if case .silent = self { return true }; return false }
    var isFaulty: Bool { if case .faulty = self { return true }; return false }
    var isDenied: Bool { if case .denied = self { return true }; return false }
    var version: DeviceVersion? { if case .version(let v) = self { return v }; return nil }
}

// -- both live documents decode, field for field ----------------------------------------------
let car = #"{"device":"ajmiddlecar","fw":"v1.0+879","build":879,"proto":2,"rolled_back":false}"#
let dongle = #"{"device":"ajdongle","fw":"v1.0+879","build":879,"proto":1,"rolled_back":false}"#
check(VersionReply.decode(Data(car.utf8)).version ==
      DeviceVersion(device: "ajmiddlecar", fw: "v1.0+879", build: 879, proto: 2, rolled_back: false),
      "the car's document decodes")
check(VersionReply.decode(Data(dongle.utf8)).version ==
      DeviceVersion(device: "ajdongle", fw: "v1.0+879", build: 879, proto: 1, rolled_back: false),
      "the adapter's document decodes")
check(VersionReply.decode(Data(#"{"device":"ajdongle","fw":"v1.0","build":-1,"proto":1,"rolled_back":true}"#.utf8)).version?.build == -1,
      "a build of -1 (no +number in fw) is a value, not a decode failure")

// -- anything else that came back is a fault ------------------------------------------------------
check(VersionReply.decode(Data("junk".utf8)).isFaulty, "junk is faulty")
check(VersionReply.decode(Data(#"{"device":"ajdongle","fw":"v1.0+879"}"#.utf8)).isFaulty,
      "a document missing fields is faulty — the shape is frozen, there is no lenient read")
check(VersionReply.decode(Data(#"{"proto":1,"usb":{"state":"up"}}"#.utf8)).isFaulty,
      "a /status body is not a /version body")

// -- the transport's errors, classified --------------------------------------------------------
check(VersionReply.of(CarError.http(status: 404, body: Data())).isAbsent,
      "404 is a board that predates /version — absent, to be updated")
check(VersionReply.of(CarError.http(status: 500, body: Data())).isFaulty, "500 is a fault")
check(VersionReply.of(CarError.truncated(got: 3, want: 9)).isFaulty, "a truncated body is a fault")
check(VersionReply.of(CarError.denied).isDenied, "denied is denied")
check(VersionReply.of(CarError.refused).isSilent, "a refused connection is silence")
check(VersionReply.of(CarError.timeout(3)).isSilent, "a timeout is silence")
check(VersionReply.of(CarError.noDongle(.notAvailable)).isSilent, "no path is silence")
check(VersionReply.of(CarError.malformed("x")).isSilent, "a malformed head is silence")
check(VersionReply.of(CancellationError()).isSilent, "a cancellation is evidence of nothing")

if failures == 0 { print("test_identity: OK") } else { exit(1) }
