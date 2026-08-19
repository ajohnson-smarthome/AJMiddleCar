import SwiftUI

/// Startup gate screen: GitHub unreachable. Car with an amber Wi-Fi-warning chip + retry.
struct NoInternetView: View {
    let palette: Palette
    let onRetry: () -> Void
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p) {
            NoInternetCarView(palette: p)
        } right: {
            VStack(alignment: .leading, spacing: 9) {
                Text(L.gateNoInternetTitle).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
                Text(L.gateNoInternetSub).font(.system(size: 13)).foregroundStyle(p.muted)
                    .fixedSize(horizontal: false, vertical: true).frame(maxWidth: 260, alignment: .leading)
                Button(action: onRetry) {
                    Text(L.fwRetry).font(.system(size: 14, weight: .semibold)).foregroundStyle(p.warn)
                        .padding(.horizontal, 16).padding(.vertical, 8)
                        .background(RoundedRectangle(cornerRadius: 10).fill(p.warn.opacity(0.15)))
                        .overlay(RoundedRectangle(cornerRadius: 10).stroke(p.warn.opacity(0.55), lineWidth: 1))
                }.buttonStyle(.plain).padding(.top, 3)
            }
        }
    }
}

/// Reference car bearing an amber Wi-Fi-warning chip, ringed by pulsing amber waves.
/// Mirrors FirmwareCarView geometry but in `palette.warn`. Rings pulse inside a single
/// Canvas (one GPU layer, no per-frame layout → no jitter), drawn behind the opaque car.
private struct NoInternetCarView: View {
    let palette: Palette
    private var metal: Color { palette.metal }
    private var warn: Color { palette.warn }
    private let ringD: [CGFloat] = [56, 80, 104]

    var body: some View {
        ZStack {
            waves     // Canvas, animates internally — behind
            car       // static, opaque — on top
        }
        .scaleEffect(1.6)
        .frame(width: 200, height: 240)
    }

    private var waves: some View {
        let op: [Double] = [0.42, 0.24, 0.11]
        return TimelineView(.animation) { ctx in
            let t = ctx.date.timeIntervalSinceReferenceDate
            let s = 1.0 + 0.08 * (0.5 + 0.5 * sin(t * 2 * .pi / 1.4))
            Canvas { gc, size in
                let c = CGPoint(x: size.width / 2, y: size.height / 2)
                for i in 0..<3 {
                    let r = ringD[i] / 2 * CGFloat(s)
                    let rect = CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r)
                    gc.stroke(Path(ellipseIn: rect), with: .color(warn.opacity(op[i])), lineWidth: 1.5)
                }
            }
        }
        .frame(width: 200, height: 240)
    }

    private var car: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10).fill(palette.bg)
                .overlay(RoundedRectangle(cornerRadius: 10).fill(palette.panel))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(metal, lineWidth: 1))
                .frame(width: 34, height: 72)
            RoundedRectangle(cornerRadius: 3).fill(palette.bg)
                .frame(width: 20, height: 8).offset(y: -25)
            ForEach(0..<4, id: \.self) { i in
                RoundedRectangle(cornerRadius: 3).fill(metal)
                    .frame(width: 11, height: 15)
                    .offset(x: i % 2 == 0 ? -18.5 : 18.5, y: i < 2 ? -20.5 : 20.5)
            }
            ZStack {
                RoundedRectangle(cornerRadius: 5).fill(palette.bg)
                RoundedRectangle(cornerRadius: 5).fill(warn.opacity(0.18))
                RoundedRectangle(cornerRadius: 5).stroke(warn, lineWidth: 1)
                Image(systemName: "wifi.exclamationmark")
                    .font(.system(size: 11, weight: .bold)).foregroundStyle(warn)
            }
            .frame(width: 20, height: 20)
            .shadow(color: warn.opacity(0.55), radius: 5)
        }
    }
}
