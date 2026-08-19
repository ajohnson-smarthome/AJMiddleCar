import Foundation

/// Open-loop differential-drive ("tank") kinematics for a trick timeline.
/// Idealized (no wheel slip) — a real maneuver skids wider; this is a planning estimate.
struct TrickSim {
    struct Pose { let x: Double; let y: Double; let theta: Double }   // metres, radians
    struct Result {
        let poses: [Pose]        // car-centre samples for drawing/animation
        let pathLenM: Double     // arc length of the centre path
        let turnRad: Double       // accumulated |heading change| → revolutions = turnRad / 2π
        let minX: Double; let minY: Double; let maxX: Double; let maxY: Double  // swept-body bbox
        var areaWM: Double { maxX - minX }
        var areaHM: Double { maxY - minY }
    }

    /// vmaxMS = π·D·rpm/60 (m/s). trackM = lateral wheel separation. car dims in metres.
    /// `initialTheta` is the starting heading (rad); 0 = nose along +x (drawn pointing right). In-place
    /// maneuvers (e.g. the wiggle) pass π/2 so the resting car reads vertical (nose up) in the preview.
    static func simulate(steps: [TrickStep], vmaxMS: Double, trackM: Double,
                         carLenM: Double, carWidM: Double, dtMS: Int = 10,
                         initialTheta: Double = 0) -> Result {
        var x = 0.0, y = 0.0, th = initialTheta
        var pathLen = 0.0, turn = 0.0
        var minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0
        var poses: [Pose] = []

        // Expand the swept bbox by the car's 4 corners at a pose.
        func expand(_ px: Double, _ py: Double, _ pth: Double) {
            let hl = carLenM / 2, hw = carWidM / 2
            let c = cos(pth), s = sin(pth)
            for sx in [hl, -hl] {
                for sy in [hw, -hw] {
                    let cx = px + sx * c - sy * s
                    let cy = py + sx * s + sy * c
                    minX = Swift.min(minX, cx); maxX = Swift.max(maxX, cx)
                    minY = Swift.min(minY, cy); maxY = Swift.max(maxY, cy)
                }
            }
        }

        let dt = Double(dtMS) / 1000
        let sampleEvery = Swift.max(1, 30 / Swift.max(1, dtMS))   // ~30 ms between drawn poses at the default dt=10 ms (>=1 tick)
        poses.append(Pose(x: x, y: y, theta: th))
        expand(x, y, th)
        var tick = 0
        for step in steps {
            let (l, r) = ControlModel.sides(t: step.t, y: step.y)
            let vL = l * vmaxMS, vR = r * vmaxMS
            let v = (vL + vR) / 2
            let w = (vR - vL) / trackM
            let n = Swift.max(1, Int((Double(step.ms) / 1000) / dt + 0.5))
            for _ in 0..<n {
                th += w * dt
                let dx = v * cos(th) * dt, dy = v * sin(th) * dt
                x += dx; y += dy
                pathLen += Swift.abs(v) * dt
                turn += abs(w) * dt
                expand(x, y, th)
                tick += 1
                if tick % sampleEvery == 0 { poses.append(Pose(x: x, y: y, theta: th)) }
            }
        }
        if let last = poses.last, last.x != x || last.y != y || last.theta != th {
            poses.append(Pose(x: x, y: y, theta: th))
        }
        return Result(poses: poses, pathLenM: pathLen, turnRad: turn,
                      minX: minX, minY: minY, maxX: maxX, maxY: maxY)
    }
}
