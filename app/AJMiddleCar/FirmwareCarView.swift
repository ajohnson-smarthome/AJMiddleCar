import SwiftUI

/// Phases of the firmware-update screen, shared by FirmwareView and its car image.
enum FwPhase { case checking, upToDate, available, downloading, downloaded, uploading, rebooting, done, failed }

/// Top-down car + center chip + OTA rings, drawn 1:1 from the reference mockup.
/// Rings sit behind the opaque car (so they ring around/under it). They pulse while waiting,
/// ping while rebooting, sit static as decoration when idle, or vanish on terminal screens.
/// Animations run via TimelineView so they keep going across phase transitions.
struct FirmwareCarView: View {
    let phase: FwPhase
    let palette: Palette

    private var metal: Color { palette.metal }

    private enum WaveMode { case none, deco, wait, active, ping }
    private var mode: WaveMode {
        switch phase {
        case .checking, .downloading, .downloaded: return .wait
        case .uploading: return .active
        case .rebooting: return .ping
        case .upToDate: return .deco
        case .available, .done, .failed: return .none
        }
    }
    private var chipIcon: String {
        switch phase {
        case .upToDate, .done: return "checkmark"
        case .failed: return "exclamationmark"
        default: return "cpu"
        }
    }
    private var chipColor: Color { phase == .failed ? palette.warn : palette.accent }

    private let ringD: [CGFloat] = [56, 80, 104]   // reference .w1/.w2/.w3

    var body: some View {
        ZStack {
            waves       // behind
            car         // opaque, on top
        }
        .scaleEffect(1.6)
        .frame(width: 200, height: 240)
        .opacity(phase == .rebooting ? 0.85 : 1)
    }

    // 1:1 reference car: body 34×72, 4 dark wheels at corners, windshield near top, chip center.
    private var car: some View {
        ZStack {
            // body (opaque: bg base under panel so rings under it are covered)
            RoundedRectangle(cornerRadius: 10).fill(palette.bg)
                .overlay(RoundedRectangle(cornerRadius: 10).fill(palette.panel))
                .overlay(RoundedRectangle(cornerRadius: 10).stroke(metal, lineWidth: 1))
                .frame(width: 34, height: 72)
            // windshield
            RoundedRectangle(cornerRadius: 3).fill(palette.bg)
                .frame(width: 20, height: 8).offset(y: -25)
            // wheels (dark, on top of the body — as in the reference)
            ForEach(0..<4, id: \.self) { i in
                RoundedRectangle(cornerRadius: 3).fill(metal)
                    .frame(width: 11, height: 15)
                    .offset(x: i % 2 == 0 ? -18.5 : 18.5, y: i < 2 ? -20.5 : 20.5)
            }
            // chip — dark-green fill, accent border + icon, glow
            ZStack {
                RoundedRectangle(cornerRadius: 5).fill(palette.bg)
                RoundedRectangle(cornerRadius: 5).fill(chipColor.opacity(0.18))
                RoundedRectangle(cornerRadius: 5).stroke(chipColor, lineWidth: 1)
                Image(systemName: chipIcon).font(.system(size: 11, weight: .bold)).foregroundStyle(chipColor)
            }
            .frame(width: 20, height: 20)
            .shadow(color: chipColor.opacity(0.55), radius: phase == .done ? 8 : 5)
        }
    }

    @ViewBuilder private var waves: some View {
        switch mode {
        case .none:
            EmptyView()
        case .deco:
            rings(scale: 1.0, opacity: [0.20, 0.11, 0.045])
        case .wait, .active:
            let op: [Double] = mode == .active ? [0.62, 0.38, 0.20] : [0.42, 0.24, 0.11]
            let period = mode == .active ? 1.05 : 1.4
            TimelineView(.animation) { ctx in
                let t = ctx.date.timeIntervalSinceReferenceDate
                let s = 1.0 + 0.08 * (0.5 + 0.5 * sin(t * 2 * .pi / period))
                rings(scale: s, opacity: op)
            }
        case .ping:
            TimelineView(.animation) { ctx in
                let t = ctx.date.timeIntervalSinceReferenceDate
                ZStack {
                    ForEach(0..<2, id: \.self) { i in
                        let ph = ((t + Double(i) * 0.9) / 1.8).truncatingRemainder(dividingBy: 1)
                        Circle().stroke(palette.accent, lineWidth: 2)
                            .frame(width: 60, height: 60)
                            .scaleEffect(0.9 + 1.0 * ph)
                            .opacity(0.6 * (1 - ph))
                    }
                }
            }
        }
    }

    private func rings(scale: Double, opacity: [Double]) -> some View {
        ZStack {
            ForEach(0..<3, id: \.self) { i in
                Circle().stroke(palette.accent, lineWidth: 1.5)
                    .frame(width: ringD[i], height: ringD[i])
                    .opacity(opacity[i])
                    .scaleEffect(scale)
            }
        }
    }
}
