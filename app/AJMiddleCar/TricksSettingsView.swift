import SwiftUI

/// Settings sub-screen: list of tricks, each with the duration it will play; tapping one opens
/// its editor.
struct TricksSettingsView: View {
    let palette: Palette
    @Environment(\.dismiss) private var dismiss
    // The same cache playback reads its speed and track from — a change there changes the numbers.
    @ObservedObject private var wheelStore = ConfigStore.shared.wheel
    @ObservedObject private var chassisStore = ConfigStore.shared.chassis
    /// Bumped on every appearance: settings live in UserDefaults, which SwiftUI does not observe,
    /// so coming back from the editor must recompute the list's durations.
    @State private var revision = 0
    private var p: Palette { palette }

    var body: some View {
        ZStack {
            p.bg.ignoresSafeArea()
            VStack(spacing: 0) {
                header
                List {
                    ForEach(Tricks.all) { trick in
                        NavigationLink {
                            TrickEditorView(trick: trick, palette: p)
                        } label: {
                            HStack(spacing: 11) {
                                Image(systemName: trick.icon).font(.system(size: 16, weight: .semibold))
                                    .foregroundStyle(p.accent).frame(width: 22)
                                Text(L.trickName(trick.nameKey)).font(.system(size: 14)).foregroundStyle(p.text)
                                Spacer()
                                Text(L.trickSec(totalSec(trick))).font(.system(size: 13))
                                    .foregroundStyle(p.muted).monospacedDigit()
                            }
                            .padding(.vertical, 4)
                        }
                        .listRowBackground(p.panel)
                        // Full-width separators: align the divider's leading edge to the row content
                        // (under the icon) so the accent icons don't visibly hang past it.
                        .alignmentGuide(.listRowSeparatorLeading) { _ in 0 }
                    }
                }
                .id(revision)
                .scrollContentBackground(.hidden)
                .tint(p.accent)
            }
        }
        .toolbar(.hidden, for: .navigationBar)
        .onAppear { revision &+= 1 }
    }

    /// What the trick will play: the same build, the same cached speed and track and the same
    /// nominal fallbacks as `ControlIntent.startTrick` — one number here, in the editor and on
    /// the drive screen's progress ring.
    private func totalSec(_ trick: Trick) -> Double {
        let built = ControlIntent.build(trick, vmaxMS: ControlIntent.vmax(wheelStore.value),
                                        trackM: ControlIntent.track(chassisStore.value))
        return Double(built.totalMs) / 1000
    }

    private var header: some View {
        HStack(spacing: 8) {
            Button { dismiss() } label: {
                Image(systemName: "chevron.left").font(.system(size: 17, weight: .semibold)).foregroundStyle(p.accent)
            }.buttonStyle(.plain)
            Text(L.tricksTitle).font(.system(size: 17, weight: .semibold)).foregroundStyle(p.text)
            Spacer()
        }
        .padding(.horizontal, 20).padding(.top, 12).padding(.bottom, 8)
    }
}
