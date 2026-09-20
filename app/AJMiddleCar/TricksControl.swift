import SwiftUI

/// The tricks control: the tricks segment of the drive screen's control bar (`ControlBar`),
/// which opens a C4 popover card of tricks below the bar. The control's own frame is one
/// segment (`ControlBarSegment.size`); the card is an overlay hanging down from it, tail up
/// toward the segment, centred under it — the bar sits in the top row, so down is the only
/// side with room.
/// Presentational — the parent owns playback state and passes `running` + `startedAt`.
/// Segment: idle ✦ (toggle popover) · open ✕ (close) · running ⏹ (stop, with a time-progress
/// ring around the glyph).
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

    private var showCard: Bool { (open || debugOpen) && !isRunning }

    /// The ring's diameter: around the glyph, inside the 32 pt segment.
    private static let ringDiameter: CGFloat = 26

    var body: some View {
        segment.overlay(alignment: .top) {
            if showCard {
                // 10 pt clear of the segment's bottom edge.
                card.padding(.top, ControlBarSegment.size.height + 10)
                    .transition(.opacity.combined(with: .scale(scale: 0.92, anchor: .top)))
            }
        }
        .animation(.easeOut(duration: 0.15), value: open)
        .onChange(of: running?.id) { _, _ in if running != nil { open = false } }
    }

    private var tint: Color { isRunning ? p.warn : p.accent }
    private var icon: String { isRunning ? "stop.fill" : ((open || debugOpen) ? "xmark" : "sparkles") }

    /// The whole slot is the button — a tap anywhere in the segment counts, not only on the
    /// glyph. The bar draws the body and the stroke; this draws nothing but the glyph.
    private var segment: some View {
        Button {
            if isRunning { onStop() } else { open.toggle() }
        } label: {
            Image(systemName: icon)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(tint)
                .frame(width: ControlBarSegment.size.width, height: ControlBarSegment.size.height)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .overlay { ringOverlay }   // ring on the button itself (not buried in the label)
    }

    // Trick-time progress ring, computed from elapsed time each frame (no withAnimation races).
    @ViewBuilder private var ringOverlay: some View {
        if isRunning {
            TimelineView(.animation) { tl in
                Circle().trim(from: 0, to: ringFill(at: tl.date))
                    .stroke(p.warn, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .frame(width: Self.ringDiameter, height: Self.ringDiameter)
            }
            .allowsHitTesting(false)
        }
    }

    private func ringFill(at date: Date) -> CGFloat {
        if let dbg = debugRingProgress { return dbg }
        guard let s = startedAt, let r = running, r.totalMs > 0 else { return 0 }
        return min(1, max(0, CGFloat(date.timeIntervalSince(s) / (Double(r.totalMs) / 1000))))
    }

    /// The card under its tail: a column, the tail on top pointing up at the segment.
    private var card: some View {
        VStack(spacing: 1) {
            tail
            cardBody
        }
    }

    private var tail: some View {
        Image(systemName: "triangle.fill")
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
