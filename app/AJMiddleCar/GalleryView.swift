#if DEBUG
import SwiftUI

/// Debug-only screen gallery: every screen/state, tap left/right to navigate. Enabled via the
/// `-gallery` launch argument (see AJMiddleCarApp). Not compiled into release builds.
struct GalleryView: View {
    @Environment(\.colorScheme) private var colorScheme
    @State private var index = 0
    private var p: Palette { Theme.current(colorScheme) }

    var body: some View {
        let frames = makeFrames(p)
        ZStack {
            p.bg.ignoresSafeArea()
            // .id forces SwiftUI to tear down + recreate on every switch — otherwise same-type frames
            // (FirmwareView/UpdateCheckView/CalibrationView) reuse @State and the .task/.onAppear that
            // seeds debugPhase/debugState never re-runs, so they'd all show one stale state.
            frames[index].view.id(index)
            HStack(spacing: 0) {
                Color.clear.contentShape(Rectangle())
                    .onTapGesture { index = (index - 1 + frames.count) % frames.count }
                Color.clear.contentShape(Rectangle())
                    .onTapGesture { index = (index + 1) % frames.count }
            }
            VStack {
                Text("\(index + 1) / \(frames.count)  \u{00B7}  \(frames[index].label)")
                    .font(.system(size: 11, weight: .medium)).monospacedDigit()
                    .foregroundStyle(p.text)
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .background(Capsule().fill(p.panel.opacity(0.9)))
                    .padding(.top, 8)
                Spacer()
            }
        }
        .statusBarHidden(true)
    }

    /// A link frozen in `.live` with plausible numbers — the gallery has no transport behind it.
    @MainActor private func mockLink(calibrated: Bool? = true, fw: String? = "v1.0+264",
                                     rssi: Int? = -55, wdtTrips: Int? = nil,
                                     busOk: Bool = true, ctl: String = CtlOwner.rt) -> CarLink {
        var t = Telemetry()
        t.calibrated = calibrated; t.rssi = rssi; t.wdtTrips = wdtTrips
        t.uptimeS = 3847; t.rxFps = 10; t.busOk = busOk; t.ctl = ctl
        return CarLink.preview(.live(t), fw: fw, radio: CarLink.Radio(fw: "3.0.6", ok: true))
    }

    @MainActor private func makeFrames(_ p: Palette) -> [(label: String, view: AnyView)] {
        let intent = ControlIntent()
        // The gallery has no car, and an unread domain honestly renders as "not read" — seed the
        // stores so the settings screens show their controls (one frame keeps the notice).
        ConfigStore.shared.ramp.seed(.default)
        ConfigStore.shared.trim.seed(.default)
        ConfigStore.shared.recover.seed(.default)
        ConfigStore.shared.wheel.seed(.default)
        ConfigStore.shared.dims.seed(.default)
        func fw(_ phase: FwPhase, forced: Bool = false) -> AnyView {
            AnyView(NavigationStack { FirmwareView(palette: p, forced: forced, debugPhase: phase, link: mockLink()) })
        }
        func calib(_ d: CalibrationView.CalDebug) -> AnyView {
            AnyView(NavigationStack { CalibrationView(palette: p, debugState: d) })
        }
        return [
            ("Connect (radar)",         AnyView(ConnectView())),
            ("No Wi-Fi",                AnyView(ConnectView(situation: .noWiFi(.notAvailable)))),
            ("Local network denied",    AnyView(ConnectView(situation: .localNetworkDenied))),
            ("NoInternet",              AnyView(NoInternetView(palette: p, onRetry: {}))),
            ("WrongCar",                AnyView(WrongCarView(palette: p, kind: .foreignDevice("esp32-car"), onRetry: {}))),
            ("WrongProto",              AnyView(WrongCarView(palette: p, kind: .protoMismatch(theirs: CarContract.proto + 1), onRetry: {}))),
            ("UpdateCheck checking",    AnyView(UpdateCheckView(palette: p, phase: .checkUpdate, client: UpdateClient(), onRetry: {}))),
            ("UpdateCheck downloading", AnyView(UpdateCheckView(palette: p, phase: .downloading, client: { let c = UpdateClient(); c.downloadProgress = 0.45; return c }(), onRetry: {}))),
            ("UpdateCheck failed",      AnyView(UpdateCheckView(palette: p, phase: .checkFailed, client: UpdateClient(), onRetry: {}))),
            ("Firmware checking",       fw(.checking)),
            ("Firmware upToDate",       fw(.upToDate)),
            ("Firmware available",      fw(.available)),
            ("Firmware downloading",    fw(.downloading)),
            ("Firmware downloaded",     fw(.downloaded)),
            ("Firmware uploading",      fw(.uploading)),
            ("Firmware rebooting",      fw(.rebooting)),
            ("Firmware done",           fw(.done)),
            ("Firmware failed",         fw(.failed)),
            ("Firmware forced",         fw(.available, forced: true)),
            ("Drive arcade",            AnyView(DriveView(link: mockLink(), intent: intent, preview: true).onAppear { UserDefaults.standard.set(Scheme.arcade.rawValue, forKey: "scheme") })),
            ("Drive tank",              AnyView(DriveView(link: mockLink(), intent: intent, preview: true).onAppear { UserDefaults.standard.set(Scheme.tank.rawValue, forKey: "scheme") })),
            ("Drive warning",           AnyView(DriveView(link: mockLink(wdtTrips: 3), intent: intent, preview: true))),
            ("Drive bus/ctl warning",   AnyView(DriveView(link: mockLink(busOk: false, ctl: CtlOwner.recover), intent: intent, preview: true))),
            ("Settings",                AnyView(NavigationStack { SettingsView(palette: p, link: mockLink()) })),
            ("Calibration spin",        calib(.spin)),
            ("Calibration spinning",    calib(.spinning)),
            ("Calibration spin failed", calib(.spinFailed)),
            ("Calibration direction",   calib(.direction)),
            ("Calibration done",        calib(.done)),
            ("Calibration saving",      calib(.saving)),
            ("Calibration failed",      calib(.failed)),
            ("Ramp",                    AnyView(NavigationStack { RampView(palette: p) })),
            ("Trim",                    AnyView(NavigationStack { TrimView(palette: p) })),
            ("Recover",                 AnyView(NavigationStack { RecoverView(palette: p) })),
            ("Car dimensions",          AnyView(NavigationStack { CarDimensionsView(palette: p, wizard: true) })),
            ("Wheel & motors",          AnyView(NavigationStack { WheelParamsView(palette: p) })),
            ("Config not read",         AnyView(NavigationStack { RampView(palette: p) }
                                            .onAppear { ConfigStore.shared.ramp.seedUnknown() })),
        ]
    }
}
#endif
