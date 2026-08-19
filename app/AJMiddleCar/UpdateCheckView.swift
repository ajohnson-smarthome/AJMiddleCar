import SwiftUI

/// Startup gate: checking GitHub / downloading the firmware into cache / check failed.
/// Reuses the firmware car animation; progress comes from the shared UpdateClient.
struct UpdateCheckView: View {
    let palette: Palette
    let phase: AppFlow.Phase
    @ObservedObject var client: UpdateClient
    let onRetry: () -> Void
    private var p: Palette { palette }

    private var fwPhase: FwPhase {
        switch phase {
        case .downloading: return .downloading
        case .checkFailed: return .failed
        default:           return .checking
        }
    }

    var body: some View {
        SplitScreen(palette: p) {
            FirmwareCarView(phase: fwPhase, palette: p)
        } right: {
            VStack(alignment: .leading, spacing: 9) {
                switch phase {
                case .downloading:
                    Text(L.fwDownloadTitle).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
                    DownloadBar(progress: client.downloadProgress,
                                caption: { "\(Int($0 * 100))%" }, palette: p)
                case .checkFailed:
                    Text(L.gateCheckFailedTitle).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
                    Text(L.gateCheckFailedSub).font(.system(size: 14)).foregroundStyle(p.muted)
                    Button(action: onRetry) {
                        Text(L.fwRetry).font(.system(size: 14, weight: .semibold)).foregroundStyle(p.accent)
                            .padding(.horizontal, 16).padding(.vertical, 8)
                            .background(RoundedRectangle(cornerRadius: 10).fill(p.accent.opacity(0.15)))
                            .overlay(RoundedRectangle(cornerRadius: 10).stroke(p.accent.opacity(0.55), lineWidth: 1))
                    }.buttonStyle(.plain).padding(.top, 3)
                default:
                    Text(L.fwChecking).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
                }
            }
        }
    }
}
