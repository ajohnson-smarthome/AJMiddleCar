// Host test for the generated /status decoder. Run with swiftc; no XCTest.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let full = #"{"proto":1,"device":{"id":"ajdongle","fw":"v1.0+789","build":789,"rolled_back":false,"idf":"v6.0.2"},"usb":{"state":"up"},"wifi":{"ssid":"AJMiddleCar","configured":true,"state":"connected","rssi_dbm":-53,"channel":1,"attempts":{"used":0,"max":5}},"relay":{"to_car_hz":10.0,"to_phone_hz":5.0,"udp_sessions":1,"tcp_connections":2,"last_error":{"errno":118,"message":"No route to host","count":3,"age_s":41}},"system":{"uptime_s":412,"free_heap":8551152}}"#
let s = try! JSONDecoder().decode(DongleStatus.self, from: Data(full.utf8))
check(s.proto == 1 && s.device.id == "ajdongle" && s.device.build == 789 && s.device.idf == "v6.0.2", "device")
check(s.usb.state == .up, "usb")
check(s.wifi.ssid == "AJMiddleCar" && s.wifi.configured && s.wifi.state == .connected, "wifi")
check(s.wifi.rssi_dbm == -53 && s.wifi.channel == 1 && s.wifi.attempts == DongleWifiAttempts(used: 0, max: 5), "wifi readings")
check(s.relay.to_car_hz == 10.0 && s.relay.udp_sessions == 1, "relay")
check(s.relay.last_error == DongleRelayError(errno: 118, message: "No route to host", count: 3, age_s: 41), "last error")
check(s.system.uptime_s == 412 && s.system.free_heap == 8551152, "system")

let idle = #"{"proto":1,"device":{"id":"ajdongle","fw":"v1.0+789","build":789,"rolled_back":true,"idf":"v6.0.2"},"usb":{"state":"up"},"wifi":{"ssid":"","configured":false,"state":"idle","rssi_dbm":null,"channel":null,"attempts":{"used":0,"max":5}},"relay":{"to_car_hz":0.0,"to_phone_hz":0.0,"udp_sessions":0,"tcp_connections":0,"last_error":null},"system":{"uptime_s":3,"free_heap":1}}"#
let i = try! JSONDecoder().decode(DongleStatus.self, from: Data(idle.utf8))
check(i.wifi.rssi_dbm == nil && i.wifi.channel == nil && i.relay.last_error == nil, "nulls decode as nil")
check(i.device.rolled_back && !i.wifi.configured && i.wifi.state == .idle, "idle after boot")

// A state word this build does not know is kept, not a decode failure.
let odd = full.replacingOccurrences(of: #""state":"connected""#, with: #""state":"dreaming""#)
check((try? JSONDecoder().decode(DongleStatus.self, from: Data(odd.utf8)))?.wifi.state == .unknown("dreaming"),
      "unknown wifi state")
// A document missing a group is not a status.
let short = #"{"proto":1,"device":{"id":"ajdongle","fw":"v1.0+789","build":789,"rolled_back":false,"idf":"v6.0.2"}}"#
check((try? JSONDecoder().decode(DongleStatus.self, from: Data(short.utf8))) == nil, "a short document throws")

// The POST /wifi reply and the error envelope.
let reply = try! JSONDecoder().decode(DongleWifiReply.self, from: Data(#"{"proto":1,"ssid":"AJMiddleCar","state":"searching"}"#.utf8))
check(reply == DongleWifiReply(proto: 1, ssid: "AJMiddleCar", state: .searching), "wifi reply")
let err = try! JSONDecoder().decode(DongleAPIError.self, from: Data(#"{"proto":1,"error":{"code":"bad_length","message":"ssid must be 1..32 bytes","field":"ssid"}}"#.utf8))
check(err.error.code == .bad_length && err.error.field == "ssid", "error envelope")
let errNoField = try! JSONDecoder().decode(DongleAPIError.self, from: Data(#"{"proto":1,"error":{"code":"bad_json","message":"x"}}"#.utf8))
check(errNoField.error.field == nil && errNoField.error.code == .bad_json, "error envelope without a field")

if failures == 0 { print("test_donglestatus: OK") } else { exit(1) }
