import SwiftUI

/// Settings sub-screen: list of tricks; tapping one opens its per-action duration editor.
struct TricksSettingsView: View {
    let palette: Palette
    @Environment(\.dismiss) private var dismiss
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
                .scrollContentBackground(.hidden)
                .tint(p.accent)
            }
        }
        .toolbar(.hidden, for: .navigationBar)
    }

    private func totalSec(_ trick: Trick) -> Double {
        Double(Tricks.withDurations(trick, TrickSettings.durations(for: trick)).totalMs) / 1000
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
