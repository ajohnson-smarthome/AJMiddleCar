import SwiftUI

/// Shown when a car answered our hello with a different device identifier.
///
/// This screen exists because both cars are a softAP serving the same API at the same address:
/// joining the wrong network is easy, and without an explicit stop the app would drive the other
/// car with this one's calibration. A wrong car must not look like an offline car — the user has
/// to switch networks, not wait — so it gets its own screen rather than a silent retry loop.
/// Identity now arrives in the hello reply, on the first exchange, so this can no longer be
/// missed by a probe that cancelled itself.
struct WrongCarView: View {
    let palette: Palette
    let found: String
    let onRetry: () -> Void
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p) {
            FirmwareCarView(phase: .failed, palette: p)
        } right: {
            VStack(alignment: .leading, spacing: 9) {
                Text(L.wrongCarTitle).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
                Text(L.wrongCarSub(found)).font(.system(size: 13)).foregroundStyle(p.muted)
                    .fixedSize(horizontal: false, vertical: true).frame(maxWidth: 260, alignment: .leading)
                Text(L.wrongCarHint(CarContract.ssid)).font(.system(size: 13)).foregroundStyle(p.muted)
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
