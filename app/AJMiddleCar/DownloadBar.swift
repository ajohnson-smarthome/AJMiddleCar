import SwiftUI

/// Progress bar for firmware download that always visibly moves: a synthetic ramp fills
/// 0→100% over `UpdateClient.downloadMinDisplay`, and the shown value is max(real, synthetic),
/// so an instant download or a missing Content-Length still animates. The caption is computed
/// from the shown fraction so the percentage matches the bar.
struct DownloadBar: View {
    let progress: Double
    let caption: (Double) -> String
    let palette: Palette
    @State private var start = Date()

    var body: some View {
        TimelineView(.animation) { ctx in
            let synthetic = min(1.0, ctx.date.timeIntervalSince(start) / UpdateClient.downloadMinDisplay)
            let shown = max(progress, synthetic)
            VStack(alignment: .leading, spacing: 9) {
                Text(caption(shown)).font(.system(size: 14)).foregroundStyle(palette.muted)
                ProgressView(value: shown).tint(palette.accent).frame(width: 160)
            }
        }
    }
}
