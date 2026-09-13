// Host test for the one piece of v1 the app still understands: who answered /status.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let v1car = #"{"device":"ajmiddlecar","fw":"v1.0+784","proto":1,"seq":1,"rx_fps":0,"rssi":0,"wdt_trips":0,"uptime_s":3,"heap":1,"calibrated":true,"bus_ok":true,"ctl":"none","rollback":false,"nvs_wiped":false,"radio":{"fw":"3.0.6","expected":"3.0.6","ok":true}}"#
check(LegacyIdentity.parse(Data(v1car.utf8)) == LegacyIdentity(device: "ajmiddlecar", fw: "v1.0+784"), "a v1 car")
let v1dongle = #"{"device":"ajdongle","fw":"v1.0+789","idf":"v6.0.2","usb":"up","rollback":false,"net":{"ssid":"","state":"idle","rssi":0}}"#
check(LegacyIdentity.parse(Data(v1dongle.utf8)) == LegacyIdentity(device: "ajdongle", fw: "v1.0+789"), "a v1 dongle")
let v2 = #"{"proto":2,"device":{"id":"ajmiddlecar","fw":"v1.0+800","build":800,"rolled_back":false},"link":{}}"#
check(LegacyIdentity.parse(Data(v2.utf8)) == LegacyIdentity(device: "ajmiddlecar", fw: "v1.0+800"), "a v2 document too")
check(LegacyIdentity.parse(Data(#"{"device":"x"}"#.utf8)) == nil, "no fw is no identity")
check(LegacyIdentity.parse(Data(#"{"device":{"id":"x"}}"#.utf8)) == nil, "no fw is no identity (v2)")
check(LegacyIdentity.parse(Data(#"[1,2]"#.utf8)) == nil, "not an object")
check(LegacyIdentity.parse(Data("junk".utf8)) == nil, "not json")

if failures == 0 { print("test_legacyidentity: OK") } else { exit(1) }
