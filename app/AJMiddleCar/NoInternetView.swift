import SwiftUI

/// Startup gate screen: GitHub unreachable. Car with an amber Wi-Fi-warning chip + retry.
///
/// The art used to be a private copy of `FirmwareCarView` — the same car, the same rings at
/// 56/80/104, the same opacities and the same 1.4 s period, re-typed in `warn` because there was
/// no way to ask the shared one for a colour. Fifty-odd lines that had to be kept in step by
/// hand, and were not: when the rings moved, they moved in one file.
struct NoInternetView: View {
    let palette: Palette
    let onRetry: () -> Void
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p) {
            DeviceScene(palette: p, rings: .wait(), ringTint: p.warn,
                        chip: (glyph: "wifi.exclamationmark", tint: p.warn)) {
                CarBody(palette: p)
            }
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
