import Foundation
import CoreGraphics

struct Telemetry {
    var rssi: Int?
    var wsFps: Int?
    var wdtTrips: Int?
    var uptimeS: Int?
    var heap: Int?
    var calibrated: Bool?

    /// Parse a WS telemetry frame; nil if not JSON or missing the core shape.
    static func parse(_ json: String) -> Telemetry? {
        guard let data = json.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        guard j["rssi"] != nil || j["uptime_s"] != nil || j["calibrated"] != nil else { return nil }
        var t = Telemetry()
        if let r = j["rssi"] as? Int, r != 0 { t.rssi = r }
        t.wsFps = j["ws_fps"] as? Int
        t.wdtTrips = j["wdt_trips"] as? Int
        t.uptimeS = j["uptime_s"] as? Int
        t.heap = j["heap"] as? Int
        t.calibrated = j["calibrated"] as? Bool
        return t
    }
}

enum Scheme: String { case arcade, tank }

enum Corner: String, CaseIterable { case fl, fr, rl, rr }

enum DiagramState { case idle, drive, spin }

/// Pure mapping from joystick axes to the firmware's (throttle, yaw) in [-1,1].
/// Screen Y is positive downward, so "up" is a negative stick Y.
enum ControlModel {
    static func clamp(_ v: Double) -> Double { min(1, max(-1, v)) }

    /// One stick: up = throttle, left/right = yaw.
    static func arcade(stickX: Double, stickY: Double) -> (t: Double, y: Double) {
        (clamp(-stickY), clamp(stickX))
    }

    /// Two vertical sticks: each drives its side. side = -stickY.
    static func tank(leftStickY: Double, rightStickY: Double) -> (t: Double, y: Double) {
        let l = -leftStickY, r = -rightStickY
        return (clamp((l + r) / 2), clamp((l - r) / 2))
    }

    /// Mixer: throttle/yaw -> normalized left/right side speeds in [-1,1] (mirrors the firmware).
    static func sides(t: Double, y: Double) -> (left: Double, right: Double) {
        var l = t + y, r = t - y
        let m = Swift.max(abs(l), abs(r), 1)
        l /= m; r /= m
        return (l, r)
    }

    /// Link-quality level 0...4 from ping (we can't read real Wi-Fi RSSI on iOS).
    static func signalLevel(online: Bool, pingMs: Int?) -> Int {
        guard online, let p = pingMs else { return 0 }
        if p < 50 { return 4 }
        if p < 120 { return 3 }
        if p < 250 { return 2 }
        return 1
    }

    /// RSSI-based link level when the car reports its AP-side RSSI; falls back to ping.
    static func signalLevel(online: Bool, rssi: Int?, pingMs: Int?) -> Int {
        guard online else { return 0 }
        if let r = rssi, r != 0 {
            if r >= -50 { return 4 }
            if r >= -60 { return 3 }
            if r >= -70 { return 2 }
            return 1
        }
        return signalLevel(online: online, pingMs: pingMs)
    }

    /// Build the /calib/save body JSON {"wheels":[...]} in FL,FR,RL,RR order.
    /// Missing corners default to (0, 1) — the wizard only calls this when all 4 are set.
    static func calibSaveBody(_ a: [Corner: (pair: Int, sign: Int)]) -> String {
        let wheels = Corner.allCases.map { c -> String in
            let v = a[c] ?? (pair: 0, sign: 1)
            return #"{"pair":\#(v.pair),"sign":\#(v.sign)}"#
        }.joined(separator: ",")
        return #"{"wheels":[\#(wheels)]}"#
    }

    /// Wire frame as a JSON object {"t":..,"y":..} (two decimals), clamped to [-1,1].
    static func frame(t: Double, y: Double) -> String {
        String(format: "{\"t\":%.2f,\"y\":%.2f}", clamp(t), clamp(y))
    }

    /// Which visual the diagram shows for a command.
    static func diagramState(t: Double, y: Double) -> DiagramState {
        if abs(t) >= 0.05 { return .drive }
        if abs(y) >= 0.05 { return .spin }
        return .idle
    }

    /// Signed path curvature ~ yaw / speed (bounded near t=0).
    static func curvature(t: Double, y: Double) -> Double {
        y / Swift.max(abs(t), 0.15)
    }

    /// Centerline of the predicted path in local space: starts at (0,0), heads "up"
    /// (screen -y), bends by yaw. The TOTAL bend is bounded (≤ ~70°) so the arc is a
    /// gentle parking-camera curve and can never close into a loop/circle, regardless
    /// of how small t is or how large y is.
    static func trajectoryPoints(t: Double, y: Double, length: Double, steps: Int) -> [CGPoint] {
        let steer = Swift.max(-1.0, Swift.min(1.0, y / Swift.max(abs(t), 0.2)))  // [-1,1]
        let totalTurn = steer * (70.0 * Double.pi / 180.0)                       // bounded
        let seg = length / Double(steps)
        let dHeading = totalTurn / Double(steps)                                 // constant → circular arc
        var pts: [CGPoint] = []
        var x = 0.0, yy = 0.0
        var heading = -Double.pi / 2
        for _ in 0...steps {
            pts.append(CGPoint(x: x, y: yy))
            heading += dHeading
            x += Foundation.cos(heading) * seg
            yy += Foundation.sin(heading) * seg
        }
        return pts
    }
}
