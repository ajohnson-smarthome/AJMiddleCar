import XCTest
@testable import ESP32Car

final class TrickSimTests: XCTestCase {
    func testStraightLine() {
        let r = TrickSim.simulate(steps: [TrickStep(t: 1, y: 0, ms: 1000)],
                                  vmaxMS: 1, trackM: 0.15, carLenM: 0.25, carWidM: 0.15)
        XCTAssertEqual(r.pathLenM, 1.0, accuracy: 0.02)
        XCTAssertEqual(r.turnRad, 0.0, accuracy: 0.01)
        XCTAssertEqual(r.areaWM, 1.25, accuracy: 0.03)
        XCTAssertEqual(r.areaHM, 0.15, accuracy: 0.01)
    }
    func testSpinInPlace() {
        let r = TrickSim.simulate(steps: [TrickStep(t: 0, y: 1, ms: 1000)],
                                  vmaxMS: 1, trackM: 0.15, carLenM: 0.25, carWidM: 0.15)
        XCTAssertEqual(r.pathLenM, 0.0, accuracy: 0.01)
        XCTAssertEqual(r.turnRad, 2.0 / 0.15, accuracy: 0.2)
    }
    func testDonutCurves() {
        let r = TrickSim.simulate(steps: [TrickStep(t: 0.7, y: 1, ms: 1000)],
                                  vmaxMS: 1, trackM: 0.15, carLenM: 0.25, carWidM: 0.15)
        XCTAssertTrue(r.pathLenM > 0.3 && r.pathLenM < 0.5)
        XCTAssertTrue(r.turnRad > 6 && r.turnRad < 9)
        XCTAssertGreaterThan(r.poses.count, 5)
    }
    func testDonutSidesRoundTrip() {
        let T = Tricks.donutTrackFallbackM
        let d = Tricks.donutSides(diameterCm: 50, trackM: T)
        XCTAssertEqual(d.t, 0.794, accuracy: 0.01)
        XCTAssertEqual(d.y, 0.206, accuracy: 0.01)
        for diaCm in [30.0, 60.0, 120.0] {
            let s = Tricks.donutSides(diameterCm: diaCm, trackM: T)
            let sides = ControlModel.sides(t: s.t, y: s.y)
            let R = T * (sides.left + sides.right) / (2 * (sides.left - sides.right))
            XCTAssertEqual(R, diaCm / 100 / 2, accuracy: 0.005)
        }
    }
    func testDonutTrackSensitivity() {
        XCTAssertNotEqual(Tricks.donutSides(diameterCm: 50, trackM: 0.10).y,
                          Tricks.donutSides(diameterCm: 50, trackM: 0.13).y)
        for tk in [0.10, 0.13, 0.16] {
            let s = Tricks.donutSides(diameterCm: 60, trackM: tk)
            let sides = ControlModel.sides(t: s.t, y: s.y)
            let R = tk * (sides.left + sides.right) / (2 * (sides.left - sides.right))
            XCTAssertEqual(R, 0.30, accuracy: 0.01)
        }
    }
    func testDonutCirclesRoundTrip() {
        for v in [0.4, 0.578, 0.9] {
            for diaCm in [30.0, 50.0, 120.0] {
                for n in [1, 2, 5] {
                    let trick = Tricks.donutTrick(diameterCm: diaCm, circles: n, vmaxMS: v,
                                                  trackM: Tricks.donutTrackFallbackM)
                    let r = TrickSim.simulate(steps: trick.steps, vmaxMS: v,
                                              trackM: Tricks.donutTrackFallbackM, carLenM: 0.25, carWidM: 0.15)
                    XCTAssertEqual(r.turnRad / (2 * Double.pi), Double(n), accuracy: 0.05)
                }
            }
        }
    }
    func testDonutDurationGuards() {
        let T = Tricks.donutTrackFallbackM
        let y50 = Tricks.donutSides(diameterCm: 50, trackM: T).y
        XCTAssertEqual(Tricks.donutDurationMs(circles: 2, y: y50, vmaxMS: Tricks.donutNominalVmaxMS, trackM: T), 6848)
        XCTAssertEqual(Tricks.donutDurationMs(circles: 2, y: 0.2, vmaxMS: 0, trackM: T), 0)
        XCTAssertEqual(Tricks.donutDurationMs(circles: 2, y: 0, vmaxMS: 0.5, trackM: T), 0)
        XCTAssertEqual(Tricks.donutDurationMs(circles: 1, y: 0.2, vmaxMS: 0.5, trackM: 0.26),
                       2 * Tricks.donutDurationMs(circles: 1, y: 0.2, vmaxMS: 0.5, trackM: 0.13))
    }
    func testDonutTrickCircles() {
        let T = Tricks.donutTrackFallbackM
        let t = Tricks.donutTrick(diameterCm: 50, circles: 2, vmaxMS: 0.578, trackM: T)
        XCTAssertEqual(t.id, Tricks.donut.id)
        XCTAssertEqual(t.steps.count, 1)
        XCTAssertEqual(t.steps[0].ms,
                       Tricks.donutDurationMs(circles: 2, y: t.steps[0].y, vmaxMS: 0.578, trackM: T))
    }
    func testSpinSpeedFormula() {
        let T = Tricks.donutTrackFallbackM, V = Tricks.donutNominalVmaxMS
        XCTAssertEqual(Tricks.spinSpeed(turns: 1, durationMs: 5000, vmaxMS: V, trackM: T), 0.141, accuracy: 0.005)
        XCTAssertEqual(Tricks.spinSpeed(turns: 2, durationMs: 5000, vmaxMS: V, trackM: T),
                       2 * Tricks.spinSpeed(turns: 1, durationMs: 5000, vmaxMS: V, trackM: T), accuracy: 1e-9)
        XCTAssertEqual(Tricks.spinSpeed(turns: 1, durationMs: 10000, vmaxMS: V, trackM: T),
                       0.5 * Tricks.spinSpeed(turns: 1, durationMs: 5000, vmaxMS: V, trackM: T), accuracy: 1e-9)
        XCTAssertEqual(Tricks.spinSpeed(turns: 6, durationMs: 1000, vmaxMS: V, trackM: T), 1.0)
        XCTAssertEqual(Tricks.spinSpeed(turns: 1, durationMs: 0, vmaxMS: V, trackM: T), 0)
        XCTAssertEqual(Tricks.spinSpeed(turns: 1, durationMs: 5000, vmaxMS: 0, trackM: T), 0)
    }
    func testSpinRoundTrip() {
        let T = Tricks.donutTrackFallbackM, V = Tricks.donutNominalVmaxMS
        for n in [1, 2, 3] {
            let trick = Tricks.spinTrick(turns: n, durationMs: 5000, vmaxMS: V, trackM: T)
            let r = TrickSim.simulate(steps: trick.steps, vmaxMS: V, trackM: T, carLenM: 0.25, carWidM: 0.15)
            XCTAssertEqual(r.turnRad / (2 * Double.pi), Double(n), accuracy: 0.05)
            XCTAssertEqual(r.pathLenM, 0.0, accuracy: 0.01)
        }
    }
    func testSpinTrick() {
        let t = Tricks.spinTrick(turns: 2, durationMs: 3000, vmaxMS: Tricks.donutNominalVmaxMS,
                                 trackM: Tricks.donutTrackFallbackM)
        XCTAssertEqual(t.id, Tricks.spin.id)
        XCTAssertEqual(t.steps.count, 1)
        XCTAssertEqual(t.steps[0].ms, 3000)
        XCTAssertEqual(t.steps[0].t, 0)
    }

    func testFigure8Geometry() {
        let T = 0.13
        let sides = Tricks.donutSides(diameterCm: 60, trackM: T)
        let g = Tricks.figure8Trick(diameterCm: 60, eights: 3, vmaxMS: 0.578, trackM: T)
        XCTAssertEqual(g.steps.count, 6)                       // 2 lobes × 3 eights
        XCTAssertEqual(g.steps[0].t, sides.t, accuracy: 1e-9)
        XCTAssertEqual(g.steps[0].y, sides.y, accuracy: 1e-9)
        XCTAssertEqual(g.steps[1].y, -sides.y, accuracy: 1e-9) // mirror lobe
        XCTAssertEqual(g.steps[0].t, g.steps[1].t, accuracy: 1e-9)
        XCTAssertGreaterThan(g.steps[0].t, 0)
        XCTAssertEqual(g.id, Tricks.figure8.id)
    }

    func testFigure8Degenerate() {
        let g = Tricks.figure8Trick(diameterCm: 50, eights: 2, vmaxMS: 0, trackM: 0.13)
        XCTAssertEqual(g.steps.count, 4)
        XCTAssertTrue(g.steps.allSatisfy { $0.ms == 0 })
    }

    func testFigure8RoundTrip() {
        let V = Tricks.donutNominalVmaxMS, T = 0.13
        for eights in [1, 2] {
            let g = Tricks.figure8Trick(diameterCm: 60, eights: eights, vmaxMS: V, trackM: T)
            let r = TrickSim.simulate(steps: g.steps, vmaxMS: V, trackM: T, carLenM: 0.25, carWidM: 0.15)
            XCTAssertEqual(r.turnRad / (2 * .pi), Double(2 * eights), accuracy: 0.2)
            let last = r.poses.last!
            XCTAssertLessThan(hypot(last.x, last.y), 0.6 * 0.5)   // figure-8 returns near its start
        }
    }

    func testWiggleStructure() {
        let w = Tricks.wiggleTrick(amplitude: 0.8, wags: 10)
        XCTAssertEqual(w.steps.count, 20)                      // 2 steps × 10 wags
        XCTAssertEqual(w.steps[0].y, 0.8, accuracy: 1e-9)
        XCTAssertEqual(w.steps[1].y, -0.8, accuracy: 1e-9)     // alternating
        XCTAssertTrue(w.steps.allSatisfy { $0.t == 0 })
        XCTAssertTrue(w.steps.allSatisfy { $0.ms == 250 })
        XCTAssertEqual(w.id, Tricks.wiggle.id)
    }

    func testWiggleDefaultMatchesBase() {
        let w = Tricks.wiggleTrick(amplitude: 0.8, wags: 10)
        let base = Tricks.wiggle
        XCTAssertEqual(w.steps.count, base.steps.count)
        for (a, b) in zip(w.steps, base.steps) {
            XCTAssertEqual(a.y, b.y, accuracy: 1e-9)
            XCTAssertEqual(a.ms, b.ms)
            XCTAssertEqual(a.t, b.t, accuracy: 1e-9)
        }
    }

    func testWiggleClamps() {
        XCTAssertEqual(Tricks.wiggleTrick(amplitude: 5.0, wags: 3).steps[0].y, 1.0, accuracy: 1e-9)
        XCTAssertEqual(Tricks.wiggleTrick(amplitude: 0.0, wags: 3).steps[0].y, 0.2, accuracy: 1e-9)
        XCTAssertEqual(Tricks.wiggleTrick(amplitude: 0.8, wags: 0).steps.count, 2)   // wags floored to 1
    }

    func testInitialThetaHeading() {
        // default 0 → a forward command heads +x (drawn pointing right)
        let r0 = TrickSim.simulate(steps: [TrickStep(t: 1, y: 0, ms: 1000)],
                                   vmaxMS: 1, trackM: 0.15, carLenM: 0.25, carWidM: 0.15)
        XCTAssertEqual(r0.poses[0].theta, 0, accuracy: 1e-9)
        XCTAssertGreaterThan(r0.maxX, 0.5)
        // π/2 → the same command heads +y (screen up), x stays ~0
        let r90 = TrickSim.simulate(steps: [TrickStep(t: 1, y: 0, ms: 1000)],
                                    vmaxMS: 1, trackM: 0.15, carLenM: 0.25, carWidM: 0.15,
                                    initialTheta: .pi / 2)
        XCTAssertEqual(r90.poses[0].theta, .pi / 2, accuracy: 1e-9)
        XCTAssertGreaterThan(r90.maxY, 0.5)
        XCTAssertEqual(r90.maxX, 0.075, accuracy: 0.02)
    }

    func testWiggleStartsVertical() {
        // The wiggle preview starts vertical (nose up = θ π/2) and stays in place.
        let w = Tricks.wiggleTrick(amplitude: 0.8, wags: 10)
        let r = TrickSim.simulate(steps: w.steps, vmaxMS: 0.578, trackM: 0.13,
                                  carLenM: 0.25, carWidM: 0.15, initialTheta: .pi / 2)
        XCTAssertEqual(r.poses[0].theta, .pi / 2, accuracy: 1e-9)
        XCTAssertLessThan(r.pathLenM, 0.05)
    }
}
