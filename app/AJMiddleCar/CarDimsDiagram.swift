import SwiftUI

/// Top-down car sized from track (lateral, mm) + wheelbase (longitudinal, mm), with dimension
/// lines + labels between the wheel centres. Same silhouette/style as DriveDiagram (rounded body,
/// dark corner wheels with a chevron tread, windshield strip at the front). Wheels + body animate
/// when a value changes; the default 130/210 renders the canonical 36×74 silhouette.
struct CarDimsDiagram: View {
    let trackMm: Int
    let wheelbaseMm: Int
    let palette: Palette
    private var p: Palette { palette }

    // DriveDiagram reference proportions, scaled by K. The default 130/210 maps onto wheel
    // track 38 / wheelbase 42 and body 36×74, so it matches the on-screen car.
    private let K = 1.5
    private var trackPx: Double { Double(trackMm) * (38.0 / 130.0) * K }
    private var basePx: Double  { Double(wheelbaseMm) * (42.0 / 210.0) * K }
    private var bodyW: Double   { trackPx * (36.0 / 38.0) }
    private var bodyL: Double   { basePx * (74.0 / 42.0) }
    private var wheelW: Double  { 12.0 * K }
    private var wheelH: Double  { 20.0 * K }

    var body: some View {
        GeometryReader { geo in
            let cx = geo.size.width / 2, cy = geo.size.height / 2
            let tHalf = trackPx / 2, bHalf = basePx / 2
            ZStack {
                // wheels (under the body) at the 4 wheel-centres
                ForEach(0..<4, id: \.self) { i in
                    wheel
                        .position(x: cx + (i % 2 == 0 ? -tHalf : tHalf),
                                  y: cy + (i < 2 ? -bHalf : bHalf))
                }
                // body + windshield
                RoundedRectangle(cornerRadius: bodyW * 11 / 36)
                    .fill(p.panel)
                    .overlay(RoundedRectangle(cornerRadius: bodyW * 11 / 36).stroke(p.metal, lineWidth: 1.5))
                    .frame(width: bodyW, height: bodyL)
                    .position(x: cx, y: cy)
                RoundedRectangle(cornerRadius: 3)
                    .fill(p.bg.opacity(0.85))
                    .frame(width: bodyW * 22 / 36, height: bodyL * 9 / 74)
                    .position(x: cx, y: cy - bodyL / 2 + bodyL * 11 / 74)
                // wheel-centre dots
                ForEach(0..<4, id: \.self) { i in
                    Circle().fill(p.accent).frame(width: 5, height: 5)
                        .position(x: cx + (i % 2 == 0 ? -tHalf : tHalf),
                                  y: cy + (i < 2 ? -bHalf : bHalf))
                }
                // TRACK dimension (above the car)
                dimLine(horizontal: true, length: trackPx, color: p.accent)
                    .position(x: cx, y: cy - bHalf - wheelH / 2 - 16)
                label("\(L.dimsTrack) \(trackMm) \(L.mmUnit)", color: p.accent)
                    .position(x: cx, y: cy - bHalf - wheelH / 2 - 28)
                // BASE dimension (right of the car)
                dimLine(horizontal: false, length: basePx, color: p.metal.opacity(0.9))
                    .position(x: cx + bodyW / 2 + 22, y: cy)
                label("\(L.dimsBase) \(wheelbaseMm) \(L.mmUnit)", color: p.metal.opacity(0.9))
                    .rotationEffect(.degrees(90))
                    .position(x: cx + bodyW / 2 + 38, y: cy)
            }
            .animation(.easeInOut(duration: 0.28), value: trackMm)
            .animation(.easeInOut(duration: 0.28), value: wheelbaseMm)
        }
        .frame(height: 220)
    }

    private var wheel: some View {
        Canvas { ctx, size in
            let wp = Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 3)
            ctx.fill(wp, with: .color(.black.opacity(0.85)))
            ctx.stroke(wp, with: .color(p.metal), lineWidth: 1)
            for oy in [size.height * 0.32, size.height * 0.62] {
                var c = Path()
                c.move(to: CGPoint(x: size.width * 0.2, y: oy + 3))
                c.addLine(to: CGPoint(x: size.width * 0.5, y: oy - 2))
                c.addLine(to: CGPoint(x: size.width * 0.8, y: oy + 3))
                ctx.stroke(c, with: .color(p.bg), style: StrokeStyle(lineWidth: 1.8, lineCap: .round, lineJoin: .round))
            }
        }
        .frame(width: wheelW, height: wheelH)
    }

    /// A dimension bar (capped line). `horizontal` → width=length, else height=length.
    private func dimLine(horizontal: Bool, length: Double, color: Color) -> some View {
        ZStack {
            Rectangle().fill(color)
                .frame(width: horizontal ? length : 1.5, height: horizontal ? 1.5 : length)
            Rectangle().fill(color).frame(width: horizontal ? 1.5 : 8, height: horizontal ? 8 : 1.5)
                .offset(x: horizontal ? -length / 2 : 0, y: horizontal ? 0 : -length / 2)
            Rectangle().fill(color).frame(width: horizontal ? 1.5 : 8, height: horizontal ? 8 : 1.5)
                .offset(x: horizontal ? length / 2 : 0, y: horizontal ? 0 : length / 2)
        }
    }

    private func label(_ text: String, color: Color) -> some View {
        Text(text).font(.system(size: 13, weight: .bold)).foregroundStyle(color).monospacedDigit()
    }
}
