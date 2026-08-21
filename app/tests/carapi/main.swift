// Host test for the generated contract. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// Defaults match the schema.
check(Wheel.default == Wheel(diameter_mm: 65, ppr: 11, gear_x100: 2100, quad: 4),
      "Wheel.default")
check(Recover.default == Recover(enabled: true, window_ms: 5000), "Recover.default")
check(Dims.default == Dims(track_mm: 130, wheelbase_mm: 210), "Dims.default")
check(Ramp.default == Ramp(ramp_ms: 300), "Ramp.default")
check(Trim.default == Trim(trim_pct: 0), "Trim.default")

// Ranges are the firmware's.
check(Wheel.diameter_mmRange == 20...150, "diameter range")
check(Trim.trim_pctRange == -30...30, "trim range")
check(Recover.window_msRange == 1000...10000, "window range")
check(Wheel.quadAllowed == [1, 2, 4], "quad allowed")

// Paths are the ones the car serves.
check(Wheel.path == "/wheel" && Dims.path == "/dims" && Ramp.path == "/ramp"
      && Trim.path == "/trim" && Recover.path == "/recover", "paths")

// Round-trips over the wire names the car actually sends.
let json = #"{"diameter_mm":70,"ppr":12,"gear_x100":960,"quad":2}"#
let decoded = try! JSONDecoder().decode(Wheel.self, from: Data(json.utf8))
check(decoded.diameter_mm == 70 && decoded.gear_x100 == 960, "decode")
let reencoded = try! JSONEncoder().encode(decoded)
let back = try! JSONDecoder().decode(Wheel.self, from: reencoded)
check(back == decoded, "round trip")

// Contract constants.
check(CarContract.proto == 1, "proto")
check(CarContract.device == "ajmiddlecar", "device")
check(CarContract.rtPort == 4210, "rt port")
check(CarContract.watchdogMs == 300, "watchdog")
check(CarContract.seqField == "seq" && CarContract.byeField == "bye", "rt frame fields")
check(TelemetryKey.rxFps == "rx_fps" && TelemetryKey.busOk == "bus_ok"
      && TelemetryKey.ctl == "ctl", "telemetry keys")

if failures == 0 { print("test_carapi: OK") } else { exit(1) }
