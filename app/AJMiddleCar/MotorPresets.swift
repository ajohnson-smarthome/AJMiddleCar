import Foundation

/// One known motor configuration. `gearX100` = gear ratio × 100 (matches firmware /wheel).
/// `rpm` is a label only (rated output speed) — it does not affect CPR.
struct MotorPreset: Identifiable, Equatable {
    let id: String       // stable key, e.g. "jga25-370-170"
    let name: String     // "JGA25-370"
    let rpm: Int         // rated output rpm, label only
    let ppr: Int         // encoder pulses per motor-shaft rev (one channel)
    let gearX100: Int
    let quad: Int        // 1 / 2 / 4
    var gear: Double { Double(gearX100) / 100 }
    var cpr: Double { MotorPresets.cpr(ppr: ppr, gearX100: gearX100, quad: quad) }
}

/// Starter presets (verify against the motor datasheet — these define CPR/speed).
/// The menu lists ONLY these; there is no "Other" item — editing any field just makes
/// `match` return nil (the UI then shows «Свои параметры»).
enum MotorPresets {
    static let all: [MotorPreset] = [
        MotorPreset(id: "jga25-370-170",   name: "JGA25-370",  rpm: 170,  ppr: 11, gearX100: 2100, quad: 4),
        MotorPreset(id: "jgb37-520b-1000", name: "JGB37-520B", rpm: 1000, ppr: 11, gearX100: 900,  quad: 4),
    ]

    /// Counts per output-shaft revolution = ppr × gear × quad.
    static func cpr(ppr: Int, gearX100: Int, quad: Int) -> Double {
        Double(ppr) * (Double(gearX100) / 100) * Double(quad)
    }

    /// The preset matching these exact numbers, or nil if the user hand-entered custom values.
    static func match(ppr: Int, gearX100: Int, quad: Int) -> MotorPreset? {
        all.first { $0.ppr == ppr && $0.gearX100 == gearX100 && $0.quad == quad }
    }
}
