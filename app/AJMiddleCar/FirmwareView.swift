import SwiftUI

/// The firmware screen — one screen, both devices.
///
/// It renders whatever `FirmwareFlow` says and nothing else, which is what makes updating the
/// adapter look exactly like updating the car: same phases, same words, same buttons, same
/// place on the screen. The only thing that differs is the object under the chip, chosen by
/// `flow.device`.
///
/// It used to carry one more line: the radio co-processor's version, car-only, kept because it
/// was the app's sole view of a pinned-version mismatch. That reason expired when the car learned
/// to correct its own radio at boot — a mismatch is now a transient state during one reboot
/// rather than something a person has to notice and act on.
struct FirmwareView: View {
    @StateObject private var flow: FirmwareFlow
    let palette: Palette
    /// Launch-gate mode: no way back, and the screen closes itself the moment the device is
    /// current. The gate also starts the update without asking — at that point there is no
    /// choice to offer, and a button would be theatre.
    var forced: Bool = false
    var onDone: (() -> Void)? = nil
    /// Gallery only: hold a phase, with no network behind it.
    var debugPhase: FwPhase? = nil

    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }
    private var device: UpdateRules.Device { flow.device }

    init(palette: Palette, flow: @autoclosure @escaping () -> FirmwareFlow, forced: Bool = false,
         onDone: (() -> Void)? = nil, debugPhase: FwPhase? = nil) {
        self.palette = palette
        // `StateObject(wrappedValue:)` takes an autoclosure and evaluates it once, on the first
        // render — so the flow (and the `UpdateClient` inside it) survives the struct being
        // rebuilt, which SwiftUI does constantly.
        _flow = StateObject(wrappedValue: flow())
        self.forced = forced
        self.onDone = onDone
        self.debugPhase = debugPhase
    }

    var body: some View {
        SplitScreen(palette: p, title: L.settingsFirmware, onBack: forced ? nil : { dismiss() }) {
            FirmwareDeviceView(device: device, phase: flow.phase, palette: p)
        } right: {
            stateBlock
        }
        .task {
            if let dp = debugPhase { flow.seed(dp); return }
            await flow.check()
            // Forced means the gate, and the gate offers no alternative — so it does not ask.
            // Opened from Settings the same phase waits for the button, because there the person
            // came on purpose and the decision is theirs.
            if forced, flow.phase == .available { await flow.download() }
        }
        .onChange(of: flow.phase) { _, new in
            // The download and the flash are one movement when the gate is driving.
            if forced, new == .downloaded, flow.reachable { Task { await flow.flash() } }
        }
    }

    @ViewBuilder private var stateBlock: some View {
        VStack(alignment: .leading, spacing: 9) {
            switch flow.phase {
            case .checking:
                title(L.fwChecking); sub(L.fwCurrent(flow.currentFw ?? "—"))
            case .upToDate:
                title(L.fwUpToDate); sub(L.fwVersionLine(flow.currentFw ?? "—"))
                if forced { Color.clear.frame(width: 0, height: 0).onAppear { onDone?() } }
                else { fwButton(L.fwRecheck, prominent: false) { Task { await flow.check() } } }
            case .available:
                title(forced ? L.gateUpdateTitle : L.fwAvailable)
                let target = flow.offlineCache
                    ? (UpdateClient.cachedTag(for: device) ?? "—") : (flow.release?.tag ?? "—")
                sub(forced ? L.gateUpdateSub
                           : L.fwTransition(flow.currentFw ?? "—", target)
                             + (flow.offlineCache ? " · " + L.fwFromCache : ""))
                if !forced { fwButton(L.fwUpdate, prominent: true) { Task { await flow.download() } } }
            case .downloading:
                title(L.fwDownloadTitle)
                DownloadBar(progress: flow.downloadProgress,
                            caption: { "\(L.fwTransition(flow.currentFw ?? "—", flow.release?.tag ?? "")) · \(Int($0 * 100))%" },
                            palette: p)
            case .downloaded:
                title(L.fwConnectTitle(device)); sub(L.fwConnectSub(device))
                fwButton(L.fwFlash, prominent: true, disabled: !flow.reachable) {
                    Task { await flow.flash() }
                }
            case .uploading:
                title(L.fwUploadTitle)
                sub("\(flow.offlineCache ? (UpdateClient.cachedTag(for: device) ?? "") : (flow.release?.tag ?? "")) · \(Int(flow.uploadProgress * 100))%")
                ProgressView(value: flow.uploadProgress).tint(p.accent).frame(width: 160)
                fwButton(L.fwCancel, prominent: false) { flow.cancelUpload() }
            case .rebooting:
                title(L.fwRebootTitle); sub(L.fwRebootWait(device))
            case .flashed:
                title(L.fwFlashedTitle); sub(L.fwFlashedSub(device))
                // Re-check, not skip. The image is written and the gate clears itself as soon as
                // the device reports the new build, so the only useful thing here is to ask
                // again. Skipping was the other option and is gone: see `GateRule`.
                if forced { fwButton(L.fwRetry, prominent: false) { Task { await flow.check() } } }
            case .done:
                title(L.fwDoneTitle); sub(L.fwDoneSub(flow.currentFw ?? "—"))
                if forced { Color.clear.frame(width: 0, height: 0).onAppear { onDone?() } }
            case .failed:
                title(L.fwFailTitle)
                sub(flow.rolledBack ? L.fwRollbackSub(device)
                    : flow.failReason.map { L.fwFailReason(device, $0) } ?? L.fwFailSub)
                fwButton(L.fwRetry, prominent: true) { Task { await flow.check() } }
            }
        }
    }

    private func title(_ t: String) -> some View {
        Text(t).font(.system(size: 22, weight: .semibold)).foregroundStyle(p.text)
    }
    private func sub(_ t: String) -> some View {
        Text(t).font(.system(size: 14)).foregroundStyle(p.muted)
    }

    /// Accent-tinted fill + accent text (prominent), or transparent with a line border (ghost).
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
}
