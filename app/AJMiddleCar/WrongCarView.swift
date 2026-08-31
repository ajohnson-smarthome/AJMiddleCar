import SwiftUI

/// Shown when a car answered our hello with a different device identifier.
///
/// This screen exists because both cars are a softAP serving the same API at the same address:
/// whichever one answers at the address the app reaches is the one it hears from, and without an
/// explicit stop the app would drive the other car with this one's calibration. A wrong car must
/// not look like an offline car — it needs to be resolved, not waited out — so it gets its own
/// screen rather than a silent retry loop.
/// Identity now arrives in the hello reply, on the first exchange, so this can no longer be
/// missed by a probe that cancelled itself.
///
/// A protocol mismatch gets the same screen: a car is there, it answered, and it still cannot be
/// driven. Both the firmware and the mock answer a hello whose `proto` they do not serve for
/// exactly this reason — so the app can name the mismatch instead of sweeping the radar forever
/// while a perfectly healthy car sits three feet away.
struct WrongCarView: View {
    enum Kind: Equatable {
        case foreignDevice(String)
        case protoMismatch(theirs: Int)
    }

    let palette: Palette
    let kind: Kind
    let onRetry: () -> Void
    private var p: Palette { palette }

    init(palette: Palette, kind: Kind, onRetry: @escaping () -> Void) {
        self.palette = palette
        self.kind = kind
        self.onRetry = onRetry
    }

    private var title: String {
        switch kind {
        case .foreignDevice: return L.wrongCarTitle
        case .protoMismatch: return L.wrongProtoTitle
        }
    }

    private var subtitle: String {
        switch kind {
        case .foreignDevice(let found): return L.wrongCarSub(found, CarContract.device)
        case .protoMismatch(let theirs): return L.wrongProtoSub(theirs, CarContract.proto)
        }
    }

    private var hint: String {
        switch kind {
        case .foreignDevice: return L.wrongCarHint
        case .protoMismatch: return L.wrongProtoHint
        }
    }

    var body: some View {
        SplitScreen(palette: p) {
            FirmwareDeviceView(phase: .failed, palette: p)
        } right: {
            VStack(alignment: .leading, spacing: 9) {
                Text(title).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
                Text(subtitle).font(.system(size: 13)).foregroundStyle(p.muted)
                    .fixedSize(horizontal: false, vertical: true).frame(maxWidth: 260, alignment: .leading)
                Text(hint).font(.system(size: 13)).foregroundStyle(p.muted)
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
