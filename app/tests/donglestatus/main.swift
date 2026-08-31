// Host test for the dongle's /status and /net decoding. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// -- a complete /status body decodes every field ---------------------------------------
let fullStatus = #"""
{"device":"ajdongle","fw":"v1.0+123","idf":"v6.0.2","usb":"\#(DongleUsbState.up)","rollback":false,
 "net":{"ssid":"benchnet","state":"\#(DongleNetState.connected)","rssi":-42}}
"""#
let status = try! DongleStatus.parse(Data(fullStatus.utf8))
check(status.device == "ajdongle", "device decodes")
check(status.fw == "v1.0+123", "fw decodes")
check(status.idf == "v6.0.2", "idf decodes")
check(status.usb == DongleUsbState.up, "usb decodes")
check(status.rollback == false, "rollback decodes")
check(status.net.ssid == "benchnet", "net.ssid decodes")
check(status.net.state == .connected, "net.state decodes")
check(status.net.rssi == -42, "net.rssi decodes")

// A second, fully distinct fixture: device, idf and usb all differ from the first fixture
// and from anything a plausible hardcode would produce (not "ajdongle", not "up") — the
// point is to prove these three are read off the wire, not assigned from a constant or a
// fixed literal that happens to match the first fixture's values.
let altStatus = #"""
{"device":"benchdongle-2","fw":"v2.0+9","idf":"idf-vX.Y-alt","usb":"provisioning","rollback":true,
 "net":{"ssid":"otherlab","state":"\#(DongleNetState.failed)","rssi":-77}}
"""#
let alt = try! DongleStatus.parse(Data(altStatus.utf8))
check(alt.device == "benchdongle-2", "device decodes from a second, distinct fixture")
check(alt.idf == "idf-vX.Y-alt", "idf decodes from a second, distinct fixture")
check(alt.usb == "provisioning", "usb decodes from a second, distinct fixture")

// -- rollback:true is readable — it is the "the update was reverted" signal ------------
let rolledBack = #"""
{"device":"ajdongle","fw":"v1.0+123","idf":"v6.0.2","usb":"\#(DongleUsbState.up)","rollback":true,
 "net":{"ssid":"benchnet","state":"\#(DongleNetState.idle)","rssi":0}}
"""#
check(try! DongleStatus.parse(Data(rolledBack.utf8)).rollback == true,
      "rollback:true is readable")

// -- net.state covers every contract case, plus an unknown one surfaced not swallowed --
func statusWith(state: String) -> Data {
    Data(#"{"device":"ajdongle","fw":"v1","idf":"v6","usb":"\#(DongleUsbState.up)","rollback":false,"net":{"ssid":"n","state":"\#(state)","rssi":0}}"#.utf8)
}
check(try! DongleStatus.parse(statusWith(state: DongleNetState.idle)).net.state == .idle, "state idle")
check(try! DongleStatus.parse(statusWith(state: DongleNetState.joining)).net.state == .joining, "state joining")
check(try! DongleStatus.parse(statusWith(state: DongleNetState.connected)).net.state == .connected, "state connected")
check(try! DongleStatus.parse(statusWith(state: DongleNetState.failed)).net.state == .failed, "state failed")
// "rebooting" is deliberately not one of DongleNetState's four — this is the one fixture in
// the file that must NOT come from the generated vocabulary, because it is testing what
// happens when the wire outgrows it.
check(try! DongleStatus.parse(statusWith(state: "rebooting")).net.state == .unknown("rebooting"),
      "an unknown state is surfaced, not silently mapped to a known one")

// -- a /status body missing a field fails to parse rather than defaulting --------------
let missingTopLevel = #"""
{"device":"ajdongle","idf":"v6.0.2","usb":"\#(DongleUsbState.up)","rollback":false,
 "net":{"ssid":"benchnet","state":"\#(DongleNetState.connected)","rssi":-42}}
"""#
do {
    _ = try DongleStatus.parse(Data(missingTopLevel.utf8))
    check(false, "a body missing a top-level field (fw) must not parse")
} catch {
    check(true, "a body missing a top-level field (fw) must not parse")
}

let missingNested = #"""
{"device":"ajdongle","fw":"v1.0+123","idf":"v6.0.2","usb":"\#(DongleUsbState.up)","rollback":false,
 "net":{"ssid":"benchnet","state":"\#(DongleNetState.connected)"}}
"""#
do {
    _ = try DongleStatus.parse(Data(missingNested.utf8))
    check(false, "a body missing a nested field (net.rssi) must not parse")
} catch {
    check(true, "a body missing a nested field (net.rssi) must not parse")
}

// -- GET /net decodes ssid and configured; a password key is accepted but not stored ---
let netWithPassword = #"{"ssid":"benchnet","configured":true,"password":"irrelevant"}"#
let net = try! DongleNet.parse(Data(netWithPassword.utf8))
check(net.ssid == "benchnet", "net ssid decodes")
check(net.configured == true, "net configured decodes")
let netMirror = Mirror(reflecting: net)
check(!netMirror.children.contains { $0.label?.lowercased().contains("password") == true },
      "DongleNet has no field that could hold a password")

// -- SSIDs with an escaped quote and a backslash both round-trip -----------------------
// net_cfg deliberately allows both bytes in an SSID; a raw \" or \\ here must decode to
// the literal " or \ rather than breaking the parse or being stripped.
let quotedSSID = #"{"ssid":"studio\"2","configured":true}"#
check(try! DongleNet.parse(Data(quotedSSID.utf8)).ssid == "studio\"2",
      "an SSID with an escaped quote round-trips")

let backslashSSID = #"{"ssid":"studio\\2","configured":true}"#
check(try! DongleNet.parse(Data(backslashSSID.utf8)).ssid == "studio\\2",
      "an SSID with a backslash round-trips")

if failures == 0 { print("test_donglestatus: OK") } else { exit(1) }
