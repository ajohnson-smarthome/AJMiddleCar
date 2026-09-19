// Host test for the generated contract. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

check(Wheel.default == Wheel(diameter_mm: 65, encoder_ppr: 11, gear_ratio: 9.0, quadrature: 4), "Wheel.default")
check(Recovery.default == Recovery(enabled: true, window_ms: 5000), "Recovery.default")
check(Chassis.default == Chassis(track_mm: 130, wheelbase_mm: 210), "Chassis.default")
check(Ramp.default == Ramp(rise_ms: 300), "Ramp.default")
check(Trim.default == Trim(balance_pct: 0), "Trim.default")

check(Wheel.diameter_mmRange == 20...150, "diameter range")
check(Wheel.gear_ratioRange == 1.0...300.0, "gear ratio range")
check(Trim.balance_pctRange == -30...30, "trim range")
check(Recovery.window_msRange == 1000...8000, "window range")
check(Wheel.quadratureAllowed == [1, 2, 4], "quadrature allowed")

check(Wheel.key == "wheel" && Chassis.key == "chassis" && Ramp.key == "ramp"
      && Trim.key == "trim" && Recovery.key == "recovery", "domain keys")
check(CarContract.configPath == "/config", "one config path")

// /config round-trips: a subset encodes without the domains it does not carry.
let cfg = #"{"proto":2,"ramp":{"rise_ms":300},"trim":{"balance_pct":0},"recovery":{"enabled":true,"window_ms":5000},"wheel":{"diameter_mm":70,"encoder_ppr":12,"gear_ratio":9.6,"quadrature":2},"chassis":{"track_mm":130,"wheelbase_mm":210}}"#
let decoded = try! JSONDecoder().decode(CarConfig.self, from: Data(cfg.utf8))
check(decoded.wheel == Wheel(diameter_mm: 70, encoder_ppr: 12, gear_ratio: 9.6, quadrature: 2), "decode wheel")
check(Wheel.pick(from: decoded) == decoded.wheel, "pick")
let subset = String(decoding: try! JSONEncoder().encode(Ramp.wrap(Ramp(rise_ms: 400))), as: UTF8.self)
check(subset.contains(#""ramp":{"rise_ms":400}"#) && !subset.contains("wheel") && !subset.contains("proto"),
      "a wrapped domain encodes alone")
let back = try! JSONDecoder().decode(CarConfig.self, from: try! JSONEncoder().encode(decoded))
check(back == decoded, "round trip")

check(CarContract.proto == 2 && CarContract.device == "ajmiddlecar" && CarContract.rtPort == 4210, "contract")
check(CarContract.seqField == "seq" && CarContract.throttleField == "throttle" && CarContract.turnField == "turn", "rt keys")
check(RTType.helloAck == "hello_ack" && RTType.drive == "drive", "rt types")
check(CarContract.maxCommand < CarContract.maxDatagram, "caps")
check(MotorsOwner.all.count == 7 && MotorsOwner.all.contains(.remote), "owner vocabulary")
check(MotorsOwner(rawValue: "safe_stop") == .safe_stop && MotorsOwner(rawValue: "x") == .unknown("x"), "owner words")
check(CalibCorner.all == [.front_left, .front_right, .rear_left, .rear_right], "corners")
check(CarErrorCode(rawValue: "out_of_range") == .out_of_range, "error codes")

// The error envelope decodes with and without a field.
let e = try! JSONDecoder().decode(CarAPIError.self, from: Data(#"{"proto":2,"error":{"code":"out_of_range","message":"m","field":"ramp.rise_ms"}}"#.utf8))
check(e.error.code == .out_of_range && e.error.field == "ramp.rise_ms", "envelope")

if failures == 0 { print("test_carapi: OK") } else { exit(1) }
