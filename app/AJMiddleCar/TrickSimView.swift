import SwiftUI

/// Top-down animated trajectory simulation for a trick. Loads wheel params (/wheel), derives the
/// motor's rated RPM from MotorPresets, runs TrickSim, and draws the swept body + centre path +
/// dimensioned bounding box + the moving car (app-style), with distance/revolutions/area stats.
/// iOS-only. Vertical layout: a compact animation box on top, stats below.
struct TrickSimView: View {
    let trick: Trick
    let durs: [Int]
    let palette: Palette
    var donutDiameterCm: Double? = nil
    var donutCircles: Int? = nil
    var spinTurns: Int? = nil
    var spinDurMs: Int? = nil
    var fig8Dia: Double? = nil
    var fig8Eights: Int? = nil
    var wiggleAmp: Double? = nil
    var wiggleWags: Int? = nil
    @State private var wheel: WheelClient.Params?
    @State private var track = Tricks.donutTrackFallbackM
    private var p: Palette { palette }

    // Car geometry — v1 constants (metres). TODO: move to settings next to the motor params.
    private static let carLenM = 0.25, carWidM = 0.15

    private var steps: [TrickStep] {
        if trick.id == Tricks.donut.id, let dia = donutDiameterCm, let n = donutCircles, let v = vmaxMS {
            return Tricks.donutTrick(diameterCm: dia, circles: n, vmaxMS: v, trackM: track).steps
        }
        if trick.id == Tricks.spin.id, let n = spinTurns, let ms = spinDurMs, let v = vmaxMS {
            return Tricks.spinTrick(turns: n, durationMs: ms, vmaxMS: v, trackM: track).steps
        }
        if trick.id == Tricks.figure8.id, let dia = fig8Dia, let n = fig8Eights, let v = vmaxMS {
            return Tricks.figure8Trick(diameterCm: dia, eights: n, vmaxMS: v, trackM: track).steps
        }
        if trick.id == Tricks.wiggle.id, let amp = wiggleAmp, let n = wiggleWags {
            return Tricks.wiggleTrick(amplitude: amp, wags: n).steps
        }
        let d = durs.isEmpty ? Tricks.baseDurations(trick) : durs
        return Tricks.withDurations(trick, d).steps
    }
    private var totalSec: Double { Double(steps.reduce(0) { $0 + $1.ms }) / 1000 }

    private var rpm: Int? {
        guard let w = wheel else { return nil }
        return MotorPresets.match(ppr: w.ppr, gearX100: w.gearX100, quad: w.quad)?.rpm
    }
    private var vmaxMS: Double? {
        guard let w = wheel, let rpm else { return nil }
        return Double.pi * (Double(w.diameterMm) / 1000) * Double(rpm) / 60
    }
    private var sim: TrickSim.Result? {
        guard let v = vmaxMS else { return nil }
        // The wiggle is in-place; start it vertical (nose up) so the resting car reads naturally.
        let theta0 = trick.id == Tricks.wiggle.id ? Double.pi / 2 : 0
        return TrickSim.simulate(steps: steps, vmaxMS: v, trackM: track,
                                 carLenM: Self.carLenM, carWidM: Self.carWidM, initialTheta: theta0)
    }

    var body: some View {
        VStack(spacing: 10) {
            if let r = sim {
                TimelineView(.animation) { tl in
                    Canvas { ctx, size in
                        draw(&ctx, size, r, time: tl.date.timeIntervalSinceReferenceDate)
                    }
                }
                .frame(width: 330, height: 224)   // animation box (~+30%)
                .frame(maxWidth: .infinity)        // centred
                stats(r)
            } else {
                Spacer(minLength: 12)
                Image(systemName: "gauge.with.dots.needle.bottom.50percent")
                    .font(.system(size: 26)).foregroundStyle(p.muted)
                Text(L.simPickMotor).font(.system(size: 13)).foregroundStyle(p.muted)
                    .multilineTextAlignment(.center).padding(.horizontal, 24)
                Spacer(minLength: 12)
            }
        }
        .padding(.horizontal, 12).padding(.top, 8)
        .task {
            wheel = await WheelClient().get()
            if let d = await DimsClient().get() { track = Double(d.trackMm) / 1000 }
        }
    }

    // MARK: stats
    private func stats(_ r: TrickSim.Result) -> some View {
        let turns = r.turnRad / (2 * .pi)
        return VStack(spacing: 6) {
            HStack(spacing: 8) {
                chip(L.simPath, String(format: "%.1f %@", r.pathLenM, L.mUnit))
                chip(L.simTurns, String(format: "%.1f", turns))
                chip(L.simArea, String(format: "%d×%d %@", Int((r.areaWM * 100).rounded()),
                                       Int((r.areaHM * 100).rounded()), L.cmUnit))
            }
            Text(L.simVerdict(totalSec, turns)).font(.system(size: 12)).foregroundStyle(p.muted)
        }
    }
    private func chip(_ key: String, _ value: String) -> some View {
        VStack(spacing: 2) {
            Text(value).font(.system(size: 15, weight: .bold)).foregroundStyle(p.accent).monospacedDigit()
            Text(key).font(.system(size: 9, weight: .semibold)).foregroundStyle(p.muted)
                .textCase(.uppercase)
        }
        .frame(maxWidth: .infinity).padding(.vertical, 6)
        .background(p.panel).clipShape(RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(p.metal.opacity(0.4), lineWidth: 1))
    }

    // MARK: drawing
    private func draw(_ ctx: inout GraphicsContext, _ size: CGSize, _ r: TrickSim.Result, time: Double) {
        let pad: CGFloat = 26   // room for dimension labels
        let bw = max(r.areaWM, 1e-3), bh = max(r.areaHM, 1e-3)
        let cx = (r.minX + r.maxX) / 2, cy = (r.minY + r.maxY) / 2
        let scale = min((size.width - 2 * pad) / bw, (size.height - 2 * pad) / bh)
        func toS(_ wx: Double, _ wy: Double) -> CGPoint {
            CGPoint(x: size.width / 2 + (wx - cx) * scale, y: size.height / 2 - (wy - cy) * scale)
        }
        let carL = Self.carLenM * scale, carW = Self.carWidM * scale

        func bodyPath(_ pose: TrickSim.Pose) -> Path {
            let c = toS(pose.x, pose.y)
            let t = CGAffineTransform(translationX: c.x, y: c.y).rotated(by: -pose.theta)
            return Path(roundedRect: CGRect(x: -carL / 2, y: -carW / 2, width: carL, height: carW),
                        cornerRadius: carW * 0.22).applying(t)
        }

        // 1) soft swept area: sparse, low-opacity body ghosts (no blinding solid disc). Skipped for an
        //    in-place maneuver (spin: path ≈ 0) where the ghosts pile into a dense disc that buries the
        //    car — leaving just the clear rotating car, so it reads as large as the donut's.
        if r.pathLenM > 0.05 {
            for (i, pose) in r.poses.enumerated() where i % 3 == 0 {
                ctx.fill(bodyPath(pose), with: .color(p.accent.opacity(0.04)))
            }
        }
        // 2) centre trajectory — dashed, drawn ON TOP so the path stays readable
        var path = Path()
        for (i, pose) in r.poses.enumerated() {
            let s = toS(pose.x, pose.y)
            if i == 0 { path.move(to: s) } else { path.addLine(to: s) }
        }
        ctx.stroke(path, with: .color(p.accent),
                   style: StrokeStyle(lineWidth: 1.5, lineCap: .round, dash: [4, 4]))
        // 3) bounding box + cm dimension labels
        let tl = toS(r.minX, r.maxY), br = toS(r.maxX, r.minY)
        let box = CGRect(x: tl.x, y: tl.y, width: br.x - tl.x, height: br.y - tl.y)
        ctx.stroke(Path(box), with: .color(p.muted.opacity(0.5)),
                   style: StrokeStyle(lineWidth: 1, dash: [3, 4]))
        let wCm = Int((r.areaWM * 100).rounded()), hCm = Int((r.areaHM * 100).rounded())
        ctx.draw(Text("\(wCm) \(L.cmUnit)").font(.system(size: 10, weight: .semibold))
                    .foregroundColor(p.muted), at: CGPoint(x: box.midX, y: box.minY - 9))
        ctx.draw(Text("\(hCm) \(L.cmUnit)").font(.system(size: 10, weight: .semibold))
                    .foregroundColor(p.muted), at: CGPoint(x: box.minX - 13, y: box.midY))
        // 4) the moving car (app-style: body + 4 corner wheels + windshield) at the current phase
        if r.poses.count > 1, totalSec > 0 {
            let phase = (time.truncatingRemainder(dividingBy: totalSec)) / totalSec
            let idx = min(r.poses.count - 1, max(0, Int(phase * Double(r.poses.count - 1))))
            drawCar(&ctx, r.poses[idx], carL: carL, carW: carW, toS: toS)
        }
    }

    /// The reference car (matches the other screens): dark corner wheels, panel body with an accent
    /// outline, a tinted windshield at the front. Forward = +x in body frame; drawn rotated to θ.
    private func drawCar(_ ctx: inout GraphicsContext, _ pose: TrickSim.Pose,
                         carL: CGFloat, carW: CGFloat, toS: (Double, Double) -> CGPoint) {
        let c = toS(pose.x, pose.y)
        let t = CGAffineTransform(translationX: c.x, y: c.y).rotated(by: -pose.theta)
        // wheels (under the body), poking out laterally at the 4 corners
        let wl = carL * 0.2, ww = carW * 0.2
        for sx in [carL * 0.3, -carL * 0.3] {
            for sy in [carW / 2, -carW / 2] {
                let wheel = CGRect(x: sx - wl / 2, y: sy - ww / 2, width: wl, height: ww)
                ctx.fill(Path(roundedRect: wheel, cornerRadius: 2).applying(t), with: .color(p.metal))
            }
        }
        let body = Path(roundedRect: CGRect(x: -carL / 2, y: -carW / 2, width: carL, height: carW),
                        cornerRadius: carW * 0.22).applying(t)
        ctx.fill(body, with: .color(p.panel))
        ctx.stroke(body, with: .color(p.accent), lineWidth: 2)
        // windshield strip near the front (+x)
        let wind = CGRect(x: carL * 0.5 - carW * 0.4, y: -carW * 0.27, width: carW * 0.32, height: carW * 0.54)
        ctx.fill(Path(roundedRect: wind, cornerRadius: 2).applying(t), with: .color(p.bg.opacity(0.75)))
    }
}
