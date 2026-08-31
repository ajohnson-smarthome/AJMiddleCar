import SwiftUI

struct FirmwareView: View {
    let palette: Palette
    var forced: Bool = false
    var onDone: (() -> Void)? = nil
    var debugPhase: FwPhase? = nil   // gallery: render a static phase, skip the network check
    @ObservedObject var link: CarLink
    @StateObject private var client = UpdateClient()

    @State private var release: UpdateClient.Release?
    @State private var binURL: URL?
    @State private var phase: FwPhase = .checking
    @State private var flashAttempted = false
    @State private var rolledBack = false
    @State private var offlineCache = false
    @State private var uploadTask: Task<UpdateClient.UploadOutcome, Never>?
    @State private var failReason: String?
    @Environment(\.dismiss) private var dismiss

    private var current: String { link.fw ?? "—" }
    private var p: Palette { palette }

    var body: some View {
        SplitScreen(palette: p, title: L.settingsFirmware, onBack: forced ? nil : { dismiss() }) {
            FirmwareCarView(phase: phase, palette: p)
        } right: {
            stateBlock
        }
        .task {
            if let dp = debugPhase { phase = dp; flashAttempted = true; return }
            link.refreshRadio()
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
                let target = offlineCache ? (UpdateClient.cachedTag ?? "—") : (release?.tag ?? "—")
                sub(forced ? L.gateUpdateSub
                           : L.fwTransition(current, target)
                             + (offlineCache ? " · " + L.fwFromCache : ""))
                fwButton(L.fwUpdate, prominent: true) { Task { await download() } }
            case .downloading:
                title(L.fwDownloadTitle)
                DownloadBar(progress: client.downloadProgress,
                            caption: { "\(L.fwTransition(current, release?.tag ?? "")) · \(Int($0 * 100))%" },
                            palette: p)
            case .downloaded:
                title(L.fwConnectTitle); sub(L.fwConnectSub)
                fwButton(L.fwFlash, prominent: true, disabled: !link.isLive) { Task { await flash() } }
            case .uploading:
                title(L.fwUploadTitle)
                sub("\(offlineCache ? (UpdateClient.cachedTag ?? "") : (release?.tag ?? "")) · \(Int(client.uploadProgress * 100))%")
                ProgressView(value: client.uploadProgress).tint(p.accent).frame(width: 160)
                fwButton(L.fwCancel, prominent: false) { uploadTask?.cancel() }
            case .rebooting:
                title(L.fwRebootTitle); sub(L.fwRebootWait)
            case .flashed:
                title(L.fwFlashedTitle); sub(L.fwFlashedSub)
                // Re-check, not skip. The image is written and the gate clears itself the moment
                // the car reports the new build — `carIdentified` re-asks on every telemetry
                // frame — so the only thing a person can usefully do here is ask again. Skipping
                // was the other option and is gone: see `GateRule`.
                if forced { fwButton(L.fwRetry, prominent: false) { Task { await check() } } }
            case .done:
                title(L.fwDoneTitle); sub(L.fwDoneSub(current))
                if forced { Color.clear.frame(width: 0, height: 0).onAppear { onDone?() } }
            case .failed:
                title(L.fwFailTitle)
                sub(rolledBack && link.rollback != false ? L.fwRollbackSub
                    : failReason.map { L.fwFailReason($0) } ?? L.fwFailSub)
                fwButton(L.fwRetry, prominent: true) { Task { await check() } }
            }
            radioLine
        }
    }

    /// The radio co-processor's firmware. Shown on every phase because it is the only place a
    /// pinned-version mismatch can be noticed: nothing else in the app reports it.
    @ViewBuilder private var radioLine: some View {
        switch link.radio {
        case .known(let fw, true):
            Text(L.fwRadio(fw)).font(.system(size: 12)).foregroundStyle(p.muted)
        case .known(let fw, false):
            Text(L.fwRadioMismatch(fw)).font(.system(size: 12)).foregroundStyle(p.warn)
                .fixedSize(horizontal: false, vertical: true).frame(maxWidth: 260, alignment: .leading)
        case .unavailable:
            Text(L.fwRadioUnknown).font(.system(size: 12)).foregroundStyle(p.muted)
        case nil:
            EmptyView()
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
        offlineCache = false
        // A rolledBack from an earlier bounce must not decorate a later, unrelated failure
        // with rollback copy (Task 6 review finding) — this is the only re-entry point for a
        // fresh check, whether from .upToDate's recheck or .failed's retry. Same for a stale
        // failReason: a previous upload's car-named reason must not survive to caption a
        // later, unrelated failure (e.g. GitHub unreachable with no usable cache).
        rolledBack = false
        failReason = nil
        if let r = await client.latestRelease() {
            release = r
            phase = UpdateClient.isUpdateAvailable(running: link.fw, latest: r.tag)
                ? .available : .upToDate
            return
        }
        release = nil
        // GitHub unreachable — the normal state on the car's internet-less AP. The launch
        // gate already downloaded the release it knew about; a cached image NEWER than the
        // car is flashable without any network (decision 4b). The car's build must be known:
        // with no car identity there is nothing to compare against.
        if UpdateClient.hasCachedFile,
           let cached = UpdateClient.cachedBuild,
           let car = UpdateRules.buildNumber(link.fw),
           cached > car {
            offlineCache = true
            binURL = UpdateClient.cachedBinURL
            phase = .available
            return
        }
        phase = .failed
    }
    private func download() async {
        if offlineCache {
            // The image is already on disk, validated at download time (decision 6); the
            // download phase would be a fetch of what we are standing on.
            phase = .downloaded
            return
        }
        guard let r = release else { return }
        phase = .downloading
        let t0 = Date()
        let recordAs = UpdateRules.buildNumber(r.tag).map { (build: $0, tag: r.tag) }
        if let url = await client.download(r.assetURL, recordAs: recordAs) {
            binURL = url
            await UpdateClient.holdAtLeast(UpdateClient.downloadMinDisplay, since: t0)
            phase = .downloaded
        } else { phase = .failed }
    }
    private func flash() async {
        guard let url = binURL else { return }
        flashAttempted = true
        rolledBack = false
        phase = .uploading
        failReason = nil
        let task = Task { await client.upload(url) }
        uploadTask = task
        let outcome = await task.value
        uploadTask = nil
        switch outcome {
        case .cancelled:
            // Back to flash-ready, not to failure: the user changed their mind, nothing broke.
            phase = .downloaded
            return
        case .failed(let reason):
            failReason = reason
            phase = .failed
            return
        case .ok:
            break
        }
        // The car acknowledged the upload: the image is written, set as boot target, and the
        // reboot is unconditional. From here the flash is COMMITTED — the question is only
        // whether this phone gets to watch the confirmation.
        phase = .rebooting
        let oldFw = link.fw
        var sawOffline = false
        let deadline = Date.now.addingTimeInterval(25)
        while Date.now < deadline {
            try? await Task.sleep(nanoseconds: 500_000_000)
            if let nf = link.fw, oldFw != nil, nf != oldFw { phase = .done; return }
            if !link.isLive { sawOffline = true }
            else if sawOffline {
                // The car came back — with the SAME firmware. That is a bootloader rollback
                // (or a flash that never took), never success: calling it done is what looped
                // the forced gate forever against a rolling-back release.
                rolledBack = true
                phase = .failed
                return
            }
        }
        // Deadline without a reconnect: the car's own reboot can outlast this window, or the
        // dongle relaying it can still be mid-rejoin of the car's Wi-Fi when it does. Either
        // way the phone's own connection to the dongle over USB never moved — nothing here
        // hopped networks — so it is the far end that has not caught up, not the link. The
        // flash is committed either way — report that, not failure.
        phase = .flashed
    }
}
