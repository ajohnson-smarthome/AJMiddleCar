import SwiftUI

/// Dedicated ramp screen: demo car on the left, slider on the right (calib/firmware layout).
struct RampView: View {
    let palette: Palette
    @State private var rampMs = 300        // live slider value (label)
    @State private var demoMs = 300        // applied on release — keeps the demo from jumping mid-drag
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p, title: L.rampTitle, onBack: { dismiss() }) {
            RampCarView(rampMs: demoMs, palette: p)
        } right: {
            rightPanel
        }
        .task { if let v = await RampClient().get() { rampMs = v; demoMs = v } }
    }

    private var rightPanel: some View {
        VStack(alignment: .leading, spacing: 9) {
            Text(L.rampHeadline).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
            Text(L.rampSub).font(.system(size: 13)).foregroundStyle(p.muted)
                .fixedSize(horizontal: false, vertical: true)
            Slider(value: Binding(
                get: { Double(rampMs) },
                set: { rampMs = Int($0 / 50) * 50 }
            ), in: 0...1000) { editing in
                if !editing {
                    demoMs = rampMs
                    Task { await RampClient().set(rampMs) }
                }
            }
            .tint(p.accent)
            .frame(width: 220)
            Text(rampMs > 0 ? L.rampValue(rampMs) : L.rampValueOff)
                .font(.system(size: 14)).foregroundStyle(p.muted).monospacedDigit()
        }
    }
}

/// Looping acceleration demo: pause → ramp up over rampMs (wheels colour up, chevrons speed up,
/// rails grow) → full speed → reset. Same car geometry as DriveDiagram.
struct RampCarView: View {
    let rampMs: Int
    let palette: Palette

    private var metal: Color { palette.metal }
    private let carW: CGFloat = 34
    private let carLen: CGFloat = 72
    private let wheelW: CGFloat = 11
    private let wheelH: CGFloat = 15
    private let railGap: CGFloat = 12
    private let railMax: CGFloat = 52

    var body: some View {
        TimelineView(.animation) { tl in
            Canvas { ctx, size in
                render(&ctx, size, time: tl.date.timeIntervalSinceReferenceDate)
            }
        }
        .frame(width: 120, height: 210)
        .scaleEffect(1.6)
    }

    private func render(_ ctx: inout GraphicsContext, _ size: CGSize, time: Double) {
        // Demo cycle: pause 0.4s → accel rampMs (min 0.15s; instant look for 0) → hold 0.8s.
        let pause = 0.4
        let accelT = max(Double(rampMs) / 1000.0, 0.15)
        let hold = 0.8
        let total = pause + accelT + hold
        let t = time.truncatingRemainder(dividingBy: total)
        let progress: Double
        if t < pause { progress = 0 }
        else if t < pause + accelT { progress = rampMs == 0 ? 1 : (t - pause) / accelT }
        else { progress = 1 }
        // Chevron phase = ∫speed dt (smooth speed-up, no jumps within the cycle).
        let tempo = 70.0
        let phase: Double
        if t < pause { phase = 0 }
        else if t < pause + accelT { let u = t - pause; phase = tempo * u * u / (2 * accelT) }
        else { phase = tempo * (accelT / 2 + (t - pause - accelT)) }

        // Car centred in the half; rails grow upward from the roof.
        let center = CGPoint(x: size.width / 2, y: size.height / 2)

        if progress > 0.01 { drawRails(&ctx, center: center, progress: progress) }
        drawCar(&ctx, center: center)
        let wx = carW / 2 + 1
        let wy = carLen / 2 - 16
        for dx in [-wx, wx] {
            for dy in [-wy, wy] {
                drawWheel(&ctx, cx: center.x + dx, cy: center.y + dy, progress: progress, phase: phase)
            }
        }
    }

    private func drawCar(_ ctx: inout GraphicsContext, center: CGPoint) {
        let body = CGRect(x: center.x - carW / 2, y: center.y - carLen / 2, width: carW, height: carLen)
        let bp = Path(roundedRect: body, cornerRadius: 11)
        ctx.fill(bp, with: .color(palette.bg))
        ctx.fill(bp, with: .color(palette.panel))
        ctx.stroke(bp, with: .color(metal), lineWidth: 1)
        let wind = CGRect(x: center.x - 11, y: body.minY + 7, width: 22, height: 9)
        ctx.fill(Path(roundedRect: wind, cornerRadius: 3), with: .color(palette.bg.opacity(0.85)))
    }

    private func drawWheel(_ ctx: inout GraphicsContext, cx: CGFloat, cy: CGFloat, progress: Double, phase: Double) {
        let rect = CGRect(x: cx - wheelW / 2, y: cy - wheelH / 2, width: wheelW, height: wheelH)
        let wp = Path(roundedRect: rect, cornerRadius: 3)
        ctx.fill(wp, with: .color(metal))
        ctx.fill(wp, with: .color(palette.accent.opacity(progress)))   // dark → green as it spools up
        guard progress > 0.03 else { return }

        var c = ctx
        c.clip(to: wp)
        let spacing: CGFloat = 13 - 6 * CGFloat(progress)
        let offset = CGFloat(phase).truncatingRemainder(dividingBy: spacing)
        let ch: CGFloat = 4
        var k = -2
        while CGFloat(k) * spacing < wheelH + spacing {
            let base = rect.maxY - CGFloat(k) * spacing + offset   // forward: chevrons run up
            var p = Path()
            p.move(to: CGPoint(x: rect.minX + 1, y: base + ch))
            p.addLine(to: CGPoint(x: rect.midX, y: base - ch))
            p.addLine(to: CGPoint(x: rect.maxX - 1, y: base + ch))
            c.stroke(p, with: .color(palette.bg), style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
            k += 1
        }
    }

    private func drawRails(_ ctx: inout GraphicsContext, center: CGPoint, progress: Double) {
        let len = railMax * CGFloat(progress)
        let startY = center.y - carLen / 2 - railGap
        let halfW = carW / 2 + 2
        let grad = Gradient(colors: [palette.accent.opacity(0.95 * progress), palette.accent.opacity(0.04)])
        for side in [CGFloat(-1), CGFloat(1)] {
            var path = Path()
            path.move(to: CGPoint(x: center.x + side * halfW, y: startY))
            path.addLine(to: CGPoint(x: center.x + side * halfW, y: startY - len))
            ctx.stroke(path, with: .linearGradient(grad,
                startPoint: CGPoint(x: center.x, y: startY),
                endPoint: CGPoint(x: center.x, y: startY - railMax)),
                style: StrokeStyle(lineWidth: 6, lineCap: .round))
        }
    }
}
