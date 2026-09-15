import SwiftUI

/// The tricks control: a ✦ FAB that opens a C4 popover card of tricks on the side `cardEdge`
/// names — leftward on the HUD, where the FAB lives on the band right of the picture and the
/// card has nowhere else to go; upward on the classic layout, where the FAB is bottom-centre
/// and a card centred on it would run off the screen. The card is an overlay: the control's
/// own frame is the FAB's 46 pt, wherever the parent positions it.
/// Presentational — the parent owns playback state and passes `running` + `startedAt`.
/// FAB: idle ✦ (toggle popover) · open ✕ (close) · running ⏹ (stop, with a time-progress ring).
struct TricksControl: View {
    /// Where the card goes, relative to the FAB: its tail always points back at the button.
    enum CardEdge { case leading, top }

    let palette: Palette
    let running: Trick?
    var startedAt: Date? = nil          // when the current trick began (parent-owned)
    let onSelect: (Trick) -> Void
    let onStop: () -> Void
    var cardEdge: CardEdge = .leading
    @State private var open = false
    private var p: Palette { palette }

    // Debug seeds for gallery screenshots.
    var debugOpen: Bool = false
    var debugRingProgress: CGFloat? = nil   // force a static progress-ring fill

    private var isRunning: Bool { running != nil || debugRingProgress != nil }

    private var showCard: Bool { (open || debugOpen) && !isRunning }

    var body: some View {
        Group {
            switch cardEdge {
            case .leading:
                fab.overlay(alignment: .trailing) {
                    if showCard {
                        card.padding(.trailing, 56)   // 10 pt clear of the FAB's leading edge
                            .transition(.opacity.combined(with: .scale(scale: 0.92, anchor: .trailing)))
                    }
                }
            case .top:
                fab.overlay(alignment: .bottom) {
                    if showCard {
                        card.padding(.bottom, 56)     // 10 pt clear of the FAB's top edge
                            .transition(.opacity.combined(with: .scale(scale: 0.92, anchor: .bottom)))
                    }
                }
            }
        }
        .animation(.easeOut(duration: 0.15), value: open)
        .onChange(of: running?.id) { _, _ in if running != nil { open = false } }
    }

    private var fabTint: Color { isRunning ? p.warn : p.accent }
    private var fabIcon: String { isRunning ? "stop.fill" : ((open || debugOpen) ? "xmark" : "sparkles") }

    private var fab: some View {
        Button {
            if isRunning { onStop() } else { open.toggle() }
        } label: {
            Image(systemName: fabIcon)
                .font(.system(size: 18, weight: .semibold))
                .foregroundStyle(fabTint)
                .frame(width: 46, height: 46)
                .background(Circle().fill(fabTint.opacity(0.16)))
                .overlay(Circle().stroke(fabTint.opacity(0.6), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .overlay { ringOverlay }   // ring on the button itself (not buried in the label)
    }

    // Trick-time progress ring, computed from elapsed time each frame (no withAnimation races).
    @ViewBuilder private var ringOverlay: some View {
        if isRunning {
            TimelineView(.animation) { tl in
                Circle().trim(from: 0, to: ringFill(at: tl.date))
                    .stroke(p.warn, style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .frame(width: 46, height: 46)
            }
            .allowsHitTesting(false)
        }
    }

    private func ringFill(at date: Date) -> CGFloat {
        if let dbg = debugRingProgress { return dbg }
        guard let s = startedAt, let r = running, r.totalMs > 0 else { return 0 }
        return min(1, max(0, CGFloat(date.timeIntervalSince(s) / (Double(r.totalMs) / 1000))))
    }

    /// The card and its tail, laid along the axis the edge implies: a row with the tail
    /// pointing right for `.leading`, a column with it pointing down for `.top`.
    @ViewBuilder private var card: some View {
        switch cardEdge {
        case .leading:
            HStack(spacing: 1) {
                cardBody
                tail(pointing: 90)     // right, toward the FAB
            }
        case .top:
            VStack(spacing: 1) {
                cardBody
                tail(pointing: 180)    // down, toward the FAB
            }
        }
    }

    private func tail(pointing degrees: Double) -> some View {
        Image(systemName: "triangle.fill").rotationEffect(.degrees(degrees))
            .font(.system(size: 9)).foregroundStyle(p.panel)
    }

    private var cardBody: some View {
        VStack(spacing: 0) {
            ForEach(Tricks.all) { trick in
                Button {
                    onSelect(trick); open = false
                } label: {
                    HStack(spacing: 10) {
                        Image(systemName: trick.icon).font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(p.accent).frame(width: 22)
                        Text(L.trickName(trick.nameKey)).font(.system(size: 13)).foregroundStyle(p.text)
                        Spacer()
                    }
                    .padding(.horizontal, 10).padding(.vertical, 8)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
        .frame(width: 156)
        .background(RoundedRectangle(cornerRadius: 12).fill(p.panel))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(p.line))
    }
}
