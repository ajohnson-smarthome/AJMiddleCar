import Foundation

/// One step of a maneuver: hold (t, y) for `ms` milliseconds.
struct TrickStep { let t: Double; let y: Double; let ms: Int }

/// A named maneuver = a timeline of steps the app streams over the WS channel.
struct Trick: Identifiable {
    let id: Int            // 1..N, unique
    let nameKey: String    // Localizable key (→ L.trickName)
    let icon: String       // SF Symbol
    let steps: [TrickStep]
    var totalMs: Int { steps.reduce(0) { $0 + $1.ms } }
}

/// Open-loop (no gyro) — angles/distances are approximate and surface/battery dependent.
/// Every trick is 5 s at base; per-action durations are editable (see TrickEditorView).
enum Tricks {
    static let spin = Trick(id: 1, nameKey: "tricks.spin", icon: "arrow.clockwise",
                            steps: [TrickStep(t: 0, y: 1, ms: 5000)])
    static let figure8 = Trick(id: 2, nameKey: "tricks.figure8", icon: "infinity",
                               steps: [TrickStep(t: 0.6, y: 0.6, ms: 2500),
                                       TrickStep(t: 0.6, y: -0.6, ms: 2500)])
    static let wiggle = Trick(id: 3, nameKey: "tricks.wiggle", icon: "wind",
                              steps: (0..<20).map { TrickStep(t: 0, y: $0 % 2 == 0 ? 0.8 : -0.8, ms: 250) })
    // Wide donut: both wheels forward (right slower) → the car drives a visible ~0.5 m circle
    // rather than pivoting in place. (t=0.8,y=0.2 → sides 1.0/0.6 → turn radius ≈ 2·track.)
    static let donut = Trick(id: 4, nameKey: "tricks.donut", icon: "circle.dashed",
                             steps: [TrickStep(t: 0.8, y: 0.2, ms: 5000)])

    static let all: [Trick] = [spin, figure8, wiggle, donut]

    // MARK: donut geometry (pure, host-tested) — radius depends only on the side ratio + track,
    // not on motor speed. Inverse of TrickSim's R = T·(l+r)/(2(l−r)).
    /// Assumed track (m) while /dims is unavailable (pre-fetch / offline). Equals the firmware
    /// default (130 mm). The pure donut functions take `trackM` explicitly — this is only the
    /// fallback supplied at the fetch sites.
    static let donutTrackFallbackM = 0.13
    static let donutDiaMinCm = 20, donutDiaMaxCm = 150, donutDiaDefaultCm = 50

    /// For a target circle diameter, hold the fast wheel at full power and solve the slow wheel's
    /// ratio, returning the (t, y) command. r is clamped to [0, 0.9] so both wheels stay forward
    /// (the maneuver remains a circle, never a pivot or near-straight line).
    static func donutSides(diameterCm: Double, trackM: Double) -> (t: Double, y: Double) {
        let R = Swift.max(0.001, diameterCm / 100 / 2)
        let T = trackM
        var r = (2 * R - T) / (2 * R + T)
        r = Swift.min(0.9, Swift.max(0.0, r))
        return ((1 + r) / 2, (1 - r) / 2)
    }

    // MARK: donut circle count (pure, host-tested) — duration back-solved from a target
    // number of full circles. ω = vmax·2y/track, so N circles take t = N·π·track/(vmax·y).
    static let donutCirclesMin = 1, donutCirclesMax = 10, donutCirclesDefault = 2
    /// Nominal linear speed (default motor JGA25-370 ~170 rpm, 65 mm wheel: π·0.065·170/60)
    /// used when /wheel is unavailable, so a circle count still maps to *some* duration.
    static let donutNominalVmaxMS = 0.578

    /// Streamed duration (ms) for `circles` full circles of a donut whose inner-wheel term is
    /// `y` (= (1−r)/2 from `donutSides`), at linear speed `vmaxMS`. Inverse of the simulation's
    /// heading sweep; 0 if speed/shape is degenerate.
    static func donutDurationMs(circles: Int, y: Double, vmaxMS: Double, trackM: Double) -> Int {
        guard vmaxMS > 0, y > 0 else { return 0 }
        let n = Double(Swift.max(donutCirclesMin, circles))
        return Int((1000 * n * Double.pi * trackM / (vmaxMS * y)).rounded())
    }

    /// The donut maneuver sized to a diameter AND timed to a circle count, at speed `vmaxMS`.
    /// Same id/name/icon; the single step's (t, y) from `donutSides`, ms from `donutDurationMs`.
    static func donutTrick(diameterCm: Double, circles: Int, vmaxMS: Double, trackM: Double) -> Trick {
        let (t, y) = donutSides(diameterCm: diameterCm, trackM: trackM)
        let ms = donutDurationMs(circles: circles, y: y, vmaxMS: vmaxMS, trackM: trackM)
        return Trick(id: donut.id, nameKey: donut.nameKey, icon: donut.icon,
                     steps: [TrickStep(t: t, y: y, ms: ms)])
    }

    // MARK: spin turns + duration (pure, host-tested) — in-place pivot. Speed is back-solved so the
    // car does `turns` full 360° rotations in `durationMs`: |ω| = 2·y·vmax/track, turns·2π = |ω|·T ⇒
    // y = turns·π·track / (T·vmax), clamped to [0, 1] (can't spin faster than full power).
    static let spinTurnsMin = 1, spinTurnsMax = 6, spinTurnsDefault = 2
    static let spinDurMinMs = 1000, spinDurMaxMs = 10000, spinDurDefaultMs = 3000

    /// Derived yaw magnitude for `turns` full in-place turns in `durationMs`, at speed `vmaxMS` / `trackM`.
    /// 0 if speed/duration is degenerate; clamped to [0, 1] (can't spin faster than full power).
    static func spinSpeed(turns: Int, durationMs: Int, vmaxMS: Double, trackM: Double) -> Double {
        guard vmaxMS > 0, durationMs > 0 else { return 0 }
        let n = Double(Swift.max(spinTurnsMin, turns))
        let tSec = Double(durationMs) / 1000
        let y = n * Double.pi * trackM / (tSec * vmaxMS)
        return Swift.min(1.0, Swift.max(0.0, y))
    }

    /// The spin maneuver: one in-place step {t:0, y: spinSpeed(...), ms: durationMs}. Keeps spin's id/name/icon.
    static func spinTrick(turns: Int, durationMs: Int, vmaxMS: Double, trackM: Double) -> Trick {
        let y = spinSpeed(turns: turns, durationMs: durationMs, vmaxMS: vmaxMS, trackM: trackM)
        return Trick(id: spin.id, nameKey: spin.nameKey, icon: spin.icon,
                     steps: [TrickStep(t: 0, y: y, ms: durationMs)])
    }

    // MARK: figure-8 geometry (pure, host-tested) — two tangent loops, each a full donut circle, the
    // second mirrored (−y). Reuses donutSides + donutDurationMs. `eights` repetitions of {left, right}.
    static let fig8DiaMinCm = 20, fig8DiaMaxCm = 150, fig8DiaDefaultCm = 50
    static let fig8EightsMin = 1, fig8EightsMax = 10, fig8EightsDefault = 1

    /// The figure-8 maneuver: `eights` repetitions of a left loop then its mirror right loop, each a full
    /// circle of the given diameter at speed `vmaxMS`. (t, y) from `donutSides`; per-lobe ms from
    /// `donutDurationMs(circles: 1)`. Keeps the figure8 id/name/icon. Degenerate speed → ms 0 (no crash).
    static func figure8Trick(diameterCm: Double, eights: Int, vmaxMS: Double, trackM: Double) -> Trick {
        let (t, y) = donutSides(diameterCm: diameterCm, trackM: trackM)
        let lobeMs = donutDurationMs(circles: 1, y: y, vmaxMS: vmaxMS, trackM: trackM)
        let n = Swift.max(fig8EightsMin, eights)
        let steps = (0..<n).flatMap { _ in
            [TrickStep(t: t, y: y, ms: lobeMs), TrickStep(t: t, y: -y, ms: lobeMs)]
        }
        return Trick(id: figure8.id, nameKey: figure8.nameKey, icon: figure8.icon, steps: steps)
    }

    // MARK: wiggle (pure, host-tested) — in-place yaw oscillation, NO geometry. amplitude = |y| per
    // step; a wag = 2 steps (+amp, −amp); fixed 250 ms tempo. `wags` wags → 2·wags steps.
    static let wiggleAmpMin = 0.2, wiggleAmpMax = 1.0, wiggleAmpDefault = 0.8
    static let wiggleWagsMin = 1, wiggleWagsMax = 20, wiggleWagsDefault = 10
    static let wiggleStepMs = 250

    /// The wiggle maneuver: `wags` full left-right wags at `amplitude`, fixed 250 ms/step. `2·wags` steps
    /// alternating {t:0, y:+amp} / {t:0, y:-amp}. amplitude clamped to [0.2, 1.0], wags floored at 1.
    /// Keeps the wiggle id/name/icon.
    static func wiggleTrick(amplitude: Double, wags: Int) -> Trick {
        let a = Swift.min(wiggleAmpMax, Swift.max(wiggleAmpMin, amplitude))
        let n = Swift.max(wiggleWagsMin, wags)
        let steps = (0..<(2 * n)).map { TrickStep(t: 0, y: $0 % 2 == 0 ? a : -a, ms: wiggleStepMs) }
        return Trick(id: wiggle.id, nameKey: wiggle.nameKey, icon: wiggle.icon, steps: steps)
    }

    // MARK: per-action durations (host-tested)
    static let durMin = 100      // ms
    static let durMax = 10000

    static func clampDur(_ ms: Int) -> Int { min(durMax, max(durMin, ms)) }

    private static func same(_ a: (t: Double, y: Double), _ t: Double, _ y: Double) -> Bool {
        abs(a.t - t) < 1e-9 && abs(a.y - y) < 1e-9
    }

    /// Distinct movements of a trick, in first-appearance order, with how many steps each spans.
    static func distinctActions(_ trick: Trick) -> [(t: Double, y: Double, count: Int)] {
        var order: [(t: Double, y: Double)] = []
        var counts: [Int] = []
        for s in trick.steps {
            if let i = order.firstIndex(where: { same($0, s.t, s.y) }) { counts[i] += 1 }
            else { order.append((s.t, s.y)); counts.append(1) }
        }
        return zip(order, counts).map { ($0.t, $0.y, $1) }
    }

    /// Base duration (ms) of each distinct action.
    static func baseDurations(_ trick: Trick) -> [Int] {
        distinctActions(trick).map { a in
            trick.steps.first(where: { same((a.t, a.y), $0.t, $0.y) })?.ms ?? durMin
        }
    }

    /// Rebuild the timeline: same order/count as the base, each step's ms = the (clamped)
    /// duration of its action. Wrong-length `durs` → the base trick unchanged.
    static func withDurations(_ trick: Trick, _ durs: [Int]) -> Trick {
        let acts = distinctActions(trick)
        guard durs.count == acts.count else { return trick }
        let steps = trick.steps.map { s -> TrickStep in
            let i = acts.firstIndex(where: { same(($0.t, $0.y), s.t, s.y) }) ?? 0
            return TrickStep(t: s.t, y: s.y, ms: clampDur(durs[i]))
        }
        return Trick(id: trick.id, nameKey: trick.nameKey, icon: trick.icon, steps: steps)
    }

    /// Movement signs for labeling: fwd ∈ {-1,0,1} (back/none/forward), turn ∈ {-1,0,1} (left/none/right).
    static func actionDescriptor(_ t: Double, _ y: Double) -> (fwd: Int, turn: Int) {
        let e = 0.05
        return (t > e ? 1 : (t < -e ? -1 : 0), y > e ? 1 : (y < -e ? -1 : 0))
    }
}
