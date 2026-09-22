// Host test for the tricks' geometry and kinematics (Tricks, TrickSim). Run with swiftc.
// Moved here from the XCTest bundle, which tools/test-all.sh does not run.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}
func near(_ a: Double, _ b: Double, _ eps: Double) -> Bool { abs(a - b) <= eps }

let carLen = 0.25, carWid = 0.15

// MARK: base layout and per-action durations

for tr in Tricks.all {
    check(!tr.steps.isEmpty, "trick \(tr.id) has steps")
    check(tr.totalMs == 5000, "trick \(tr.id) is 5 s at base")
    for s in tr.steps {
        check(s.t >= -1 && s.t <= 1 && s.y >= -1 && s.y <= 1 && s.ms > 0,
              "trick \(tr.id): axes inside [-1,1], positive duration")
    }
}
check(Set(Tricks.all.map { $0.id }).count == Tricks.all.count, "trick ids are unique")

check(Tricks.distinctActions(Tricks.spin).count == 1, "spin has one action")
check(Tricks.distinctActions(Tricks.figure8).count == 2, "figure-8 has two actions")
let wiggleActs = Tricks.distinctActions(Tricks.wiggle)
check(wiggleActs.count == 2, "wiggle has two actions")
check(wiggleActs.count == 2 && wiggleActs[0].count == 10 && wiggleActs[1].count == 10,
      "each wiggle action spans ten steps")

check(Tricks.baseDurations(Tricks.figure8) == [2500, 2500], "figure-8 base durations")
check(Tricks.baseDurations(Tricks.wiggle) == [250, 250], "wiggle base durations")

let redone = Tricks.withDurations(Tricks.wiggle, [400, 100])
check(redone.steps.count == 20, "withDurations keeps the step count")
check(redone.steps[0].ms == 400 && redone.steps[1].ms == 100, "withDurations sets each action's ms")
check(redone.totalMs == 10 * 400 + 10 * 100, "withDurations total")
check(Tricks.withDurations(Tricks.spin, [99]).steps[0].ms == 100, "withDurations clamps to durMin")
check(Tricks.withDurations(Tricks.spin, [1, 2]).totalMs == 5000, "wrong-length durations → base")

check(Tricks.actionDescriptor(0, 1) == (0, 1), "descriptor: turn in place")
check(Tricks.actionDescriptor(0.6, -0.6) == (1, -1), "descriptor: forward, left")

// MARK: kinematics

let straight = TrickSim.simulate(steps: [TrickStep(t: 1, y: 0, ms: 1000)],
                                 vmaxMS: 1, trackM: 0.15, carLenM: carLen, carWidM: carWid)
check(near(straight.pathLenM, 1.0, 0.02), "straight: path 1 m")
check(near(straight.turnRad, 0, 0.01), "straight: no turn")
check(near(straight.areaWM, 1.25, 0.03), "straight: area width = path + body length")
check(near(straight.areaHM, 0.15, 0.01), "straight: area height = body width")

let pivot = TrickSim.simulate(steps: [TrickStep(t: 0, y: 1, ms: 1000)],
                              vmaxMS: 1, trackM: 0.15, carLenM: carLen, carWidM: carWid)
check(near(pivot.pathLenM, 0, 0.01), "pivot: centre stays put")
check(near(pivot.turnRad, 2.0 / 0.15, 0.2), "pivot: ω = 2·vmax/track")

let curve = TrickSim.simulate(steps: [TrickStep(t: 0.7, y: 1, ms: 1000)],
                              vmaxMS: 1, trackM: 0.15, carLenM: carLen, carWidM: carWid)
check(curve.pathLenM > 0.3 && curve.pathLenM < 0.5, "curve: path length")
check(curve.turnRad > 6 && curve.turnRad < 9, "curve: turn")
check(curve.poses.count > 5, "curve: sampled poses")

// Initial heading: 0 heads +x, π/2 heads +y.
let r0 = TrickSim.simulate(steps: [TrickStep(t: 1, y: 0, ms: 1000)],
                           vmaxMS: 1, trackM: 0.15, carLenM: carLen, carWidM: carWid)
check(near(r0.poses[0].theta, 0, 1e-9), "default heading is 0")
check(r0.maxX > 0.5, "heading 0 drives along +x")
let r90 = TrickSim.simulate(steps: [TrickStep(t: 1, y: 0, ms: 1000)],
                            vmaxMS: 1, trackM: 0.15, carLenM: carLen, carWidM: carWid,
                            initialTheta: .pi / 2)
check(near(r90.poses[0].theta, .pi / 2, 1e-9), "initial heading π/2 is kept")
check(r90.maxY > 0.5, "heading π/2 drives along +y")
check(near(r90.maxX, 0.075, 0.02), "heading π/2 stays on x ≈ 0")

// MARK: donut — N circles → N turns, radius D/2, slow side never reverses

let T = Tricks.donutTrackFallbackM
let d50 = Tricks.donutSides(diameterCm: 50, trackM: T)
check(near(d50.t, 0.794, 0.01) && near(d50.y, 0.206, 0.01), "donut sides at 50 cm")
for diaCm in [30.0, 60.0, 120.0] {
    let s = Tricks.donutSides(diameterCm: diaCm, trackM: T)
    let sides = ControlModel.sides(t: s.t, y: s.y)
    let R = T * (sides.left + sides.right) / (2 * (sides.left - sides.right))
    check(near(R, diaCm / 100 / 2, 0.005), "donut radius is D/2 at \(diaCm) cm")
}
for tk in [0.10, 0.13, 0.16] {
    let s = Tricks.donutSides(diameterCm: 60, trackM: tk)
    let sides = ControlModel.sides(t: s.t, y: s.y)
    let R = tk * (sides.left + sides.right) / (2 * (sides.left - sides.right))
    check(near(R, 0.30, 0.01), "donut radius follows the track \(tk)")
}
check(Tricks.donutSides(diameterCm: 50, trackM: 0.10).y != Tricks.donutSides(diameterCm: 50, trackM: 0.13).y,
      "donut sides depend on the track")
for diaCm in [0.0, 5.0, 20.0, 50.0, 150.0, 1000.0] {
    let s = Tricks.donutSides(diameterCm: diaCm, trackM: T)
    let sides = ControlModel.sides(t: s.t, y: s.y)
    check(sides.left >= 0 && sides.right >= 0, "donut slow side never reverses at \(diaCm) cm")
}

for v in [0.4, 0.578, 0.9] {
    for diaCm in [30.0, 50.0, 120.0] {
        for n in [1, 2, 5] {
            let trick = Tricks.donutTrick(diameterCm: diaCm, circles: n, vmaxMS: v, trackM: T)
            let r = TrickSim.simulate(steps: trick.steps, vmaxMS: v, trackM: T,
                                      carLenM: carLen, carWidM: carWid)
            check(near(r.turnRad / (2 * .pi), Double(n), 0.05),
                  "donut: \(n) circles → \(n) turns (v \(v), \(diaCm) cm)")
        }
    }
}

let y50 = d50.y
check(Tricks.donutDurationMs(circles: 2, y: y50, vmaxMS: 0.578, trackM: T) == 6848, "donut duration")
check(Tricks.donutDurationMs(circles: 2, y: 0.2, vmaxMS: 0, trackM: T) == 0, "donut: zero speed → 0")
check(Tricks.donutDurationMs(circles: 2, y: 0, vmaxMS: 0.5, trackM: T) == 0, "donut: zero y → 0")
check(Tricks.donutDurationMs(circles: 1, y: 0.2, vmaxMS: 0.5, trackM: 0.26)
      == 2 * Tricks.donutDurationMs(circles: 1, y: 0.2, vmaxMS: 0.5, trackM: 0.13),
      "donut duration scales with the track")

let donut2 = Tricks.donutTrick(diameterCm: 50, circles: 2, vmaxMS: 0.578, trackM: T)
check(donut2.id == Tricks.donut.id, "donutTrick keeps the donut id")
check(donut2.steps.count == 1, "donutTrick is one step")
check(donut2.steps[0].ms == Tricks.donutDurationMs(circles: 2, y: donut2.steps[0].y, vmaxMS: 0.578, trackM: T),
      "donutTrick's duration is donutDurationMs")

// MARK: spin — N turns in T, never faster than full power

let V = 0.578
func spin(_ n: Int, _ ms: Int, _ v: Double = V) -> Double {
    Tricks.spinSpeed(turns: n, durationMs: ms, vmaxMS: v, trackM: T)
}
check(near(spin(1, 5000), 0.141, 0.005), "spin speed formula")
check(near(spin(2, 5000), 2 * spin(1, 5000), 1e-9), "spin speed ∝ turns")
check(near(spin(1, 10000), 0.5 * spin(1, 5000), 1e-9), "spin speed ∝ 1/duration")
check(spin(6, 1000) == 1.0, "spin is never faster than full power")
check(spin(1, 0) == 0, "spin: zero duration → 0")
check(spin(1, 5000, 0) == 0, "spin: zero speed → 0")

for n in [1, 2, 3] {
    let trick = Tricks.spinTrick(turns: n, durationMs: 5000, vmaxMS: Tricks.donutNominalVmaxMS, trackM: T)
    let r = TrickSim.simulate(steps: trick.steps, vmaxMS: Tricks.donutNominalVmaxMS, trackM: T,
                              carLenM: carLen, carWidM: carWid)
    check(near(r.turnRad / (2 * .pi), Double(n), 0.05), "spin: \(n) turns in 5 s")
    check(near(r.pathLenM, 0, 0.01), "spin: \(n) turns in place")
}

let spin2 = Tricks.spinTrick(turns: 2, durationMs: 3000, vmaxMS: Tricks.donutNominalVmaxMS, trackM: T)
check(spin2.id == Tricks.spin.id, "spinTrick keeps the spin id")
check(spin2.steps.count == 1, "spinTrick is one step")
check(spin2.steps[0].ms == 3000, "spinTrick lasts its duration")
check(spin2.steps[0].t == 0, "spinTrick does not drive forward")

// MARK: figure-8 — two mirrored lobes, returns near its start

let fT = 0.13
let fSides = Tricks.donutSides(diameterCm: 60, trackM: fT)
let fig = Tricks.figure8Trick(diameterCm: 60, eights: 3, vmaxMS: V, trackM: fT)
check(fig.steps.count == 6, "figure-8: 2 lobes × 3 eights")
check(near(fig.steps[0].t, fSides.t, 1e-9) && near(fig.steps[0].y, fSides.y, 1e-9),
      "figure-8: first lobe is a donut of the diameter")
check(near(fig.steps[1].y, -fSides.y, 1e-9), "figure-8: second lobe is mirrored")
check(near(fig.steps[0].t, fig.steps[1].t, 1e-9), "figure-8: lobes share t")
check(fig.steps[0].t > 0, "figure-8 drives forward")
check(fig.id == Tricks.figure8.id, "figure8Trick keeps the figure-8 id")

let figZero = Tricks.figure8Trick(diameterCm: 50, eights: 2, vmaxMS: 0, trackM: fT)
check(figZero.steps.count == 4, "figure-8 at zero speed keeps its steps")
check(figZero.steps.allSatisfy { $0.ms == 0 }, "figure-8 at zero speed → 0 ms")

for eights in [1, 2] {
    let g = Tricks.figure8Trick(diameterCm: 60, eights: eights, vmaxMS: Tricks.donutNominalVmaxMS, trackM: fT)
    let r = TrickSim.simulate(steps: g.steps, vmaxMS: Tricks.donutNominalVmaxMS, trackM: fT,
                              carLenM: carLen, carWidM: carWid)
    check(near(r.turnRad / (2 * .pi), Double(2 * eights), 0.2), "figure-8: \(eights) eights → \(2 * eights) turns")
    let last = r.poses.last!
    check(hypot(last.x, last.y) < 0.6 * 0.5, "figure-8 returns near its start (\(eights))")
}

// MARK: wiggle — alternating yaw in place, no geometry

let wig = Tricks.wiggleTrick(amplitude: 0.8, wags: 10)
check(wig.steps.count == 20, "wiggle: 2 steps × 10 wags")
check(near(wig.steps[0].y, 0.8, 1e-9) && near(wig.steps[1].y, -0.8, 1e-9), "wiggle alternates")
check(wig.steps.allSatisfy { $0.t == 0 }, "wiggle does not drive forward")
check(wig.steps.allSatisfy { $0.ms == 250 }, "wiggle's fixed tempo")
check(wig.id == Tricks.wiggle.id, "wiggleTrick keeps the wiggle id")
check(wig.steps.count == Tricks.wiggle.steps.count, "default wiggle has the base step count")
for (a, b) in zip(wig.steps, Tricks.wiggle.steps) {
    check(near(a.y, b.y, 1e-9) && a.ms == b.ms && near(a.t, b.t, 1e-9), "default wiggle equals the base")
}
check(near(Tricks.wiggleTrick(amplitude: 5.0, wags: 3).steps[0].y, 1.0, 1e-9), "wiggle amplitude clamps high")
check(near(Tricks.wiggleTrick(amplitude: 0.0, wags: 3).steps[0].y, 0.2, 1e-9), "wiggle amplitude clamps low")
check(Tricks.wiggleTrick(amplitude: 0.8, wags: 0).steps.count == 2, "wiggle wags floored to 1")

let wigSim = TrickSim.simulate(steps: wig.steps, vmaxMS: V, trackM: 0.13,
                               carLenM: carLen, carWidM: carWid, initialTheta: .pi / 2)
check(near(wigSim.poses[0].theta, .pi / 2, 1e-9), "wiggle preview starts nose up")
check(wigSim.pathLenM < 0.05, "wiggle stays in place")

// MARK: degenerate input — zero speed or track never becomes NaN

let degenerate: [Trick] = [
    Tricks.donutTrick(diameterCm: 50, circles: 2, vmaxMS: 0, trackM: T),
    Tricks.donutTrick(diameterCm: 50, circles: 2, vmaxMS: V, trackM: 0),
    Tricks.spinTrick(turns: 2, durationMs: 3000, vmaxMS: 0, trackM: T),
    Tricks.spinTrick(turns: 2, durationMs: 0, vmaxMS: V, trackM: T),
    Tricks.figure8Trick(diameterCm: 50, eights: 2, vmaxMS: 0, trackM: T),
    Tricks.figure8Trick(diameterCm: 50, eights: 2, vmaxMS: V, trackM: 0),
]
for tr in degenerate {
    check(tr.steps.allSatisfy { $0.t.isFinite && $0.y.isFinite && $0.ms >= 0 },
          "trick \(tr.id) stays finite on degenerate input")
    let r = TrickSim.simulate(steps: tr.steps, vmaxMS: 0, trackM: T, carLenM: carLen, carWidM: carWid)
    check(r.pathLenM.isFinite && r.turnRad.isFinite && r.areaWM.isFinite && r.areaHM.isFinite,
          "simulation of trick \(tr.id) stays finite at zero speed")
}

// MARK: assembly mode — what the list shows is what plays (AJM-60)

// An in-memory store: a suite on disk would leave a plist behind in ~/Library/Preferences.
final class MemoryDefaults: UserDefaults {
    var d: [String: Any] = [:]
    override func object(forKey k: String) -> Any? { d[k] }
    override func array(forKey k: String) -> [Any]? { d[k] as? [Any] }
    override func string(forKey k: String) -> String? { d[k] as? String }
    override func set(_ v: Any?, forKey k: String) { d[k] = v }
    override func set(_ v: Int, forKey k: String) { d[k] = v }
    override func set(_ v: Double, forKey k: String) { d[k] = v }
    override func removeObject(forKey k: String) { d[k] = nil }
}
TrickSettings.store = MemoryDefaults(suiteName: nil)!

let VN = Tricks.donutNominalVmaxMS
func played(_ tr: Trick) -> Trick {   // ControlIntent.build's body, at the nominals of an empty cache
    Tricks.assemble(tr, TrickSettings.params(for: tr), vmaxMS: VN, trackM: T)
}

for tr in Tricks.all {
    check(TrickSettings.mode(for: tr) == .geometry, "trick \(tr.id) defaults to geometry")
}
// The bug: the list summed the base layout (5.0 s); the spin plays its 3 s duration.
check(Tricks.withDurations(Tricks.spin, TrickSettings.durations(for: Tricks.spin)).totalMs == 5000,
      "the old list number for the spin was 5.0 s")
check(played(Tricks.spin).totalMs == Tricks.spinDurDefaultMs, "spin at defaults plays 3.0 s")
check(played(Tricks.donut).totalMs == Tricks.donutTrick(diameterCm: 50, circles: 2, vmaxMS: VN, trackM: T).totalMs,
      "donut at defaults plays its geometry")
check(played(Tricks.donut).totalMs != 5000, "donut at defaults is not the base 5 s")

// A geometry parameter moves the number.
let donutBefore = played(Tricks.donut).totalMs
TrickSettings.setDonutCircles(4)
check(played(Tricks.donut).totalMs > donutBefore, "more donut circles → longer")
TrickSettings.resetDonutCircles()

// Manual: the spin's single action at 2 s plays 2 s; the mode survives a re-read.
TrickSettings.setMode(Tricks.spin, .manual)
TrickSettings.setDuration(Tricks.spin, action: 0, ms: 2000)
check(TrickSettings.mode(for: Tricks.spin) == .manual, "manual mode is stored")
check(played(Tricks.spin).totalMs == 2000, "manual spin at 2 s plays 2 s")
check(played(Tricks.spin).steps.map { $0.y } == Tricks.spin.steps.map { $0.y },
      "manual mode keeps the base layout's axes")
check(played(Tricks.donut).totalMs == donutBefore, "one trick's mode leaves the others alone")

// Switching the mode switches the assembly, with the durations kept for the next switch.
TrickSettings.setMode(Tricks.spin, .geometry)
check(played(Tricks.spin).totalMs == Tricks.spinDurDefaultMs, "back to geometry → 3 s again")
check(TrickSettings.durations(for: Tricks.spin) == [2000], "manual durations survive the switch")
TrickSettings.setMode(Tricks.spin, .manual)
TrickSettings.reset(Tricks.spin, action: 0)
check(played(Tricks.spin).totalMs == 5000, "reset returns the action's base duration")

// clampDur holds 100…10000 ms on write and on read.
TrickSettings.setDuration(Tricks.spin, action: 0, ms: 50)
check(TrickSettings.durations(for: Tricks.spin) == [Tricks.durMin], "a write below range clamps to 100 ms")
TrickSettings.store.set([60000], forKey: "trick.durs.\(Tricks.spin.id)")
check(TrickSettings.durations(for: Tricks.spin) == [Tricks.durMax], "a stored 60 s reads as 10 s")
check(played(Tricks.spin).totalMs == Tricks.durMax, "playback sees the clamped value too")
TrickSettings.store.set("sideways", forKey: "trick.mode.\(Tricks.spin.id)")
check(TrickSettings.mode(for: Tricks.spin) == .geometry, "an unknown stored mode reads as geometry")

// Every trick, both modes, the preview's input included: axes inside [-1,1], nothing non-finite.
var manualAll = TrickParams(); manualAll.mode = .manual
for tr in Tricks.all {
    for p in [TrickParams(), manualAll] {
        for (v, trk) in [(VN, T), (0.0, T), (VN, 0.0)] {
            let built = Tricks.assemble(tr, p, vmaxMS: v, trackM: trk)
            check(built.id == tr.id && !built.steps.isEmpty, "trick \(tr.id) \(p.mode) assembles")
            check(built.steps.allSatisfy { $0.t.isFinite && $0.y.isFinite && abs($0.t) <= 1 && abs($0.y) <= 1 && $0.ms >= 0 },
                  "trick \(tr.id) \(p.mode) at v=\(v) track=\(trk): finite, axes in [-1,1]")
        }
    }
    check(Tricks.assemble(tr, manualAll, vmaxMS: VN, trackM: T).totalMs == 5000,
          "trick \(tr.id) manual with no durations → its base 5 s")
}

// MARK: nominal speed — one fallback for the preview and playback (AJM-61)

// Unknown wheel or unmatched preset: both ControlIntent.vmax and the preview land here.
check(Tricks.vmaxMS(diameterMm: nil, rpm: nil) == VN, "no /wheel → nominal speed")
check(Tricks.vmaxMS(diameterMm: 65, rpm: nil) == VN, "wheel known, preset unmatched → nominal speed")
check(Tricks.vmaxMS(diameterMm: nil, rpm: 1000) == VN, "rpm without a diameter → nominal speed")
check(near(Tricks.vmaxMS(diameterMm: 65, rpm: 1000), VN, 0.001),
      "the nominal is the stock motor on the default 65 mm wheel")
check(near(Tricks.vmaxMS(diameterMm: 80, rpm: 500), Double.pi * 0.08 * 500 / 60, 1e-9),
      "a matched preset gives π·D·rpm/60")

// The preview's input on an unmatched preset is playback's: same steps, same totalMs, and a
// trajectory with numbers rather than a blank.
TrickSettings.setMode(Tricks.spin, .geometry)
let unmatched = Tricks.vmaxMS(diameterMm: 65, rpm: nil)
for tr in Tricks.all {
    let preview = Tricks.assemble(tr, TrickSettings.params(for: tr), vmaxMS: unmatched, trackM: T)
    let playback = played(tr)
    check(preview.steps.map { [$0.t, $0.y, Double($0.ms)] } == playback.steps.map { [$0.t, $0.y, Double($0.ms)] }
          && preview.totalMs == playback.totalMs,
          "trick \(tr.id): preview and playback agree on an unmatched preset")
    let r = TrickSim.simulate(steps: preview.steps, vmaxMS: unmatched, trackM: T,
                              carLenM: carLen, carWidM: carWid)
    check(r.pathLenM.isFinite && r.turnRad.isFinite && r.areaWM > 0 && r.areaHM > 0,
          "trick \(tr.id): nominal preview has numbers")
}

if failures == 0 { print("test_tricks: OK") } else { exit(1) }
