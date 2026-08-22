import SwiftUI

/// Link-loss auto-return: toggle + history-window slider. Split layout like RampView. The
/// controls appear only once the car's own configuration has been read.
struct RecoverView: View {
    let palette: Palette
    @ObservedObject private var store = ConfigStore.shared.recover
    @State private var enabled = Recover.default.enabled
    @State private var windowSec = Recover.default.window_ms / 1000
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    private static let secRange = Recover.window_msRange.lowerBound / 1000 ... Recover.window_msRange.upperBound / 1000

    var body: some View {
        SplitScreen(palette: p, title: L.recoverTitle, onBack: { dismiss() }) {
            RecoverCarView(active: store.value != nil && enabled, palette: p)
        } right: {
            rightPanel
        }
        .task { await store.loadIfNeeded(); adopt() }
    }

    private func adopt() {
        guard let v = store.value else { return }
        enabled = v.enabled
        windowSec = max(Self.secRange.lowerBound, min(Self.secRange.upperBound, v.window_ms / 1000))
    }

    private func save() {
        Task { await store.save(Recover(enabled: enabled, window_ms: windowSec * 1000)) }
    }

    private var rightPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(L.recoverHeadline).font(.system(size: 20, weight: .semibold)).foregroundStyle(p.text)
            if store.value != nil {
                Toggle(L.recoverEnable, isOn: $enabled)
                    .tint(p.accent)
                    .frame(width: 230)
                    .onChange(of: enabled) { _, _ in save() }
                VStack(alignment: .leading, spacing: 5) {
                    HStack {
                        Text(L.recoverWindow).font(.system(size: 12)).foregroundStyle(p.muted)
                        Spacer()
                        Text(L.recoverWindowValue(windowSec)).font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(enabled ? p.accent : p.muted).monospacedDigit()
                    }
                    Slider(value: Binding(
                        get: { Double(windowSec) },
                        set: { windowSec = Int($0.rounded()) }
                    ), in: Double(Self.secRange.lowerBound)...Double(Self.secRange.upperBound), step: 1) { editing in
                        if !editing { save() }
                    }
                    .tint(p.accent)
                    .disabled(!enabled)
                }
                .frame(width: 230)
                .opacity(enabled ? 1 : 0.4)
                Text(enabled ? L.recoverSubOn : L.recoverSubOff)
                    .font(.system(size: 12)).foregroundStyle(p.muted)
                    .fixedSize(horizontal: false, vertical: true).frame(maxWidth: 250, alignment: .leading)
            } else {
                ConfigNotice(palette: p, error: store.error) {
                    Task { await store.reload(); adopt() }
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
    }
}

/// Centred reference car (same geometry as RampCarView/TrimCarView) with a dashed
/// "retrace" trail behind it. The car is NOT moved — only the trail signals the feature.
struct RecoverCarView: View {
    let active: Bool
    let palette: Palette

    private var metal: Color { palette.metal }
    private let carW: CGFloat = 34
    private let carLen: CGFloat = 72
    private let wheelW: CGFloat = 11
    private let wheelH: CGFloat = 15

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
        drawTrail(&ctx, center: center, time: time)
        drawCar(&ctx, center: center)
        let wx = carW / 2 + 1
        let wy = carLen / 2 - 16
        let phase = active ? time * 60 : 0
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

    // Chevron-tread wheel; treads scroll in REVERSE (backward) when active, static when off.
    private func drawWheel(_ ctx: inout GraphicsContext, cx: CGFloat, cy: CGFloat, phase: Double) {
        let rect = CGRect(x: cx - wheelW / 2, y: cy - wheelH / 2, width: wheelW, height: wheelH)
        let wp = Path(roundedRect: rect, cornerRadius: 3)
        ctx.fill(wp, with: .color(metal))
        guard active else { return }                       // off → plain dark wheel, no motion
        var c = ctx
        c.clip(to: wp)
        let spacing: CGFloat = 7
        let offset = CGFloat(-phase).truncatingRemainder(dividingBy: spacing)  // negative → reverse
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

    // Dashed trail behind the car; dashes march toward the car (retrace) when active.
    private func drawTrail(_ ctx: inout GraphicsContext, center: CGPoint, time: Double) {
        let startY = center.y + carLen / 2 + 6
        let endY = startY + 58
        var path = Path()
        path.move(to: CGPoint(x: center.x, y: startY))
        path.addLine(to: CGPoint(x: center.x, y: endY))
        // dashPhase increasing along a startY→endY path moves dashes away from the car;
        // negate so they march toward the car (the path being retraced).
        let dashPhase = active ? CGFloat((-time * 24).truncatingRemainder(dividingBy: 13)) : 0
        ctx.stroke(path, with: .color(palette.accent.opacity(active ? 0.55 : 0.12)),
                   style: StrokeStyle(lineWidth: 2, lineCap: .round, dash: [6, 7], dashPhase: dashPhase))
        var chev = Path()
        chev.move(to: CGPoint(x: center.x - 5, y: endY - 5))
        chev.addLine(to: CGPoint(x: center.x, y: endY))
        chev.addLine(to: CGPoint(x: center.x + 5, y: endY - 5))
        ctx.stroke(chev, with: .color(palette.accent.opacity(active ? 0.7 : 0.12)),
                   style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
    }
}
