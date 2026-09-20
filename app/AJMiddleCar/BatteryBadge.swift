import SwiftUI

/// The pack in the top row, next to the link: a 22 × 11 battery icon filled to `soc_pct` and
/// the caption «P % · V В · W Вт» in the same 10-pt as the picture's numbers. What it shows is
/// `BatteryGauge`'s verdict, not its own: `low` turns the whole badge `warn`, no monitor is an
/// empty icon in `muted` with «—», a start not yet determined is an empty icon over live volts
/// and watts, and current into the pack wears the bolt. Part of the row, nothing of the layout
/// — `DriveLayout` does not know it exists.
struct BatteryBadge: View {
    let battery: BatteryInfo?
    let palette: Palette

    private static let box = CGSize(width: 20, height: 11)
    private static let nose = CGSize(width: 2, height: 5)
    private static let inset: CGFloat = 2

    var body: some View {
        let g = BatteryGauge.make(battery)
        let tint = colour(g.tone)
        HStack(spacing: 5) {
            icon(g, tint)
            Text(caption(g))
                .font(.system(size: 10))
                .foregroundStyle(tint)
        }
        .accessibilityElement(children: .combine)
    }

    private func colour(_ tone: BatteryGauge.Tone) -> Color {
        switch tone {
        case .text: return palette.text
        case .warn: return palette.warn
        case .muted: return palette.muted
        }
    }

    private func caption(_ g: BatteryGauge) -> String {
        guard let st = g.stats else { return L.batteryAbsent }
        return L.batteryStats(pct: st.pct, v: st.volts, w: st.watts)
    }

    /// The outline, the fill from the left, the nose on the right, and the bolt over it all
    /// when charging — the bolt sits on a knock-out of the background so it reads over the fill
    /// as well as over the empty body.
    private func icon(_ g: BatteryGauge, _ tint: Color) -> some View {
        let b = Self.box, inset = Self.inset
        return HStack(spacing: 0) {
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2.5).stroke(tint, lineWidth: 1)
                if let f = g.fill, f > 0 {
                    RoundedRectangle(cornerRadius: 1)
                        .fill(tint)
                        .frame(width: (b.width - 2 * inset) * f, height: b.height - 2 * inset)
                        .padding(.leading, inset)
                }
                if g.charging {
                    ZStack {
                        Image(systemName: "bolt.fill").font(.system(size: 9, weight: .black))
                            .foregroundStyle(palette.bg)
                        Image(systemName: "bolt.fill").font(.system(size: 7, weight: .bold))
                            .foregroundStyle(tint)
                    }
                    .frame(width: b.width, height: b.height)
                }
            }
            .frame(width: b.width, height: b.height)
            RoundedRectangle(cornerRadius: 0.5)
                .fill(tint)
                .frame(width: Self.nose.width, height: Self.nose.height)
        }
    }
}
