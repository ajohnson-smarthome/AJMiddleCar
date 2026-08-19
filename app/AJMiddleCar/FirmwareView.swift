import SwiftUI

struct FirmwareView: View {
    let palette: Palette
    var forced: Bool = false
    var onDone: (() -> Void)? = nil
    var debugPhase: FwPhase? = nil   // gallery: render a static phase, skip the network check
    @ObservedObject var status: CarStatus
    @StateObject private var client = UpdateClient()

    @State private var release: UpdateClient.Release?
    @State private var binURL: URL?
    @State private var phase: FwPhase = .checking
    @Environment(\.dismiss) private var dismiss

    private var current: String { status.fw ?? "—" }
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p, title: L.settingsFirmware, onBack: forced ? nil : { dismiss() }) {
            FirmwareCarView(phase: phase, palette: p)
        } right: {
            stateBlock
        }
        .task {
            if let dp = debugPhase { phase = dp; return }
            await check()
        }
    }

    @ViewBuilder private var stateBlock: some View {
        VStack(alignment: .leading, spacing: 9) {
            switch phase {
            case .checking:
                title(L.fwChecking); sub(L.fwCurrent(current))
            case .upToDate:
                title(L.fwUpToDate); sub(L.fwVersionLine(current))
                if forced { Color.clear.frame(width: 0, height: 0).onAppear { onDone?() } }
                else { fwButton(L.fwRecheck, prominent: false) { Task { await check() } } }
            case .available:
                title(forced ? L.gateUpdateTitle : L.fwAvailable)
                sub(forced ? L.gateUpdateSub : L.fwTransition(current, release?.tag ?? "—"))
                fwButton(L.fwUpdate, prominent: true) { Task { await download() } }
            case .downloading:
                title(L.fwDownloadTitle)
                DownloadBar(progress: client.downloadProgress,
                            caption: { "\(L.fwTransition(current, release?.tag ?? "")) · \(Int($0 * 100))%" },
                            palette: p)
            case .downloaded:
                title(L.fwConnectTitle); sub(L.fwConnectSub)
                fwButton(L.fwFlash, prominent: true, disabled: !status.online) { Task { await flash() } }
            case .uploading:
                title(L.fwUploadTitle)
                sub("\(release?.tag ?? "") · \(Int(client.uploadProgress * 100))%")
                ProgressView(value: client.uploadProgress).tint(p.accent).frame(width: 160)
            case .rebooting:
                title(L.fwRebootTitle); sub(L.fwRebootWait)
            case .done:
                title(L.fwDoneTitle); sub(L.fwDoneSub(current))
                if forced { Color.clear.frame(width: 0, height: 0).onAppear { onDone?() } }
            case .failed:
                title(L.fwFailTitle); sub(L.fwFailSub)
                fwButton(L.fwRetry, prominent: true) { Task { await check() } }
            }
        }
    }

    private func title(_ t: String) -> some View {
        Text(t).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
    }
    private func sub(_ t: String) -> some View {
        Text(t).font(.system(size: 14)).foregroundStyle(p.muted)
    }

    /// Custom pill button matching the mockups: accent-tinted fill + accent text (prominent),
    /// or transparent + muted text with a line border (ghost). Dimmed when disabled.
    private func fwButton(_ text: String, prominent: Bool, disabled: Bool = false,
                          _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(text)
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(disabled ? p.muted.opacity(0.5) : (prominent ? p.accent : p.muted))
                .padding(.horizontal, 16).padding(.vertical, 8)
                .background(RoundedRectangle(cornerRadius: 10)
                    .fill(prominent && !disabled ? p.accent.opacity(0.15) : Color.clear))
                .overlay(RoundedRectangle(cornerRadius: 10)
                    .stroke(disabled ? p.line.opacity(0.6) : (prominent ? p.accent.opacity(0.55) : p.line), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .disabled(disabled)
        .padding(.top, 3)
    }

    private func check() async {
        phase = .checking
        let r = await client.latestRelease()
        release = r
        guard let r else { phase = .failed; return }
        phase = UpdateClient.isUpdateAvailable(running: status.fw, latest: r.tag) ? .available : .upToDate
    }
    private func download() async {
        guard let r = release else { return }
        phase = .downloading
        let t0 = Date()
        if let url = await client.download(r.assetURL) {
            binURL = url
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
            phase = .downloaded
        } else { phase = .failed }
    }
    private func flash() async {
        guard let url = binURL else { return }
        phase = .uploading
        guard await client.upload(url) else { phase = .failed; return }
        phase = .rebooting
        let oldFw = status.fw
        var sawOffline = false
        let deadline = Date.now.addingTimeInterval(25)
        while Date.now < deadline {
            try? await Task.sleep(nanoseconds: 500_000_000)
            // Success = the firmware version changed (reboot confirmed) OR the classic
            // offline→online bounce. The version check also catches a fast reboot that never
            // tripped the offline debounce.
            if let nf = status.fw, oldFw != nil, nf != oldFw { phase = .done; return }
            if !status.online { sawOffline = true }
            else if sawOffline { phase = .done; return }
        }
        phase = .failed
    }
}
