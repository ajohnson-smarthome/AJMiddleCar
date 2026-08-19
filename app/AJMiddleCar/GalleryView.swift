#if DEBUG
import SwiftUI

/// Debug-only screen gallery: every screen/state, tap left/right to navigate. Enabled via the
/// `-gallery` launch argument (see ESP32CarApp). Not compiled into release builds.
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

    @MainActor private func mockStatus(online: Bool = true, calibrated: Bool? = true,
                                       fw: String? = "v1.0+264", rssi: Int? = -55,
                                       wdtTrips: Int? = nil) -> CarStatus {
        let s = CarStatus()
        s.online = online; s.calibrated = calibrated; s.fw = fw
        s.rssi = rssi; s.wdtTrips = wdtTrips; s.uptimeS = 3847
        return s
    }

    @MainActor private func makeFrames(_ p: Palette) -> [(label: String, view: AnyView)] {
        let conn = CarConnection()
        func fw(_ phase: FwPhase, forced: Bool = false) -> AnyView {
            AnyView(NavigationStack { FirmwareView(palette: p, forced: forced, debugPhase: phase, status: mockStatus()) })
        }
        func calib(_ d: CalibrationView.CalDebug) -> AnyView {
            AnyView(NavigationStack { CalibrationView(palette: p, debugState: d) })
        }
        return [
            ("Connect (radar)",         AnyView(ConnectView())),
            ("NoInternet",              AnyView(NoInternetView(palette: p, onRetry: {}))),
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
            ("Drive arcade",            AnyView(DriveView(conn: conn, status: mockStatus(), preview: true).onAppear { UserDefaults.standard.set(Scheme.arcade.rawValue, forKey: "scheme") })),
            ("Drive tank",              AnyView(DriveView(conn: conn, status: mockStatus(), preview: true).onAppear { UserDefaults.standard.set(Scheme.tank.rawValue, forKey: "scheme") })),
            ("Drive warning",           AnyView(DriveView(conn: conn, status: mockStatus(wdtTrips: 3), preview: true))),
            ("Settings",                AnyView(NavigationStack { SettingsView(palette: p, status: mockStatus()) })),
            ("Calibration spin",        calib(.spin)),
            ("Calibration direction",   calib(.direction)),
            ("Calibration done",        calib(.done)),
            ("Calibration saving",      calib(.saving)),
            ("Calibration failed",      calib(.failed)),
            ("Ramp",                    AnyView(NavigationStack { RampView(palette: p) })),
            ("Trim",                    AnyView(NavigationStack { TrimView(palette: p) })),
            ("Recover",                 AnyView(NavigationStack { RecoverView(palette: p) })),
            ("Car dimensions",          AnyView(NavigationStack { CarDimensionsView(palette: p, wizard: true) })),
        ]
    }
}
#endif
