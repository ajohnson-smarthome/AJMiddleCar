import SwiftUI

/// Bottom-centre tricks control: a ✦ FAB that opens a C4 popover card of tricks.
/// Presentational — the parent owns playback state and passes `running` + `startedAt`.
/// FAB: idle ✦ (toggle popover) · open ✕ (close) · running ⏹ (stop, with a time-progress ring).
struct TricksControl: View {
    let palette: Palette
    let running: Trick?
    var startedAt: Date? = nil          // when the current trick began (parent-owned)
    let onSelect: (Trick) -> Void
    let onStop: () -> Void
    @State private var open = false
    private var p: Palette { palette }

    // Debug seeds for gallery screenshots.
    var debugOpen: Bool = false
    var debugRingProgress: CGFloat? = nil   // force a static progress-ring fill

    private var isRunning: Bool { running != nil || debugRingProgress != nil }

    var body: some View {
        ZStack(alignment: .bottom) {
            if (open || debugOpen) && !isRunning {
                card.padding(.bottom, 56)
                    .transition(.opacity.combined(with: .scale(scale: 0.92, anchor: .bottom)))
            }
            fab
        }
        .animation(.easeOut(duration: 0.15), value: open)
        .onChange(of: running?.id) { _ in if running != nil { open = false } }
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

    private var card: some View {
        VStack(spacing: 1) {
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
            // little downward tail toward the FAB
            Image(systemName: "triangle.fill").rotationEffect(.degrees(180))
                .font(.system(size: 9)).foregroundStyle(p.panel)
        }
    }
}
