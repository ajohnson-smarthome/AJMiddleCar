import SwiftUI

/// Dedicated straight-line-trim screen: demo car with bending rails left, slider right.
struct TrimView: View {
    let palette: Palette
    @State private var trimPct = 0          // live slider value
    @State private var demoPct = 0          // applied on release (keeps the demo steady mid-drag)
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p, title: L.trimTitle, onBack: { dismiss() }) {
            TrimCarView(trimPct: demoPct, palette: p)
        } right: {
            rightPanel
        }
        .task { if let v = await TrimClient().get() { trimPct = v; demoPct = v } }
    }

    private var valueText: String {
        if trimPct == 0 { return L.trimCenter }
        return trimPct > 0 ? L.trimLeft(trimPct) : L.trimRight(-trimPct)
    }

    private var rightPanel: some View {
        VStack(alignment: .leading, spacing: 9) {
            Text(L.trimTitle).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
            Text(L.trimSub).font(.system(size: 13)).foregroundStyle(p.muted)
                .fixedSize(horizontal: false, vertical: true)
            Slider(value: Binding(
                get: { Double(trimPct) },
                set: { trimPct = Int($0.rounded()) }
            ), in: -30...30) { editing in
                if !editing {
                    demoPct = trimPct
                    Task { await TrimClient().set(trimPct) }
                }
            }
            .tint(p.accent)
            .frame(width: 220)
            Text(valueText)
                .font(.system(size: 14)).foregroundStyle(p.muted).monospacedDigit()
        }
    }
}

/// Car driving forward with rails that bend toward the correction (trim>0 → left); straight at 0.
struct TrimCarView: View {
    let trimPct: Int
    let palette: Palette

    private var metal: Color { palette.metal }
    private let carW: CGFloat = 34
    private let carLen: CGFloat = 72
    private let wheelW: CGFloat = 11
    private let wheelH: CGFloat = 15
    private let railGap: CGFloat = 12
    private let railLen: CGFloat = 52

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
        let center = CGPoint(x: size.width / 2, y: size.height / 2)
        drawRails(&ctx, center: center)
        drawCar(&ctx, center: center)
        let wx = carW / 2 + 1
        let wy = carLen / 2 - 16
        let phase = time * 70
        for dx in [-wx, wx] {
            for dy in [-wy, wy] {
                drawWheel(&ctx, cx: center.x + dx, cy: center.y + dy, phase: phase)
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

    private func drawWheel(_ ctx: inout GraphicsContext, cx: CGFloat, cy: CGFloat, phase: Double) {
        let rect = CGRect(x: cx - wheelW / 2, y: cy - wheelH / 2, width: wheelW, height: wheelH)
        let wp = Path(roundedRect: rect, cornerRadius: 3)
        ctx.fill(wp, with: .color(palette.accent))
        var c = ctx
        c.clip(to: wp)
        let spacing: CGFloat = 7
        let offset = CGFloat(phase).truncatingRemainder(dividingBy: spacing)
        let ch: CGFloat = 4
        var k = -2
        while CGFloat(k) * spacing < wheelH + spacing {
            let base = rect.maxY - CGFloat(k) * spacing + offset
            var p = Path()
            p.move(to: CGPoint(x: rect.minX + 1, y: base + ch))
            p.addLine(to: CGPoint(x: rect.midX, y: base - ch))
            p.addLine(to: CGPoint(x: rect.maxX - 1, y: base + ch))
            c.stroke(p, with: .color(palette.bg), style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
            k += 1
        }
    }

    private func drawRails(_ ctx: inout GraphicsContext, center: CGPoint) {
        // bend toward the correction: trim>0 (slow left) pulls the car left → rails curve left
        let bend = -CGFloat(trimPct) / 30 * 26
        let startY = center.y - carLen / 2 - railGap
        let halfW = carW / 2 + 2
        let grad = Gradient(colors: [palette.accent.opacity(0.95), palette.accent.opacity(0.04)])
        for side in [CGFloat(-1), CGFloat(1)] {
            let x0 = center.x + side * halfW
            var path = Path()
            path.move(to: CGPoint(x: x0, y: startY))
            path.addQuadCurve(to: CGPoint(x: x0 + bend, y: startY - railLen),
                              control: CGPoint(x: x0, y: startY - railLen * 0.55))
            ctx.stroke(path, with: .linearGradient(grad,
                startPoint: CGPoint(x: center.x, y: startY),
                endPoint: CGPoint(x: center.x, y: startY - railLen)),
                style: StrokeStyle(lineWidth: 6, lineCap: .round))
        }
    }
}
