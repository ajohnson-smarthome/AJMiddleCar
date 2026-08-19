import SwiftUI

/// Accumulates a rotation angle by integrating angular velocity over time, so the spin arrows
/// keep their position when speed changes (no jump back to the start).
final class SpinClock {
    var angle = 0.0
    private var last: Double?
    func tick(_ time: Double, omega: Double) -> Double {
        if let last, time - last < 0.5 { angle += omega * (time - last) }   // ignore gaps (spin paused)
        last = time
        return angle
    }
}

/// Animated top-down car diagram (1:1 with the firmware/calibration car) driven by (t, y):
/// dark corner wheels that colour by side speed + run a chevron tread, predicted-trajectory rails,
/// or a gradient "comet" spin indicator.
struct DriveDiagram: View {
    let t: Double
    let y: Double
    let palette: Palette

    @State private var spinClock = SpinClock()
    private var metal: Color { palette.metal }
    private let carW: CGFloat = 36
    private let carLen: CGFloat = 74
    private let wheelW: CGFloat = 12
    private let wheelH: CGFloat = 20
    private let railGap: CGFloat = 14

    var body: some View {
        TimelineView(.animation) { tl in
            Canvas { ctx, size in
                render(&ctx, size, time: tl.date.timeIntervalSinceReferenceDate)
            }
        }
        .frame(width: 112, height: 200)
        .scaleEffect(1.32)   // car size (+10% then +20%)
    }

    private func render(_ ctx: inout GraphicsContext, _ size: CGSize, time: Double) {
        let center = CGPoint(x: size.width / 2, y: size.height * 0.5)
        let sides = ControlModel.sides(t: t, y: y)
        switch ControlModel.diagramState(t: t, y: y) {
        case .drive: drawRails(&ctx, center: center)
        case .spin:  drawSpin(&ctx, center: center, time: time)
        case .idle:  break
        }
        drawCar(&ctx, center: center)
        let wx = carW / 2 + 1
        let wy = carLen / 2 - 16
        drawWheel(&ctx, cx: center.x - wx, cy: center.y - wy, speed: sides.left,  time: time)
        drawWheel(&ctx, cx: center.x - wx, cy: center.y + wy, speed: sides.left,  time: time)
        drawWheel(&ctx, cx: center.x + wx, cy: center.y - wy, speed: sides.right, time: time)
        drawWheel(&ctx, cx: center.x + wx, cy: center.y + wy, speed: sides.right, time: time)
    }

    private func wheelColor(_ s: Double) -> Color {
        if s > 0.03 { return palette.accent }
        if s < -0.03 { return palette.warn }
        return metal
    }

    private func drawCar(_ ctx: inout GraphicsContext, center: CGPoint) {
        let body = CGRect(x: center.x - carW / 2, y: center.y - carLen / 2, width: carW, height: carLen)
        let bp = Path(roundedRect: body, cornerRadius: 11)
        ctx.fill(bp, with: .color(palette.bg))      // opaque base so rails/spin under the car are covered
        ctx.fill(bp, with: .color(palette.panel))
        ctx.stroke(bp, with: .color(metal), lineWidth: 1)
        let wind = CGRect(x: center.x - 11, y: body.minY + 7, width: 22, height: 9)
        ctx.fill(Path(roundedRect: wind, cornerRadius: 3), with: .color(palette.bg.opacity(0.85)))
    }

    private func drawWheel(_ ctx: inout GraphicsContext, cx: CGFloat, cy: CGFloat, speed: Double, time: Double) {
        let rect = CGRect(x: cx - wheelW / 2, y: cy - wheelH / 2, width: wheelW, height: wheelH)
        let wp = Path(roundedRect: rect, cornerRadius: 3)
        ctx.fill(wp, with: .color(wheelColor(speed)))
        let mag = min(abs(speed), 1)
        guard mag > 0.03 else { return }

        var c = ctx
        c.clip(to: wp)
        let up = speed > 0
        let spacing = 13 - 6 * CGFloat(mag)
        let tempo = 70 * mag
        let offset = (CGFloat(time) * tempo).truncatingRemainder(dividingBy: spacing)
        let ch: CGFloat = 4
        var k = -2
        while CGFloat(k) * spacing < wheelH + spacing {
            let base = up ? (rect.maxY - CGFloat(k) * spacing + offset)
                          : (rect.minY + CGFloat(k) * spacing - offset)
            var p = Path()
            if up {
                p.move(to: CGPoint(x: rect.minX + 1, y: base + ch))
                p.addLine(to: CGPoint(x: rect.midX, y: base - ch))
                p.addLine(to: CGPoint(x: rect.maxX - 1, y: base + ch))
            } else {
                p.move(to: CGPoint(x: rect.minX + 1, y: base - ch))
                p.addLine(to: CGPoint(x: rect.midX, y: base + ch))
                p.addLine(to: CGPoint(x: rect.maxX - 1, y: base - ch))
            }
            c.stroke(p, with: .color(palette.bg), style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
            k += 1
        }
    }

    private func drawRails(_ ctx: inout GraphicsContext, center: CGPoint) {
        let forward = t >= 0
        let railColor = forward ? palette.accent : palette.warn
        let length = 50 + 80 * min(abs(t), 1)
        let halfW = carW / 2 + 2
        let startY = forward ? (center.y - carLen / 2 - railGap)
                             : (center.y + carLen / 2 + railGap)
        let pts = ControlModel.trajectoryPoints(t: t, y: y, length: length, steps: 24)
        let endY = startY + CGFloat(forward ? -length : length)
        let grad = Gradient(colors: [railColor.opacity(0.95), railColor.opacity(0.04)])
        for side in [CGFloat(-1), CGFloat(1)] {
            var path = Path()
            for (i, pt) in pts.enumerated() {
                let px = center.x + side * halfW + CGFloat(pt.x)
                let py = startY + (forward ? CGFloat(pt.y) : -CGFloat(pt.y))
                if i == 0 { path.move(to: CGPoint(x: px, y: py)) } else { path.addLine(to: CGPoint(x: px, y: py)) }
            }
            ctx.stroke(path, with: .linearGradient(grad,
                startPoint: CGPoint(x: center.x, y: startY),
                endPoint: CGPoint(x: center.x, y: endY)),
                style: StrokeStyle(lineWidth: 6, lineCap: .round, lineJoin: .round))
        }
    }

    private func drawSpin(_ ctx: inout GraphicsContext, center: CGPoint, time: Double) {
        var c = ctx
        c.translateBy(x: center.x, y: center.y)
        let sp: Double = y >= 0 ? 1 : -1
        let spinColor = sp >= 0 ? palette.accent : palette.warn   // colour by rotation direction
        // angle is *accumulated* (∫ω dt) so changing speed keeps the arrow where it is — no jump.
        let s = ControlModel.sides(t: t, y: y)
        let mag = min(max(abs(s.left), abs(s.right)), 1.0)
        let omega = 360.0 * mag / 1.5 * sp        // deg/s, signed (full deflection ≈ 1.5 s/turn)
        c.rotate(by: .degrees(spinClock.tick(time, omega: omega)))

        let r = 46.0
        let sweep = 115.0 * Double.pi / 180
        let n = 24
        for base in [0.0, Double.pi] {
            let start = base - sp * sweep / 2
            var pts: [CGPoint] = []
            for i in 0...n {
                let a = start + sp * sweep * Double(i) / Double(n)
                pts.append(CGPoint(x: Foundation.cos(a) * r, y: Foundation.sin(a) * r))
            }
            var path = Path()
            path.move(to: pts[0])
            for pt in pts.dropFirst() { path.addLine(to: pt) }
            // gradient comet: faint tail (pts[0]) → bright head (pts.last), like the rails
            let grad = Gradient(colors: [spinColor.opacity(0.0), spinColor.opacity(0.9)])
            c.stroke(path, with: .linearGradient(grad, startPoint: pts[0], endPoint: pts[pts.count - 1]),
                     style: StrokeStyle(lineWidth: 5, lineCap: .round, lineJoin: .round))
            // arrowhead: base at arc end, tip extending along the tangent
            let last = pts[pts.count - 1], prev = pts[pts.count - 2]
            let dir = Foundation.atan2(Double(last.y - prev.y), Double(last.x - prev.x))
            let aLen = 11.0
            let tip = CGPoint(x: last.x + CGFloat(Foundation.cos(dir) * aLen),
                              y: last.y + CGFloat(Foundation.sin(dir) * aLen))
            c.fill(arrowHead(tip: tip, angle: dir, size: aLen), with: .color(spinColor))
        }
    }

    private func arrowHead(tip: CGPoint, angle: Double, size: CGFloat) -> Path {
        let back = angle + Double.pi
        var p = Path()
        p.move(to: tip)
        p.addLine(to: CGPoint(x: tip.x + Foundation.cos(back + 0.5) * Double(size),
                              y: tip.y + Foundation.sin(back + 0.5) * Double(size)))
        p.addLine(to: CGPoint(x: tip.x + Foundation.cos(back - 0.5) * Double(size),
                              y: tip.y + Foundation.sin(back - 0.5) * Double(size)))
        p.closeSubpath()
        return p
    }
}
