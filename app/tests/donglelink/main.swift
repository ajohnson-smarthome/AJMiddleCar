// Host test for DongleLink.next(status:expectedSSID:) — the network-only half of the launch
// sequence for the adapter: given its /status and the car's own network name, what happens
// next. Identity, rollback and version are VersionRule's now, decided from /version before this
// is ever asked — see versionrule/main.swift for those. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

/// Builds a `/status` body — no `device` group, `system.idf` present — and decodes it straight
/// into `DongleStatus`, varying only the two fields `next` reads.
func status(ssid: String, state: String) -> DongleStatus {
    let json = #"""
    {"proto":1,
     "usb":{"state":"up"},
     "wifi":{"ssid":"\#(ssid)","configured":\#(!ssid.isEmpty),"state":"\#(state)","rssi_dbm":-50,"channel":1,
             "attempts":{"used":0,"max":5}},
     "relay":{"to_car_hz":0.0,"to_phone_hz":0.0,"udp_sessions":0,"tcp_connections":0,"last_error":null},
     "system":{"uptime_s":1,"free_heap":1,"idf":"v6.0.2"}}
    """#
    return try! JSONDecoder().decode(DongleStatus.self, from: Data(json.utf8))
}

let carSSID = CarContract.ssid
check(DongleLink.next(status: status(ssid: "", state: "idle"), expectedSSID: carSSID) == .sendCredentials,
      "never configured: send the car's network")
check(DongleLink.next(status: status(ssid: "someOtherNetwork", state: "connected"), expectedSSID: carSSID) == .sendCredentials,
      "configured for another network: re-point it, even though it is connected there")
check(DongleLink.next(status: status(ssid: carSSID, state: "searching"), expectedSSID: carSSID) == .searchingCar, "searching")
check(DongleLink.next(status: status(ssid: carSSID, state: "joining"), expectedSSID: carSSID) == .waiting, "joining: wait")
check(DongleLink.next(status: status(ssid: carSSID, state: "failed"), expectedSSID: carSSID) == .retryJoin, "failed: ask again")
check(DongleLink.next(status: status(ssid: carSSID, state: "idle"), expectedSSID: carSSID) == .retryJoin,
      "idle with the network in place: the join was never recorded — ask again")
check(DongleLink.next(status: status(ssid: carSSID, state: "connected"), expectedSSID: carSSID) == .readyForCar, "connected: the car's turn")
if failures == 0 { print("test_donglelink: OK") } else { exit(1) }
