import SwiftUI

/// The firmware screen — one screen, both devices.
///
/// It renders whatever `FirmwareFlow` says and nothing else, which is what makes updating the
/// adapter look exactly like updating the car: same phases, same words, same buttons, same
/// place on the screen. The only thing that differs is the object under the chip, chosen by
/// `flow.device`.
///
/// The car's screen carries one more line: the radio co-processor's firmware against what this
/// build expects, from `/status` (`CarLink.radio`). The car corrects its own radio at boot, so a
/// mismatch is usually a transient state during one reboot — but one that stands after the
/// car's attempt budget is spent, or a radio that never answered, is visible nowhere else in
/// the app, and it costs five seconds of every boot (AJM-92). The adapter has no radio.
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
    /// The car's link, for the radio line — `nil` on the adapter's screen, which has none.
    /// Not observed here: `RadioLine` observes it, so the screen redraws for the radio only
    /// where the radio is drawn.
    private let link: CarLink?

    @Environment(\.dismiss) private var dismiss
    private var p: Palette { palette }
    private var device: UpdateRules.Device { flow.device }

    init(palette: Palette, flow: @autoclosure @escaping () -> FirmwareFlow, link: CarLink? = nil,
         forced: Bool = false, onDone: (() -> Void)? = nil, debugPhase: FwPhase? = nil) {
        self.palette = palette
        // `StateObject(wrappedValue:)` takes an autoclosure and evaluates it once, on the first
        // render — so the flow (and the `UpdateClient` inside it) survives the struct being
        // rebuilt, which SwiftUI does constantly.
        _flow = StateObject(wrappedValue: flow())
        self.link = link
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
            link?.refreshRadio()
            // Forced means the gate, and the gate offers no alternative — so it does not ask:
            // `runForced` is the whole automaton, check through flash, in this one task,
            // which lives as long as the screen. Opened from Settings the phases wait for
            // their buttons, because there the person came on purpose and the decision is
            // theirs.
            if forced { await flow.runForced { onDone?() } } else { await flow.check() }
        }
        .onChange(of: flow.phase) { _, phase in
            // An OTA just behind us may have changed the radio's answer — the car delivers its
            // radio image on the first boot of a new build, before it serves `/version`. Not in
            // the gallery, whose frozen link is the whole point of the frame.
            if phase == .done, debugPhase == nil { link?.refreshRadio() }
        }
    }

    @ViewBuilder private var stateBlock: some View {
        VStack(alignment: .leading, spacing: 9) {
            switch flow.phase {
            case .checking:
                title(L.fwChecking); sub(L.fwCurrent(flow.currentFw ?? "—"))
            case .upToDate:
                title(L.fwUpToDate); sub(L.fwVersionLine(flow.currentFw ?? "—"))
                // Forced: nothing to press — the automaton above hands the board back.
                if !forced { fwButton(L.fwRecheck, prominent: false) { Task { await flow.check() } } }
            case .available:
                title(forced ? L.gateUpdateTitle : L.fwAvailable)
                sub(forced ? L.gateUpdateSub
                           : L.fwTransition(flow.currentFw ?? "—", flow.targetTag ?? "—")
                             + (flow.offlineCache ? " · " + L.fwFromCache : ""))
                if !forced { fwButton(L.fwUpdate, prominent: true) { Task { await flow.download() } } }
            case .downloading:
                title(L.fwDownloadTitle)
                DownloadBar(progress: flow.downloadProgress,
                            caption: { "\(L.fwTransition(flow.currentFw ?? "—", flow.release?.tag ?? "")) · \(Int($0 * 100))%" },
                            palette: p)
            case .downloaded:
                title(L.fwConnectTitle(device))
                if forced {
                    // No button: the gate flashes by itself the moment the device answers
                    // (`flashWhenReachable`), and the line says so instead of offering a
                    // control that used to sit disabled with nothing explaining why.
                    sub(L.fwWaitingSub)
                } else {
                    sub(L.fwConnectSub(device))
                    fwButton(L.fwFlash, prominent: true, disabled: !flow.reachable) {
                        Task { await flow.flash() }
                    }
                }
            case .uploading:
                title(L.fwUploadTitle)
                sub("\(flow.targetTag ?? "") · \(Int(flow.uploadProgress * 100))%")
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
                // Forced: the automaton above hands the board back.
            case .failed:
                title(L.fwFailTitle)
                sub(flow.rolledBack ? L.fwRollbackSub(device) : L.fwFailLine(device, flow.failReason))
                if forced, flow.rolledBack {
                    // A rollback's «Повторить» goes back to the ladder, not through `check()`:
                    // `check()` would find the board behind the same release, and the
                    // automaton would flash the same image into the same rollback, forever.
                    // The ladder reads `rolled_back` from `/version` and applies the one rule
                    // that knows what rolled back — the build recorded at this flash's `ok`
                    // (`VersionRule`, AJM-132): a release newer than it is offered, the same
                    // one is not.
                    fwButton(L.fwRetry, prominent: true) { onDone?() }
                } else {
                    fwButton(L.fwRetry, prominent: true) { Task { await flow.check() } }
                }
            }
            if let link { RadioLine(link: link, palette: p) }
        }
    }

    /// The car's radio against what this build expects, on every phase — it is the only place
    /// in the app a standing mismatch can be noticed (AJM-92). Its own view so that only this
    /// line redraws when `/status` answers, and so that the adapter's screen, which passes no
    /// link, carries nothing.
    private struct RadioLine: View {
        @ObservedObject var link: CarLink
        let palette: Palette

        var body: some View {
            switch link.radio {
            case .known(let r) where r.state == .ok:
                line(L.fwRadio(r.fw ?? "—"), palette.muted)
            case .known(let r):
                // `mismatch`, or a state this build does not know — anything but `ok` names both
                // versions. `unavailable` is the radio not answering the car: no version to
                // name, only the one expected.
                line(r.fw.map { L.fwRadioMismatch($0, r.expected) } ?? L.fwRadioSilent(r.expected),
                     palette.warn)
            case .unavailable:
                line(L.fwRadioUnknown, palette.muted)
            case nil:
                EmptyView()
            }
        }

        private func line(_ t: String, _ color: Color) -> some View {
            Text(t).font(.system(size: 12)).foregroundStyle(color)
                .fixedSize(horizontal: false, vertical: true).frame(maxWidth: 260, alignment: .leading)
                .padding(.top, 6)
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
