import Foundation
import CoreGraphics

enum Scheme: String { case arcade, tank }

enum Corner: String, CaseIterable { case fl, fr, rl, rr }

enum DiagramState { case idle, drive, spin }

/// Pure mapping from joystick axes to the firmware's (throttle, yaw) in [-1,1].
/// Screen Y is positive downward, so "up" is a negative stick Y.
enum ControlModel {
    /// Non-finite input is a stop, not a direction — same rule as `RTFrame.clamp`, because a
    /// NaN that reaches either becomes a held command.
    static func clamp(_ v: Double) -> Double { v.isFinite ? min(1, max(-1, v)) : 0 }

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

    /// RSSI-based link level when the car reports its AP-side RSSI, and the received-frame rate
    /// when it cannot.
    ///
    /// The car reports `rssi: 0` whenever the AP station list is unavailable — on this board an
    /// RPC over SDIO to the radio co-processor, so "unavailable" is ordinary. Falling through to
    /// zero there painted empty red bars next to a pill saying «На связи»: a live link rendered
    /// as a dead one. `rx_fps` is the car counting what actually arrived out of the 10 Hz the app
    /// sends, which is a truer picture of the link than RSSI anyway.
    static func signalLevel(online: Bool, rssi: Int?, rxFps: Int?, expectedFps: Int) -> Int {
        guard online else { return 0 }
        if let r = rssi, r != 0 {
            if r >= -50 { return 4 }
            if r >= -60 { return 3 }
            if r >= -70 { return 2 }
            return 1
        }
        // Live but unmeasurable is one bar, never zero: zero is reserved for "no link".
        guard let f = rxFps, f > 0, expectedFps > 0 else { return 1 }
        let ratio = Double(f) / Double(expectedFps)
        if ratio >= 0.9 { return 4 }
        if ratio >= 0.7 { return 3 }
        if ratio >= 0.4 { return 2 }
        return 1
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
