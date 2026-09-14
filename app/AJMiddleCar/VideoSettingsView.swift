import SwiftUI

/// The FPV encoder's bitrate. One slider, applied at the next stream start: leave the drive
/// screen and come back, and the new figure is in effect.
struct VideoSettingsView: View {
    let palette: Palette
    @ObservedObject private var store = ConfigStore.shared.video
    @State private var kbps = Video.default.bitrate_kbps
    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p, title: L.videoTitle, onBack: { dismiss() }) {
            Image(systemName: "video")
                .font(.system(size: 64, weight: .light))
                .foregroundStyle(p.metal)
        } right: {
            rightPanel
        }
        .task { await store.loadIfNeeded(); adopt() }
    }

    private func adopt() {
        guard let v = store.value else { return }
        kbps = v.bitrate_kbps
    }

    private var rightPanel: some View {
        VStack(alignment: .leading, spacing: 9) {
            Text(L.videoHeadline).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
            Text(L.videoSub).font(.system(size: 13)).foregroundStyle(p.muted)
                .fixedSize(horizontal: false, vertical: true)
            if store.value != nil {
                Slider(value: Binding(
                    get: { Double(kbps) },
                    set: { kbps = Int($0 / 100) * 100 }
                ), in: Double(Video.bitrate_kbpsRange.lowerBound)...Double(Video.bitrate_kbpsRange.upperBound)) { editing in
                    if !editing { Task { await store.save(Video(bitrate_kbps: kbps)) } }
                }
                .tint(p.accent)
                .frame(width: 220)
                Text(L.videoValue(kbps)).font(.system(size: 14)).foregroundStyle(p.muted).monospacedDigit()
            } else {
                ConfigNotice(palette: p, error: store.error) {
                    Task { await store.reload(); adopt() }
                }
            }
        }
    }
}
